/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisLiquifyRenderer.h"

#include <algorithm>
#include <functional>
#include <vector>

#include <QMutex>
#include <QtMath>
#include <QRegion>
#include <QSharedPointer>
#include <QThread>
#include <QThreadPool>
#include <QWaitCondition>

#include <KoColor.h>
#include <KoColorSpace.h>
#include <KoMixColorsOp.h>

#include "kis_grid_interpolation_tools.h"
#include "kis_paint_device.h"

namespace {

const int tileSize = 128;

// runs work(0..count-1) on the calling thread and the global pool; a helper the pool starts late finds nothing to do
void forEachOnAllThreads(int count, const std::function<void(int)> &work)
{
    if (count <= 0) return;

    struct Shared {
        QAtomicInt next {0};
        QAtomicInt done {0};
        int count {0};
        std::function<void(int)> work;
        QMutex mutex;
        QWaitCondition finished;
    };
    QSharedPointer<Shared> shared(new Shared);
    shared->count = count;
    shared->work = work;

    auto run = [shared]() {
        int i;
        while ((i = shared->next.fetchAndAddRelaxed(1)) < shared->count) {
            shared->work(i);
            if (shared->done.fetchAndAddOrdered(1) + 1 == shared->count) {
                QMutexLocker locker(&shared->mutex);
                shared->finished.wakeAll();
            }
        }
    };

    const int helpers = qMin(QThread::idealThreadCount() - 1, count - 1);
    for (int i = 0; i < helpers; i++) {
        QThreadPool::globalInstance()->start(run);
    }
    run();

    QMutexLocker locker(&shared->mutex);
    while (shared->done.loadAcquire() < count) {
        shared->finished.wait(&shared->mutex);
    }
}

// where the row y crosses the polygon's edges, as QPolygonF::containsPoint finds them: a pixel x is
// inside when an odd number of the crossings are at or before it
inline int rowCrossings(const QPolygonF &polygon, qreal y, qreal *xs)
{
    int n = 0;
    auto edge = [&](const QPointF &p1, const QPointF &p2) {
        qreal x1 = p1.x();
        qreal y1 = p1.y();
        qreal x2 = p2.x();
        qreal y2 = p2.y();
        if (qFuzzyCompare(y1, y2)) return;
        if (y2 < y1) {
            std::swap(x1, x2);
            std::swap(y1, y2);
        }
        if (y >= y1 && y < y2) {
            xs[n++] = x1 + ((x2 - x1) / (y2 - y1)) * (y - y1);
        }
    };
    edge(polygon[0], polygon[1]);
    edge(polygon[1], polygon[2]);
    edge(polygon[2], polygon[3]);
    if (polygon[3] != polygon[0]) {
        edge(polygon[3], polygon[0]);
    }
    std::sort(xs, xs + n);
    return n;
}

// the first whole pixel at or after x, kept within [low, high]
inline int pixelFrom(qreal x, int low, int high)
{
    if (!(x > low)) return low;
    if (x > high) return high;
    return int(std::ceil(x));
}

struct Cell {
    int index;
    QRect bounds;
};

// a block of the source read at once; samples outside it are read one by one
struct SourceWindow {
    KisPaintDeviceSP device;
    int pixelSize {0};
    QRect rect;
    std::vector<quint8> bytes;

    void read(const QRect &wanted) {
        rect = wanted;
        bytes.resize(size_t(rect.width()) * rect.height() * pixelSize);
        device->readBytes(bytes.data(), rect);
    }

    inline const quint8 *at(int x, int y) const {
        return bytes.data() + (size_t(y - rect.top()) * rect.width() + (x - rect.left())) * pixelSize;
    }
};

}

namespace {

struct Job {
    KisPaintDeviceSP src;
    KisPaintDeviceSP dst;
    bool sameColorSpace;
    const KoMixColorsOp *mixOp;
    int pixelSize;
    QRect clip;
    bool clear;
    std::vector<const Cell *> cells;
};

}

KisLiquifyRenderer::KisLiquifyRenderer(const QSize &gridSize,
                                       const QVector<QPointF> &originalPoints,
                                       const QVector<QPointF> &transformedPoints,
                                       const QRect &subGrid)
    : m_gridSize(gridSize)
    , m_originalPoints(originalPoints)
    , m_transformedPoints(transformedPoints)
    , m_subGrid(subGrid)
{
}

