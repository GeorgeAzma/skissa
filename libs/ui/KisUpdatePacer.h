/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UPDATE_PACER_H
#define KIS_UPDATE_PACER_H

#include "kritaui_export.h"

/**
 * The windows with the canvas are shown on a frame clock: their repaints
 * held for its frames, one every refresh of the screen (or every few, as
 * long as a frame takes), not one for each report of the mouse.
 *
 * And they are always composed by the desktop, as any other window is: a
 * screen with a variable refresh rate (G-Sync, FreeSync) would otherwise
 * take a window covering it for a game, and refresh only as Krita shows
 * frames (48 Hz at rest), flickering as that rate changes.
 */
class QString;

namespace KisUpdatePacer
{
KRITAUI_EXPORT void install();

/// For development: \p what, with the time, in the frame log (KRITA_DEV_FRAMELOG), if there is one
KRITAUI_EXPORT void log(const QString &what);
}

#endif
