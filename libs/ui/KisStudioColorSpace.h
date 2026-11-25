/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_STUDIO_COLOR_SPACE_H
#define KIS_STUDIO_COLOR_SPACE_H

#include "kritaui_export.h"
#include "kis_types.h"

class KisViewManager;

/// Converting the image's colors in the Studio: the kind of color and its depth as tabs, the profile beside them,
/// in place of Krita's dialog; with \p layer, only that layer
namespace KisStudioColorSpace
{
KRITAUI_EXPORT void open(KisViewManager *viewManager, KisLayerSP layer = KisLayerSP());
}

#endif
