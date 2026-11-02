/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaDetailWidget.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QVBoxLayout>

#include <climits>
#include <cmath>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KisValueSteps.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioCurve.h>

#include "SkissaDetailFilter.h"

using namespace SkissaDetail;

namespace
{

/// The drag on the image across which a point goes from bottom to top
constexpr qreal dragForWholeCurve = 300;
/// A point taken on the image holds the curve this far either side: one size changes alone
constexpr qreal holdAround = 0.15;
/// The part of the image the tiles show, at its own size so that fine detail shows, and the room read around it
const QSize tileSize(128, 80);
constexpr int tileMargin = 48;

QString number(qreal value, int decimals)
{
    return KisValueSteps::withoutTrailingZeros(QString::number(value, 'f', decimals));
}

QString radiusText(qreal radius)
{
    return i18nc("a size in pixels", "%1 px", number(radius, radius < 10 ? 1 : 0));
}

QString gainText(float gain)
{
    return QStringLiteral("×") + number(gain, 2);
}

KisDoubleSliderSpinBox *slider(QWidget *parent, qreal maximum, qreal softMaximum, int decimals, const QString &suffix)
{
    auto *box = new KisDoubleSliderSpinBox(parent);
    box->setRange(0, maximum, decimals);
    box->setSoftRange(0, softMaximum);
    box->setSuffix(suffix);
    return box;
}

struct Preset {
    QString name;
    KisStudioCurve curve;
    qreal edges;
};

/// A curve through gains at radii
KisStudioCurve curveOf(std::initializer_list<std::pair<qreal, float>> points)
{
    KisStudioCurve curve;
    for (const auto &point : points) curve.points << QPointF(point.first < 1 ? 0.0 : xOfRadius(point.first), yOfGain(point.second));
    return curve;
}

/// The middle of the most textured part of \p bounds, where changes to fine detail show: the block of a small copy
/// whose pixels differ most from their neighbours
QPoint mostDetailed(KisPaintDeviceSP device, const QRect &bounds)
{
    const QImage thumb = device->createThumbnailUncached(384, 384, bounds).convertToFormat(QImage::Format_ARGB32);
    if (thumb.width() < 3 || thumb.height() < 3) return bounds.center();
    auto light = [&thumb](int x, int y) {
        const QRgb pixel = thumb.pixel(x, y);
        return qGray(pixel) * qAlpha(pixel) / 255.0;
    };
    const int blocks = 12;
    const int bw = qMax(3, thumb.width() / blocks), bh = qMax(3, thumb.height() / blocks);
    QPoint best = bounds.center();
    qreal most = -1;
    for (int by = 1; by + bh < thumb.height(); by += bh) {
        for (int bx = 1; bx + bw < thumb.width(); bx += bw) {
            qreal sum = 0;
            for (int y = by; y < by + bh; y++) {
                for (int x = bx; x < bx + bw; x++) {
                    sum += std::abs(4 * light(x, y) - light(x - 1, y) - light(x + 1, y) - light(x, y - 1) - light(x, y + 1));
                }
            }
            if (sum > most) {
                most = sum;
                best = bounds.topLeft() + QPoint(qRound((bx + bw / 2.0) * bounds.width() / thumb.width()),
                                                 qRound((by + bh / 2.0) * bounds.height() / thumb.height()));
            }
        }
    }
    return best;
}

/// Rings of every size from coarse to fine, for a layer with nothing on it
QImage zonePlate(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32);
    const qreal k = M_PI / (2.0 * size.width());
    for (int y = 0; y < size.height(); y++) {
        for (int x = 0; x < size.width(); x++) {
            const qreal dx = x - size.width() / 2.0, dy = y - size.height() / 2.0;
            const int v = qRound(127.5 + 127.5 * std::cos(k * (dx * dx + dy * dy)));
            image.setPixel(x, y, qRgb(v, v, v));
        }
    }
    return image;
}

} // namespace

SkissaDetailSliders::SkissaDetailSliders(QWidget *parent, Kind kind)
    : KisConfigWidget(parent)
    , m_kind(kind)
{
    auto *layout = new QFormLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto add = [&](const char *key, const QString &name, KisDoubleSliderSpinBox *box, qreal shownPerValue, const QString &tip = QString()) {
        box->setToolTip(tip);
        layout->addRow(name, box);
        m_sliders << Slider{key, box, shownPerValue};
        connect(box, qOverload<double>(&KisDoubleSliderSpinBox::valueChanged), this, &KisConfigWidget::sigConfigurationItemChanged);
    };
    const QString px = i18nc("pixels, after a number", " px");
    if (kind == Kind::Sharpen) {
        add("amount", i18nc("effect setting", "Amount"), slider(this, 1000, 300, 0, QStringLiteral("%")), 100);
    }
    add("radius", i18nc("effect setting", "Radius"), slider(this, 500, kind == Kind::Sharpen ? 10 : 100, 1, px), 1);
    if (kind == Kind::Smooth) {
        add("edges", i18nc("effect setting", "Edges"), slider(this, 100, 100, 0, QStringLiteral("%")), 100,
            i18nc("@info:tooltip", "Keeps edges sharp"));
    }
}

