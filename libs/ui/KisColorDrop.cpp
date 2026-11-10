/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisColorDrop.h"
#include "KisColorAreas.h"
#include "widgets/KisStudioValueRow.h"
#include <kis_default_bounds_base.h>
#include <QAbstractSpinBox>

#include <KisModernStyle.h>
#include "KisRedoLast.h"

#include <QApplication>
#include <QCheckBox>
#include <QDrag>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QFutureWatcher>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTabletEvent>
#include <QTimer>
#include <QToolButton>
#include <new>
#include <vector>

#include <QtConcurrent>
#include <QtMath>

#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KoCanvasController.h>
#include <KoToolManager.h>
#include <KisSwatch.h>
#include <kconfiggroup.h>
#include <klocalizedstring.h>
#include <ksharedconfig.h>

#include "KisDocument.h"
#include "KisModernStyle.h"
#include "KisSettingsRegistry.h"
#include "KisStudioGlass.h"
#include "KisView.h"
#include "KisViewManager.h"
#include <kundo2stack.h>
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_decoration.h"
#include "floodfill/KisLineArtFill.h"
#include "kis_canvas_resource_provider.h"
#include "kis_coordinates_converter.h"
#include "kis_fill_painter.h"
#include "kis_group_layer.h"
#include "kis_icon.h"
#include "kis_image.h"
#include "kis_paint_layer.h"
#include <KoProperties.h>
#include "kis_sequential_iterator.h"
#include "kis_pixel_selection.h"
#include "kis_resources_snapshot.h"
#include "kis_selection.h"
#include "kis_transaction.h"
#include "processing/fill_processing_visitor.h"
#include <commands/kis_image_layer_add_command.h>
#include <commands_new/kis_processing_command.h>
#include <commands_new/kis_update_command.h>
#include <kis_command_utils.h>
#include <kis_stroke_strategy_undo_command_based.h>

namespace
{

/// The settings a drop shares with the fill tool.
struct Options {
    int threshold {8};
    int closeGap {0};
    bool underLines {true};
    int speckSize {4};

    static Options read()
    {
        const KConfigGroup group = KSharedConfig::openConfig()->group("KritaFill/KisToolFill");
        Options o;
        o.threshold = qBound(1, group.readEntry<int>("thresholdAmount", 8), 100);
        o.closeGap = qBound(0, group.readEntry<int>("closeGapAmount", 0), 32);
        o.underLines = group.readEntry<bool>("fillUnderLines", true);
        o.speckSize = qBound(0, group.readEntry<int>("speckSize", 4), 32);
        return o;
    }

    /// Kept for the next drops and the fill tool, which share them
    void write() const
    {
        KConfigGroup group = KSharedConfig::openConfig()->group("KritaFill/KisToolFill");
        group.writeEntry("thresholdAmount", threshold);
        group.writeEntry("closeGapAmount", closeGap);
        group.writeEntry("fillUnderLines", underLines);
        KisSettingsRegistry::instance()->scheduleSync();
    }
};

QString colorsLayerName()
{
    return i18nc("@item:layer name: flat colors under line art", "Colors");
}

/// A layer for fills' colors, marked as one: its name may change, or be another language's
KisPaintLayerSP newColorsLayer(KisImageSP image)
{
    KisPaintLayerSP layer = new KisPaintLayer(image.data(), colorsLayerName(), OPACITY_OPAQUE_U8);
    layer->setNodeProperty(KisColorDrop::colorsMark, true);
    return layer;
}

/**
 * The gap to close: the fill tool's, or one sized from the lines near \p point (a small one without lines
 * nearby, for gaps further away). \p share picks which lines: flats size it from the bolder ones, since
 * fine hatching beside them would make every gap look small.
 */
KisPixelSelectionSP regionAt(KisPaintDeviceSP reference, const QPoint &point, int threshold, int closeGap, const Options &options,
                             const QRect &bounds);

/// The widest Auto tries when the gap the lines suggest lets the fill out
constexpr int widestAutoGap = 32;

/**
 * The gap to close: the one set, or Auto, twice the width of the lines near \p point. A fill from inside a shape
 * that still runs out to the image's edge (\p threshold given) went through an opening wider than that: Auto then
 * widens the gap until the fill stays well inside, and keeps the one it had when no width does.
 */
int gapFor(const Options &options, KisPaintDeviceSP reference, const QPoint &point, const QRect &bounds, qreal share = 0.5, int threshold = -1)
{
    if (options.closeGap > 0) return options.closeGap;
    const int width = KisLineArtFill::lineWidthNear(reference, point, bounds, share);
    const int gap = width > 0 ? qBound(4, 2 * width, 12) : 4;
    if (threshold < 0) return gap;
    auto reachesEdge = [&](const QRect &rect) {
        return rect.left() <= bounds.left() || rect.top() <= bounds.top() || rect.right() >= bounds.right() || rect.bottom() >= bounds.bottom();
    };
    const QRect first = regionAt(reference, point, threshold, gap, options, bounds)->selectedExactRect();
    if (!reachesEdge(first)) return gap;
    for (int wider = 2 * gap; wider <= widestAutoGap; wider *= 2) {
        const QRect kept = regionAt(reference, point, threshold, wider, options, bounds)->selectedExactRect();
        // kept in: off the edges and a good deal smaller than what got out
        if (!kept.isEmpty() && !reachesEdge(kept) && qint64(kept.width()) * kept.height() * 2 < qint64(first.width()) * first.height()) return wider;
    }
    return gap;
}

/// What a fill from \p point would cover.
KisPixelSelectionSP regionAt(KisPaintDeviceSP reference, const QPoint &point, int threshold, int closeGap, const Options &options,
                             const QRect &bounds)
{
    KisFillPainter painter;
    painter.setFillThreshold(threshold);
    painter.setOpacitySpread(100);
    painter.setCloseGap(closeGap);
    painter.setLineArt(options.underLines, options.speckSize);
    painter.setAntiAlias(false);
    painter.setCareForSelection(false);
    painter.setUseCompositing(true);
    painter.setWidth(bounds.width());
    painter.setHeight(bounds.height());
    return painter.createFloodSelection(new KisPixelSelection(), point.x(), point.y(), reference, nullptr);
}

/// A sparse look at \p device around \p point, every fourth pixel and row as far as a shape's outline is: how much of it
/// is paper (clear, or light and gray: white, cream, a scan's off-white) and clear, and how its ink lies
struct Look {
    int all {0};
    int paper {0};
    int clear {0};
    /// Ink in runs across rows and down columns, thin (lines) and wide (shapes)
    qint64 thinInk {0};
    qint64 wideInk {0};

    /// drawn in lines: most of its ink is thin
    bool lines() const { return thinInk + wideInk > 0 && thinInk >= wideInk; }

    /// covered with paint, light or dark: line art leaves most of its layer paper, however dark its lines
    bool painting() const { return all > 64 && paper * 10 < all * 3; }
};

/// A line is at most this wide
constexpr int lineWidth = 32;

Look lookAt(KisPaintDeviceSP device, const QPoint &point)
{
    Look look;
    if (!device) return look;
    const QRect window = QRect(point - QPoint(384, 384), QSize(768, 768)) & device->defaultBounds()->bounds();
    if (window.isEmpty()) return look;
    const KoColorSpace *srgb = KoColorSpaceRegistry::instance()->rgb8();
    const bool isSrgb = *device->colorSpace() == *srgb;
    std::vector<quint8> raw(size_t(std::max(window.width(), window.height())) * device->pixelSize());
    std::vector<quint8> colors(size_t(std::max(window.width(), window.height())) * 4);
    std::vector<char> inked;
    // a line of pixels, every one read for its runs of ink, every fourth counted
    auto scan = [&](int x, int y, int w, int h, bool counted) {
        const int length = std::max(w, h);
        device->readBytes(raw.data(), x, y, w, h);
        if (!isSrgb) {
            device->colorSpace()->convertPixelsTo(raw.data(), colors.data(), srgb, length, KoColorConversionTransformation::internalRenderingIntent(),
                                                  KoColorConversionTransformation::internalConversionFlags());
        }
        const quint8 *p = isSrgb ? raw.data() : colors.data();
        inked.assign(size_t(length), 0);
        for (int i = 0; i < length; i++) {
            // blue, green, red, alpha
            const quint8 *px = p + size_t(i) * 4;
            inked[size_t(i)] = px[3] >= 128 && KisLineArtFill::inkOf(qRgba(px[2], px[1], px[0], px[3])) >= KisLineArtFill::lineInk;
            if (!counted || i % 4) continue;
            look.all++;
            const int high = std::max({px[0], px[1], px[2]}), low = std::min({px[0], px[1], px[2]});
            if (px[3] < 128) look.clear++;
            if (px[3] < 128 || (low >= 200 && high - low <= 40)) look.paper++;
        }
        for (int i = 0; i < length;) {
            if (!inked[size_t(i)]) {
                i++;
                continue;
            }
            int end = i;
            while (end < length && inked[size_t(end)]) end++;
            (end - i <= lineWidth ? look.thinInk : look.wideInk) += end - i;
            i = end;
        }
    };
    for (int y = window.top(); y <= window.bottom(); y += 4) scan(window.left(), y, window.width(), 1, true);
    for (int x = window.left(); x <= window.right(); x += 4) scan(x, window.top(), 1, window.height(), false);
    return look;
}

/**
 * How a drop fills by color: the layer that takes it, and whether it recolors that layer's own pixels (a painting, a
 * flat of a Colors layer) or paints over the picture as it is seen, on a clear layer above it. Empty where the drop
 * fills line art, which closes gaps and reaches under the lines instead.
 */
struct ByColor {
    KisNodeSP node;
    bool inPlace {true};

