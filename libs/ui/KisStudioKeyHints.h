/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOKEYHINTS_H
#define KISSTUDIOKEYHINTS_H

#include "kritaui_export.h"

/**
 * Alt held alone shows every button's key on it as a key cap, until let go: keys are recognized on the
 * buttons people already know instead of recalled from a list.
 */
namespace KisStudioKeyHints
{
/// Starts watching for Alt held; once is enough for every window
KRITAUI_EXPORT void install();
}

#endif
