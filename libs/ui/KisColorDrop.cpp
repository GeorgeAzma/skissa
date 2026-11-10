/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisColorDrop.h"
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
#include <numeric>
#include <vector>

#include <QtConcurrent>

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
/// is paper (clear, or light and gray: white, cream, a scan's off-white), clear, and dark ink
struct Look {
    int all {0};
    int paper {0};
    int clear {0};
    int ink {0};

    /// covered with paint, light or dark: line art leaves most of its layer paper, however dark its lines
    bool painting() const { return all > 64 && paper * 10 < all * 3; }
    /// mostly clear, and what is on it isn't mostly lines
    bool clearOfLines() const
    {
        const int painted = all - clear;
        return all > 64 && clear * 10 >= all * 7 && (painted == 0 || ink * 2 < painted);
    }
};

Look lookAt(KisPaintDeviceSP device, const QPoint &point)
{
    Look look;
    if (!device) return look;
    const QRect window = QRect(point - QPoint(384, 384), QSize(768, 768)) & device->defaultBounds()->bounds();
    if (window.isEmpty()) return look;
    const KoColorSpace *srgb = KoColorSpaceRegistry::instance()->rgb8();
    const bool isSrgb = *device->colorSpace() == *srgb;
    const int width = window.width();
    std::vector<quint8> row(size_t(width) * device->pixelSize()), colors(size_t(width) * 4);
    for (int y = window.top(); y <= window.bottom(); y += 4) {
        device->readBytes(row.data(), window.left(), y, width, 1);
        if (!isSrgb) {
            device->colorSpace()->convertPixelsTo(row.data(), colors.data(), srgb, width, KoColorConversionTransformation::internalRenderingIntent(),
                                                  KoColorConversionTransformation::internalConversionFlags());
        }
        const quint8 *p = isSrgb ? row.data() : colors.data();
        for (int x = 0; x < width; x += 4) {
            // blue, green, red, alpha
            const quint8 *px = p + size_t(x) * 4;
            look.all++;
            const int high = std::max({px[0], px[1], px[2]}), low = std::min({px[0], px[1], px[2]});
            if (px[3] < 128) look.clear++;
            if (px[3] < 128 || (low >= 200 && high - low <= 40)) look.paper++;
            if (px[3] >= 128 && KisLineArtFill::inkOf(qRgba(px[2], px[1], px[0], px[3])) >= KisLineArtFill::lineInk) look.ink++;
        }
    }
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
    const Look own = lookAt(active->paintDevice(), point);
    if (own.painting()) {
        way.node = active;
        return way;
    }
    // a flat beside the line art
    for (KisNodeSP beside : {active->prevSibling(), active->nextSibling()}) {
        if (flatAt(beside)) {
            way.node = beside;
            return way;
        }
    }
    // a layer of its own over a painting, as an artist keeps one to paint on
    if (own.clearOfLines() && lookAt(image->projection(), point).painting()) {
        way.node = active;
        way.inPlace = false;
    }
    return way;
}

KisPixelSelectionSP regionByColorAlone(KisPaintDeviceSP device, const QPoint &seed, int threshold, bool everywhere, const QRect &bounds);

/// The distance between two premultiplied colors
float colorDistance(const float *a, const float *b)
{
    float d2 = 0;
    for (int c = 0; c < 4; c++) d2 += (a[c] - b[c]) * (a[c] - b[c]);
    return std::sqrt(d2);
}

/// How far across a soft edge between two other colors is looked through
constexpr int blendWidth = 3;

/**
 * \p region without the pixels that only blend two others: the soft edge between two colors may pass through one like
 * the seed's, which isn't an area of it (a sliver along every stripe of another color). A pixel goes when, along a
 * row, a column or a diagonal, the region ends within blendWidth on both sides of it and it lies between the colors
 * found there.
 */
