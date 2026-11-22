/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPEEK_H
#define KISSTUDIOPEEK_H

#include "kritaui_export.h"

class QAction;
class KisCanvas2;

/**
 * Views to look at the picture differently for a moment. A switch's key tapped turns it on or off; held, it
 * was a look and goes back when let go (grayscale, mirrored, squint). Squint shows the picture blurred, so only
 * its big shapes and values are left.
 */
namespace KisStudioPeek
{
/// \p action (checkable, with a key) springs back when its key was held
KRITAUI_EXPORT void springBack(QAction *action);

/// Shows or hides the squint view over \p canvas
KRITAUI_EXPORT void setSquint(KisCanvas2 *canvas, bool on);
}

#endif