namespace {

inline void cellPolygons(int tl, int gridWidth,
                         const QVector<QPointF> &originalPoints,
                         const QVector<QPointF> &transformedPoints,
                         QPolygonF &src, QPolygonF &dst)
{
    const int indexes[4] = {tl, tl + 1, tl + gridWidth + 1, tl + gridWidth};
    for (int i = 0; i < 4; i++) {
        src[i] = originalPoints[indexes[i]];
        dst[i] = transformedPoints[indexes[i]];
    }
    GridIterationTools::adjustAlignedPolygon(src);
    GridIterationTools::adjustAlignedPolygon(dst);
}

void drawJob(const Job &job,
             const QSize &gridSize,
             const QVector<QPointF> &originalPoints,
             const QVector<QPointF> &transformedPoints);

}

void KisLiquifyRenderer::render(KisPaintDeviceSP src, KisPaintDeviceSP dst) const
{
    draw(src, dst, QVector<QRect>(), false);
}

void KisLiquifyRenderer::render(KisPaintDeviceSP src, KisPaintDeviceSP dst, const QVector<QRect> &areas) const
{
    if (areas.isEmpty()) return;
    draw(src, dst, areas, true);
}

QRect KisLiquifyRenderer::cellsBounds(const QVector<int> &indexes) const
{
    const int w = m_gridSize.width();
    const int h = m_gridSize.height();
    QRectF bounds;
    for (int index : indexes) {
        const int col = index % w;
        const int row = index / w;
        for (int r = qMax(0, row - 1); r <= qMin(h - 2, row); r++) {
            for (int c = qMax(0, col - 1); c <= qMin(w - 2, col); c++) {
                const int tl = c + r * w;
                for (int i : {tl, tl + 1, tl + w + 1, tl + w}) {
                    KisAlgebra2D::accumulateBounds(m_transformedPoints[i], &bounds);
                    KisAlgebra2D::accumulateBounds(m_originalPoints[i], &bounds);
                }
            }
        }
    }
    return bounds.toAlignedRect().adjusted(-1, -1, 1, 1);
}

