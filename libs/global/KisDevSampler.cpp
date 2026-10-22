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

#include "KisStackWalk.h"

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
        if (GetThreadContext(s->thread, &context)) count = KisStackWalk::walk(context, frames, maxFrames, s->stackLow, s->stackHigh);
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
        GetCurrentThreadStackLimits(&s->stackLow, &s->stackHigh);
        std::snprintf(s->path, sizeof(s->path), "%s", path);
        if (const char *seconds = std::getenv("KRITA_DEV_SAMPLE_SECONDS")) s->seconds = std::atoi(seconds) > 0 ? std::atoi(seconds) : 20;
        // loaded with the process, on its main thread
        s->thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, GetCurrentThreadId());
        if (s->thread) CloseHandle(CreateThread(nullptr, 0, run, s, 0, nullptr));
    }
} start;

} // namespace