void dropBlends(KisPixelSelectionSP region, KisPaintDeviceSP device, const QRect &bounds)
{
    const QRect rect = region->selectedExactRect() & bounds;
    if (rect.isEmpty()) return;
    const KoColorSpace *cs = device->colorSpace();
    const KoColorProfile *profile = cs->colorModelId() == RGBAColorModelID ? cs->profile() : nullptr;
    const KoColorSpace *floats = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), profile);
    constexpr int band = 64;
    QVector<QRect> bands;
    for (int y = rect.top(); y <= rect.bottom(); y += band) bands.append(QRect(rect.left(), y, rect.width(), std::min(band, rect.bottom() - y + 1)));
    QVector<std::vector<QPoint>> dropped(bands.size());
    QVector<int> order(bands.size());
    std::iota(order.begin(), order.end(), 0);
    QtConcurrent::blockingMap(order, [&](int n) {
        const QRect r = bands[n];
        const QRect wide = r.adjusted(-blendWidth, -blendWidth, blendWidth, blendWidth) & bounds;
        const int w = wide.width(), h = wide.height();
        std::vector<quint8> in(size_t(w) * h);
        region->readBytes(in.data(), wide);
        auto index = [&](int x, int y) { return size_t(y - wide.top()) * w + size_t(x - wide.left()); };
        // outside the region but in the image: beyond its edges there is nothing to blend with
        auto outside = [&](int x, int y) { return wide.contains(x, y) && in[index(x, y)] < 128; };
        static const QPoint axes[] = {QPoint(1, 0), QPoint(0, 1), QPoint(1, 1), QPoint(1, -1)};
        struct Candidate {
            QPoint at;
            QPoint ends[4][2];
            int count;
        };
        std::vector<Candidate> candidates;
        for (int y = r.top(); y <= r.bottom(); y++) {
            for (int x = r.left(); x <= r.right(); x++) {
                if (in[index(x, y)] < MAX_SELECTED) continue;
                Candidate candidate {QPoint(x, y), {}, 0};
                for (const QPoint &axis : axes) {
                    QPoint ends[2];
                    bool found[2] = {false, false};
                    for (int side = 0; side < 2; side++) {
                        const QPoint step = side ? axis : -axis;
                        for (int k = 1; k <= blendWidth && !found[side]; k++) {
                            const QPoint q = QPoint(x, y) + step * k;
                            if (outside(q.x(), q.y())) {
                                ends[side] = q;
                                found[side] = true;
                            }
                        }
                    }
                    if (found[0] && found[1]) {
                        candidate.ends[candidate.count][0] = ends[0];
                        candidate.ends[candidate.count][1] = ends[1];
                        candidate.count++;
                    }
                }
                if (candidate.count) candidates.push_back(candidate);
            }
        }
        if (candidates.empty()) return;
        std::vector<quint8> pixels(size_t(w) * h * cs->pixelSize());
        device->readBytes(pixels.data(), wide);
        std::vector<float> f(size_t(w) * h * 4);
        cs->convertPixelsTo(pixels.data(), reinterpret_cast<quint8 *>(f.data()), floats, w * h, KoColorConversionTransformation::internalRenderingIntent(),
                            KoColorConversionTransformation::internalConversionFlags());
        for (size_t i = 0; i < size_t(w) * h; i++) {
            for (int c = 0; c < 3; c++) f[i * 4 + c] *= f[i * 4 + 3];
        }
        auto between = [&](const float *p, const float *a, const float *b) {
            const float span = colorDistance(a, b);
            if (span < 0.1f) return false;
            float along = 0;
            for (int c = 0; c < 4; c++) along += (p[c] - b[c]) * (a[c] - b[c]);
            const float share = along / (span * span);
            if (share < 0.04f || share > 0.96f) return false;
            float off = 0;
            for (int c = 0; c < 4; c++) {
                const float d = p[c] - b[c] - share * (a[c] - b[c]);
                off += d * d;
            }
            return std::sqrt(off) < 0.25f * span;
        };
        for (const Candidate &candidate : candidates) {
            const float *p = f.data() + index(candidate.at.x(), candidate.at.y()) * 4;
            for (int i = 0; i < candidate.count; i++) {
                const QPoint a = candidate.ends[i][0], b = candidate.ends[i][1];
                if (between(p, f.data() + index(a.x(), a.y()) * 4, f.data() + index(b.x(), b.y()) * 4)) {
                    dropped[n].push_back(candidate.at);
                    break;
                }
            }
        }
    });
    const quint8 none = MIN_SELECTED;
    for (const std::vector<QPoint> &points : dropped) {
        for (const QPoint &at : points) region->writeBytes(&none, at.x(), at.y(), 1, 1);
    }
    region->invalidateOutlineCache();
}