    explicit operator bool() const { return node; }
    bool operator==(const ByColor &other) const { return node == other.node && inPlace == other.inPlace; }
};

ByColor byColorAt(KisImageSP image, KisNodeSP active, const QPoint &point)
{
    ByColor way;
    if (!image || !active || !qobject_cast<KisPaintLayer *>(active.data()) || !active->isEditable()) return way;
    auto flatAt = [&point](KisNodeSP node) { return KisColorDrop::isColorsLayer(node) && node->paintDevice()->pixel(point).opacityU8() >= 128; };
    if (KisColorDrop::isColorsLayer(active)) {
        if (flatAt(active)) way.node = active;
        return way;
    }
    // a flat beside the line art
    for (KisNodeSP beside : {active->prevSibling(), active->nextSibling()}) {
        if (flatAt(beside)) {
            way.node = beside;
            return way;
        }
    }
    // line art (on its layer, or as the picture shows: its ink mostly thin) closes gaps and fills under the lines instead
    const Look own = lookAt(active->paintDevice(), point);
    if (own.lines() || lookAt(image->projection(), point).lines()) return way;
    // a picture of colors and shapes: the layer's own paint under the drop is recolored, a clear spot painted over as it is seen
    way.node = active;
    way.inPlace = active->paintDevice()->pixel(point).opacityU8() >= 128;
    return way;
}

/// Spread as the smallest color step that is an edge, in Oklab distance: 8 about the least difference the eye notices
/// (0.02), 100 a quarter of black to white
qreal stepOf(int spread)
{
    return 0.0025 * spread;
}

int spreadOf(qreal step)
{
    return qBound(1, qRound(step / 0.0025), 100);
}

/// What a drop by color covers, worked out by KisColorAreas over \c bounds, and the Spread it was found with
struct Covered {
    KisPixelSelectionSP region;
    QSharedPointer<const KisColorAreas::Cover> cover;
    QRect bounds;
    int spread {0};

    explicit operator bool() const { return region; }
};

/// What a drop at \p point covers; \p spread 0 finds it for the area there
Covered coveredAt(const KisColorAreas &areas, const QPoint &point, int reach, int spread, bool everywhere)
{
    Covered covered;
    const qreal step = spread > 0 ? stepOf(spread) : areas.autoStep(point, reach);
    covered.spread = spreadOf(step);
    covered.bounds = areas.bounds();
    auto cover = QSharedPointer<KisColorAreas::Cover>::create(areas.cover(point, reach, step, everywhere));
    covered.region = new KisPixelSelection();
    const QRect rect = cover->rect, bounds = covered.bounds;
    for (int y = rect.top(); !rect.isEmpty() && y <= rect.bottom(); y++) {
        covered.region->writeBytes(cover->mask.data() + size_t(y - bounds.top()) * bounds.width() + (rect.left() - bounds.left()), rect.left(), y,
                                   rect.width(), 1);
    }
    covered.cover = cover;
    return covered;
}

/**
 * \p covered made \p color, written into \p device, so that it looks as if the area had been painted that color.
 * \p seen is the picture before the fill: \p device's own pixels when it is recolored in place, else the picture as it
 * is seen, painted over on \p device (a clear layer above it). A pixel at its edge is a blend of the color inside and
 * the color beyond (premultiplied, in the picture's own encoding, where its edges were blended: the two colors' line,
 * as in Levin's matting); its share of the inside color, found by least squares, takes the new color. Over the
 * picture the area is solid and its edge is the least see-through paint, no less than its share, that laid over the
 * picture looks right. A clear area (\p clear) gets the color under what is there. With \p preview, \p device (empty)
 * gets the picture as it will look, only where it changes. Returns what changed.
 */
QRect fillByColor(KisPaintDeviceSP device, KisPaintDeviceSP seen, const Covered &covered, const KoColor &color, bool inPlace, bool clear, bool preview)
{
    const KisColorAreas::Cover &cover = *covered.cover;
    const QRect bounds = covered.bounds;
    if (cover.rect.isEmpty()) return QRect();
    constexpr int reach = KisColorAreas::edgeReach;
    const QRect rect = cover.rect.adjusted(-reach, -reach, reach, reach) & bounds;
    const KoColorSpace *cs = device->colorSpace();
    const KoColorSpace *seenCs = seen->colorSpace();
    const KoColorProfile *profile = seenCs->colorModelId() == RGBAColorModelID ? seenCs->profile() : nullptr;
    const KoColorSpace *floats = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), profile);
    const KoColor own = color.convertedTo(cs);
    KoColor solid = own;
    cs->setOpacity(solid.data(), OPACITY_OPAQUE_U8, 1);
    const KoColor paintF = color.convertedTo(floats);
    const float *paint = reinterpret_cast<const float *>(paintF.data());
    const int pixelSize = cs->pixelSize(), seenSize = seenCs->pixelSize();
    const auto intent = KoColorConversionTransformation::internalRenderingIntent();
    const auto flags = KoColorConversionTransformation::internalConversionFlags();
    const int bw = bounds.width();
    auto inArea = [&](int x, int y) { return cover.mask[size_t(y - bounds.top()) * bw + (x - bounds.left())] != 0; };

    // tiles on Krita's own tile grid, so no two threads write one tile
    constexpr int tile = 128;
    QVector<QRect> tiles;
    for (int y = rect.top() - ((rect.top() % tile) + tile) % tile; y <= rect.bottom(); y += tile) {
        for (int x = rect.left() - ((rect.left() % tile) + tile) % tile; x <= rect.right(); x += tile) {
            const QRect t = QRect(x, y, tile, tile) & rect;
            if (!t.isEmpty()) tiles.append(t);
        }
    }
    QtConcurrent::blockingMap(tiles, [&](const QRect &t) {
        // the edge pixels in the tile
        std::vector<KisColorAreas::Blend> blends;
        for (int y = t.top(); y <= t.bottom(); y++) {
            const int first = (y - bounds.top()) * bw + (t.left() - bounds.left()), last = first + t.width() - 1;
            auto it = std::lower_bound(cover.blends.begin(), cover.blends.end(), first, [](const KisColorAreas::Blend &b, int at) { return b.at < at; });
            for (; it != cover.blends.end() && it->at <= last; ++it) blends.push_back(*it);
        }
        bool whole = blends.empty(), any = !blends.empty();
        for (int y = t.top(); y <= t.bottom(); y++) {
            for (int x = t.left(); x <= t.right(); x++) {
                const bool in = inArea(x, y);
                whole = whole && in;
                any = any || in;
            }
        }
        if (!any) return;
        if (whole && !inPlace) {
            device->fill(t, solid);
            return;
        }
        // read as far around as an edge pixel's two colors may be
        const QRect wide = blends.empty() ? t : t.adjusted(-reach, -reach, reach, reach) & bounds;
        const int w = wide.width(), h = wide.height();
        auto index = [&](int x, int y) { return size_t(y - wide.top()) * w + size_t(x - wide.left()); };
        auto indexOf = [&](int at) { return index(bounds.left() + at % bw, bounds.top() + at / bw); };
        std::vector<quint8> pixels(size_t(w) * h * seenSize);
        seen->readBytes(pixels.data(), wide);
        // the tile as it will be written: nothing yet for a preview, else the layer as it is
        std::vector<quint8> out(size_t(t.width()) * t.height() * pixelSize, 0);
        if (preview) {
            cs->setOpacity(out.data(), OPACITY_TRANSPARENT_U8, t.width() * t.height());
        } else if (inPlace) {
            for (int y = t.top(); y <= t.bottom(); y++) {
                memcpy(out.data() + size_t(y - t.top()) * t.width() * pixelSize, pixels.data() + index(t.left(), y) * seenSize,
                       size_t(t.width()) * pixelSize);
            }
        } else {
            device->readBytes(out.data(), t);
        }
        auto outAt = [&](int x, int y) { return out.data() + (size_t(y - t.top()) * t.width() + (x - t.left())) * pixelSize; };
        // the area itself; on a clear area, paint there goes over the color
        std::vector<QPoint> under;
        for (int y = t.top(); y <= t.bottom(); y++) {
            for (int x = t.left(); x <= t.right(); x++) {
                if (!inArea(x, y)) continue;
                if (!inPlace) {
                    memcpy(outAt(x, y), solid.data(), pixelSize);
                    continue;
                }
                const quint8 *src = pixels.data() + index(x, y) * seenSize;
                if (clear && seenCs->opacityU8(src) > 0) {
                    under.push_back(QPoint(x, y));
                    continue;
                }
                memcpy(outAt(x, y), own.data(), pixelSize);
                cs->setOpacity(outAt(x, y), clear ? 1.0 : seenCs->opacityF(src), 1);
            }
        }
        if (blends.empty() && under.empty()) {
            device->writeBytes(out.data(), t);
            return;
        }
        // premultiplied, so a blend is a straight line between two colors, transparency included
        std::vector<float> f(size_t(w) * h * 4);
        seenCs->convertPixelsTo(pixels.data(), reinterpret_cast<quint8 *>(f.data()), floats, w * h, intent, flags);
        for (size_t i = 0; i < size_t(w) * h; i++) {
            for (int c = 0; c < 3; c++) f[i * 4 + c] *= f[i * 4 + 3];
        }
        std::vector<QPoint> changed;
        std::vector<float> results;
        for (const QPoint &at : under) {
            const float *p = f.data() + index(at.x(), at.y()) * 4;
            float q[4];
            for (int c = 0; c < 3; c++) q[c] = p[c] + (1 - p[3]) * paint[c];
            q[3] = 1;
            changed.push_back(at);
            results.insert(results.end(), q, q + 4);
        }
        // over the picture: what the layer holds at the edge, which the paint goes over
        std::vector<float> layer;
        if (!inPlace && !preview && !blends.empty()) {
            std::vector<quint8> held(blends.size() * pixelSize);
            for (size_t k = 0; k < blends.size(); k++) {
                memcpy(held.data() + k * pixelSize, outAt(bounds.left() + blends[k].at % bw, bounds.top() + blends[k].at / bw), pixelSize);
            }
            layer.resize(blends.size() * 4);
            cs->convertPixelsTo(held.data(), reinterpret_cast<quint8 *>(layer.data()), floats, int(blends.size()), intent, flags);
        }
        for (size_t k = 0; k < blends.size(); k++) {
            const KisColorAreas::Blend &blend = blends[k];
            const QPoint at(bounds.left() + blend.at % bw, bounds.top() + blend.at / bw);
            const float *p = f.data() + indexOf(blend.at) * 4;
            const float *o = f.data() + indexOf(blend.inside) * 4;
            // a mark on the area moves with it; an edge pixel by its share of the inside color, by least squares: the
            // pixel as share × the inside color + the rest × the color beyond
            float share = 1;
            if (blend.beyond >= 0) {
                const float *b = f.data() + indexOf(blend.beyond) * 4;
                float along = 0, span2 = 0;
                for (int c = 0; c < 4; c++) {
                    along += (p[c] - b[c]) * (o[c] - b[c]);
                    span2 += (o[c] - b[c]) * (o[c] - b[c]);
                }
                share = span2 > 1e-8f ? qBound(0.0f, along / span2, 1.0f) : 0.0f;
            }
            if (share <= 0) continue;
            // the inside color made the new one: as opaque as it was, or on a clear area, opaque
            float made[4];
            for (int c = 0; c < 3; c++) made[c] = paint[c] * (clear ? 1.0f : o[3]);
            made[3] = clear ? 1.0f : o[3];
            float target[4];
            for (int c = 0; c < 4; c++) target[c] = p[c] + share * (made[c] - o[c]);
            target[3] = qBound(0.0f, target[3], 1.0f);
            for (int c = 0; c < 3; c++) target[c] = qBound(0.0f, target[c], target[3]);
            float q[4];
            if (inPlace || preview) {
                memcpy(q, target, sizeof(q));
            } else {
                // paint that, laid over the picture, gives the target: as see-through as it can be but no less than its
                // share, so the layer holds the area's shape, then laid over what the layer already holds
                float alpha;
                if (p[3] < 0.999f) {
                    alpha = qBound(0.0f, (target[3] - p[3]) / (1 - p[3]), 1.0f);
                } else {
                    alpha = share;
                    for (int c = 0; c < 3; c++) {
                        if (target[c] < p[c] && p[c] > 0) alpha = std::max(alpha, (p[c] - target[c]) / p[c]);
                        if (target[c] > p[c] && p[c] < 1) alpha = std::max(alpha, (target[c] - p[c]) / (1 - p[c]));
                    }
                    alpha = qBound(0.0f, alpha, 1.0f);
                }
                const float *held = layer.data() + k * 4;
                for (int c = 0; c < 3; c++) {
                    const float added = qBound(0.0f, target[c] - (1 - alpha) * p[c], alpha);
                    q[c] = added + (1 - alpha) * held[c] * held[3];
                }
                q[3] = alpha + (1 - alpha) * held[3];
            }
            changed.push_back(at);
            results.insert(results.end(), q, q + 4);
        }
        for (size_t k = 0; k < changed.size(); k++) {
            float *q = results.data() + k * 4;
            q[3] = qBound(0.0f, q[3], 1.0f);
            for (int c = 0; c < 3; c++) q[c] = q[3] > 1e-6f ? std::max(0.0f, q[c]) / q[3] : 0.0f;
        }
        std::vector<quint8> native(changed.size() * pixelSize);
        floats->convertPixelsTo(reinterpret_cast<const quint8 *>(results.data()), native.data(), cs, int(changed.size()), intent, flags);
        for (size_t k = 0; k < changed.size(); k++) memcpy(outAt(changed[k].x(), changed[k].y()), native.data() + k * pixelSize, pixelSize);
        device->writeBytes(out.data(), t);
    });
    return rect;
}

