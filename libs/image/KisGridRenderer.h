/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISGRIDRENDERER_H
#define KISGRIDRENDERER_H

#include <QPointF>
#include <QRect>
#include <QSize>
#include <QVector>

#include "kis_types.h"
#include "kritaimage_export.h"

#include <functional>

/**
 * Draws a transformed grid (liquify, warp) exactly as GridIterationTools does
 * with PaintDevicePolygonOp, pixel for pixel, but on all threads: the result is
 * cut into tiles, each drawing the cells that reach it in grid order (so folds
 * stack the same), reading and writing whole blocks instead of single pixels.
 */
class KRITAIMAGE_EXPORT KisGridRenderer
{
public:
    /// How GridIterationTools makes a cell's polygons
    enum Cells {
        Nudged, ///< iterateThroughGrid: corners nudged by adjustAlignedPolygon (liquify)
        Exact ///< processGrid: corners as they are (warp)
    };

    KisGridRenderer(const QSize &gridSize,
                    const QVector<QPointF> &originalPoints,
                    const QVector<QPointF> &transformedPoints,
                    const QRect &subGrid,
                    Cells cells = Nudged);

    /// Runs \p work(0..count-1) on the calling thread and the global thread pool
    static void forEachOnAllThreads(int count, const std::function<void(int)> &work);

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
    const Cells m_cells;
};

#endif
