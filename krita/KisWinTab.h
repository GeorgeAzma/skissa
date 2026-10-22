/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISWINTAB_H
#define KISWINTAB_H

namespace KisWinTab
{
/**
 * Whether the tablet driver has WinTab but refuses to open it to anyone: Qt would then give up on the tablet,
 * and the pen would reach the canvas only as a mouse.
 */
bool refused();
}

#endif // KISWINTAB_H