/// The picture as it will look after a fill by color, only where it changes, at most \p longest pixels across
QImage previewOf(KisPaintDeviceSP seen, const Covered &covered, const KoColor &color, bool inPlace, const QPoint &seed, QRect *rect, int longest = 4096)
{
    KisPaintDeviceSP shown = new KisPaintDevice(seen->colorSpace());
    const bool clear = seen->pixel(seed).opacityU8() < 128;
    *rect = fillByColor(shown, seen, covered, color, inPlace, clear, true);
    if (rect->isEmpty()) return QImage();
    QImage image = shown->convertToQImage(nullptr, *rect);
    if (std::max(image.width(), image.height()) > longest) image = image.scaled(QSize(longest, longest), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

/// \p region in \p color, at most 1600 pixels across.
QImage tintedOverlay(KisPixelSelectionSP region, const QRect &rect, const QColor &color)
{
    if (rect.isEmpty()) return QImage();
    QImage mask(rect.size(), QImage::Format_Alpha8);
    QVector<quint8> bytes(rect.width() * rect.height());
    region->readBytes(bytes.data(), rect);
    for (int y = 0; y < rect.height(); y++) memcpy(mask.scanLine(y), bytes.constData() + y * rect.width(), rect.width());
    const qreal scale = qMin<qreal>(1.0, 1600.0 / qMax(rect.width(), rect.height()));
    if (scale < 1) mask = mask.scaled((QSizeF(rect.size()) * scale).toSize().expandedTo(QSize(1, 1)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    QImage overlay(mask.size(), QImage::Format_ARGB32_Premultiplied);
    // as the fill will look
    overlay.fill(color);
    QPainter painter(&overlay);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    painter.drawImage(0, 0, mask);
    return overlay;
}

/// The area under the color as the drag goes.
class Preview : public KisCanvasDecoration
{
public:
    explicit Preview(QPointer<KisView> view)
        : KisCanvasDecoration(QStringLiteral("colorDropPreview"), view)
    {
        setVisible(true);
    }

    QImage overlay;
    QRect rect;

protected:
    void drawDecoration(QPainter &gc, const QRectF &, const KisCoordinatesConverter *converter, KisCanvas2 *) override
    {
        if (overlay.isNull()) return;
        gc.save();
        gc.setTransform(converter->imageToWidgetTransform(), false);
        gc.setRenderHint(QPainter::SmoothPixmapTransform, true);
        gc.drawImage(QRectF(rect), overlay);
        gc.restore();
    }
};

/// The button by a drop that keeps filling with its color: a fill bucket on a round chip.
class KeepFillingChip : public QToolButton
{
public:
    explicit KeepFillingChip(QWidget *parent)
        : QToolButton(parent)
    {
        setCheckable(true);
        setFixedSize(34, 34);
        setCursor(Qt::PointingHandCursor);
        setFocusPolicy(Qt::NoFocus);
        setIcon(KisIconUtils::loadIcon(QStringLiteral("krita_tool_color_fill")));
        setIconSize(QSize(18, 18));
        setToolTip(i18nc("@info:tooltip", "Keep filling"));
    }

    QColor color;

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // on, or under the pointer: a ring around it
        if (isChecked() || underMouse()) {
            QColor ring = palette().color(QPalette::WindowText);
            ring.setAlphaF(isChecked() ? 0.9 : 0.3);
            painter.setPen(QPen(ring, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75));
        }
        // a drop of the color it fills with, the bucket on it in an ink that shows
        painter.setPen(QPen(QColor(0, 0, 0, 50), 1));
        painter.setBrush(color);
        painter.drawEllipse(QRectF(rect()).adjusted(3.5, 3.5, -3.5, -3.5));
        const QColor ink = qGray(color.rgb()) > 150 ? QColor(24, 24, 24) : QColor(Qt::white);
        QPixmap glyph = window() && window()->windowHandle() ? icon().pixmap(window()->windowHandle(), iconSize()) : icon().pixmap(iconSize());
        {
            QPainter tint(&glyph);
            tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
            tint.fillRect(glyph.rect(), ink);
        }
        painter.drawPixmap(QRect(QPoint((width() - iconSize().width()) / 2, (height() - iconSize().height()) / 2), iconSize()), glyph);
    }
};

/**
 * By a drop: how it filled, changed in place (the fill is made again while it is the newest step), and the
 * bucket that keeps filling with its color. It goes a moment after the pointer leaves it.
 */
class DropPanel : public QWidget
{
public:
    explicit DropPanel(QWidget *canvas)
        : QWidget(canvas)
    {
        setAttribute(Qt::WA_Hover);
        // a pen's taps stay on the panel (its sliders get them as mouse presses), not on the canvas under it
        setAttribute(Qt::WA_NoMousePropagation);
        // the canvas under it hides the pointer (it draws the brush's outline instead), which the panel would take on
        setCursor(Qt::ArrowCursor);
        auto *row = new QHBoxLayout(this);
        row->setContentsMargins(KisModernStyle::space2, KisModernStyle::space2, KisModernStyle::space3, KisModernStyle::space2);
        row->setSpacing(KisModernStyle::space2);
        bucket = new KeepFillingChip(this);
        row->addWidget(bucket, 0, Qt::AlignVCenter);
        // the values as the Studio's rows: drag, a click on the bar, double-click types, right-click resets
        auto *grid = new QVBoxLayout();
        grid->setContentsMargins(0, 0, 0, 0);
        grid->setSpacing(KisModernStyle::hairline);
        auto value = [this](const QString &name, int low, int high, const QString &suffix, int reset) {
            auto *valueRow = new KisStudioValueRow(name, this);
            KisStudioValueRow::Range range;
            range.minimum = low;
            range.maximum = high;
            range.suffix = suffix;
            valueRow->setRange(range);
            valueRow->setClickSetsValue(true);
            valueRow->setDefaultValue(reset);
            valueRow->setCompact(true);
            valueRow->setMinimumWidth(10 * fontMetrics().averageCharWidth() + 120);
            return valueRow;
        };
        gap = value(i18nc("color drop: openings in the lines it closes", "Gap"), 0, 32, i18nc("unit", " px"), 0);
        gap->setToolTip(i18nc("@info:tooltip", "Seals breaks in the lines"));
        spread = value(i18nc("color drop: how far into similar colors it fills", "Spread"), 1, 100, QString(), 8);
        {
            // by ratios: the small values, where most fills are, get most of the bar
            KisStudioValueRow::Range range = spread->range();
            range.size = true;
            spread->setRange(range);
        }
        spread->setToolTip(i18nc("@info:tooltip", "Higher takes in nearby shades"));
        underLines = new QCheckBox(i18nc("color drop: the fill reaches under the lines", "Under lines"), this);
        underLines->setFocusPolicy(Qt::NoFocus);
        underLines->setToolTip(i18nc("@info:tooltip", "Fills to the middle of the lines"));
        grid->addWidget(gap);
        grid->addWidget(spread);
        grid->addWidget(underLines);
        row->addLayout(grid);
        // the fill is made as it drops: done keeps it as it is, cancel takes it back
        auto *ends = new QHBoxLayout();
        ends->setContentsMargins(0, 0, 0, 0);
        ends->setSpacing(KisModernStyle::hairline);
        auto end = [this](const QString &icon, const QString &toolTip) {
            auto *button = new QToolButton(this);
            button->setIcon(KisIconUtils::loadIcon(icon));
            button->setToolTip(toolTip);
            button->setAutoRaise(true);
            button->setFocusPolicy(Qt::NoFocus);
            button->setFixedSize(28, 26);
            return button;
        };
        // every area of the color, not only the one dropped on
        everywhere = end(QStringLiteral("studio_everywhere"), i18nc("@info:tooltip color drop", "Every area of this color"));
        everywhere->setCheckable(true);
        QObject::connect(everywhere, &QToolButton::toggled, everywhere, [this]() { showEverywhere(); });
        done = end(QStringLiteral("studio_done"), i18nc("@info:tooltip", "Done (Enter)"));
        cancel = end(QStringLiteral("studio_cancel"), i18nc("@info:tooltip", "Take back the fill (Esc)"));
        ends->addWidget(everywhere);
        ends->addWidget(done);
        ends->addWidget(cancel);
        row->addLayout(ends);
        row->setAlignment(ends, Qt::AlignVCenter);
        for (QWidget *widget : std::initializer_list<QWidget *>{gap, spread, underLines, everywhere}) {
            KisStudioGlass::setSeeThrough(widget, true);
        }
        adjustSize();
    }

    KeepFillingChip *bucket {nullptr};
    KisStudioValueRow *gap {nullptr};
    KisStudioValueRow *spread {nullptr};
    QCheckBox *underLines {nullptr};
    QToolButton *everywhere {nullptr};

    /// Drawn solid while on
    void showEverywhere()
    {
        everywhere->setIcon(KisIconUtils::loadIcon(everywhere->isChecked() ? QStringLiteral("studio_everywhere_on") : QStringLiteral("studio_everywhere")));
    }
    QToolButton *done {nullptr};
    QToolButton *cancel {nullptr};

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QPainterPath shape = KisStudioGlass::paint(painter, this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::panelRadius);
        QColor border = palette().color(QPalette::WindowText);
        border.setAlphaF(0.14);
        KisStudioGlass::paintBorder(painter, this, shape, border);
    }

    bool event(QEvent *event) override
    {
        // presses on the panel stay on it, not on the canvas under it
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease) {
            QWidget::event(event);
            event->accept();
            return true;
        }
        return QWidget::event(event);
    }
};

using Target = KisColorDrop::FillTarget;

bool isColorsLayer(KisNodeSP node, bool multiply)
{
    // layers from before the mark are known by their name
    return node && qobject_cast<KisPaintLayer *>(node.data())
        && (node->nodeProperties().boolProperty(KisColorDrop::colorsMark) || node->name() == colorsLayerName())
        && node->isEditable() && (node->compositeOpId() == COMPOSITE_MULT) == multiply;
}

/**
 * The layer to fill. When the active layer holds the lines around \p point, the fill goes on a layer of its
 * own: below clear line art, or above opaque line art with Multiply, so the lines stay on top either way.
 */
Target targetFor(KisImageSP image, KisNodeSP active, const QPoint &point)
{
    Target target;
    // a painting: the layer it is on takes the paint, as a brush's would
    if (active && qobject_cast<KisPaintLayer *>(active.data()) && !isColorsLayer(active, false) && !isColorsLayer(active, true)
        && KisColorDrop::isPaintingAt(active, point)) {
        target.node = active;
        return target;
    }
    if (!active) {
        target.newLayer = newColorsLayer(image);
        target.parent = image->root();
        target.above = image->root()->lastChild();
        return target;
    }
    if (!qobject_cast<KisPaintLayer *>(active.data())) {
        // a group, a mask or a layer that cannot be painted: a new layer above it (above a mask's layer)
        KisNodeSP anchor = active;
        while (anchor->parent() && !qobject_cast<KisLayer *>(anchor.data())) anchor = anchor->parent();
        target.newLayer = newColorsLayer(image);
        target.parent = anchor->parent() ? anchor->parent() : image->root();
        target.above = anchor->parent() ? anchor : image->root()->lastChild();
        return target;
    }

    // a layer of colors from earlier drops takes the next ones too
    if (isColorsLayer(active, false) || isColorsLayer(active, true)) {
        target.node = active;
        return target;
    }

    // Lines are the dark ink of the picture around the point; does the active layer hold them? Far enough around
    // that the middle of a large shape still sees its outline
    const QRect window = QRect(point - QPoint(768, 768), QSize(1536, 1536)) & image->bounds();
    // and is the layer's paper (what isn't ink) clear, or painted? Counted over the window: the point itself may
    // be on a line. Every other row is enough, as the counts are compared as shares and lines cross rows; of the
    // layer only its opacity is read, and the picture is converted only if it isn't 8-bit sRGB already.
    KisPaintDeviceSP picture = image->projection();
    KisPaintDeviceSP own = active->paintDevice();
    const KoColorSpace *srgb = KoColorSpaceRegistry::instance()->rgb8();
    const bool pictureIsSrgb = *picture->colorSpace() == *srgb;
    const int width = window.width();
    std::vector<quint8> pictureRow(size_t(width) * picture->pixelSize()), colors(size_t(width) * 4);
    std::vector<quint8> ownRow(size_t(width) * own->pixelSize()), ownAlpha(static_cast<size_t>(width));
    int lines = 0, held = 0, clear = 0, opaque = 0;
    for (int y = window.top(); !window.isEmpty() && y <= window.bottom(); y += 2) {
        picture->readBytes(pictureRow.data(), window.left(), y, width, 1);
        if (!pictureIsSrgb) {
            picture->colorSpace()->convertPixelsTo(pictureRow.data(), colors.data(), srgb, width,
                                                   KoColorConversionTransformation::internalRenderingIntent(),
                                                   KoColorConversionTransformation::internalConversionFlags());
        }
        own->readBytes(ownRow.data(), window.left(), y, width, 1);
        own->colorSpace()->copyOpacityU8(ownRow.data(), ownAlpha.data(), width);
        // blue, green, red, alpha
        const quint8 *p = pictureIsSrgb ? pictureRow.data() : colors.data();
        for (int x = 0; x < width; x++, p += 4) {
            if (KisLineArtFill::inkOf(qRgba(p[2], p[1], p[0], p[3])) < KisLineArtFill::lineInk) {
                (ownAlpha[x] < 128 ? clear : opaque)++;
                continue;
            }
            lines++;
            if (ownAlpha[x] >= 128) held++;
        }
    }
    // half the rows were counted
    if (lines * 2 < 24 || held * 5 < lines * 3) {
        target.node = active;
        return target;
    }
    const bool clearPaper = clear >= opaque;
    KisNodeSP neighbor = clearPaper ? active->prevSibling() : active->nextSibling();
    if (isColorsLayer(neighbor, !clearPaper)) {
        target.node = neighbor;
        return target;
    }
    target.newLayer = newColorsLayer(image);
    if (!clearPaper) target.newLayer->setCompositeOpId(COMPOSITE_MULT);
    target.parent = active->parent() ? active->parent() : image->root();
    target.above = clearPaper ? active->prevSibling() : active;
    return target;
}

void addLayerJob(KisImageSP image, KisStrokeId stroke, const Target &target)
{
    if (!target.newLayer) return;
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                              KUndo2CommandSP(new KisImageLayerAddCommand(image, target.newLayer, target.parent, target.above)),
                              false, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
}

} // namespace

