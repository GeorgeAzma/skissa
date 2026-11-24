/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOLAYERSTYLES_H
#define KISSTUDIOLAYERSTYLES_H

#include <kis_types.h>

#include <QPoint>
#include <QVector>

#include "kritaui_export.h"

class KisViewManager;
class QPainter;
class QRectF;
class QColor;
class QString;

/**
 * Layer styles as pictures: each style a tile showing the layer with it, a click turning it on or off, and the
 * chosen one's size, distance, opacity and color as value rows, seen on the canvas as they change and undone
 * as one step. Krita's dialog stays behind More….
 */
namespace KisStudioLayerStyles
{
/// Opens the popup for \p layer while the Studio is on, at \p at (the pointer when null), with style \p kind
/// chosen and turned on; false leaves it to Krita's dialog
KRITAUI_EXPORT bool open(KisViewManager *viewManager, KisLayerSP layer, int kind = -1, const QPoint &at = QPoint());
/// The styles turned on on \p layer, by kind
KRITAUI_EXPORT QVector<int> shownOn(KisLayerSP layer);
KRITAUI_EXPORT int kindCount();
KRITAUI_EXPORT QString nameOf(int kind);
/// A little drawing of style \p kind in \p ink, for a layer's row and card
KRITAUI_EXPORT void paintMark(QPainter *painter, const QRectF &rect, int kind, const QColor &ink);
}

#endif
