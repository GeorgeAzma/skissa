/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOFLASH_H
#define KISSTUDIOFLASH_H

#include "kritaui_export.h"

class QWidget;

/**
 * A change shown where it happened instead of in a message over the picture: the control that holds what changed
 * lights for a moment (the brush chip when the brush is switched by a key, the size when a key changes it). A
 * caller falls back to its message when the control isn't on screen (the bars tucked away, canvas-only mode).
 */
namespace KisStudioFlash
{
/// The widget named \p name in \p from's window, if it is on screen
KRITAUI_EXPORT QWidget *shown(QWidget *from, const char *name);
/// Lights \p target for a moment; false when it isn't on screen
KRITAUI_EXPORT bool flash(QWidget *target);
/// Lights the widget named \p name in \p from's window; false when there is none on screen
KRITAUI_EXPORT bool flash(QWidget *from, const char *name);
}

#endif