void KisLiquifyRenderer::draw(KisPaintDeviceSP src, KisPaintDeviceSP dst, const QVector<QRect> &areas, bool clear) const
{
    const QPoint start = m_subGrid.topLeft();
    const QPoint end = m_subGrid.bottomRight();
    const int rows = end.y() - start.y();
    const int cols = end.x() - start.x();
    if (rows <= 0 || cols <= 0) {
        if (clear) {
            for (const QRect &area : areas) dst->clear(area);
        }
        return;
    }

    QRect areasBounds;
    for (const QRect &area : areas) areasBounds |= area;

    // every cell's place, a row of cells at a time
    std::vector<std::vector<Cell>> rowCells(rows);
    forEachOnAllThreads(rows, [&](int i) {
        const int row = start.y() + i;
        QPolygonF srcPolygon(4);
        QPolygonF dstPolygon(4);
        std::vector<Cell> &cells = rowCells[i];
        cells.reserve(cols);
        for (int col = start.x(); col < end.x(); col++) {
            const int tl = col + row * m_gridSize.width();
            cellPolygons(tl, m_gridSize.width(), m_originalPoints, m_transformedPoints, srcPolygon, dstPolygon);
            const QRect bounds = dstPolygon.boundingRect().toAlignedRect();
            if (bounds.isEmpty()) continue;
            if (clear && !bounds.intersects(areasBounds)) continue;
            cells.push_back(Cell{tl, bounds});
        }
    });

    QRect reach;
    for (const std::vector<Cell> &cells : rowCells) {
        for (const Cell &cell : cells) reach |= cell.bounds;
    }

    // the tiles to draw, each with the cells reaching it in grid order
    QVector<QRect> clips;
    if (clear) {
        QRegion region;
        region.setRects(areas.constData(), areas.size());
        for (const QRect &rect : region) clips << rect;
    } else if (!reach.isEmpty()) {
        clips << reach;
    }
    if (clips.isEmpty()) return;

    const QRect bounds = clear ? areasBounds : reach;
    const int tileLeft = int(std::floor(qreal(bounds.left()) / tileSize));
    const int tileTop = int(std::floor(qreal(bounds.top()) / tileSize));
    const int tilesAcross = int(std::floor(qreal(bounds.right()) / tileSize)) - tileLeft + 1;
    const int tilesDown = int(std::floor(qreal(bounds.bottom()) / tileSize)) - tileTop + 1;

    std::vector<std::vector<const Cell *>> tileCells(size_t(tilesAcross) * tilesDown);
    for (const std::vector<Cell> &cells : rowCells) {
        for (const Cell &cell : cells) {
            const QRect r = cell.bounds & bounds;
            if (r.isEmpty()) continue;
            const int x0 = int(std::floor(qreal(r.left()) / tileSize)) - tileLeft;
            const int x1 = int(std::floor(qreal(r.right()) / tileSize)) - tileLeft;
            const int y0 = int(std::floor(qreal(r.top()) / tileSize)) - tileTop;
            const int y1 = int(std::floor(qreal(r.bottom()) / tileSize)) - tileTop;
            for (int y = y0; y <= y1; y++) {
                for (int x = x0; x <= x1; x++) {
                    tileCells[size_t(y) * tilesAcross + x].push_back(&cell);
                }
            }
        }
    }

    std::vector<Job> jobs;
    for (const QRect &clip : clips) {
        const int x0 = int(std::floor(qreal(clip.left()) / tileSize)) - tileLeft;
        const int x1 = int(std::floor(qreal(clip.right()) / tileSize)) - tileLeft;
        const int y0 = int(std::floor(qreal(clip.top()) / tileSize)) - tileTop;
        const int y1 = int(std::floor(qreal(clip.bottom()) / tileSize)) - tileTop;
        for (int y = y0; y <= y1; y++) {
            for (int x = x0; x <= x1; x++) {
                const std::vector<const Cell *> &cells = tileCells[size_t(y) * tilesAcross + x];
                if (cells.empty() && !clear) continue;
                const QRect tile((tileLeft + x) * tileSize, (tileTop + y) * tileSize, tileSize, tileSize);
                Job job {src, dst,
                         src->colorSpace() == dst->colorSpace(),
                         src->colorSpace()->mixColorsOp(),
                         int(dst->pixelSize()),
                         tile & clip, clear, {}};
                for (const Cell *cell : cells) {
                    if (cell->bounds.intersects(job.clip)) job.cells.push_back(cell);
                }
                if (job.cells.empty() && !clear) continue;
                jobs.push_back(std::move(job));
            }
        }
    }

    forEachOnAllThreads(int(jobs.size()), [&](int i) {
        drawJob(jobs[size_t(i)], m_gridSize, m_originalPoints, m_transformedPoints);
    });
}

