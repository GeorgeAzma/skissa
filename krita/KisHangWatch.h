/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_HANG_WATCH_H
#define KIS_HANG_WATCH_H

/**
 * Notices when the window stops responding: if the main thread hasn't run its event loop for ten
 * seconds, where it is stuck goes to krita-hang.log in the profile folder (its call stack, as the
 * crash log has it), once per hang. A hang otherwise leaves nothing to go on.
 */
namespace KisHangWatch
{
void start();
}

#endif
