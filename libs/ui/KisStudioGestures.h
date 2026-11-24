/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOGESTURES_H
#define KISSTUDIOGESTURES_H

class KisMainWindow;

/**
 * The gestures that can't be seen on the buttons, each a little looping drawing of a hand doing it: what a
 * drag, a hold or a right click does where. Opened by its command (the main menu, Find).
 */
namespace KisStudioGestures
{
/// Gives \p window the command that opens them
void install(KisMainWindow *window);
}

#endif
