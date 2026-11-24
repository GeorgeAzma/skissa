/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_STUDIO_PATTERN_DRAG_H
#define KIS_STUDIO_PATTERN_DRAG_H

#include <functional>

#include <KoPattern.h>

#include "kis_types.h"
#include "kritaui_export.h"

class QMimeData;
class QWidget;
class KisViewManager;

/// A pattern carried by a drag, as a color or a gradient is: dropped where it should paint
namespace KisStudioPatternDrag
{
KRITAUI_EXPORT QString mimeType();
KRITAUI_EXPORT bool carries(const QMimeData *data);
KRITAUI_EXPORT KoPatternSP patternOf(const QMimeData *data);
/// Runs a drag of \p pattern from \p source
KRITAUI_EXPORT void start(QWidget *source, KoPatternSP pattern);
/// Lets \p widget be dragged from, carrying what \p pattern gives when the drag starts
KRITAUI_EXPORT void makeDraggable(QWidget *widget, std::function<KoPatternSP()> pattern);
/// Inside the selection on the layer in use; without one, a layer of its own over the whole image
KRITAUI_EXPORT void paintOnCanvas(KisViewManager *viewManager, KoPatternSP pattern);
/// Paints \p node's paint with the pattern, keeping where it is see-through
KRITAUI_EXPORT bool canFillLayer(KisNodeSP node);
KRITAUI_EXPORT void fillLayer(KisImageSP image, KisNodeSP node, KoPatternSP pattern);
}

#endif
