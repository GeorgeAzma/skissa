/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISPATTERNTINT_H
#define KISPATTERNTINT_H

#include <KoColor.h>
#include <KoPattern.h>

#include "kritaui_export.h"

/**
 * Tint: a pattern painted in the current color, its darkness as how much paint (ink on paper), so any
 * texture fills in any color. One switch, `StudioPatternTint`, for every fill that takes the pattern.
 */
namespace KisPatternTint
{
KRITAUI_EXPORT bool isOn();
KRITAUI_EXPORT void setOn(bool on);
/// The pattern as fills take it: tinted with \p color while Tint is on, else as it is
KRITAUI_EXPORT KoPatternSP applied(KoPatternSP pattern, const KoColor &color);
}

#endif