struct KisColorDrop::Private {
    KisColorDrop *q {nullptr};
    QPointer<KisView> view;
    KisSharedPtr<Preview> preview;

    KoColor color;
    Options options;
    QPoint pointer;
    QPoint seed {-1, -1};
    int threshold {8};
    /// The area last shown, to skip work while the pointer stays inside it.
    KisPixelSelectionSP region;
    /// How it was worked out, so a drop on it takes it over instead of working it out again
    ByColor previewWay;
    KisPaintDeviceSP previewReference;
    Covered previewCovered;
    QPoint previewSeed {-1, -1};
    int previewReach {1};
    bool previewEverywhere {false};
    /// The last picture's edges and areas, read once while it stays as it is (the device and its sequence number)
    QSharedPointer<KisColorAreas> areas;
    const KisPaintDevice *areasOf {nullptr};
    int areasSequence {-1};
    int generation {0};
    bool computing {false};
    bool pending {false};

    QPointer<DropPanel> panel;
    /// The gap Auto chose for the last fill, shown on the panel and kept while it is made again; 0 before one
    int autoChosen {0};
    /// The gap Auto chose for the area shown, worked out with it in the background; 0 while none is
    int previewGap {0};
    /// The gap a drop takes over from the area shown under it
    int dropGap {0};
    /// The last fill was by color (ByColor): the panel has Spread and Everywhere, not the lines' settings
    ByColor lastWay;
    /// The picture before it, which it is made again from as the panel changes it, its areas, and the area to cover
    KisPaintDeviceSP lastReference;
    QSharedPointer<KisColorAreas> lastAreas;
    int lastReach {1};
    Covered readyCovered;
    /// The spread the last fill by color found for itself
    int autoSpread {8};
    int fillTicket {0};
    bool fillComputing {false};
    bool fillPending {false};
    /// Every area of the color, not only the one dropped on (a recoloring drop's)
    bool everywhere {false};
    bool keepFilling {false};
    KoColor keepColor;

