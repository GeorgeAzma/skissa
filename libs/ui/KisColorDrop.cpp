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
#include <vector>

#include <QtConcurrent>

#include <KisValueSteps.h>
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

/// The layer a drop at \p point recolors by its own colors: a painting, or a flat on a layer of colors (the active one or
/// the one beside the line art); null where it fills line art
KisNodeSP recoloredAt(KisNodeSP active, const QPoint &point)
{
    if (!active || !qobject_cast<KisPaintLayer *>(active.data()) || !active->isEditable()) return nullptr;
    auto flatAt = [&point](KisNodeSP node) { return KisColorDrop::isColorsLayer(node) && node->paintDevice()->pixel(point).opacityU8() >= 128; };
    if (KisColorDrop::isColorsLayer(active)) return flatAt(active) ? active : nullptr;
    if (KisColorDrop::isPaintingAt(active, point)) return active;
    if (flatAt(active->prevSibling())) return active->prevSibling();
    if (flatAt(active->nextSibling())) return active->nextSibling();
    return nullptr;
}

/// What a recoloring drop at \p seed covers on \p device: the touching area of colors like the seed's, or every one
KisPixelSelectionSP recolorRegion(KisPaintDeviceSP device, const QPoint &seed, int threshold, bool everywhere, const QRect &bounds)
{
    KisFillPainter painter;
    painter.setFillThreshold(threshold);
    painter.setOpacitySpread(100);
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
constexpr int softRim = 3;

/**
 * \p region of \p device made \p color, as if the area had been painted that color. A pixel beside it is read as
 * a share of the area's color blended with what lies beyond, and that share takes the new color, so the soft edge
 * keeps its blend instead of a faint ring of the old color. On a clear area (\p clear) the color goes under what is
 * there; on paint each pixel keeps its opacity. \p original is the device as it was. Returns what changed.
 */
QRect recolorIn(KisPaintDeviceSP device, KisPaintDeviceSP original, KisPixelSelectionSP region, const KoColor &color, bool clear,
                const QRect &bounds)
{
    const QRect area = region->selectedExactRect();
    if (area.isEmpty()) return QRect();
    const QRect rect = area.adjusted(-softRim, -softRim, softRim, softRim) & bounds;
    const KoColorSpace *cs = device->colorSpace();
    // blends are read in the layer's own encoding, where its soft edges were blended
    const KoColorProfile *profile = cs->colorModelId() == RGBAColorModelID ? cs->profile() : nullptr;
    const KoColorSpace *floats = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(), profile);
    const KoColor own = color.convertedTo(cs);
    const KoColor paintF = color.convertedTo(floats);
    const float *paint = reinterpret_cast<const float *>(paintF.data());
    const int pixelSize = cs->pixelSize();
    const int margin = 2 * softRim;

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
        const QRect wide = t.adjusted(-margin - softRim, -margin - softRim, margin + softRim, margin + softRim) & bounds;
        const int w = wide.width(), h = wide.height();
        std::vector<quint8> in(size_t(w) * h), rows(size_t(w) * h), near(size_t(w) * h);
        region->readBytes(in.data(), wide);
        // near: the region within softRim, by a separable maximum
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                quint8 m = 0;
                for (int i = std::max(0, x - softRim); i <= std::min(w - 1, x + softRim); i++) m = std::max(m, in[size_t(y) * w + i]);
                rows[size_t(y) * w + x] = m;
            }
        }
        bool any = false;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                quint8 m = 0;
                for (int i = std::max(0, y - softRim); i <= std::min(h - 1, y + softRim); i++) m = std::max(m, rows[size_t(i) * w + x]);
                near[size_t(y) * w + x] = m;
                any = any || (m >= 128 && t.contains(wide.left() + x, wide.top() + y));
            }
        }
        if (!any) return;
        auto index = [&](int x, int y) { return size_t(y - wide.top()) * w + size_t(x - wide.left()); };

        std::vector<quint8> pixels(size_t(w) * h * pixelSize);
        original->readBytes(pixels.data(), wide);
        std::vector<quint8> out(size_t(t.width()) * t.height() * pixelSize);
        for (int y = t.top(); y <= t.bottom(); y++) {
            memcpy(out.data() + size_t(y - t.top()) * t.width() * pixelSize, pixels.data() + index(t.left(), y) * pixelSize,
                   size_t(t.width()) * pixelSize);
        }
        // pixels that need blending: the soft edge, and (on a clear area) paint the color goes under
        std::vector<QPoint> blended;
        for (int y = t.top(); y <= t.bottom(); y++) {
            for (int x = t.left(); x <= t.right(); x++) {
                const size_t i = index(x, y);
                if (near[i] < 128) continue;
                const quint8 *src = pixels.data() + i * pixelSize;
                quint8 *dst = out.data() + (size_t(y - t.top()) * t.width() + (x - t.left())) * pixelSize;
                if (in[i] >= 128 && (!clear || cs->opacityU8(src) == 0)) {
                    const qreal opacity = clear ? 1.0 : cs->opacityF(src);
                    memcpy(dst, own.data(), pixelSize);
                    cs->setOpacity(dst, opacity, 1);
                    continue;
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
        cs->convertPixelsTo(pixels.data(), reinterpret_cast<quint8 *>(f.data()), floats, w * h,
                            KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
        for (size_t i = 0; i < size_t(w) * h; i++) {
            for (int c = 0; c < 3; c++) f[i * 4 + c] *= f[i * 4 + 3];
        }
        std::vector<float> results(blended.size() * 4);
        for (size_t k = 0; k < blended.size(); k++) {
            const QPoint at = blended[k];
            const float *p = f.data() + index(at.x(), at.y()) * 4;
            float q[4];
            if (clear) {
                // the color under what is there
                for (int c = 0; c < 3; c++) q[c] = p[c] + (1 - p[3]) * paint[c];
                q[3] = p[3] + (1 - p[3]);
            } else {
                // the area's color beside it and what lies beyond, nearer pixels counting more
                float o[4] = {0, 0, 0, 0}, b[4] = {0, 0, 0, 0}, wo = 0, wb = 0;
                for (int dy = -margin; dy <= margin; dy++) {
                    for (int dx = -margin; dx <= margin; dx++) {
                        const int x = at.x() + dx, y = at.y() + dy;
                        if ((dx == 0 && dy == 0) || !wide.contains(x, y)) continue;
                        const size_t i = index(x, y);
                        const float weight = 1.0f / float(dx * dx + dy * dy);
                        const float *s = f.data() + i * 4;
                        if (in[i] >= 128) {
                            for (int c = 0; c < 4; c++) o[c] += weight * s[c];
                            wo += weight;
                        } else if (near[i] < 128) {
                            for (int c = 0; c < 4; c++) b[c] += weight * s[c];
                            wb += weight;
                        }
                    }
                }
                for (int c = 0; c < 4; c++) o[c] /= std::max(wo, 1e-6f);
                float share = 0;
                if (wb > 0) {
                    // the pixel as share × the area's color + the rest × what is beyond, and how far off that line it is
                    float ob[4], pb[4], len2 = 0, along = 0;
                    for (int c = 0; c < 4; c++) {
                        b[c] /= wb;
                        ob[c] = o[c] - b[c];
                        pb[c] = p[c] - b[c];
                        len2 += ob[c] * ob[c];
                        along += pb[c] * ob[c];
                    }
                    if (len2 > 1e-4f) {
                        share = qBound(0.0f, along / len2, 1.0f);
                        float off = 0;
                        for (int c = 0; c < 4; c++) off += (pb[c] - share * ob[c]) * (pb[c] - share * ob[c]);
                        share *= qBound(0.0f, 1.0f - std::sqrt(off / len2), 1.0f);
                    }
                } else {
                    // a speck inside the area: changed as far as it is like it
                    float d2 = 0;
                    for (int c = 0; c < 4; c++) d2 += (p[c] - o[c]) * (p[c] - o[c]);
                    share = qBound(0.0f, 1.0f - std::sqrt(d2) / 0.25f, 1.0f);
                }
                const float alpha = o[3];
                for (int c = 0; c < 3; c++) q[c] = p[c] + share * (paint[c] * alpha - o[c]);
                q[3] = p[3];
            }
            q[3] = qBound(0.0f, q[3], 1.0f);
            for (int c = 0; c < 3; c++) {
                q[c] = std::max(0.0f, q[c]);
                q[c] = q[3] > 1e-6f ? q[c] / q[3] : 0.0f;
            }
            memcpy(results.data() + k * 4, q, sizeof(q));
        }
        std::vector<quint8> native(blended.size() * pixelSize);
        floats->convertPixelsTo(reinterpret_cast<const quint8 *>(results.data()), native.data(), cs, int(blended.size()),
                                KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
        for (size_t k = 0; k < blended.size(); k++) {
            const QPoint at = blended[k];
            memcpy(out.data() + (size_t(at.y() - t.top()) * t.width() + (at.x() - t.left())) * pixelSize, native.data() + k * pixelSize, pixelSize);
        }
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

/// The area under the color as the drag goes, and how far it spreads while it is tuned.
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
    /// A value being tuned, shown above the pointer (widget coordinates)
    QString label;
    QPoint labelAt;

protected:
    void drawDecoration(QPainter &gc, const QRectF &, const KisCoordinatesConverter *converter, KisCanvas2 *) override
    {
        if (!overlay.isNull()) {
            gc.save();
            gc.setTransform(converter->imageToWidgetTransform(), false);
            gc.setRenderHint(QPainter::SmoothPixmapTransform, true);
            gc.drawImage(QRectF(rect), overlay);
            gc.restore();
        }
        if (label.isEmpty()) return;
        gc.save();
        gc.setRenderHint(QPainter::Antialiasing, true);
        const QPalette palette = qApp->palette();
        gc.setFont(qApp->font());
        const QFontMetrics metrics(qApp->font());
        const QSizeF size(metrics.horizontalAdvance(label) + 2 * KisModernStyle::space3, metrics.height() + KisModernStyle::space2);
        // above the pointer and the drag's picture under it
        const QRectF pill(QPointF(labelAt) + QPointF(-size.width() / 2, -size.height() - 24), size);
        QColor base = palette.color(QPalette::Window);
        base.setAlphaF(0.92);
        gc.setPen(Qt::NoPen);
        gc.setBrush(base);
        gc.drawPath(KisModernStyle::squircle(pill, KisModernStyle::tileRadius));
        gc.setPen(palette.color(QPalette::WindowText));
        gc.drawText(pill, Qt::AlignCenter, label);
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
        everywhere = new QCheckBox(i18nc("color drop: every area of the color, not only the one dropped on", "Everywhere"), this);
        everywhere->setFocusPolicy(Qt::NoFocus);
        everywhere->setToolTip(i18nc("@info:tooltip", "Every area of this color"));
        grid->addWidget(gap);
        grid->addWidget(spread);
        grid->addWidget(underLines);
        grid->addWidget(everywhere);
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
        done = end(QStringLiteral("studio_done"), i18nc("@info:tooltip", "Done (Enter)"));
        cancel = end(QStringLiteral("studio_cancel"), i18nc("@info:tooltip", "Take back the fill (Esc)"));
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
    QCheckBox *everywhere {nullptr};
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
    /// The last fill recolored a painting or a flat by its own colors: the panel has Spread and Everywhere, not the lines' settings
    bool recolor {false};
    /// Every area of the color, not only the one dropped on (a recoloring drop's)
    bool everywhere {false};
    /// Held still while dragging a color: then sliding sideways tunes Spread, the fill staying where it rested
    QTimer hold;
    bool tuning {false};
    QPoint restAt {-1000, -1000};
    int tuneX {0};
    int tuneFrom {8};
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

    /// The spread being tuned, above the pointer
    void showSpread()
    {
        if (!preview) return;
        preview->label = i18nc("color drop: the spread being tuned while dragging", "Spread %1", threshold);
        preview->labelAt = pointer;
        showPreview();
    }

    void clearPreview()
    {
        generation++;
        pending = false;
        region = nullptr;
        previewGap = 0;
        seed = QPoint(-1, -1);
        hold.stop();
        tuning = false;
        restAt = QPoint(-1000, -1000);
        if (preview) {
            preview->overlay = QImage();
            preview->label.clear();
        }
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
        KisNodeSP recolored = inSelection ? nullptr : recoloredAt(active, at);
        // a snapshot, as the image may change while this runs
        KisPaintDeviceSP reference = new KisPaintDevice(recolored ? *recolored->paintDevice() : *image->projection());
        const QColor tint = color.toQColor();
        struct Result {
            KisPixelSelectionSP region;
            QImage overlay;
            QRect rect;
            int gap {0};
        };
        auto *watcher = new QFutureWatcher<Result>(q);
        QObject::connect(watcher, &QFutureWatcher<Result>::finished, q, [this, watcher, ticket]() {
            const Result result = watcher->result();
            watcher->deleteLater();
            computing = false;
            if (ticket == generation && preview) {
                region = result.region;
                previewGap = result.gap;
                preview->overlay = result.overlay;
                preview->rect = result.rect;
                showPreview();
            }
            if (pending) compute();
        });
        const bool byOwnColors = recolored;
        watcher->setFuture(QtConcurrent::run([reference, at, t, o, bounds, tint, byOwnColors, all]() {
            Result result;
            // out of memory on a huge image: no preview, rather than an exception thrown into the window's thread
            try {
                if (byOwnColors) {
                    result.region = recolorRegion(reference, at, t, all, bounds);
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
        if (panel->gap->isVisibleTo(panel) == !recolor && panel->everywhere->isVisibleTo(panel) == recolor) return;
        panel->gap->setVisible(!recolor);
        panel->underLines->setVisible(!recolor);
        panel->everywhere->setVisible(recolor);
        // the layout sees the rows go only once it is run again
        panel->layout()->activate();
        panel->resize(panel->sizeHint());
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
            QObject::connect(panel->everywhere, &QCheckBox::toggled, q, [this]() { changed(); });
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
    d->hold.setSingleShot(true);
    connect(&d->hold, &QTimer::timeout, this, [this]() {
        if (d->seed == QPoint(-1, -1)) return;
        d->tuning = true;
        d->tuneX = d->pointer.x();
        d->tuneFrom = d->threshold;
        d->showSpread();
    });
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
    KisPaintDeviceSP device = layer && qobject_cast<KisPaintLayer *>(layer.data()) ? layer->paintDevice() : nullptr;
    if (!device) return false;
    // a sparse look around the point, as wide as a shape's outline is far: every fourth pixel and row
    const QRect window = QRect(point - QPoint(384, 384), QSize(768, 768)) & device->defaultBounds()->bounds();
    if (window.isEmpty()) return false;
    const KoColorSpace *srgb = KoColorSpaceRegistry::instance()->rgb8();
    const bool isSrgb = *device->colorSpace() == *srgb;
    const int width = window.width();
    std::vector<quint8> row(size_t(width) * device->pixelSize()), colors(size_t(width) * 4);
    int paper = 0, all = 0;
    for (int y = window.top(); y <= window.bottom(); y += 4) {
        device->readBytes(row.data(), window.left(), y, width, 1);
        if (!isSrgb) {
            device->colorSpace()->convertPixelsTo(row.data(), colors.data(), srgb, width, KoColorConversionTransformation::internalRenderingIntent(),
                                                  KoColorConversionTransformation::internalConversionFlags());
        }
        const quint8 *p = isSrgb ? row.data() : colors.data();
        for (int x = 0; x < width; x += 4) {
            const quint8 *px = p + size_t(x) * 4;
            all++;
            // paper: clear, or light and gray (white, cream, a scan's off-white)
            const int high = std::max({px[0], px[1], px[2]}), low = std::min({px[0], px[1], px[2]});
            if (px[3] < 128 || (low >= 200 && high - low <= 40)) paper++;
        }
    }
    // line art leaves most of its layer paper, however dark its lines; a painting covers it, light or dark
    return all > 64 && paper * 10 < all * 3;
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

    // tuning: sideways is Spread, by ratios as sizes are, the fill staying where it rested
    if (d->tuning) {
        const int spread = qBound(1, qRound(KisValueSteps::draggedBy(d->tuneFrom, canvasPos.x() - d->tuneX, 99, true, 1)), 100);
        if (spread != d->threshold) {
            d->threshold = spread;
            d->compute();
        }
        d->showSpread();
        return;
    }
    // resting a moment starts tuning
    if ((canvasPos - d->restAt).manhattanLength() > KisModernStyle::dragDistance()) {
        d->restAt = canvasPos;
        d->hold.start(KisModernStyle::holdInterval());
    }

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
    // tuned: the fill where it rested, with the spread slid to, kept for the next drops
    const bool tuned = d->tuning && d->seed != QPoint(-1, -1);
    const QPoint point = tuned ? d->seed : d->imagePoint(canvasPos);
    const int spread = d->threshold;
    // the gap Auto found for the area shown under it, if it was worked out there
    d->dropGap = d->region && d->region->pixel(point).opacityU8() > 0 ? d->previewGap : 0;
    d->options = Options::read();
    if (tuned) {
        d->options.threshold = spread;
        d->options.write();
    }
    d->threshold = d->options.threshold;
    d->clearPreview();
    if (!fillAt(color, point)) return false;
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
    // a painting or a flat: recolored by its own colors, its soft edges taking the new color by their share
    KisNodeSP recolored = inSelection ? nullptr : recoloredAt(active, point);
    d->recolor = recolored;
    if (d->panel) d->fitPanel();

    d->lastSeed = point;
    d->lastColor = color;
    // made again while its panel changes it and it is still the newest step
    d->redo.made(d->view->document() ? d->view->document()->undoStack() : nullptr, kundo2_i18n("Fill"), [this]() { refillLast(); });
    KisStrokeStrategyUndoCommandBased *strategy =
        new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Fill"), false, image.data());
    strategy->setSupportsWrapAroundMode(true);
    KisStrokeId stroke = image->startStroke(strategy);
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(stroke, false);

    if (recolored) {
        const bool everywhere = d->everywhere;
        const QRect bounds = image->bounds();
        image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                                  KUndo2CommandSP(new KisCommandUtils::LambdaCommand([recolored, point, threshold, everywhere, color, bounds]() mutable -> KUndo2Command * {
                                      KisPaintDeviceSP device = recolored->paintDevice();
                                      // read as it was: the tiles are written side by side
                                      KisPaintDeviceSP original = new KisPaintDevice(*device);
                                      KisPixelSelectionSP region = recolorRegion(original, point, threshold, everywhere, bounds);
                                      if (region->selectedExactRect().isEmpty()) return nullptr;
                                      const bool clear = original->pixel(point).opacityU8() < 128;
                                      KisTransaction transaction(device);
                                      const QRect changed = recolorIn(device, original, region, color, clear, bounds);
                                      recolored->setDirty(changed);
                                      return transaction.endAndTake();
                                  })),
                                  false, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
        image->endStroke(stroke);
        return true;
    }

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