namespace {

void drawJob(const Job &job,
             const QSize &gridSize,
             const QVector<QPointF> &originalPoints,
             const QVector<QPointF> &transformedPoints)
{
    const QRect clip = job.clip;
    KisPaintDeviceSP dst = job.dst;
    const int pixelSize = job.pixelSize;
    const KoColor defaultColor = dst->defaultPixel();
    const quint8 *defaultPixel = defaultColor.data();

    std::vector<quint8> out(size_t(clip.width()) * clip.height() * pixelSize);
    for (size_t i = 0; i < out.size(); i += pixelSize) {
        memcpy(out.data() + i, defaultPixel, pixelSize);
    }
    QRect written;
    auto outAt = [&](int x, int y) {
        return out.data() + (size_t(y - clip.top()) * clip.width() + (x - clip.left())) * pixelSize;
    };

    QPolygonF srcPolygon(4);
    QPolygonF dstPolygon(4);

    // the source the cells read, at once when it is near
    SourceWindow window;
    window.device = job.src;
    window.pixelSize = pixelSize;
    QRect sources;
    for (const Cell *cell : job.cells) {
        cellPolygons(cell->index, gridSize.width(), originalPoints, transformedPoints, srcPolygon, dstPolygon);
        sources |= srcPolygon.boundingRect().toAlignedRect().adjusted(-1, -1, 2, 2);
    }
    const qint64 near = 4 * qint64(tileSize + 16) * (tileSize + 16);
    if (!sources.isEmpty() && qint64(sources.width()) * sources.height() <= near) {
        window.read(sources);
    }

    std::vector<quint8> corners(4 * pixelSize);
    std::vector<quint8> fallback(pixelSize);

    // KisRandomSubAccessor::sampledOldRawData, from the window
    auto sample = [&](const QPointF &point, quint8 *dst) {
        const quint8 *pixels[4];
        qint16 weights[4];
        const int x = qFloor(point.x());
        const int y = qFloor(point.y());

        double hsub = point.x() - x;
        if (hsub < 0.0) hsub = 1.0 + hsub;
        double vsub = point.y() - y;
        if (vsub < 0.0) vsub = 1.0 + vsub;

        int sumOfWeights = 0;
        weights[0] = qRound((1.0 - hsub) * (1.0 - vsub) * 255);
        sumOfWeights += weights[0];
        weights[1] = qRound((1.0 - vsub) * hsub * 255);
        sumOfWeights += weights[1];
        weights[2] = qRound(vsub * (1.0 - hsub) * 255);
        sumOfWeights += weights[2];
        weights[3] = qRound(hsub * vsub * 255);
        sumOfWeights += weights[3];

        if (window.rect.contains(x, y) && window.rect.contains(x + 1, y + 1)) {
            pixels[0] = window.at(x, y);
            pixels[1] = pixels[0] + pixelSize;
            pixels[2] = window.at(x, y + 1);
            pixels[3] = pixels[2] + pixelSize;
        } else {
            job.src->readBytes(corners.data(), x, y, 2, 2);
            pixels[0] = corners.data();
            pixels[1] = corners.data() + pixelSize;
            pixels[2] = corners.data() + 2 * pixelSize;
            pixels[3] = corners.data() + 3 * pixelSize;
        }
        job.mixOp->mixColors(pixels, weights, 4, dst, sumOfWeights);
    };

    for (const Cell *cell : job.cells) {
        cellPolygons(cell->index, gridSize.width(), originalPoints, transformedPoints, srcPolygon, dstPolygon);
        const QRect area = cell->bounds & clip;
        if (area.isEmpty()) continue;
        written |= area;

        const bool samePolygon = job.sameColorSpace &&
            KisAlgebra2D::fuzzyPointCompare(srcPolygon, dstPolygon, 0.001);

        if (samePolygon && KisAlgebra2D::isPolygonPixelAlignedRect(dstPolygon, 0.001)) {
            const QRect source = dstPolygon.boundingRect().toAlignedRect() & area;
            if (window.rect.contains(source)) {
                for (int y = source.top(); y <= source.bottom(); y++) {
                    memcpy(outAt(source.left(), y), window.at(source.left(), y), size_t(source.width()) * pixelSize);
                }
            } else {
                for (int y = source.top(); y <= source.bottom(); y++) {
                    job.src->readBytes(outAt(source.left(), y), source.left(), y, source.width(), 1);
                }
            }
            continue;
        }

        if (!window.rect.contains(srcPolygon.boundingRect().toAlignedRect().adjusted(0, 0, 1, 1))) {
            const QRect cellSource = srcPolygon.boundingRect().toAlignedRect();
            window.read(cellSource.adjusted(-16, -16, 16 + tileSize, 16));
        }

        KisFourPointInterpolatorBackward interp(srcPolygon, dstPolygon);
        const bool valid = interp.isValid(0.1);
        if (!valid) {
            sample(interp.fallbackSourcePoint(), fallback.data());
        }

        qreal crossings[4];
        for (int y = area.top(); y <= area.bottom(); y++) {
            const int n = rowCrossings(dstPolygon, y, crossings);
            if (n == 0) continue;
            if (valid) interp.setY(y);

            for (int i = 0; i < n; i += 2) {
                const int from = pixelFrom(crossings[i], area.left(), area.right() + 1);
                const int to = i + 1 < n ? pixelFrom(crossings[i + 1], area.left(), area.right() + 1) : area.right() + 1;
                for (int x = from; x < to; x++) {
                    quint8 *pixel = outAt(x, y);
                    if (valid) {
                        interp.setX(x);
                        sample(interp.getValue(), pixel);
                    } else {
                        memcpy(pixel, fallback.data(), pixelSize);
                    }
                }
            }
        }
    }

    if (job.clear) {
        dst->writeBytes(out.data(), clip);
    } else if (!written.isEmpty()) {
        if (written == clip) {
            dst->writeBytes(out.data(), clip);
        } else {
            const size_t rowBytes = size_t(written.width()) * pixelSize;
            std::vector<quint8> part(rowBytes * written.height());
            for (int y = written.top(); y <= written.bottom(); y++) {
                memcpy(part.data() + (y - written.top()) * rowBytes, outAt(written.left(), y), rowBytes);
            }
            dst->writeBytes(part.data(), written);
        }
    }
}

}
