/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UPDATE_PACER_H
#define KIS_UPDATE_PACER_H

#include "kritaui_export.h"

/**
 * The windows with the canvas are shown on a steady frame clock while the
 * mouse or pen moves: every refresh of the screen (or every few, as long as
 * a frame takes), whatever changed, their repaints held for its frames;
 * after the last move it slows down little by little, then stops.
 *
 * A screen with a variable refresh rate (G-Sync, FreeSync) refreshes when a
 * frame comes: shown as unevenly as the mouse reports (only when it moves)
 * and as the canvas finishes, the pointer would stutter, and many screens
 * flicker as their rate keeps changing.
 */
class QString;

namespace KisUpdatePacer
{
KRITAUI_EXPORT void install();

/// For development: \p what, with the time, in the frame log (KRITA_DEV_FRAMELOG), if there is one
KRITAUI_EXPORT void log(const QString &what);
}

#endif
