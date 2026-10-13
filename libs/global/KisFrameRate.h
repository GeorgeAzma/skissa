/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_FRAME_RATE_H
#define KIS_FRAME_RATE_H

#include <QtGlobal>

#include "kritaglobal_export.h"

/**
 * Frame timing based on the fastest screen's refresh rate, so animations,
 * the canvas and drag feedback are not capped at 60 Hz on faster screens.
 */
namespace KisFrameRate
{
/// The refresh rate of the fastest screen in Hz, or 60 if unknown.
KRITAGLOBAL_EXPORT qreal screenRate();

/// One frame of the fastest screen in whole milliseconds (rounded down, at least 1).
KRITAGLOBAL_EXPORT int frameMs();

/**
 * Makes Qt's animations advance once per frame of the fastest screen instead
 * of every 16 ms. Call once from the GUI thread after the application exists.
 */
KRITAGLOBAL_EXPORT void installAnimationDriver();
}

#endif
