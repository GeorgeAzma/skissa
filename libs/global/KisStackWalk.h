/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTACKWALK_H
#define KISSTACKWALK_H

#include <windows.h>

#include "kritaglobal_export.h"

/**
 * The return addresses on a stopped thread's stack (x64), for the startup sampler and the hang watch.
 * It takes no locks and allocates nothing, since the stopped thread may hold the heap's or the loader's.
 */
namespace KisStackWalk
{
/// Walks from \p context, at most \p most frames, giving up when the stack pointer leaves \p low to \p high
KRITAGLOBAL_EXPORT int walk(CONTEXT context, DWORD64 *frames, int most, ULONG_PTR low, ULONG_PTR high);
}

#endif // KISSTACKWALK_H