    // the last drop, made again as its panel changes it while it is still the newest step
    QPoint lastSeed {-1, -1};
    KoColor lastColor;
    KisRedoLast redo;
    /// Making the last fill again: its layer is the one active at the drop, not what is active now
    bool refilling {false};
    /// The layer active when the last fill was made
    KisNodeWSP lastActive;
    QPoint pressAt;
    bool pressed {false};
    /// Space held on the canvas: its presses pan, as without the chip
    bool spaceHeld {false};

    KisCanvas2 *canvas() const { return view ? view->canvasBase() : nullptr; }
    QWidget *canvasWidget() const { return canvas() ? canvas()->canvasWidget() : nullptr; }

    QPoint imagePoint(const QPoint &canvasPos) const
    {
        return canvas()->coordinatesConverter()->widgetToImage(QPointF(canvasPos)).toPoint();
    }

    /// How far the pointer covers in the image: two screen pixels, so a drop takes the area it is mostly on
    int reach() const
    {
        const qreal zoom = canvas() ? canvas()->coordinatesConverter()->effectiveZoom() : 1.0;
        return qBound(1, qCeil(2.0 / qMax<qreal>(zoom, 1e-3)), 64);
    }

    /// The areas of \p source as already read, if it hasn't changed since
    QSharedPointer<KisColorAreas> knownAreas(KisPaintDeviceSP source) const
    {
        return areas && areasOf == source.data() && areasSequence == source->sequenceNumber() ? areas : QSharedPointer<KisColorAreas>();
    }

    void showPreview()
    {
        if (canvas()) canvas()->updateCanvasDecorations();
    }

    void clearPreview()
    {
        generation++;
        pending = false;
        region = nullptr;
        previewWay = ByColor();
        previewReference = nullptr;
        previewCovered = Covered();
        previewGap = 0;
        seed = QPoint(-1, -1);
        if (preview) preview->overlay = QImage();
        if (canvas()) canvas()->updateCanvasDecorations();
    }

