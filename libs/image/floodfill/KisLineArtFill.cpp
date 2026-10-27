/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisLineArtFill.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <new>
#include <vector>

#include <QColor>
#include <QImage>
#include <QVector>

#include <KoColor.h>
#include <KoColorSpace.h>

#include "KisColorSelectionPolicies.h"
#include "kis_global.h"
#include "kis_paint_device.h"
#include "kis_pixel_selection.h"
#include "kis_sequential_iterator.h"

namespace
{

/// How far a fill may reach under the lines beyond where the flood stopped.
constexpr int reach = 64;
/// The most pixels a fill grows over at once (about 4900 x 4900, some 250 MB of work); larger fills grow by tiles.
constexpr qint64 largestArea = 24'000'000;
/// A tile of a large fill, and the room around it read too, so what it decides near its edges sees what is beyond
constexpr int tileSize = 4096;
constexpr int tileRoom = 2 * reach;

template<typename Policy>
void measure(const Policy &policy, KisPaintDeviceSP device, const QRect &rect, std::vector<quint8> &out)
{
    KisSequentialConstIterator it(device, rect);
    size_t i = 0;
    while (it.nextPixel()) out[i++] = policy.difference(it.rawDataConst());
}

/// How much each pixel of \p rect differs from \p seedColor, on the scale of the flood fill's threshold.
std::vector<quint8> differences(KisPaintDeviceSP device, const KoColor &seedColor, int threshold, const QRect &rect)
{
    using namespace KisColorSelectionPolicies;
    std::vector<quint8> out(size_t(rect.width()) * rect.height());
    const KoColor seed = seedColor.convertedTo(device->colorSpace());
    switch (device->pixelSize()) {
    case 1: measure(OptimizedDifferencePolicy<quint8>(seed, threshold), device, rect, out); break;
    case 2: measure(OptimizedDifferencePolicy<quint16>(seed, threshold), device, rect, out); break;
    case 4: measure(OptimizedDifferencePolicy<quint32>(seed, threshold), device, rect, out); break;
    case 8: measure(OptimizedDifferencePolicy<quint64>(seed, threshold), device, rect, out); break;
    default: measure(SlowDifferencePolicy(seed, threshold), device, rect, out); break;
    }
    return out;
}

template<typename Visit>
inline void forNeighbors(int i, int width, int height, Visit visit)
{
    const int x = i % width, y = i / width;
    if (x > 0) visit(i - 1);
    if (x < width - 1) visit(i + 1);
    if (y > 0) visit(i - width);
    if (y < height - 1) visit(i + width);
}

/**
 * Gives every pixel without a label (0) the label of the region that reaches it first, the regions
 * spreading through the lowest \p level first: a watershed. Regions meet along the ridges of the
 * levels, the middle of the lines. Negative labels spread too.
 */
void watershed(const std::vector<quint8> &level, std::vector<qint32> &labels, int width, int height)
{
    const int count = width * height;
    std::array<std::vector<qint32>, 256> buckets;
    for (int i = 0; i < count; i++) {
        if (!labels[i]) continue;
        bool open = false;
        forNeighbors(i, width, height, [&](int j) { open |= labels[j] == 0; });
        if (open) buckets[level[i]].push_back(i);
    }
    for (int current = 0; current < 256; current++) {
        std::vector<qint32> &bucket = buckets[current];
        // pixels reached at this level join the same bucket, so it grows while it is read
        for (size_t n = 0; n < bucket.size(); n++) {
            const int i = bucket[n];
            forNeighbors(i, width, height, [&](int j) {
                if (labels[j]) return;
                labels[j] = labels[i];
                buckets[qMax<int>(level[j], current)].push_back(j);
            });
        }
        std::vector<qint32>().swap(bucket);
    }
}

/// The value a share \p quantile of \p values are at most, 0 if there are too few to say.
int quantile(std::vector<int> &values, qreal quantile, size_t enough)
{
    if (values.size() < enough) return 0;
    const size_t at = qMin(values.size() - 1, size_t(values.size() * quantile));
    std::nth_element(values.begin(), values.begin() + at, values.end());
    return values[at];
}

} // namespace

