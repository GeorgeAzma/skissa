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
 * Fading in and out, as everything moves (see KisModernStyle::motionCurve): a window by its opacity, a widget by an opacity effect
 * that is removed once done.
 */
namespace KisStudioMotion
{
/// Shows \p widget, fading in; nothing happens if it is already shown
KRITAUI_EXPORT void showFaded(QWidget *widget);
/// Fades \p widget out, then hides it; shown again meanwhile, it stays
KRITAUI_EXPORT void hideFaded(QWidget *widget);
/// Visible and not fading out (fading out, the pointer already reaches what is under it)
KRITAUI_EXPORT bool isShown(const QWidget *widget);
}

#endif // KISSTUDIOMOTION_H
