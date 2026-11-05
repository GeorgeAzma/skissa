/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UPDATE_PACER_H
#define KIS_UPDATE_PACER_H

#include "kritaui_export.h"

class QOpenGLContext;
class QWidget;
class QString;

/**
 * Windows only: canvas windows repaint once per screen refresh rather than per mouse report, and
 * are kept composed by the desktop so VRR screens don't flicker.
 */

namespace KisUpdatePacer
{
KRITAUI_EXPORT void install();

/**
 * Shows \p canvas, a widget drawn with OpenGL that changed, in a frame of its own: Qt repaints every widget over
 * it when it is dirty in the same frame as any other, though they hold none of its pixels, so a frame where a
 * slider or a panel changed while the image did cost all the panels' painting. False where there is no pacer: the
 * canvas updates itself.
 */
KRITAUI_EXPORT bool presentCanvas(QWidget *canvas);

/// Lets Qt compose \p context's widget without waiting for the GPU, where the driver keeps shared contexts in order.
/// Call it with \p context current.
KRITAUI_EXPORT void composeWithoutWaiting(QOpenGLContext *context);

/// Development aid: appends \p what, timestamped, to the KRITA_DEV_FRAMELOG frame log if enabled.
KRITAUI_EXPORT void log(const QString &what);
/// Whether log() writes anywhere, to skip building lines nobody reads
KRITAUI_EXPORT bool logging();
}

#endif