/// \p region grown within \p room (of the image's \p bounds), written into \p into where it is inside \p written
static void growArea(KisPixelSelectionSP region, KisPaintDeviceSP reference, const KoColor &seedColor, int threshold,
                     bool underLines, int speckSize, int closeGap, const QRect &bounds, KisPaintDeviceSP target,
                     const QRect &room, KisPixelSelectionSP into, const QRect &written)
{
    const QRect filled = region->selectedExactRect() & room;
    if (filled.isEmpty()) return;
    const QRect area = filled.adjusted(-reach, -reach, reach, reach) & room;
    const int width = area.width(), height = area.height(), count = width * height;

    const std::vector<quint8> level = differences(reference, seedColor, threshold, area);
    auto open = [&](int i) { return level[i] <= threshold; };

    // the flooded region is label 1
    std::vector<qint32> labels(count, 0);
    std::vector<quint8> own(count, 0);
    {
        KisSequentialConstIterator it(region, area);
        int i = 0;
        while (it.nextPixel()) {
            if (*it.rawDataConst() > MIN_SELECTED) {
                own[i] = 1;
                labels[i] = 1;
            }
            i++;
        }
    }

    // Every other area of one color competes for the pixels between it and the region: the paper beyond a
    // line, a color filled earlier, the solid middle of a thick line. Areas without an inside have no say:
    // the soft edges of lines (thin bands, however long), specks, and corners the gap closing cut off
    // (they touch the region without a line between).
    // the color difference of KoColorSpace::differenceA, from Lab taken once for the whole area: per pair
    // of pixels, it would convert both every time
    std::vector<quint16> lab(size_t(count) * 4);
    {
        const int pixelSize = reference->pixelSize();
        std::vector<quint8> pixels(size_t(count) * pixelSize);
        reference->readBytes(pixels.data(), area);
        reference->colorSpace()->toLabA16(pixels.data(), reinterpret_cast<quint8 *>(lab.data()), count);
    }
    auto difference = [&](int i, int j) {
        const quint16 *a = lab.data() + size_t(i) * 4, *b = lab.data() + size_t(j) * 4;
        const int alphaA = qRound(a[3] / 257.0), alphaB = qRound(b[3] / 257.0);
        if (alphaA == 0 || alphaB == 0) return qRound(100.0 / 255.0 * qAbs(alphaA - alphaB));
        const qreal dL = (int(a[0]) - int(b[0])) / 655.35;
        const qreal da = (int(a[1]) - int(b[1])) / 257.0;
        const qreal db = (int(a[2]) - int(b[2])) / 257.0;
        const qreal dAlpha = (int(a[3]) - int(b[3])) * 100.0 / 65535.0;
        return int(qMin<qreal>(255.0, std::sqrt(dL * dL + da * da + db * db + dAlpha * dAlpha)));
    };
    const int speckArea = qMax(speckSize * speckSize, 4);
    const int cornerArea = closeGap > 0 ? 9 * closeGap * closeGap : 0;
    // while areas are found, their pixels carry a tag below these
    constexpr qint32 fenced = -1, absorbed = -2;
    std::vector<qint32> stack, members;
    qint32 next = 2, tag = absorbed;
    for (int start = 0; start < count; start++) {
        if (labels[start]) continue;
        members.clear();
        stack.push_back(start);
        labels[start] = --tag;
        bool touchesRegion = false;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            members.push_back(i);
            forNeighbors(i, width, height, [&](int j) {
                if (own[j]) {
                    touchesRegion = true;
                } else if (!labels[j] && difference(start, j) <= threshold) {
                    labels[j] = tag;
                    stack.push_back(j);
                }
            });
        }
        // its inside: pixels with all four neighbors in it
        int inside = 0;
        for (int i : members) {
            const int x = i % width, y = i / width;
            if (x > 0 && x < width - 1 && y > 0 && y < height - 1 && labels[i - 1] == tag && labels[i + 1] == tag
                && labels[i - width] == tag && labels[i + width] == tag) {
                inside++;
            }
        }
        const int size = int(members.size());
        const bool says = inside >= speckArea && !(touchesRegion && open(start) && size < cornerArea);
        for (int i : members) labels[i] = says ? next : absorbed;
        if (says) next++;
    }
    for (qint32 &label : labels) {
        if (label == absorbed) label = 0;
    }

    // What lies beyond the edges of the area (but inside the image) belongs to someone else.
    auto fence = [&](int i) {
        if (!labels[i]) labels[i] = fenced;
    };
    if (area.left() > bounds.left()) for (int y = 0; y < height; y++) fence(y * width);
    if (area.right() < bounds.right()) for (int y = 0; y < height; y++) fence(y * width + width - 1);
    if (area.top() > bounds.top()) for (int x = 0; x < width; x++) fence(x);
    if (area.bottom() < bounds.bottom()) for (int x = 0; x < width; x++) fence((height - 1) * width + x);

    watershed(level, labels, width, height);

    // under lines only where the layer filled has no ink of its own: on the layer of the lines it would cover them
    std::vector<bool> targetInk;
    if (underLines && target) {
        const QImage image = target->convertToQImage(nullptr, area.x(), area.y(), area.width(), area.height());
        targetInk.resize(size_t(count));
        for (int y = 0; y < height; y++) {
            const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < width; x++) targetInk[size_t(y) * width + x] = qAlpha(row[x]) * (255 - qGray(row[x])) / 255 > 64;
        }
    }
    const QRect out = area & written;
    if (out.isEmpty()) return;
    KisSequentialIterator it(into, out);
    while (it.nextPixel()) {
        const int i = (it.y() - area.y()) * width + it.x() - area.x();
        if (!own[i] && labels[i] == 1 && (open(i) || (underLines && (targetInk.empty() || !targetInk[i])))) *it.rawData() = MAX_SELECTED;
    }
    into->invalidateOutlineCache();
}

