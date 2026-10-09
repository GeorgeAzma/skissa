/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOMOTION_H
#define KISSTUDIOMOTION_H

#include "kritaui_export.h"

class QWidget;

/**
 * Appearing and disappearing that is seen but never waited for: about a
 * tenth of a second, fast at first. A window fades with its opacity, a
 * widget inside one with an opacity effect that is removed once done.
 */
namespace KisStudioMotion
{
/// Shows \p widget, fading in; nothing happens if it is already shown
KRITAUI_EXPORT void showFaded(QWidget *widget);
/// Fades \p widget out, then hides it; shown again meanwhile, it stays
KRITAUI_EXPORT void hideFaded(QWidget *widget);
}

#endif // KISSTUDIOMOTION_H
