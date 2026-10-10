/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_FRAME_RATE_H
#define KIS_FRAME_RATE_H

#include <QtGlobal>

#include "kritaglobal_export.h"

/**
 * The pace of what moves on screen: the refresh rate of the fastest screen,
 * so that nothing (an animation, the canvas, feedback while dragging) is held
 * to a fixed rate such as 60 Hz on a faster screen.
 */
namespace KisFrameRate
{
/// The refresh rate of the fastest screen, in Hz (60 if it is not known)
KRITAGLOBAL_EXPORT qreal screenRate();

/// One frame of the fastest screen, in whole milliseconds (rounded down, at least 1)
KRITAGLOBAL_EXPORT int frameMs();

/**
 * Qt's animations (QVariantAnimation, QScroller, ...) advance once per frame
 * of the fastest screen, instead of every 16 ms. Call once, from the GUI
 * thread, after the application is created.
 */
KRITAGLOBAL_EXPORT void installAnimationDriver();
}

#endif