void KisLineArtFill::grow(KisPixelSelectionSP region, KisPaintDeviceSP reference, const KoColor &seedColor, int threshold,
                          bool underLines, int speckSize, int closeGap, const QRect &bounds, KisPaintDeviceSP target)
{
    if (!region || !reference || (!underLines && speckSize <= 1 && closeGap <= 0)) return;
    const QRect filled = region->selectedExactRect() & bounds;
    if (filled.isEmpty()) return;
    const QRect area = filled.adjusted(-reach, -reach, reach, reach) & bounds;
    // without the memory it needs, the fill stays as Krita's flood fill made it
    try {
        if (qint64(area.width()) * area.height() <= largestArea) {
            growArea(region, reference, seedColor, threshold, underLines, speckSize, closeGap, bounds, target, bounds, region, bounds);
            return;
        }
        // each tile reads the fill as the flood made it, not as the tiles before grew it
        KisPixelSelectionSP flooded = new KisPixelSelection(*region);
        for (int y = area.top(); y <= area.bottom(); y += tileSize) {
            for (int x = area.left(); x <= area.right(); x += tileSize) {
                const QRect tile = QRect(x, y, tileSize, tileSize) & area;
                const QRect room = tile.adjusted(-tileRoom, -tileRoom, tileRoom, tileRoom) & bounds;
                growArea(flooded, reference, seedColor, threshold, underLines, speckSize, closeGap, bounds, target, room, region, tile);
            }
        }
    } catch (const std::bad_alloc &) {
    }
}

int KisLineArtFill::lineWidthNear(KisPaintDeviceSP reference, const QPoint &point, const QRect &bounds, qreal share)
{
    const QRect area = QRect(point - QPoint(256, 256), QSize(512, 512)) & bounds;
    if (area.isEmpty() || !reference) return 0;
    const QImage picture = reference->convertToQImage(nullptr, area).convertToFormat(QImage::Format_ARGB32);
    const int width = picture.width(), height = picture.height();
    std::vector<quint8> ink(size_t(width) * height);
    for (int y = 0; y < height; y++) {
        const QRgb *row = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
        for (int x = 0; x < width; x++) ink[y * width + x] = quint8(qAlpha(row[x]) * (255 - qGray(row[x])) / 255);
    }
    // runs of ink with something else on both sides, across and down
    std::vector<int> runs;
    auto scan = [&](int lines, int length, auto at) {
        for (int a = 0; a < lines; a++) {
            int run = 0;
            bool bounded = false;
            for (int b = 0; b < length; b++) {
                if (ink[at(a, b)] > 96) {
                    run++;
                } else {
                    if (run && bounded && run <= 64) runs.push_back(run);
                    run = 0;
                    bounded = true;
                }
            }
        }
    };
    scan(height, width, [width](int y, int x) { return y * width + x; });
    scan(width, height, [width](int x, int y) { return y * width + x; });
    return quantile(runs, share, 8);
}