    /// Works out in the background what a fill from the seed covers, and shows it.
    void compute()
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image) return;
        if (computing) {
            pending = true;
            return;
        }
        computing = true;
        pending = false;
        const int ticket = ++generation;
        const QRect bounds = image->bounds();
        const QPoint at = seed;
        const int t = threshold;
        const Options o = options;
        const bool all = everywhere;
        // as the fill will be made: a painting or a flat by the layer's own colors, line art by the picture
        KisNodeSP active = view->viewManager() ? view->viewManager()->activeNode() : nullptr;
        KisSelectionSP selection = view->viewManager() ? view->viewManager()->selection() : nullptr;
        const bool inSelection = selection && selection->projection()->pixel(at).opacityU8() > 0;
        const ByColor way = inSelection ? ByColor() : byColorAt(image, active, at);
        // a snapshot, as the image may change while this runs
        KisPaintDeviceSP source = way && way.inPlace ? way.node->paintDevice() : image->projection();
        KisPaintDeviceSP reference = new KisPaintDevice(*source);
        const QSharedPointer<KisColorAreas> known = way ? knownAreas(source) : QSharedPointer<KisColorAreas>();
        const KisPaintDevice *sourceOf = source.data();
        const int sequence = source->sequenceNumber();
        const int pointer = reach();
        const KoColor fill = color;
        struct Result {
            KisPixelSelectionSP region;
            Covered covered;
            QSharedPointer<KisColorAreas> areas;
            QImage overlay;
            QRect rect;
            int gap {0};
        };
        auto *watcher = new QFutureWatcher<Result>(q);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, q, [=]() {
            const Result result = watcher->result();
            watcher->deleteLater();
            computing = false;
            if (result.areas) {
                areas = result.areas;
                areasOf = sourceOf;
                areasSequence = sequence;
            }
            if (ticket == generation && preview) {
                region = result.region;
                previewWay = way;
                previewReference = reference;
                previewCovered = result.covered;
                previewSeed = at;
                previewReach = pointer;
                previewEverywhere = all;
                previewGap = result.gap;
                preview->overlay = result.overlay;
                preview->rect = result.rect;
                showPreview();
            }
            if (pending) compute();
        });
        const bool inPlace = way.inPlace;
        const bool byColor = bool(way);
        watcher->setFuture(QtConcurrent::run([=]() {
            Result result;
            // out of memory on a huge image: no preview, rather than an exception thrown into the window's thread
            try {
                if (byColor) {
                    // the picture as it will look
                    result.areas = known ? known : QSharedPointer<KisColorAreas>::create(reference, bounds);
                    result.covered = coveredAt(*result.areas, at, pointer, 0, all);
                    result.region = result.covered.region;
                    result.overlay = previewOf(reference, result.covered, fill, inPlace, at, &result.rect);
                } else {
                    // Auto's search for a gap that keeps the fill inside floods several times: here, never on the window's thread
                    result.gap = gapFor(o, reference, at, bounds, 0.5, t);
                    result.region = regionAt(reference, at, t, result.gap, o, bounds);
                    result.rect = result.region->selectedExactRect();
                    result.overlay = tintedOverlay(result.region, result.rect, fill.toQColor());
                }
            } catch (const std::bad_alloc &) {
                result = Result();
            }
            return result;
        }));
    }

    /// Recoloring by the layer's own colors there are no lines: the gap and filling under them go, Everywhere comes
    void fitPanel()
    {
        if (!panel) return;
        const bool recolor = bool(lastWay);
        if (panel->gap->isVisibleTo(panel) == !recolor && panel->everywhere->isVisibleTo(panel) == recolor) return;
        panel->gap->setVisible(!recolor);
        panel->underLines->setVisible(!recolor);
        panel->everywhere->setVisible(recolor);
        // the layout sees the rows go only once it is run again
        panel->layout()->activate();
        panel->resize(panel->sizeHint());
    }

    /// A fill by color from \p point: the area shown under the drop if it was worked out for this same fill, else
    /// worked out in the background; either way never on the window's thread
    void startByColor(const ByColor &way, const QPoint &point, const KoColor &fill)
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image) return;
        lastWay = way;
        lastColor = fill;
        if (previewCovered && previewReference && previewWay == way && previewEverywhere == everywhere
            && (point == previewSeed || region->pixel(point).opacityU8() > 0)) {
            lastSeed = previewSeed;
            lastReach = previewReach;
            lastReference = previewReference;
            lastAreas = areas;
            showSpread(previewCovered.spread);
            commitByColor(previewCovered);
            return;
        }
        KisPaintDeviceSP source = way.inPlace ? way.node->paintDevice() : image->projection();
        lastSeed = point;
        lastReach = reach();
        lastReference = new KisPaintDevice(*source);
        lastAreas = knownAreas(source);
        coverInBackground(false);
    }

    /// The spread a fill by color found for itself: shown, what a right click on it goes back to, and what it is made
    /// again with until it is dragged
    void showSpread(int spread)
    {
        threshold = spread;
        autoSpread = spread;
        if (!panel) return;
        const QSignalBlocker blocker(panel->spread);
        panel->spread->setValue(spread);
        panel->spread->setDefaultValue(spread);
    }

    /**
     * What the last fill by color covers with the settings now, worked out from the picture before it; then the fill is
     * made (\p again: made again in place of the last, while that is still the newest step). One at a time: each one
     * done is shown, and the newest change waiting is worked out next, so a drag shows every step it can keep up with.
     */
    void coverInBackground(bool again)
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image || !lastReference) return;
        if (fillComputing) {
            fillPending = true;
            return;
        }
        fillComputing = true;
        fillPending = false;
        const KisPaintDeviceSP reference = lastReference;
        const QSharedPointer<KisColorAreas> known = lastAreas;
        const QPoint at = lastSeed;
        const int pointer = lastReach;
        // a new drop finds its own spread; made again, the one dragged to
        const int t = again ? threshold : 0;
        const bool all = everywhere;
        const QRect bounds = image->bounds();
        struct Result {
            Covered covered;
            QSharedPointer<KisColorAreas> areas;
        };
        auto *watcher = new QFutureWatcher<Result>(q);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, q, [this, watcher, again]() {
            const Result result = watcher->result();
            watcher->deleteLater();
            fillComputing = false;
            if (result.areas) lastAreas = result.areas;
            if (result.covered) {
                if (again) {
                    readyCovered = result.covered;
                    redo.settingChanged();
                } else {
                    showSpread(result.covered.spread);
                    commitByColor(result.covered);
                }
            }
            if (fillPending) coverInBackground(true);
        });
        watcher->setFuture(QtConcurrent::run([reference, known, at, pointer, t, all, bounds]() {
            Result result;
            try {
                result.areas = known ? known : QSharedPointer<KisColorAreas>::create(reference, bounds);
                result.covered = coveredAt(*result.areas, at, pointer, t, all);
            } catch (const std::bad_alloc &) {
                result = Result();
            }
            return result;
        }));
    }

    /// The last fill by color made over what \p covered covers, as one step
    void commitByColor(const Covered &covered)
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image || !lastWay || !covered) return;
        readyCovered = covered;
        redo.made(view->document() ? view->document()->undoStack() : nullptr, kundo2_i18n("Fill"), [this]() { commitByColor(readyCovered); });
        KisStrokeStrategyUndoCommandBased *strategy = new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Fill"), false, image.data());
        KisStrokeId stroke = image->startStroke(strategy);
        KIS_SAFE_ASSERT_RECOVER_RETURN(stroke);
        KisNodeSP node = lastWay.node;
        const bool inPlace = lastWay.inPlace;
        const KisPaintDeviceSP reference = lastReference;
        const KoColor fill = lastColor;
        const QPoint at = lastSeed;
        image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                                  KUndo2CommandSP(new KisCommandUtils::LambdaCommand([node, inPlace, reference, covered, fill, at]() mutable -> KUndo2Command * {
                                      if (covered.region->selectedExactRect().isEmpty()) return nullptr;
                                      KisPaintDeviceSP device = node->paintDevice();
                                      const bool clear = reference->pixel(at).opacityU8() < 128;
                                      KisTransaction transaction(device);
                                      const QRect changed = fillByColor(device, reference, covered, fill, inPlace, clear, false);
                                      node->setDirty(changed);
                                      return transaction.endAndTake();
                                  })),
                                  false, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
        image->endStroke(stroke);
    }

    /// Auto shows the size it chose for the last fill
    void showAutoGap()
    {
        if (!panel) return;
        if (qRound(panel->gap->value()) != 0) {
            panel->gap->setValueText(QString());
            return;
        }
        panel->gap->setValueText(autoChosen > 0 ? i18nc("color drop gap sized from the lines, and the size", "Auto %1 px", autoChosen)
                                                : i18nc("color drop gap: sized from the lines", "Auto"));
    }

    /// The last fill taken back, while it is still the newest step
    void takeBack()
    {
        redo.takeBack();
    }

    void hidePanel()
    {
        keepFilling = false;
        everywhere = false;
        redo.forget();
        qApp->removeEventFilter(q);
        if (panel) panel->deleteLater();
        panel = nullptr;
    }

    /// The panel's values changed: kept for the next drops, and the last drop is made again with them
    void changed()
    {
        if (!panel) return;
        everywhere = panel->everywhere->isChecked();
        // by color: the spread is this fill's own (each drop finds one); the new area is worked out in the background
        // first, the fill made again only once it is ready
        if (lastWay) {
            threshold = qRound(panel->spread->value());
            coverInBackground(true);
            return;
        }
        options.closeGap = qRound(panel->gap->value());
        options.threshold = qRound(panel->spread->value());
        showAutoGap();
        options.underLines = panel->underLines->isChecked();
        options.write();
        threshold = options.threshold;
        redo.settingChanged();
    }

    void showPanel(const QPoint &canvasPos)
    {
        QWidget *widget = canvasWidget();
        if (!widget) return;
        if (!panel) {
            panel = new DropPanel(widget);
            QObject::connect(panel->bucket, &QToolButton::toggled, q, [this](bool on) { keepFilling = on; });
            QObject::connect(panel->done, &QToolButton::clicked, q, [this]() { hidePanel(); });
            QObject::connect(panel->cancel, &QToolButton::clicked, q, [this]() {
                takeBack();
                hidePanel();
            });
            // Enter and Esc in the window while it shows, a press on the canvas, and keeping on filling
            qApp->installEventFilter(q);
            QObject::connect(panel->gap, &KisStudioValueRow::valueChanged, q, [this]() { changed(); });
            QObject::connect(panel->spread, &KisStudioValueRow::valueChanged, q, [this]() { changed(); });
            QObject::connect(panel->underLines, &QCheckBox::toggled, q, [this]() { changed(); });
            QObject::connect(panel->everywhere, &QToolButton::toggled, q, [this]() { changed(); });
            // beside the drop, clear of the pointer
            const QPoint at = canvasPos + QPoint(22, 22);
            panel->move(qBound(0, at.x(), widget->width() - panel->width()), qBound(0, at.y(), widget->height() - panel->height()));
        }
        {
            const QSignalBlocker linesBlocker(panel->underLines);
            const QSignalBlocker everywhereBlocker(panel->everywhere);
            panel->gap->setValue(options.closeGap);
            panel->spread->setValue(threshold);
            // a right click goes back to the spread a fill by color found, or the line fills' default
            panel->spread->setDefaultValue(lastWay ? autoSpread : 8);
            panel->underLines->setChecked(options.underLines);
            panel->everywhere->setChecked(everywhere);
            panel->showEverywhere();
        }
        showAutoGap();
        fitPanel();
        panel->bucket->color = color.toQColor();
        keepColor = color;
        panel->show();
        panel->raise();
        panel->bucket->update();
    }
};

KisColorDrop::KisColorDrop(KisView *view)
    : QObject(view)
    , d(new Private)
{
    d->q = this;
    d->view = view;
    d->preview = new Preview(view);
    view->canvasBase()->addDecoration(KisCanvasDecorationSP(d->preview.data()));
    // picking another tool ends keeping on filling
    if (KoToolManager *tools = view && view->viewManager() ? view->viewManager()->toolManager() : nullptr) {
        connect(tools, &KoToolManager::changedTool, this, [this]() {
            if (d->keepFilling) d->hidePanel();
        });
    }
}