void SkissaDetailSliders::setConfiguration(const KisPropertiesConfigurationSP config)
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(SkissaDetailFilter::idOf(m_kind).id());
    KisFilterConfigurationSP defaults = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
    Q_FOREACH (const Slider &slider, m_sliders) {
        QSignalBlocker blocker(slider.box);
        const qreal value = config && config->hasProperty(slider.key) ? config->getDouble(slider.key) : defaults->getDouble(slider.key);
        slider.box->setValue(value * slider.shownPerValue);
    }
}

KisPropertiesConfigurationSP SkissaDetailSliders::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(SkissaDetailFilter::idOf(m_kind).id());
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    Q_FOREACH (const Slider &slider, m_sliders) config->setProperty(slider.key, slider.box->value() / slider.shownPerValue);
    return config;
}

struct SkissaDetailWidget::Private {
    KisPaintDeviceSP original;
    KisStudioCurve curve {flatCurve()};
    qreal edges {0};
    QVector<Preset> presets;
    QVector<QImage> pictures;

    KisStudioChips *tiles {nullptr};
    KisStudioCurveView *view {nullptr};
    KisDoubleSliderSpinBox *edgesBox {nullptr};

    /// The drag on the image in progress: the point it moves and where that started
    int dragPoint {-1};
    qreal dragStart {0};

    void makePresets()
    {
        presets = {
            {i18nc("detail effect preset", "Original"), flatCurve(), 0},
            {i18nc("detail effect preset", "Soften"), curveOf({{0, 0.3f}, {1.5, 0.3f}, {6, 1}, {1024, 1}}), 0},
            {i18nc("detail effect preset", "Smooth"), curveOf({{0, 0}, {3, 0}, {12, 1}, {1024, 1}}), 0.4},
            {i18nc("detail effect preset", "Crisp"), curveOf({{0, 2}, {1.4, 2}, {6, 1}, {1024, 1}}), 0},
            {i18nc("detail effect preset", "Clarity"), curveOf({{0, 1}, {4, 1}, {16, 1.6f}, {64, 1.6f}, {300, 1}, {1024, 1}}), 0.4},
            {i18nc("detail effect preset", "Even"), curveOf({{0, 1}, {32, 1}, {256, 0.5f}, {1024, 0.5f}}), 0},
        };
    }

    void makePictures()
    {
        // the most textured part of the layer, at its own size, with room around it for the blurs
        QImage base;
        QPoint corner;
        if (original) {
            const QRect bounds = original->exactBounds();
            if (!bounds.isEmpty()) {
                const QPoint detailed = mostDetailed(original, bounds);
                QRect crop(detailed - QPoint(tileSize.width() / 2, tileSize.height() / 2), tileSize);
                crop = crop.adjusted(-tileMargin, -tileMargin, tileMargin, tileMargin);
                base = original->convertToQImage(nullptr, crop);
                corner = QPoint(tileMargin, tileMargin);
            }
        }
        if (base.isNull()) {
            base = zonePlate(tileSize + QSize(2 * tileMargin, 2 * tileMargin));
            corner = QPoint(tileMargin, tileMargin);
        }
        pictures.clear();
        Q_FOREACH (const Preset &preset, presets) {
            pictures << applied(base, planOfCurve(preset.curve, preset.edges)).copy(QRect(corner, tileSize));
        }
    }
};