/**
 * \p region joined by the area around \p seed that changes only gradually, so a gradient (a sky, water, a glow)
 * fills whole up to the edges of shapes. From pixel to pixel a gradient changes by a steady amount, while an edge,
 * however faint, is a sudden change of it: a pixel joins when it differs from the one it is reached from by little
 * (\p threshold sets how little) and that difference is close to the one before it.
 */
void followGradients(KisPixelSelectionSP region, KisPaintDeviceSP device, const QPoint &seed, int threshold, const QRect &bounds)
{
    if (!bounds.contains(seed)) return;
    const int w = bounds.width(), h = bounds.height();
    const size_t count = size_t(w) * h;
    const KoColorSpace *srgb = KoColorSpaceRegistry::instance()->rgb8();
    // the picture as premultiplied 8-bit sRGB: a change is the largest change of a channel
    std::vector<quint8> rgba(count * 4);
    if (*device->colorSpace() == *srgb) {
        device->readBytes(rgba.data(), bounds);
    } else {
        std::vector<quint8> raw(count * device->pixelSize());
        device->readBytes(raw.data(), bounds);
        device->colorSpace()->convertPixelsTo(raw.data(), rgba.data(), srgb, int(count), KoColorConversionTransformation::internalRenderingIntent(),
                                              KoColorConversionTransformation::internalConversionFlags());
    }
    for (size_t i = 0; i < count; i++) {
        quint8 *p = rgba.data() + i * 4;
        for (int c = 0; c < 3; c++) p[c] = quint8((p[c] * p[3] + 127) / 255);
    }
    auto pixel = [&](int x, int y) { return rgba.data() + (size_t(y) * w + x) * 4; };
    // the largest change of a channel from \p a to \p b, and how much that differs from the change from \p before to \p a
    auto change = [](const quint8 *a, const quint8 *b) {
        return std::max({std::abs(a[0] - b[0]), std::abs(a[1] - b[1]), std::abs(a[2] - b[2]), std::abs(a[3] - b[3])});
    };
    auto bend = [](const quint8 *before, const quint8 *a, const quint8 *b) {
        int most = 0;
        for (int c = 0; c < 4; c++) most = std::max(most, std::abs(int(b[c]) - 2 * int(a[c]) + int(before[c])));
        return most;
    };
    // 8-bit steps of a gradient differ by a level or two
    const int step = 3 + threshold / 2;
    const int curve = 2 + threshold / 16;

    std::vector<quint8> mine(count, 0);
    std::vector<int> stack;
    stack.reserve(4096);
    // from the color most of the pixels around the drop have: a drop on a hairline (a faint reflection) fills what is
    // around it, one on a shape a few pixels wide fills the shape
    const int sx = seed.x() - bounds.left(), sy = seed.y() - bounds.top();
    constexpr int around = 3;
    const QRect near = QRect(sx - around, sy - around, 2 * around + 1, 2 * around + 1) & QRect(0, 0, w, h);
    auto alikeAround = [&](int x, int y) {
        int alike = 0;
        for (int v = near.top(); v <= near.bottom(); v++) {
            for (int u = near.left(); u <= near.right(); u++) alike += change(pixel(x, y), pixel(u, v)) <= step;
        }
        return alike;
    };
    // the drop's own color unless another is more common
    QPoint main(sx, sy);
    int most = alikeAround(sx, sy);
    for (int y = near.top(); y <= near.bottom(); y++) {
        for (int x = near.left(); x <= near.right(); x++) {
            const int alike = alikeAround(x, y);
            if (alike > most) {
                most = alike;
                main = QPoint(x, y);
            }
        }
    }
    for (int y = near.top(); y <= near.bottom(); y++) {
        for (int x = near.left(); x <= near.right(); x++) {
            if (change(pixel(x, y), pixel(main.x(), main.y())) > step) continue;
            mine[size_t(y) * w + x] = MAX_SELECTED;
            stack.push_back(y * w + x);
        }
    }
    int left = sx, top = sy, right = sx, bottom = sy;
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        const int x = at % w, y = at / w;
        left = std::min(left, x);
        right = std::max(right, x);
        top = std::min(top, y);
        bottom = std::max(bottom, y);
        const quint8 *from = pixel(x, y);
        static const QPoint directions[] = {QPoint(-1, 0), QPoint(1, 0), QPoint(0, -1), QPoint(0, 1)};
        for (const QPoint &d : directions) {
            const int qx = x + d.x(), qy = y + d.y();
            if (qx < 0 || qy < 0 || qx >= w || qy >= h || mine[size_t(qy) * w + qx]) continue;
            const quint8 *to = pixel(qx, qy);
            if (change(from, to) > step) continue;
            const int bx = x - d.x(), by = y - d.y();
            if (bx >= 0 && by >= 0 && bx < w && by < h && bend(pixel(bx, by), from, to) > curve) continue;
            mine[size_t(qy) * w + qx] = MAX_SELECTED;
            stack.push_back(qy * w + qx);
        }
    }
    // joined with what the fill tool found, over what this reached
    const QRect reached(QPoint(left, top), QPoint(right, bottom));
    const QRect rect = reached.translated(bounds.topLeft());
    std::vector<quint8> joined(size_t(rect.width()) * rect.height());
    region->readBytes(joined.data(), rect);
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            quint8 &value = joined[size_t(y) * rect.width() + x];
            value = std::max(value, mine[size_t(y + reached.top()) * w + (x + reached.left())]);
        }
    }
    region->writeBytes(joined.data(), rect);
    region->invalidateOutlineCache();
}

