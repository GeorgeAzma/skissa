/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISLIQUIFYRENDERER_H
#define KISLIQUIFYRENDERER_H

#include <QPointF>
#include <QRect>
#include <QSize>
#include <QVector>

#include "kis_types.h"
#include "kritaimage_export.h"

/**
 * Draws a liquify grid exactly as GridIterationTools::iterateThroughGrid with
 * PaintDevicePolygonOp does, pixel for pixel, but on all threads: the result is
 * cut into tiles, each drawing the cells that reach it in grid order (so folds
 * stack the same), reading and writing whole blocks instead of single pixels.
 */
class KRITAIMAGE_EXPORT KisLiquifyRenderer
{
public:
    KisLiquifyRenderer(const QSize &gridSize,
                       const QVector<QPointF> &originalPoints,
                       const QVector<QPointF> &transformedPoints,
                       const QRect &subGrid);

    /// Draws the cells of the sub-grid into \p dst, which is expected to be clear
    void render(KisPaintDeviceSP src, KisPaintDeviceSP dst) const;

    /// Clears \p areas of \p dst and draws there what render() draws
    void render(KisPaintDeviceSP src, KisPaintDeviceSP dst, const QVector<QRect> &areas) const;

    /// Where the cells around the grid points \p indexes are drawn
    QRect cellsBounds(const QVector<int> &indexes) const;

private:
    void draw(KisPaintDeviceSP src, KisPaintDeviceSP dst, const QVector<QRect> &areas, bool clear) const;

    const QSize m_gridSize;
    const QVector<QPointF> &m_originalPoints;
    const QVector<QPointF> &m_transformedPoints;
    const QRect m_subGrid;
};

#endif