KisPaintDeviceSP KisLineArtFill::flats(KisPaintDeviceSP lines, const QRect &bounds, int closeGap, int speckSize,
                                       const KoColorSpace *colorSpace, const std::function<bool()> &cancelled)
{
    auto stop = [&]() { return cancelled && cancelled(); };
    if (!lines || bounds.isEmpty()) return nullptr;
    // How much each pixel is ink: dark and opaque. Paper, white or clear, is none. The picture goes once read
    // (four bytes a pixel, a large share of what this needs on a big image)
    int width = 0, height = 0;
    std::vector<quint8> ink;
    {
        const QImage picture = lines->convertToQImage(nullptr, bounds).convertToFormat(QImage::Format_ARGB32);
        width = picture.width();
        height = picture.height();
        ink.resize(size_t(width) * height);
        for (int y = 0; y < height; y++) {
            const QRgb *row = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
            for (int x = 0; x < width; x++) {
                const QRgb p = row[x];
                ink[y * width + x] = quint8(qAlpha(p) * (255 - qGray(p)) / 255);
            }
        }
    }
    const int count = width * height;
    if (!count) return nullptr;
    const quint8 lineInk = 64;
    if (stop()) return nullptr;

    // Distance to the nearest line, so gaps narrower than the closing stay shut (3-4 chamfer).
    std::vector<quint16> distance(count);
    const quint16 far = 60000;
    for (int i = 0; i < count; i++) distance[i] = ink[i] > lineInk ? 0 : far;
    auto relax = [&](int i, int j, int step) {
        if (distance[j] + step < distance[i]) distance[i] = quint16(distance[j] + step);
    };
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int i = y * width + x;
            if (x > 0) relax(i, i - 1, 3);
            if (y > 0) {
                relax(i, i - width, 3);
                if (x > 0) relax(i, i - width - 1, 4);
                if (x < width - 1) relax(i, i - width + 1, 4);
            }
        }
    }
    for (int y = height - 1; y >= 0; y--) {
        for (int x = width - 1; x >= 0; x--) {
            const int i = y * width + x;
            if (x < width - 1) relax(i, i + 1, 3);
            if (y < height - 1) {
                relax(i, i + width, 3);
                if (x < width - 1) relax(i, i + width + 1, 4);
                if (x > 0) relax(i, i + width - 1, 4);
            }
        }
    }
    if (stop()) return nullptr;

    // The cores of the areas: pixels further from any line than half the gap.
    const int clearance = closeGap > 0 ? 3 * closeGap / 2 : 0;
    const int speckArea = speckSize * speckSize;
    std::vector<qint32> labels(count, 0);
    std::vector<quint8> seen(count, 0);
    std::vector<qint32> stack, members;
    struct Area {
        qint64 size {0};
        bool edge {false};
    };
    std::vector<Area> areas(1);
    for (int start = 0; start < count; start++) {
        if (seen[start] || ink[start] > lineInk || distance[start] <= clearance) continue;
        members.clear();
        stack.push_back(start);
        seen[start] = 1;
        bool edge = false;
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            members.push_back(i);
            const int x = i % width, y = i / width;
            edge |= x == 0 || y == 0 || x == width - 1 || y == height - 1;
            forNeighbors(i, width, height, [&](int j) {
                if (!seen[j] && ink[j] <= lineInk && distance[j] > clearance) {
                    seen[j] = 1;
                    stack.push_back(j);
                }
            });
        }
        if (int(members.size()) < speckArea) continue;
        const qint32 label = qint32(areas.size());
        areas.push_back({qint64(members.size()), edge});
        for (int i : members) labels[i] = label;
    }
    if (areas.size() < 2 || stop()) return nullptr;
    // not needed from here on
    std::vector<quint16>().swap(distance);
    std::vector<quint8>().swap(seen);
    std::vector<qint32>().swap(stack);
    std::vector<qint32>().swap(members);

    watershed(ink, labels, width, height);
    if (stop()) return nullptr;

    // the paper around the drawing: the largest area that reaches the edge
    qint32 paper = 0;
    for (size_t a = 1; a < areas.size(); a++) {
        if (areas[a].edge && (!paper || areas[a].size > areas[paper].size)) paper = qint32(a);
    }

    // Colors spread around the hue circle by the golden angle, so neighbors rarely look alike.
    std::vector<QRgb> colors(areas.size(), 0);
    for (size_t a = 1; a < areas.size(); a++) {
        if (qint32(a) == paper) continue;
        const qreal hue = std::fmod(0.11 + a * 0.6180339887, 1.0);
        const qreal lightness = 0.68 + 0.08 * ((a * 7) % 3) / 2.0;
        colors[a] = QColor::fromHslF(hue, 0.45, lightness).rgba();
    }
    QImage out(width, height, QImage::Format_ARGB32);
    for (int y = 0; y < height; y++) {
        QRgb *row = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < width; x++) {
            const qint32 label = labels[y * width + x];
            row[x] = label > 0 ? colors[label] : 0;
        }
    }
    KisPaintDeviceSP device = new KisPaintDevice(colorSpace);
    device->convertFromQImage(out, nullptr, bounds.x(), bounds.y());
    return device;
}
