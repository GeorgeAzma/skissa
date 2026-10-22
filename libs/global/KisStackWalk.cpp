/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStackWalk.h"

namespace
{

#if defined(_M_X64) || defined(__x86_64__)
/**
 * The unwind entry of \p pc, read from its module's exception directory directly: RtlLookupFunctionEntry
 * takes the loader's locks, which the stopped thread may hold (it loads libraries at startup)
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
#endif

} // namespace

int KisStackWalk::walk(CONTEXT context, DWORD64 *frames, int most, ULONG_PTR low, ULONG_PTR high)
{
    int count = 0;
#if defined(_M_X64) || defined(__x86_64__)
    while (count < most && context.Rip) {
        if (context.Rsp < low || context.Rsp + 8 > high || (context.Rsp & 7)) break;
        frames[count++] = context.Rip;
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = lookup(context.Rip, &imageBase);
        if (function) {
            PVOID handlerData = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData, &establisher, nullptr);
        } else {
            // a leaf function: the return address is on top of the stack
            context.Rip = *reinterpret_cast<DWORD64 *>(context.Rsp);
            context.Rsp += 8;
        }
    }
#else
    (void)context;
    (void)frames;
    (void)most;
    (void)low;
    (void)high;
#endif
    return count;
}