/// What a drop by color at \p seed covers on \p device: the touching area of colors like the seed's, or every one
KisPixelSelectionSP regionByColor(KisPaintDeviceSP device, const QPoint &seed, int threshold, bool everywhere, const QRect &bounds)
{
    KisPixelSelectionSP region = regionByColorAlone(device, seed, threshold, everywhere, bounds);
    if (!everywhere) followGradients(region, device, seed, threshold, bounds);
    dropBlends(region, device, bounds);
    return region;
}

/**
 * The area of colors like the seed's, as the fill tool finds it: wholly within \p threshold, then fading out over half
 * as far again, so where it ends inside a gradient the new color fades into the old instead of stopping at a hard line.
 */
KisPixelSelectionSP regionByColorAlone(KisPaintDeviceSP device, const QPoint &seed, int threshold, bool everywhere, const QRect &bounds)
{
    // Krita's soft fill: covered wholly up to threshold × (1 − softness), then less and less up to the threshold
    const int reach = threshold + std::max(3, threshold / 2);
    const int softness = qBound(1, qRound(100.0 * (reach - threshold) / reach), 100);
    KisFillPainter painter;
    painter.setFillThreshold(reach);
    painter.setOpacitySpread(100 - softness);
    painter.setAntiAlias(false);
    painter.setCareForSelection(false);
    painter.setWidth(bounds.width());
    painter.setHeight(bounds.height());
    KisPixelSelectionSP region = new KisPixelSelection();
    if (everywhere) {
        painter.createSimilarColorsSelection(region, device->pixel(seed), device, bounds, nullptr);
        return region;
    }
    return painter.createFloodSelection(region, seed.x(), seed.y(), device, nullptr);
}

/// How far beside an area its soft edge is looked for: anti-aliasing and a little softness
constexpr int softRim = 2;
/// How far around a pixel of the soft edge the area's color and what lies beyond are read
constexpr int rimReach = 2 * softRim;
/// How unlike the area a pixel (premultiplied, 0 to 1 a channel) may be and still be a blend of it: closer neighbours
/// than this are the same color, a gradient or a speck of texture, not something beyond its edge
constexpr float alike = 0.15f;

/**
 * \p region of the picture made \p color, written into \p device, so that it looks as if the area had been painted that
 * color with nothing to fix up. \p seen is the picture before the fill: \p device's own pixels when it is recolored in
 * place, else the picture as it is seen, painted over on \p device (a clear layer above it). A pixel beside the area is
 * read as a share of the area's color blended with what lies beyond (premultiplied, in the picture's own encoding,
 * where its soft edges were blended), and that share takes the new color, so the edge keeps its blend instead of a ring
 * of the old color; specks of texture inside take it as far as they are like it. Over the picture the area is solid and
 * its edge is the least see-through color that looks right. A clear area (\p clear) gets the color under what is
 * there. Returns what changed.
 */
