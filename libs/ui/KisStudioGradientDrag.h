/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOGRADIENTDRAG_H
#define KISSTUDIOGRADIENTDRAG_H

#include <KoAbstractGradient.h>
#include <QPoint>
#include <kis_types.h>

#include "kritaui_export.h"

class QMimeData;
class QWidget;
class KisViewManager;

/**
 * Gradients dragged like swatches: onto the canvas they paint the selection, or a new layer, across its width;
 * onto a layer they become its gradient map.
 */
namespace KisStudioGradientDrag
{
KRITAUI_EXPORT bool carries(const QMimeData *data);
KRITAUI_EXPORT KoAbstractGradientSP gradientOf(const QMimeData *data);
/// The gradient as Krita's gradient map keeps it (a "gradient" element with its stops)
KRITAUI_EXPORT QString xmlOf(KoAbstractGradientSP gradient);
KRITAUI_EXPORT void start(QWidget *source, KoAbstractGradientSP gradient);
/// Fills the area a color would fill at \p seed (image pixels; \p shown, the area previewed there, if any) with the
/// gradient from top to bottom, its ends left as handles to drag
KRITAUI_EXPORT void dropOnCanvas(KisViewManager *viewManager, KoAbstractGradientSP gradient, const QPoint &seed, KisPixelSelectionSP shown);
KRITAUI_EXPORT void mapLayer(KisImageSP image, KisNodeSP node, KoAbstractGradientSP gradient);
}

#endif