KisColorDrop::~KisColorDrop() = default;

KisColorDrop::FillTarget KisColorDrop::fillTarget(KisImageSP image, KisNodeSP active, const QPoint &point, KisSelectionSP selection)
{
    const bool inSelection = selection && selection->projection()->pixel(point).opacityU8() > 0;
    if (inSelection && active && qobject_cast<KisPaintLayer *>(active.data())) return Target{active, nullptr, nullptr, nullptr};
    return targetFor(image, active, point);
}

void KisColorDrop::addFillLayer(KisImageSP image, KisStrokeId stroke, const FillTarget &target)
{
    addLayerJob(image, stroke, target);
}

int KisColorDrop::autoGap(KisPaintDeviceSP reference, const QPoint &point, const QRect &bounds)
{
    Options automatic;
    automatic.closeGap = 0;
    return gapFor(automatic, reference, point, bounds);
}

const QString KisColorDrop::colorsMark = QStringLiteral("skissacolors");

bool KisColorDrop::isColorsLayer(KisNodeSP node)
{
    return ::isColorsLayer(node, false) || ::isColorsLayer(node, true);
}

bool KisColorDrop::isPaintingAt(KisNodeSP layer, const QPoint &point)
{
    return layer && qobject_cast<KisPaintLayer *>(layer.data()) && lookAt(layer->paintDevice(), point).painting();
}

bool KisColorDrop::carriesColor(const QMimeData *data)
{
    return data && (data->hasColor() || data->hasFormat(QStringLiteral("krita/x-colorsetentry")));
}

bool KisColorDrop::colorOf(const QMimeData *data, KoColor *color)
{
    if (!data) return false;
    if (data->hasColor()) {
        *color = KoColor(data->colorData().value<QColor>(), KoColorSpaceRegistry::instance()->rgb8());
        return true;
    }
    if (data->hasFormat(QStringLiteral("krita/x-colorsetentry"))) {
        QByteArray bytes = data->data(QStringLiteral("krita/x-colorsetentry"));
        *color = KisSwatch::fromByteArray(bytes).color();
        return true;
    }
    return false;
}

void KisColorDrop::hover(const KoColor &color, const QPoint &canvasPos)
{
    KisImageSP image = d->view ? d->view->image() : nullptr;
    if (!image || !d->canvas()) return;
    if (d->seed == QPoint(-1, -1)) {
        d->options = Options::read();
        d->threshold = d->options.threshold;
    }
    d->color = color;
    d->pointer = canvasPos;

    const QPoint point = d->imagePoint(canvasPos);
    if (!image->bounds().contains(point)) {
        d->clearPreview();
        return;
    }
    // still over the area shown: nothing new to work out
    if (d->region && d->region->pixel(point).opacityU8() > 0) {
        d->showPreview();
        return;
    }
    d->seed = point;
    d->compute();
}

void KisColorDrop::leave()
{
    d->clearPreview();
}

bool KisColorDrop::drop(const KoColor &color, const QPoint &canvasPos)
{
    if (!d->canvas()) return false;
    const QPoint point = d->imagePoint(canvasPos);
    // the gap Auto found for the area shown under it, if it was worked out there
    d->dropGap = d->region && d->region->pixel(point).opacityU8() > 0 ? d->previewGap : 0;
    d->options = Options::read();
    d->threshold = d->options.threshold;
    const bool filled = fillAt(color, point);
    d->clearPreview();
    if (!filled) return false;
    d->color = color;
    d->showPanel(canvasPos);
    return true;
}

void KisColorDrop::refillLast()
{
    KisImageSP image = d->view ? d->view->image() : nullptr;
    if (!image || d->lastSeed == QPoint(-1, -1)) return;
    // the fill was just taken back, as a stroke of its own: done before the fill reads which layers there are
    image->waitForDone();
    d->refilling = true;
    fillAt(d->lastColor, d->lastSeed);
    d->refilling = false;
}