QRect fillByColor(KisPaintDeviceSP device, KisPaintDeviceSP seen, KisPixelSelectionSP region, const KoColor &color, bool inPlace, bool clear,
                  const QRect &bounds)
{
    const QRect area = region->selectedExactRect();
    if (area.isEmpty()) return QRect();
    const QRect rect = area.adjusted(-softRim, -softRim, softRim, softRim) & bounds;
    const KoColorSpace *cs = device->colorSpace();
    const KoColorSpace *seenCs = seen->colorSpace();
    // blends are read in the picture's own encoding, where its soft edges were blended
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
        // the region far enough around to tell what is beside it and what lies beyond
        const QRect wide = t.adjusted(-rimReach - softRim, -rimReach - softRim, rimReach + softRim, rimReach + softRim) & bounds;
        const int w = wide.width(), h = wide.height();
        std::vector<quint8> in(size_t(w) * h), rows(size_t(w) * h), near(size_t(w) * h);
        region->readBytes(in.data(), wide);
        auto index = [&](int x, int y) { return size_t(y - wide.top()) * w + size_t(x - wide.left()); };
        // all of the tile wholly in the area: over the picture, solid paint
        bool whole = true;
        for (int y = t.top(); y <= t.bottom() && whole; y++) {
            for (int x = t.left(); x <= t.right(); x++) {
                if (in[index(x, y)] < MAX_SELECTED) {
                    whole = false;
                    break;
                }
            }
        }
        if (whole && !inPlace) {
            device->fill(t, solid);
            return;
        }
        // near: within softRim of the area wholly covered, by a separable maximum
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                quint8 m = 0;
                for (int i = std::max(0, x - softRim); i <= std::min(w - 1, x + softRim); i++) m = std::max(m, in[size_t(y) * w + i] == MAX_SELECTED ? quint8(255) : quint8(0));
                rows[size_t(y) * w + x] = m;
            }
        }
        bool any = false;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                quint8 m = 0;
                for (int i = std::max(0, y - softRim); i <= std::min(h - 1, y + softRim); i++) m = std::max(m, rows[size_t(i) * w + x]);
                near[size_t(y) * w + x] = m;
                any = any || ((m >= 128 || in[size_t(y) * w + x] > 0) && t.contains(wide.left() + x, wide.top() + y));
            }
        }
        if (!any) return;

        std::vector<quint8> pixels(size_t(w) * h * seenSize);
        seen->readBytes(pixels.data(), wide);
        // the tile as it will be written: the layer as it is, its area painted
        std::vector<quint8> out(size_t(t.width()) * t.height() * pixelSize);
        if (inPlace) {
            for (int y = t.top(); y <= t.bottom(); y++) {
                memcpy(out.data() + size_t(y - t.top()) * t.width() * pixelSize, pixels.data() + index(t.left(), y) * pixelSize,
                       size_t(t.width()) * pixelSize);
            }
        } else {
            device->readBytes(out.data(), t);
        }
        auto outAt = [&](int x, int y) { return out.data() + (size_t(y - t.top()) * t.width() + (x - t.left())) * pixelSize; };
        // pixels that need blending: the soft edge, and (in place on a clear area) paint the color goes under
        std::vector<QPoint> blended;
        for (int y = t.top(); y <= t.bottom(); y++) {
            for (int x = t.left(); x <= t.right(); x++) {
                const size_t i = index(x, y);
                if (near[i] < 128 && in[i] == 0) continue;
                if (in[i] == MAX_SELECTED) {
                    if (!inPlace) {
                        memcpy(outAt(x, y), solid.data(), pixelSize);
                        continue;
                    }
                    const quint8 *src = pixels.data() + i * seenSize;
                    if (!clear || seenCs->opacityU8(src) == 0) {
                        const qreal opacity = clear ? 1.0 : seenCs->opacityF(src);
                        memcpy(outAt(x, y), own.data(), pixelSize);
                        cs->setOpacity(outAt(x, y), opacity, 1);
                        continue;
                    }
                }
                blended.push_back(QPoint(x, y));
            }
        }
        if (blended.empty()) {
            device->writeBytes(out.data(), t);
            return;
        }
        // premultiplied, so a blend is a straight line between two colors, transparency included
        std::vector<float> f(size_t(w) * h * 4);
        seenCs->convertPixelsTo(pixels.data(), reinterpret_cast<quint8 *>(f.data()), floats, w * h, intent, flags);
        for (size_t i = 0; i < size_t(w) * h; i++) {
            for (int c = 0; c < 3; c++) f[i * 4 + c] *= f[i * 4 + 3];
        }
        // over the picture: what the layer holds there, which the paint goes over
        std::vector<float> layer;
        if (!inPlace) {
            std::vector<quint8> held(blended.size() * pixelSize);
            for (size_t k = 0; k < blended.size(); k++) memcpy(held.data() + k * pixelSize, outAt(blended[k].x(), blended[k].y()), pixelSize);
            layer.resize(blended.size() * 4);
            cs->convertPixelsTo(held.data(), reinterpret_cast<quint8 *>(layer.data()), floats, int(blended.size()), intent, flags);
        }
        std::vector<float> results(blended.size() * 4);
        for (size_t k = 0; k < blended.size(); k++) {
            const QPoint at = blended[k];
            const float *p = f.data() + index(at.x(), at.y()) * 4;
            // what the picture should look like there (premultiplied), and how much of it is the area
            float target[4];
            float share = 1;
            if (clear) {
                // the color under what is there
                for (int c = 0; c < 3; c++) target[c] = p[c] + (1 - p[3]) * paint[c];
                target[3] = 1;
            } else {
                // the area's color beside it and what lies beyond, nearer pixels counting more; without anything beyond
                // within reach, the unlike pixel nearby that most stands out (a thin line, a speck's core)
                float o[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0}, wo = 0, wb = 0;
                const float *apart = nullptr;
                float apartBy = -1;
                for (int pass = 0; pass < 2; pass++) {
                    for (int dy = -rimReach; dy <= rimReach; dy++) {
                        for (int dx = -rimReach; dx <= rimReach; dx++) {
                            const int x = at.x() + dx, y = at.y() + dy;
                            if ((dx == 0 && dy == 0) || !wide.contains(x, y)) continue;
                            const size_t i = index(x, y);
                            const float *s = f.data() + i * 4;
                            const float weight = 1.0f / float(dx * dx + dy * dy);
                            if (pass == 0) {
                                if (in[i] == MAX_SELECTED) {
                                    for (int c = 0; c < 4; c++) o[c] += weight * s[c];
                                    wo += weight;
                                }
                            } else if (in[i] < MAX_SELECTED) {
                                if (near[i] < 128 && in[i] == 0) {
                                    for (int c = 0; c < 4; c++) b[c] += weight * s[c];
                                    wb += weight;
                                }
                                const float by = colorDistance(s, o);
                                if (by > apartBy) {
                                    apartBy = by;
                                    apart = s;
                                }
                            }
                        }
                    }
                    if (pass == 0) {
                        for (int c = 0; c < 4; c++) o[c] /= std::max(wo, 1e-6f);
                    }
                }
                if (wb > 0) {
                    for (int c = 0; c < 4; c++) b[c] /= wb;
                } else if (apart) {
                    for (int c = 0; c < 4; c++) b[c] = apart[c];
                } else {
                    for (int c = 0; c < 4; c++) b[c] = p[c];
                }
                const float span = colorDistance(o, b);
                // a detail inside the area, a little unlike it (a faint line, a speck, texture): in the new color, as
                // unlike it as it was
                const bool detail = wb <= 0 && span < alike;
                if (detail) {
                    share = 1;
                } else if (span < alike) {
                    // nothing unlike beyond: a gradient going on, changed as far as it is like the area
                    share = qBound(0.0f, 1.0f - colorDistance(p, o) / alike, 1.0f);
                } else {
                    // the pixel as share × the area's color + the rest × what is beyond, and how far off that line it is
                    float along = 0, off = 0;
                    for (int c = 0; c < 4; c++) along += (p[c] - b[c]) * (o[c] - b[c]);
                    share = qBound(0.0f, along / (span * span), 1.0f);
                    for (int c = 0; c < 4; c++) {
                        const float d = p[c] - b[c] - share * (o[c] - b[c]);
                        off += d * d;
                    }
                    share *= qBound(0.0f, 1.0f - std::sqrt(off) / span, 1.0f);
                }
                // a pixel in the fading part of the area: at least as much as it is covered
                const float covered = in[index(at.x(), at.y())] / 255.0f;
                if (wo <= 0) {
                    // nothing wholly covered nearby: only the fade
                    for (int c = 0; c < 4; c++) o[c] = p[c];
                    share = 0;
                }
                share = std::max(share, covered);
                for (int c = 0; c < 3; c++) target[c] = p[c] + share * (paint[c] * o[3] - o[c]);
                target[3] = p[3];
                for (int c = 0; c < 3; c++) target[c] = qBound(0.0f, target[c], target[3]);
            }
            float q[4];
            if (inPlace) {
                memcpy(q, target, sizeof(q));
            } else {
                // paint that, laid over the picture, gives the target: as see-through as it can be (but no less than its
                // share, so the layer holds the area's shape), then laid over what the layer already holds
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
            q[3] = qBound(0.0f, q[3], 1.0f);
            for (int c = 0; c < 3; c++) q[c] = q[3] > 1e-6f ? std::max(0.0f, q[c]) / q[3] : 0.0f;
            memcpy(results.data() + k * 4, q, sizeof(q));
        }
        std::vector<quint8> native(blended.size() * pixelSize);
        floats->convertPixelsTo(reinterpret_cast<const quint8 *>(results.data()), native.data(), cs, int(blended.size()), intent, flags);
        for (size_t k = 0; k < blended.size(); k++) memcpy(outAt(blended[k].x(), blended[k].y()), native.data() + k * pixelSize, pixelSize);
        device->writeBytes(out.data(), t);
    });
    return rect;
}

