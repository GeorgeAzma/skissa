/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UPDATE_PACER_H
#define KIS_UPDATE_PACER_H

#include "kritaui_export.h"

/**
 * While the mouse or pen moves over a window with the canvas, the window
 * shows a frame for every refresh of the screen, as over the canvas itself
 * (where the brush outline is redrawn on every move).
 *
 * A screen with a variable refresh rate (G-Sync, FreeSync) refreshes as often
 * as the window shows frames: over a panel, where a move changes little, it
 * would slow down, the pointer would stutter, and some screens flicker as
 * their rate changes. Also: Qt 5 on Windows holds posted repaints back while
 * input waits; they are sent with these frames.
 */
namespace KisUpdatePacer
{
KRITAUI_EXPORT void install();
}

#endif