SkissaDetailWidget::SkissaDetailWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    // the pixels as they were before the preview changes them; their tiles are shared until then
    if (dev) d->original = new KisPaintDevice(*dev);
    d->makePresets();
    d->makePictures();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    d->tiles = new KisStudioChips(this);
    d->tiles->setTileColumns(6, qMax(64, 2 * fontMetrics().height() + 34));
    connect(d->tiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->presets.size()) return;
        d->curve = d->presets[index].curve;
        d->edges = d->presets[index].edges;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->tiles);

    d->view = new KisStudioCurveView(this);
    d->view->changed = [this](const KisStudioCurve &curve) {
        d->curve = curve;
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    };
    d->view->describe = [](qreal x, qreal y) { return radiusText(radiusAt(x)) + QStringLiteral("  ") + gainText(gainOf(y)); };
    d->view->backdrop = [this](QPainter &painter, const QRectF &box) {
        // dots from the finest features to the largest along the bottom
        QColor ink = palette().color(QPalette::WindowText);
        ink.setAlphaF(0.22);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        for (int octave = 0; octave <= int(octaves); octave++) {
            const qreal x = box.left() + box.width() * octave / octaves;
            const qreal size = 2 + 1.4 * octave;
            painter.drawEllipse(QPointF(qBound(box.left() + size, x, box.right() - size), box.bottom() - KisModernStyle::space2 - size / 2), size / 2, size / 2);
        }
    };
    layout->addWidget(d->view);

    auto *edgesRow = new QHBoxLayout();
    edgesRow->setSpacing(KisModernStyle::space2);
    edgesRow->addWidget(new QLabel(i18nc("effect setting", "Edges"), this));
    d->edgesBox = slider(this, 100, 100, 0, QStringLiteral("%"));
    d->edgesBox->setToolTip(i18nc("@info:tooltip", "Keeps edges sharp"));
    connect(d->edgesBox, qOverload<double>(&KisDoubleSliderSpinBox::valueChanged), this, [this](double value) {
        d->edges = value / 100;
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    });
    edgesRow->addWidget(d->edgesBox, 1);
    layout->addLayout(edgesRow);

    refresh();
}

SkissaDetailWidget::~SkissaDetailWidget()
{
}

void SkissaDetailWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->curve = config ? KisStudioCurve::fromString(config->getString("curve"), flatCurve()) : flatCurve();
    d->edges = config ? qBound(0.0, config->getDouble("edges", 0), 1.0) : 0;
    refresh();
}

KisPropertiesConfigurationSP SkissaDetailWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(SkissaDetailFilter::idOf(Kind::Detail).id());
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    config->setProperty("curve", d->curve.toString());
    config->setProperty("edges", d->edges);
    return config;
}

QSize SkissaDetailWidget::sizeHint() const
{
    const int width = 500;
    return QSize(width, layout()->heightForWidth(width));
}

void SkissaDetailWidget::refresh()
{
    d->view->setCurve(d->curve, flatCurve());
    {
        QSignalBlocker blocker(d->edgesBox);
        d->edgesBox->setValue(d->edges * 100);
    }
    refreshTiles();
}

void SkissaDetailWidget::refreshTiles()
{
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < d->presets.size(); i++) {
        KisStudioChips::Chip chip;
        chip.text = d->presets[i].name;
        chip.on = d->curve == d->presets[i].curve && std::abs(d->edges - d->presets[i].edges) < 0.005;
        const QImage picture = d->pictures.value(i);
        chip.picture = [picture](QPainter &painter, const QRectF &rect) {
            painter.save();
            painter.setClipPath(KisModernStyle::squircle(rect, KisModernStyle::cornerRadius));
            const QSizeF size = QSizeF(picture.size()).scaled(rect.size(), Qt::KeepAspectRatioByExpanding);
            painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
            painter.drawImage(QRectF(rect.center() - QPointF(size.width(), size.height()) / 2, size), picture);
            painter.restore();
        };
        chips << chip;
    }
    d->tiles->setChips(chips);
}

void SkissaDetailWidget::canvasHovered(const QPoint &)
{
}

bool SkissaDetailWidget::canvasPressed(const QPoint &pixel)
{
    if (!d->original) return false;
    // the size of feature that stands out around the pixel
    const QRect window(pixel - QPoint(256, 256), QSize(512, 512));
    const QImage around = d->original->convertToQImage(nullptr, window);
    if (around.isNull()) return false;
    const qreal x = xOfRadius(busiestRadius(around, pixel - window.topLeft()));
    d->dragPoint = d->view->pointAt(x, holdAround);
    d->dragStart = d->view->curve().points.value(d->dragPoint).y();
    d->view->setShownPoint(d->dragPoint);
    d->curve = d->view->curve();
    refreshTiles();
    return true;
}

QString SkissaDetailWidget::canvasDragged(const QPointF &moved, bool fine)
{
    if (d->dragPoint < 0) return QString();
    d->view->setPointY(d->dragPoint, d->dragStart - moved.y() * (fine ? 0.25 : 1.0) / dragForWholeCurve);
    const QPointF point = d->view->curve().points.value(d->dragPoint);
    return radiusText(radiusAt(point.x())) + QStringLiteral("  ") + gainText(gainOf(point.y()));
}

void SkissaDetailWidget::canvasReleased()
{
    d->dragPoint = -1;
    d->view->setShownPoint(-1);
}

#include "SkissaDetailWidget.moc"
