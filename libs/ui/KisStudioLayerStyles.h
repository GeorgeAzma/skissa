/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOLAYERSTYLES_H
#define KISSTUDIOLAYERSTYLES_H

#include <kis_types.h>

#include "kritaui_export.h"

class KisViewManager;

/**
 * Layer styles as pictures: each style a tile showing the layer with it, a click turning it on or off, and the
 * chosen one's size, distance, opacity and color as value rows, seen on the canvas as they change and undone
 * as one step. Krita's dialog stays behind More….
 */
namespace KisStudioLayerStyles
{
/// Opens the popup for \p layer while the Studio is on; false leaves it to Krita's dialog
KRITAUI_EXPORT bool open(KisViewManager *viewManager, KisLayerSP layer);
}

#endif
