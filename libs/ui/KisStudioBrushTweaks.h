/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHTWEAKS_H
#define KISSTUDIOBRUSHTWEAKS_H

#include "kritaui_export.h"

class KisMainWindow;

/**
 * Brush tweaks kept across a restart. Krita keeps a brush changed and not saved ("Keep brush tweaks") only in
 * memory, so quitting lost it. Each tweaked brush's settings are written to a file of the profile's a moment
 * after they change, and dropped once the brush is saved or put back; at the next start they go back onto
 * their brushes, which show as tweaked again. A brush whose file changed in between is left as saved. The
 * brush files themselves are never written.
 */
namespace KisStudioBrushTweaks
{
/// Follows \p window's brush; the first call puts back the tweaks kept from before
KRITAUI_EXPORT void watch(KisMainWindow *window);
}

#endif // KISSTUDIOBRUSHTWEAKS_H
