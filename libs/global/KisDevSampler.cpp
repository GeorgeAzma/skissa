/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

/**
 * For development: KRITA_DEV_SAMPLE=<file> samples the main thread's stack
 * about every millisecond from the moment Krita loads, for
 * KRITA_DEV_SAMPLE_SECONDS (default 20), and writes the samples and the
 * loaded modules to <file>. krita-deps/profile.py turns them into a profile.
 * Nothing happens without the variable.
 */

#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

constexpr int maxFrames = 96;

struct Sampler {
    HANDLE thread {nullptr};
    std::vector<DWORD64> samples; ///< per sample: time (us), frame count, frames
    char path[MAX_PATH] {};
    int seconds {20};
    /// The thread's stack: a walk that leaves it has lost its way
    ULONG_PTR stackLow {0};
    ULONG_PTR stackHigh {0};
};

Sampler *sampler = nullptr;

/**
 * The unwind entry of \p pc, read from its module's exception directory
 * directly: RtlLookupFunctionEntry takes the loader's locks, which the
 * suspended thread may hold (it loads libraries at startup), a deadlock
 */
PRUNTIME_FUNCTION lookup(DWORD64 pc, DWORD64 *imageBase)
{
    MEMORY_BASIC_INFORMATION memory;
    if (!VirtualQuery(reinterpret_cast<void *>(pc), &memory, sizeof(memory)) || memory.Type != MEM_IMAGE) return nullptr;
    auto *base = static_cast<BYTE *>(memory.AllocationBase);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return nullptr;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.VirtualAddress || !directory.Size) return nullptr;
    auto *table = reinterpret_cast<PRUNTIME_FUNCTION>(base + directory.VirtualAddress);
    const DWORD rva = DWORD(pc - DWORD64(base));
    int low = 0;
    int high = int(directory.Size / sizeof(RUNTIME_FUNCTION)) - 1;
    while (low <= high) {
        const int middle = (low + high) / 2;
        if (rva < table[middle].BeginAddress) high = middle - 1;
        else if (rva >= table[middle].EndAddress) low = middle + 1;
        else {
            *imageBase = DWORD64(base);
            return &table[middle];
        }
    }
    return nullptr;
}

/// Walks the stack of a suspended thread from its context, taking no locks (it may hold them: the heap's, the loader's)
int walk(CONTEXT context, DWORD64 *frames)
{
    int count = 0;
    while (count < maxFrames && context.Rip) {
        if (context.Rsp < sampler->stackLow || context.Rsp + 8 > sampler->stackHigh || (context.Rsp & 7)) break;
        frames[count++] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = lookup(context.Rip, &imageBase);
        if (function) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        } else {
            // a leaf function: the return address is on top of the stack
            if (!context.Rsp) break;
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8;
        }
    }
    return count;
}

DWORD WINAPI run(LPVOID parameter)
{
    Sampler *s = static_cast<Sampler *>(parameter);
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    s->samples.reserve(size_t(s->seconds) * 1100 * 24);
    DWORD64 frames[maxFrames];
    for (;;) {
        QueryPerformanceCounter(&now);
        const DWORD64 micros = DWORD64((now.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
        if (micros > DWORD64(s->seconds) * 1000000) break;
        if (SuspendThread(s->thread) == DWORD(-1)) break;
        CONTEXT context {};
        context.ContextFlags = CONTEXT_FULL;
        int count = 0;
        if (GetThreadContext(s->thread, &context)) count = walk(context, frames);
        ResumeThread(s->thread);
        // filled only while the thread runs: growing the vector may take the heap's lock
        if (s->samples.size() + 2 + count > s->samples.capacity()) break;
        s->samples.push_back(micros);
        s->samples.push_back(DWORD64(count));
        s->samples.insert(s->samples.end(), frames, frames + count);
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -10000; // 1 ms
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        } else {
            Sleep(1);
        }
    }

    FILE *out = std::fopen(s->path, "w");
    if (!out) return 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    MODULEENTRY32W module;
    module.dwSize = sizeof(module);
    for (BOOL more = Module32FirstW(snapshot, &module); more; more = Module32NextW(snapshot, &module)) {
        std::fprintf(out, "module %llx %lx %ls\n", (unsigned long long)module.modBaseAddr, module.modBaseSize, module.szExePath);
    }
    CloseHandle(snapshot);
    for (size_t i = 0; i + 1 < s->samples.size();) {
        const DWORD64 micros = s->samples[i];
        const size_t count = size_t(s->samples[i + 1]);
        std::fprintf(out, "sample %llu", (unsigned long long)micros);
        for (size_t f = 0; f < count; f++) std::fprintf(out, " %llx", (unsigned long long)s->samples[i + 2 + f]);
        std::fprintf(out, "\n");
        i += 2 + count;
    }
    std::fclose(out);
    return 0;
}

struct Start {
    Start()
    {
        const char *path = std::getenv("KRITA_DEV_SAMPLE");
        if (!path || !*path) return;
        auto *s = new Sampler;
        sampler = s;
        GetCurrentThreadStackLimits(&s->stackLow, &s->stackHigh);
        std::snprintf(s->path, sizeof(s->path), "%s", path);
        if (const char *seconds = std::getenv("KRITA_DEV_SAMPLE_SECONDS")) s->seconds = std::atoi(seconds) > 0 ? std::atoi(seconds) : 20;
        // loaded with the process, on its main thread
        s->thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
        if (s->thread) CloseHandle(CreateThread(nullptr, 0, run, s, 0, nullptr));
    }
} start;

} // namespace