/// \p region tinted with \p color, at most 1600 pixels across.
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
    QColor tint = color;
    tint.setAlphaF(0.72);
    overlay.fill(tint);
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
    /// How it was worked out, so a drop on it takes it over instead of flooding again
    ByColor previewWay;
    KisPaintDeviceSP previewReference;
    QPoint previewSeed {-1, -1};
    int previewThreshold {-1};
    bool previewEverywhere {false};
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
    /// The picture before it, which it is made again from as the panel changes it, and the area it is to cover
    KisPaintDeviceSP lastReference;
    KisPixelSelectionSP readyRegion;
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
        KisPaintDeviceSP reference = new KisPaintDevice(way && way.inPlace ? *way.node->paintDevice() : *image->projection());
        const QColor tint = color.toQColor();
        struct Result {
            KisPixelSelectionSP region;
            QImage overlay;
            QRect rect;
            int gap {0};
        };
        auto *watcher = new QFutureWatcher<Result>(q);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, q, [this, watcher, ticket, way, reference, at, t, all]() {
            const Result result = watcher->result();
            watcher->deleteLater();
            computing = false;
            if (ticket == generation && preview) {
                region = result.region;
                previewWay = way;
                previewReference = reference;
                previewSeed = at;
                previewThreshold = t;
                previewEverywhere = all;
                previewGap = result.gap;
                preview->overlay = result.overlay;
                preview->rect = result.rect;
                showPreview();
            }
            if (pending) compute();
        });
        const bool byOwnColors = bool(way);
        watcher->setFuture(QtConcurrent::run([reference, at, t, o, bounds, tint, byOwnColors, all]() {
            Result result;
            // out of memory on a huge image: no preview, rather than an exception thrown into the window's thread
            try {
                if (byOwnColors) {
                    result.region = regionByColor(reference, at, t, all, bounds);
                } else {
                    // Auto's search for a gap that keeps the fill inside floods several times: here, never on the window's thread
                    result.gap = gapFor(o, reference, at, bounds, 0.5, t);
                    result.region = regionAt(reference, at, t, result.gap, o, bounds);
                }
                result.rect = result.region->selectedExactRect();
                result.overlay = tintedOverlay(result.region, result.rect, tint);
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
    /// worked out in the background; either way never flooded on the window's thread
    void startByColor(const ByColor &way, const QPoint &point, const KoColor &fill)
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image) return;
        lastWay = way;
        lastColor = fill;
        if (region && previewReference && previewWay == way && previewThreshold == threshold && previewEverywhere == everywhere
            && region->pixel(point).opacityU8() > 0) {
            lastSeed = previewSeed;
            lastReference = previewReference;
            commitByColor(region);
            return;
        }
        lastSeed = point;
        lastReference = new KisPaintDevice(way.inPlace ? *way.node->paintDevice() : *image->projection());
        regionInBackground(false);
    }

    /// The area of the last fill by color with the settings now, worked out from the picture before it; then the fill
    /// is made (\p again: made again in place of the last, while that is still the newest step)
    void regionInBackground(bool again)
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image || !lastReference) return;
        if (fillComputing) {
            fillPending = true;
            return;
        }
        fillComputing = true;
        fillPending = false;
        const int ticket = ++fillTicket;
        const KisPaintDeviceSP reference = lastReference;
        const QPoint at = lastSeed;
        const int t = threshold;
        const bool all = everywhere;
        const QRect bounds = image->bounds();
        auto *watcher = new QFutureWatcher<KisPixelSelectionSP>(q);
        QObject::connect(watcher, &QFutureWatcher<KisPixelSelectionSP>::finished, q, [this, watcher, ticket, again]() {
            const KisPixelSelectionSP filled = watcher->result();
            watcher->deleteLater();
            fillComputing = false;
            // a newer change waits: only its area is worth filling
            if (fillPending) {
                regionInBackground(true);
                return;
            }
            if (ticket != fillTicket || !filled) return;
            if (again) {
                readyRegion = filled;
                redo.settingChanged();
            } else {
                commitByColor(filled);
            }
        });
        watcher->setFuture(QtConcurrent::run([reference, at, t, all, bounds]() -> KisPixelSelectionSP {
            try {
                return regionByColor(reference, at, t, all, bounds);
            } catch (const std::bad_alloc &) {
                return nullptr;
            }
        }));
    }

    /// The last fill by color made over \p filled, as one step
    void commitByColor(KisPixelSelectionSP filled)
    {
        KisImageSP image = view ? view->image() : nullptr;
        if (!image || !lastWay || !filled) return;
        readyRegion = filled;
        redo.made(view->document() ? view->document()->undoStack() : nullptr, kundo2_i18n("Fill"), [this]() { commitByColor(readyRegion); });
        KisStrokeStrategyUndoCommandBased *strategy = new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Fill"), false, image.data());
        KisStrokeId stroke = image->startStroke(strategy);
        KIS_SAFE_ASSERT_RECOVER_RETURN(stroke);
        KisNodeSP node = lastWay.node;
        const bool inPlace = lastWay.inPlace;
        const KisPaintDeviceSP reference = lastReference;
        const KoColor fill = lastColor;
        const QPoint at = lastSeed;
        const QRect bounds = image->bounds();
        image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                                  KUndo2CommandSP(new KisCommandUtils::LambdaCommand([node, inPlace, reference, filled, fill, at, bounds]() mutable -> KUndo2Command * {
                                      if (filled->selectedExactRect().isEmpty()) return nullptr;
                                      KisPaintDeviceSP device = node->paintDevice();
                                      const bool clear = reference->pixel(at).opacityU8() < 128;
                                      KisTransaction transaction(device);
                                      const QRect changed = fillByColor(device, reference, filled, fill, inPlace, clear, bounds);
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
        options.closeGap = qRound(panel->gap->value());
        options.threshold = qRound(panel->spread->value());
        showAutoGap();
        options.underLines = panel->underLines->isChecked();
        everywhere = panel->everywhere->isChecked();
        options.write();
        threshold = options.threshold;
        // by color: the new area worked out in the background first, the fill made again only once it is ready
        if (lastWay) {
            regionInBackground(true);
            return;
        }
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
