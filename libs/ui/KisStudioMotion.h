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
 * Fade-in and fade-out on the KisModernStyle::motionCurve(). A window fades through its window opacity, a child widget
 * through a temporary opacity effect that is removed when the fade ends.
 */
namespace KisStudioMotion
{
/// Shows \p widget with a fade-in; does nothing if it is already shown.
KRITAUI_EXPORT void showFaded(QWidget *widget);
/// Fades \p widget out and hides it; showing it again during the fade keeps it.
KRITAUI_EXPORT void hideFaded(QWidget *widget);
/// True if \p widget is visible and not fading out. A fading widget already lets clicks through.
KRITAUI_EXPORT bool isShown(const QWidget *widget);
/// Ends a fade where it is, at full opacity, leaving \p widget shown or hidden as it is now.
KRITAUI_EXPORT void stopFade(QWidget *widget);
}

#endif // KISSTUDIOMOTION_H