bool KisColorDrop::fillAt(const KoColor &color, const QPoint &point)
{
    KisImageSP image = d->view ? d->view->image() : nullptr;
    KisViewManager *viewManager = d->view ? d->view->viewManager() : nullptr;
    if (!image || !viewManager) return false;
    const int threshold = d->threshold;
    if (!image->bounds().contains(point)) return false;

    KisNodeSP active = viewManager->activeNode();
    // a fill made again: the layer it made was just taken back with it, and may still be the active one
    if (d->refilling || (active && !active->parent())) active = d->lastActive;
    if (active && !active->isEditable()) {
        d->view->showFloatingMessage(i18nc("@info:status", "Locked"), KisIconUtils::loadIcon(QStringLiteral("object-locked")));
        return false;
    }
    d->lastActive = active;

    KisSelectionSP selection = viewManager->selection();
    const bool inSelection = selection && selection->projection()->pixel(point).opacityU8() > 0;
    // a painting, a flat, or a clear layer over a painting: filled by color, its soft edges taking the new color by their share
    const ByColor way = inSelection ? ByColor() : byColorAt(image, active, point);
    d->lastWay = way;
    if (d->panel) d->fitPanel();
    if (way) {
        d->startByColor(way, point, color);
        return true;
    }

    d->lastSeed = point;
    d->lastColor = color;
    // made again while its panel changes it and it is still the newest step
    d->redo.made(d->view->document() ? d->view->document()->undoStack() : nullptr, kundo2_i18n("Fill"), [this]() { refillLast(); });
    KisStrokeStrategyUndoCommandBased *strategy =
        new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Fill"), false, image.data());
    strategy->setSupportsWrapAroundMode(true);
    KisStrokeId stroke = image->startStroke(strategy);
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(stroke, false);

    const Target target = fillTarget(image, active, point, selection);
    KisNodeSP node = target.filled();
    addLayerJob(image, stroke, target);

    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image, node, viewManager->canvasResourceProvider()->resourceManager());
    resources->setFGColorOverride(color.convertedTo(image->colorSpace()));
    // a new layer is empty: with Preserve Alpha the fill would show nowhere
    if (target.newLayer) resources->setGlobalAlphaLock(false);

    FillProcessingVisitor *visitor = new FillProcessingVisitor(image->projection(), resources->activeSelection(), resources);
    visitor->setSeedPoint(point);
    visitor->setSelectionOnly(inSelection);
    visitor->setUseFastMode(false);
    visitor->setUseCustomBlendingOptions(true);
    visitor->setCustomOpacity(1.0);
    visitor->setCustomCompositeOp(COMPOSITE_OVER);
    visitor->setRegionFillingMode(KisFillPainter::RegionFillingMode_FloodFill);
    visitor->setFillThreshold(threshold);
    visitor->setOpacitySpread(100);
    // Auto: the gap found with the preview, or once for a drop without one; made again (Spread changed), the one it had
    int gap = d->options.closeGap;
    if (gap <= 0) {
        gap = d->refilling && d->autoChosen > 0 ? d->autoChosen
            : d->dropGap > 0                     ? d->dropGap
                                                 : gapFor(d->options, image->projection(), point, image->bounds());
        d->autoChosen = gap;
    }
    d->dropGap = 0;
    d->showAutoGap();
    visitor->setCloseGap(gap);
    visitor->setUseSelectionAsBoundary(true);
    visitor->setAntiAlias(true);
    visitor->setLineArt(d->options.underLines, d->options.speckSize);
    QSharedPointer<QRect> dirty(new QRect);
    visitor->setOutDirtyRect(dirty);
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(KUndo2CommandSP(new KisProcessingCommand(visitor, node)), false,
                                                                     KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(KUndo2CommandSP(new KisUpdateCommand(node, dirty, image.data())), false,
                                                                     KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->endStroke(stroke);
    return true;
}

void KisColorDrop::startDrag(QWidget *source, const QColor &color)
{
    auto *data = new QMimeData;
    data->setColorData(color);
    // a swatch may be rebuilt while the drag runs (the color it set changes the panel): the drag isn't its child
    QDrag *drag = new QDrag(qApp);
    drag->setMimeData(data);
    const qreal ratio = source->devicePixelRatioF();
    QPixmap pixmap(QSize(30, 30) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    {
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        // a dark and a light ring, so it shows over any color
        painter.setPen(QPen(QColor(0, 0, 0, 110), 4));
        painter.setBrush(color);
        painter.drawEllipse(QRectF(3, 3, 24, 24));
        painter.setPen(QPen(QColor(255, 255, 255, 220), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(3, 3, 24, 24));
    }
    drag->setPixmap(pixmap);
    drag->setHotSpot(QPoint(15, 15));
    drag->exec(Qt::CopyAction);
    drag->deleteLater();
}

bool KisColorDrop::canRecolor(KisNodeSP node)
{
    // pixels of its own to recolor: a fill or filter layer's device, or a mask's, is only where it applies
    return node && qobject_cast<KisPaintLayer *>(node.data()) && node->isEditable(false);
}

void KisColorDrop::recolor(KisImageSP image, KisNodeSP node, const KoColor &color)
{
    if (!image || !canRecolor(node)) return;
    KisStrokeStrategyUndoCommandBased *strategy =
        new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Recolor Layer"), false, image.data());
    KisStrokeId stroke = image->startStroke(strategy);
    KIS_SAFE_ASSERT_RECOVER_RETURN(stroke);
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                              KUndo2CommandSP(new KisCommandUtils::LambdaCommand([node, color]() mutable -> KUndo2Command * {
                                  KisPaintDeviceSP device = node->paintDevice();
                                  const QRect rect = device->exactBounds();
                                  if (rect.isEmpty()) return nullptr;
                                  KisTransaction transaction(device);
                                  const KoColorSpace *colorSpace = device->colorSpace();
                                  const KoColor paint = color.convertedTo(colorSpace);
                                  const int pixelSize = colorSpace->pixelSize();
                                  // the new color in every pixel, each keeping its own transparency
                                  KisSequentialIterator it(device, rect);
                                  while (it.nextPixel()) {
                                      quint8 *pixel = it.rawData();
                                      const qreal opacity = colorSpace->opacityF(pixel);
                                      if (opacity <= 0) continue;
                                      memcpy(pixel, paint.data(), pixelSize);
                                      colorSpace->setOpacity(pixel, opacity, 1);
                                  }
                                  node->setDirty(rect);
                                  return transaction.endAndTake();
                              })),
                              false, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->endStroke(stroke);
}

void KisColorDrop::autoFlats(KisViewManager *viewManager)
{
    KisImageSP image = viewManager ? viewManager->image() : nullptr;
    if (!image) return;
    KisNodeSP active = viewManager->activeNode();
    const bool fromLayer = active && qobject_cast<KisPaintLayer *>(active.data()) && !active->paintDevice()->exactBounds().isEmpty();
    // its working buffers take about twenty bytes a pixel
    if (qint64(image->bounds().width()) * image->bounds().height() > 60'000'000) {
        viewManager->showFloatingMessage(i18nc("@info:status", "Too large for flats"), KisIconUtils::loadIcon(QStringLiteral("krita_tool_color_fill")), 2000);
        return;
    }
    KisPaintDeviceSP lines = new KisPaintDevice(fromLayer ? *active->paintDevice() : *image->projection());
    const QRect bounds = image->bounds();
    const Options options = Options::read();
    const KoColorSpace *colorSpace = image->colorSpace();
    viewManager->showFloatingMessage(i18nc("@info:status", "Flats…"), KisIconUtils::loadIcon(QStringLiteral("krita_tool_color_fill")), 2000);

    KisImageWSP weakImage = image;
    QPointer<KisViewManager> manager = viewManager;
    KisNodeWSP weakActive = active;
    auto *watcher = new QFutureWatcher<KisPaintDeviceSP>(viewManager);
    QObject::connect(watcher, &QFutureWatcher<KisPaintDeviceSP>::finished, viewManager, [watcher, weakImage, manager, weakActive, fromLayer]() {
        KisPaintDeviceSP flats = watcher->result();
        watcher->deleteLater();
        KisImageSP image = weakImage;
        if (!image || !manager || !flats) return;
        KisNodeSP active = weakActive;
        Target target;
        target.newLayer = new KisPaintLayer(image.data(), i18nc("@item:layer name: a flat color for every area of the line art", "Flats"),
                                            OPACITY_OPAQUE_U8, flats);
        if (fromLayer && active && active->parent()) {
            // under clear line art; over opaque line art, multiplied, so the lines show through
            const bool opaque = active->paintDevice()->exactBounds().contains(image->bounds())
                && active->paintDevice()->pixel(image->bounds().center()).opacityU8() == OPACITY_OPAQUE_U8;
            if (opaque) target.newLayer->setCompositeOpId(COMPOSITE_MULT);
            target.parent = active->parent();
            target.above = opaque ? active : active->prevSibling();
        } else {
            target.parent = image->root();
            target.above = image->root()->lastChild();
        }
        KisStrokeStrategyUndoCommandBased *strategy =
            new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Auto Flats"), false, image.data());
        KisStrokeId stroke = image->startStroke(strategy);
        KIS_SAFE_ASSERT_RECOVER_RETURN(stroke);
        addLayerJob(image, stroke, target);
        image->endStroke(stroke);
    });
    watcher->setFuture(QtConcurrent::run([lines, bounds, options, colorSpace]() -> KisPaintDeviceSP {
        // an exception thrown here would be thrown again in the window's event loop, and end the app
        try {
            return KisLineArtFill::flats(lines, bounds, gapFor(options, lines, bounds.center(), bounds, 0.75), options.speckSize, colorSpace);
        } catch (const std::bad_alloc &) {
            return nullptr;
        }
    }));
}

bool KisColorDrop::eventFilter(QObject *watched, QEvent *event)
{
    QWidget *canvas = d->canvasWidget();
    if (!d->panel || !canvas) return false;
    // Space pans: its presses only look around
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Space && !key->isAutoRepeat()) d->spaceHeld = event->type() == QEvent::KeyPress;
    } else if (event->type() == QEvent::ApplicationDeactivate) {
        d->spaceHeld = false;
    }
    // While the panel shows, the fill is still being decided: Enter keeps it as it is and Esc takes it back (keeping
    // on filling, Esc only stops), unless a number is being typed; carrying on painting keeps it too.
    if (event->type() == QEvent::KeyPress && !d->keepFilling) {
        QWidget *focus = QApplication::focusWidget();
        const bool first = focus ? watched == focus : watched == canvas->window();
        // a slider box dragged holds the focus without being typed in: its text is read only until a double click
        const QLineEdit *edit = qobject_cast<QAbstractSpinBox *>(focus) ? focus->findChild<QLineEdit *>() : qobject_cast<QLineEdit *>(focus);
        const bool typing = edit && !edit->isReadOnly();
        if (first && !typing && (!focus || focus->window() == canvas->window())) {
            const int key = static_cast<QKeyEvent *>(event)->key();
            if (key == Qt::Key_Return || key == Qt::Key_Enter) {
                d->hidePanel();
                return true;
            }
            if (key == Qt::Key_Escape) {
                d->takeBack();
                d->hidePanel();
                return true;
            }
        }
        return false;
    }
    if (watched != canvas) return false;
    if (!d->keepFilling) {
        // painting on keeps the fill; panning, zooming, turning (other buttons, modifiers, Space) look around with it open
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TabletPress) {
            const Qt::MouseButton button = event->type() == QEvent::MouseButtonPress ? static_cast<QMouseEvent *>(event)->button()
                                                                                     : static_cast<QTabletEvent *>(event)->button();
            if (button == Qt::LeftButton && static_cast<QInputEvent *>(event)->modifiers() == Qt::NoModifier && !d->spaceHeld) d->hidePanel();
        }
        return false;
    }
    // Keeping on filling: a tap on the canvas fills, a drag does nothing, Esc or a right click stops.
    // a pen's tap arrives as a tablet event and then again as a mouse event made from it: one fill
    if (auto *mouse = dynamic_cast<QMouseEvent *>(event); mouse && mouse->source() != Qt::MouseEventNotSynthesized) return true;
    if (dynamic_cast<QTabletEvent *>(event)) event->accept();
    auto position = [event]() -> QPoint {
        if (auto *tablet = dynamic_cast<QTabletEvent *>(event)) return tablet->pos();
        return static_cast<QMouseEvent *>(event)->pos();
    };
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress: {
        const bool right = event->type() == QEvent::MouseButtonPress
            ? static_cast<QMouseEvent *>(event)->button() == Qt::RightButton
            : static_cast<QTabletEvent *>(event)->button() == Qt::RightButton;
        if (right) {
            d->hidePanel();
            return true;
        }
        // only a plain tap fills: middle button, modifiers and Space keep panning, zooming and picking
        const Qt::MouseButton button = event->type() == QEvent::MouseButtonPress ? static_cast<QMouseEvent *>(event)->button()
                                                                                 : static_cast<QTabletEvent *>(event)->button();
        if (button != Qt::LeftButton || static_cast<QInputEvent *>(event)->modifiers() != Qt::NoModifier || d->spaceHeld) {
            return false;
        }
        d->pressed = true;
        d->pressAt = position();
        return true;
    }
    case QEvent::MouseMove:
    case QEvent::TabletMove:
        return d->pressed;
    case QEvent::MouseButtonRelease:
    case QEvent::TabletRelease:
        if (!d->pressed) return false;
        d->pressed = false;
        if ((position() - d->pressAt).manhattanLength() <= KisModernStyle::dragDistance(static_cast<QInputEvent *>(event))) {
            // the panel stays where it is: what it changes is this newest fill
            d->options = Options::read();
            d->threshold = d->options.threshold;
            fillAt(d->keepColor, d->imagePoint(d->pressAt));
        }
        return true;
    case QEvent::KeyPress:
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            d->hidePanel();
            return true;
        }
        return false;
    default:
        return false;
    }
}
