/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaDetailWidget.h"

#include <QFormLayout>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QtMath>
#include <QRandomGenerator>
#include <QToolButton>
#include <QVBoxLayout>

#include <climits>
#include <cmath>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KisValueSteps.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_icon_utils.h>
#include <kis_default_bounds_base.h>
#include <kis_paint_device.h>
#include <kis_slider_spin_box.h>
#include <klocalizedstring.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioCurve.h>

#include "SkissaDetailFilter.h"
#include "SkissaNoise.h"

using namespace SkissaDetail;

namespace
{

/// The drag on the image across which a point goes from bottom to top
constexpr qreal dragForWholeCurve = 300;
/// A point taken on the image holds the curve this far either side: one size changes alone
constexpr qreal holdAround = 0.15;
/// The part of the image the tiles show, at its own size so that fine detail shows, and the room read around it
const QSize tileSize(96, 60);
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

/// The usual part of a slider's range; Krita adds a switch to the whole range, so only one shorter than it is set
void setUsualRange(KisDoubleSliderSpinBox *box, qreal low, qreal high)
{
    if (low > box->minimum() || high < box->maximum()) box->setSoftRange(low, high);
}

KisDoubleSliderSpinBox *slider(QWidget *parent, qreal maximum, qreal softMaximum, int decimals, const QString &suffix, qreal minimum = 0)
{
    auto *box = new KisDoubleSliderSpinBox(parent);
    box->setRange(minimum, maximum, decimals);
    setUsualRange(box, minimum, softMaximum);
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

/// A tile's picture: \p picture filling it, its middle shown, or all of it with \p whole
std::function<void(QPainter &, const QRectF &)> tilePicture(const QImage &picture, bool whole = false)
{
    return [picture, whole](QPainter &painter, const QRectF &rect) {
        painter.save();
        painter.setClipPath(KisModernStyle::squircle(rect, KisModernStyle::cornerRadius));
        const QSizeF size = QSizeF(picture.size()).scaled(rect.size(), whole ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(QRectF(rect.center() - QPointF(size.width(), size.height()) / 2, size), picture);
        painter.restore();
    };
}

/// Dots from the finest features to the largest along the bottom of a curve over sizes
void paintSizes(QPainter &painter, const QRectF &box, QColor ink)
{
    ink.setAlphaF(0.22);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    for (int octave = 0; octave <= int(octaves); octave++) {
        const qreal x = box.left() + box.width() * octave / octaves;
        const qreal size = 2 + 1.4 * octave;
        painter.drawEllipse(QPointF(qBound(box.left() + size, x, box.right() - size), box.bottom() - KisModernStyle::space2 - size / 2), size / 2, size / 2);
    }
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

SkissaSliders::SkissaSliders(QWidget *parent, const QString &filterId, const QVector<SkissaSlider> &sliders)
    : KisConfigWidget(parent)
    , m_filterId(filterId)
{
    auto *layout = new QFormLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    Q_FOREACH (const SkissaSlider &spec, sliders) {
        KisDoubleSliderSpinBox *box = slider(this, spec.maximum, spec.softMaximum, spec.decimals, spec.suffix, spec.minimum);
        box->setToolTip(spec.tip);
        layout->addRow(spec.name, box);
        m_sliders << qMakePair(spec, box);
        connect(box, qOverload<double>(&KisDoubleSliderSpinBox::valueChanged), this, &KisConfigWidget::sigConfigurationItemChanged);
    }
}

void SkissaSliders::setConfiguration(const KisPropertiesConfigurationSP config)
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(m_filterId);
    KisFilterConfigurationSP defaults = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
    for (const auto &slider : m_sliders) {
        QSignalBlocker blocker(slider.second);
        const char *key = slider.first.key;
        const qreal value = config && config->hasProperty(key) ? config->getDouble(key) : defaults->getDouble(key);
        slider.second->setValue(value * slider.first.shownPerValue);
    }
}

KisPropertiesConfigurationSP SkissaSliders::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(m_filterId);
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    for (const auto &slider : m_sliders) config->setProperty(slider.first.key, slider.second->value() / slider.first.shownPerValue);
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
    d->view->backdrop = [this](QPainter &painter, const QRectF &box) { paintSizes(painter, box, palette().color(QPalette::WindowText)); };
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
        chip.picture = tilePicture(d->pictures.value(i));
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

QString SkissaDetailWidget::canvasDragged(const QPointF &moved, const QPointF &)
{
    if (d->dragPoint < 0) return QString();
    d->view->setPointY(d->dragPoint, d->dragStart - moved.y() / dragForWholeCurve);
    const QPointF point = d->view->curve().points.value(d->dragPoint);
    return radiusText(radiusAt(point.x())) + QStringLiteral("  ") + gainText(gainOf(point.y()));
}

void SkissaDetailWidget::canvasReleased()
{
    d->dragPoint = -1;
    d->view->setShownPoint(-1);
}

namespace
{

/// The drag on the image across which the noise grows or shrinks by a factor of e
constexpr qreal dragForFactor = 150;
/// Room around the tiles' part of the image, for the pixels a wobble takes from beside it
constexpr int noiseTileMargin = 32;

struct NoisePreset {
    QString name;
    SkissaNoise::Settings settings;
};

/// A curve of strengths at radii
KisStudioCurve sizesOf(std::initializer_list<std::pair<qreal, qreal>> points)
{
    KisStudioCurve curve;
    for (const auto &point : points) curve.points << QPointF(point.first < 1 ? 0.0 : xOfRadius(point.first), point.second);
    return curve;
}

SkissaNoise::Settings noiseOf(const KisStudioCurve &sizes, std::initializer_list<std::pair<SkissaNoise::Target, float>> amounts, float mids = 0)
{
    SkissaNoise::Settings settings;
    settings.sizes = sizes;
    std::fill(std::begin(settings.amount), std::end(settings.amount), 0.0f);
    for (const auto &amount : amounts) settings.amount[amount.first] = amount.second;
    settings.mids = mids;
    return settings;
}

QString targetName(int target)
{
    switch (target) {
    case SkissaNoise::Lightness: return i18nc("noise effect: what the noise changes", "Lightness");
    case SkissaNoise::Color: return i18nc("noise effect: what the noise changes", "Color");
    case SkissaNoise::Transparency: return i18nc("noise effect: what the noise changes", "Transparency");
    default: return i18nc("noise effect: what the noise changes", "Displace");
    }
}

} // namespace

struct SkissaNoiseWidget::Private {
    KisPaintDeviceSP original;
    SkissaNoise::Settings settings;
    QVector<NoisePreset> presets;
    QVector<QImage> pictures;

    KisStudioChips *tiles {nullptr};
    KisStudioCurveView *view {nullptr};
    KisDoubleSliderSpinBox *amounts[SkissaNoise::Targets] {};
    KisDoubleSliderSpinBox *mids {nullptr};

    /// The amounts when the drag on the image began
    float dragStart[SkissaNoise::Targets] {};
    bool dragging {false};

    static qreal shownPer(int target) { return target == SkissaNoise::Displace ? 1 : 100; }

    void makePresets()
    {
        using namespace SkissaNoise;
        const KisStudioCurve fine = sizesOf({{0, 1}, {1, 1}, {2, 0}, {1024, 0}});
        presets = {
            {i18nc("noise effect preset", "Grain"), noiseOf(fineSizes(), {{Lightness, 0.3f}}, 0.6f)},
            {i18nc("noise effect preset", "Color"), noiseOf(fine, {{Lightness, 0.2f}, {Color, 0.6f}})},
            {i18nc("noise effect preset", "Paper"), noiseOf(sizesOf({{0, 0.6}, {2, 1}, {8, 1}, {32, 0.5}, {128, 0}, {1024, 0}}), {{Lightness, 0.12f}})},
            {i18nc("noise effect preset", "Clouds"), noiseOf(sizesOf({{0, 0}, {16, 0}, {64, 1}, {256, 1}, {1024, 0.6}}), {{Lightness, 0.5f}})},
            {i18nc("noise effect preset", "Scatter"), noiseOf(fine, {{Displace, 2.0f}})},
            {i18nc("noise effect preset", "Wobble"), noiseOf(sizesOf({{0, 0}, {8, 0}, {16, 1}, {32, 1}, {128, 0}, {1024, 0}}), {{Displace, 4.0f}})},
        };
    }

    void makePictures()
    {
        // the middle of the layer at its own size, where grain shows as it will
        QImage base;
        QPoint corner;
        if (original) {
            const QRect bounds = original->exactBounds();
            if (!bounds.isEmpty()) {
                QRect crop(bounds.center() - QPoint(tileSize.width() / 2, tileSize.height() / 2), tileSize);
                crop = crop.adjusted(-noiseTileMargin, -noiseTileMargin, noiseTileMargin, noiseTileMargin);
                base = original->convertToQImage(nullptr, crop);
                corner = crop.topLeft();
            }
        }
        if (base.isNull()) {
            base = QImage(tileSize + QSize(2 * noiseTileMargin, 2 * noiseTileMargin), QImage::Format_ARGB32);
            base.fill(QColor(128, 128, 128));
        }
        pictures.clear();
        Q_FOREACH (const NoisePreset &preset, presets) {
            pictures << SkissaNoise::applied(base, corner, preset.settings).copy(QRect(QPoint(noiseTileMargin, noiseTileMargin), tileSize));
        }
    }
};

SkissaNoiseWidget::SkissaNoiseWidget(QWidget *parent, KisPaintDeviceSP dev)
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
        const quint32 seed = d->settings.seed;
        d->settings = d->presets[index].settings;
        d->settings.seed = seed;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->tiles);

    auto *curveRow = new QHBoxLayout();
    curveRow->setSpacing(KisModernStyle::space1);
    d->view = new KisStudioCurveView(this);
    d->view->changed = [this](const KisStudioCurve &curve) {
        d->settings.sizes = curve;
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    };
    d->view->describe = [](qreal x, qreal y) { return radiusText(radiusAt(x)) + QStringLiteral("  ") + QString::number(qRound(y * 100)) + QStringLiteral("%"); };
    d->view->backdrop = [this](QPainter &painter, const QRectF &box) { paintSizes(painter, box, palette().color(QPalette::WindowText)); };
    curveRow->addWidget(d->view, 1);
    auto *shuffle = new QToolButton(this);
    shuffle->setAutoRaise(true);
    shuffle->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_dice")));
    shuffle->setToolTip(i18nc("@info:tooltip", "Other noise like it"));
    connect(shuffle, &QToolButton::clicked, this, [this]() {
        d->settings.seed = QRandomGenerator::global()->bounded(1, 0x7fffffff);
        Q_EMIT sigConfigurationItemChanged();
    });
    curveRow->addWidget(shuffle, 0, Qt::AlignTop);
    layout->addLayout(curveRow);

    // two settings a row, so the panel stays short
    auto *rows = new QGridLayout();
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setHorizontalSpacing(KisModernStyle::space2);
    rows->setVerticalSpacing(KisModernStyle::space1);
    rows->setColumnStretch(1, 1);
    rows->setColumnStretch(3, 1);
    auto addRow = [this, rows](int index, const QString &name, QWidget *box) {
        rows->addWidget(new QLabel(name, this), index / 2, (index % 2) * 2);
        rows->addWidget(box, index / 2, (index % 2) * 2 + 1);
    };
    for (int target = 0; target < SkissaNoise::Targets; target++) {
        const bool displace = target == SkissaNoise::Displace;
        KisDoubleSliderSpinBox *box = displace ? slider(this, 200, 20, 1, i18nc("pixels, after a number", " px"))
                                               : slider(this, 400, 100, 0, QStringLiteral("%"));
        connect(box, qOverload<double>(&KisDoubleSliderSpinBox::valueChanged), this, [this, target](double value) {
            d->settings.amount[target] = float(value / Private::shownPer(target));
            refreshTiles();
            Q_EMIT sigConfigurationItemChanged();
        });
        addRow(target, targetName(target), box);
        d->amounts[target] = box;
    }
    d->mids = slider(this, 100, 100, 0, QStringLiteral("%"));
    d->mids->setToolTip(i18nc("@info:tooltip", "Grain shows most in the middle tones, as on film"));
    connect(d->mids, qOverload<double>(&KisDoubleSliderSpinBox::valueChanged), this, [this](double value) {
        d->settings.mids = float(value / 100);
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    });
    addRow(SkissaNoise::Targets, i18nc("noise effect setting", "Midtones"), d->mids);
    layout->addLayout(rows);

    refresh();
}

SkissaNoiseWidget::~SkissaNoiseWidget()
{
}

void SkissaNoiseWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? SkissaNoise::Settings::from(SkissaNoise::Kind::Noise, *config) : SkissaNoise::Settings();
    refresh();
}

KisPropertiesConfigurationSP SkissaNoiseWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_noise"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaNoiseWidget::sizeHint() const
{
    const int width = 500;
    return QSize(width, layout()->heightForWidth(width));
}

void SkissaNoiseWidget::refresh()
{
    d->view->setCurve(d->settings.sizes, SkissaNoise::fineSizes());
    for (int target = 0; target < SkissaNoise::Targets; target++) {
        QSignalBlocker blocker(d->amounts[target]);
        d->amounts[target]->setValue(d->settings.amount[target] * Private::shownPer(target));
    }
    {
        QSignalBlocker blocker(d->mids);
        d->mids->setValue(d->settings.mids * 100);
    }
    refreshTiles();
}

void SkissaNoiseWidget::refreshTiles()
{
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < d->presets.size(); i++) {
        KisStudioChips::Chip chip;
        chip.text = d->presets[i].name;
        // another throw of the same noise is still the same choice
        SkissaNoise::Settings preset = d->presets[i].settings;
        preset.seed = d->settings.seed;
        chip.on = d->settings == preset;
        chip.picture = tilePicture(d->pictures.value(i));
        chips << chip;
    }
    d->tiles->setChips(chips);
}

void SkissaNoiseWidget::canvasHovered(const QPoint &)
{
}

bool SkissaNoiseWidget::canvasPressed(const QPoint &)
{
    std::copy(std::begin(d->settings.amount), std::end(d->settings.amount), std::begin(d->dragStart));
    d->dragging = true;
    return true;
}

QString SkissaNoiseWidget::canvasDragged(const QPointF &moved, const QPointF &)
{
    if (!d->dragging) return QString();
    // up makes all of it stronger, by ratios, so a faint grain and a strong wobble grow alike
    const float factor = float(std::exp(-moved.y() / dragForFactor));
    QStringList shown;
    bool any = false;
    for (int target = 0; target < SkissaNoise::Targets; target++) {
        if (d->dragStart[target] <= 0) continue;
        any = true;
        const qreal shown100 = Private::shownPer(target);
        const qreal value = qMin(d->amounts[target]->maximum(), d->dragStart[target] * factor * shown100);
        d->amounts[target]->setValue(value);
        shown << targetName(target) + QStringLiteral(" ") + d->amounts[target]->text();
    }
    if (!any) {
        // nothing on yet: up brings the lightness in
        const qreal value = qBound(0.0, -moved.y() / 3, d->amounts[SkissaNoise::Lightness]->maximum());
        d->amounts[SkissaNoise::Lightness]->setValue(value);
        shown << targetName(SkissaNoise::Lightness) + QStringLiteral(" ") + d->amounts[SkissaNoise::Lightness]->text();
    }
    return shown.join(QStringLiteral("   "));
}

void SkissaNoiseWidget::canvasReleased()
{
    d->dragging = false;
}

namespace
{

/// The tiles' picture of the whole image, large enough for a tile on a sharp screen
const QSize lensPictureSize(2 * tileSize.width(), 2 * tileSize.height());

/// Lines a lens visibly bends and colors, for a layer with nothing on it
QImage linesPicture(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32);
    image.fill(QColor(235, 235, 235));
    QPainter painter(&image);
    painter.setPen(QPen(QColor(20, 20, 20), 1.5));
    for (int x = 8; x < size.width(); x += 16) painter.drawLine(QPointF(x, 0), QPointF(x, size.height()));
    for (int y = 8; y < size.height(); y += 16) painter.drawLine(QPointF(0, y), QPointF(size.width(), y));
    return image;
}

SkissaLens::Settings lensOf(std::initializer_list<std::pair<float SkissaLens::Settings::*, float>> values)
{
    SkissaLens::Settings settings;
    for (const auto &value : values) settings.*value.first = value.second;
    return settings;
}

} // namespace

struct SkissaLensWidget::Private {
    KisPaintDeviceSP original;
    QRect bounds;
    /// The half diagonal, in pixels: sizes are shares of it
    qreal unit {1};
    SkissaLens::Settings settings {SkissaLens::vintage()};
    QVector<QPair<QString, SkissaLens::Settings>> presets;
    QImage frame;
    qreal frameScale {1};
    QVector<QImage> pictures;
    QPointF picturesCenter {-1, -1};

    KisStudioChips *tiles {nullptr};
    KisDoubleSliderSpinBox *bulge {nullptr};
    KisDoubleSliderSpinBox *fringes {nullptr};
    KisDoubleSliderSpinBox *halo {nullptr};
    KisDoubleSliderSpinBox *vignette {nullptr};
    KisDoubleSliderSpinBox *glow {nullptr};
    KisDoubleSliderSpinBox *glowSize {nullptr};

    void makePresets()
    {
        using S = SkissaLens::Settings;
        presets = {
            {i18nc("lens effect preset", "Original"), S()},
            {i18nc("lens effect preset", "Vintage"), SkissaLens::vintage()},
            {i18nc("lens effect preset", "Vignette"), lensOf({{&S::vignette, 0.45f}})},
            {i18nc("lens effect preset", "Wide"), lensOf({{&S::bulge, 0.35f}, {&S::fringes, 0.002f}, {&S::vignette, 0.3f}})},
            {i18nc("lens effect preset", "Fisheye"), lensOf({{&S::bulge, 1.0f}, {&S::fringes, 0.003f}, {&S::vignette, 0.35f}})},
            {i18nc("lens effect preset", "Glow"), lensOf({{&S::halo, 0.001f}, {&S::glow, 0.35f}, {&S::glowSize, 0.08f}})},
        };
    }

    void makeFrame()
    {
        if (original && original->exactBounds().intersects(bounds)) {
            const qreal scale = qMin(qreal(lensPictureSize.width()) / bounds.width(), qreal(lensPictureSize.height()) / bounds.height());
            const QImage picture = original->createThumbnailUncached(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale), bounds);
            if (!picture.isNull()) {
                frame = picture;
                frameScale = qreal(picture.width()) / bounds.width();
                return;
            }
        }
        frame = linesPicture(lensPictureSize);
        frameScale = qreal(frame.width()) / bounds.width();
    }

    /// The tiles show each lens with its middle where it is now
    void makePictures()
    {
        if (picturesCenter == settings.center && pictures.size() == presets.size()) return;
        picturesCenter = settings.center;
        pictures.clear();
        for (const auto &preset : presets) {
            SkissaLens::Settings lens = preset.second;
            lens.center = settings.center;
            pictures << SkissaLens::applied(frame, lens, frameScale);
        }
    }
};

SkissaLensWidget::SkissaLensWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    if (dev) {
        // the pixels as they were before the preview changes them; their tiles are shared until then
        d->original = new KisPaintDevice(*dev);
        d->bounds = dev->defaultBounds()->bounds();
    }
    if (d->bounds.isEmpty()) d->bounds = QRect(0, 0, 1600, 1000);
    d->unit = std::hypot(d->bounds.width(), d->bounds.height()) / 2;
    d->makePresets();
    d->makeFrame();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    d->tiles = new KisStudioChips(this);
    d->tiles->setTileColumns(6, qMax(64, 2 * fontMetrics().height() + 34));
    connect(d->tiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->presets.size()) return;
        const QPointF center = d->settings.center;
        d->settings = d->presets[index].second;
        d->settings.center = center;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->tiles);

    // two settings a row, so the panel stays short
    auto *rows = new QGridLayout();
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setHorizontalSpacing(KisModernStyle::space2);
    rows->setVerticalSpacing(KisModernStyle::space1);
    rows->setColumnStretch(1, 1);
    rows->setColumnStretch(3, 1);
    int index = 0;
    const QString px = i18nc("pixels, after a number", " px");
    const QString percent = QStringLiteral("%");
    // sizes are kept as shares of the half diagonal and shown in this image's pixels
    auto add = [&](const QString &name, float SkissaLens::Settings::*value, qreal low, qreal high, qreal soft, int decimals, const QString &suffix,
                   qreal shownPer, const QString &tip = QString()) {
        auto *box = new KisDoubleSliderSpinBox(this);
        box->setRange(low * shownPer, high * shownPer, decimals);
        setUsualRange(box, low < 0 ? -soft * shownPer : low * shownPer, soft * shownPer);
        box->setSuffix(suffix);
        box->setToolTip(tip);
        connect(box, qOverload<double>(&KisDoubleSliderSpinBox::valueChanged), this, [this, value, shownPer](double shown) {
            d->settings.*value = float(shown / shownPer);
            refreshTiles();
            Q_EMIT sigConfigurationItemChanged();
        });
        rows->addWidget(new QLabel(name, this), index / 2, (index % 2) * 2);
        rows->addWidget(box, index / 2, (index % 2) * 2 + 1);
        index++;
        return box;
    };
    using S = SkissaLens::Settings;
    const int sizeDecimals = d->unit < 500 ? 1 : 0;
    d->bulge = add(i18nc("lens effect setting", "Bulge"), &S::bulge, -1, 1, 1, 0, percent, 100);
    d->vignette = add(i18nc("lens effect setting", "Vignette"), &S::vignette, 0, 0.95, 0.95, 0, percent, 100);
    d->fringes = add(i18nc("lens effect setting", "Fringes"), &S::fringes, -0.02, 0.02, 0.006, 1, px, d->unit);
    d->halo = add(i18nc("lens effect setting", "Halo"), &S::halo, -0.01, 0.01, 0.003, 1, px, d->unit,
                  i18nc("@info:tooltip", "Purple halos, green below zero"));
    d->glow = add(i18nc("lens effect setting", "Glow"), &S::glow, 0, 1, 1, 0, percent, 100);
    d->glowSize = add(i18nc("lens effect setting", "Glow size"), &S::glowSize, 0.001, 1, 0.2, sizeDecimals, px, d->unit);
    layout->addLayout(rows);

    refresh();
}

SkissaLensWidget::~SkissaLensWidget()
{
}

void SkissaLensWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? SkissaLens::Settings::from(*config) : SkissaLens::vintage();
    refresh();
}

KisPropertiesConfigurationSP SkissaLensWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_lens"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaLensWidget::sizeHint() const
{
    const int width = 500;
    return QSize(width, layout()->heightForWidth(width));
}

void SkissaLensWidget::refresh()
{
    const std::pair<KisDoubleSliderSpinBox *, qreal> shown[] = {
        {d->bulge, d->settings.bulge * 100.0},  {d->vignette, d->settings.vignette * 100.0}, {d->fringes, d->settings.fringes * d->unit},
        {d->halo, d->settings.halo * d->unit},  {d->glow, d->settings.glow * 100.0},         {d->glowSize, d->settings.glowSize * d->unit},
    };
    for (const auto &box : shown) {
        QSignalBlocker blocker(box.first);
        box.first->setValue(box.second);
    }
    refreshTiles();
}

void SkissaLensWidget::refreshTiles()
{
    d->makePictures();
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < d->presets.size(); i++) {
        KisStudioChips::Chip chip;
        chip.text = d->presets[i].first;
        chip.on = d->settings.sameLens(d->presets[i].second);
        // a lens shows most at the corners
        chip.picture = tilePicture(d->pictures.value(i), true);
        chips << chip;
    }
    d->tiles->setChips(chips);
}

void SkissaLensWidget::placeCenter(const QPointF &at)
{
    const QPointF center(qBound(0.0, (at.x() - d->bounds.left()) / d->bounds.width(), 1.0),
                         qBound(0.0, (at.y() - d->bounds.top()) / d->bounds.height(), 1.0));
    if (center == d->settings.center) return;
    d->settings.center = center;
    Q_EMIT sigConfigurationItemChanged();
}

void SkissaLensWidget::canvasHovered(const QPoint &)
{
}

bool SkissaLensWidget::canvasPressed(const QPoint &pixel)
{
    placeCenter(QPointF(pixel) + QPointF(0.5, 0.5));
    return true;
}

QString SkissaLensWidget::canvasDragged(const QPointF &, const QPointF &at)
{
    placeCenter(at);
    return QString();
}

void SkissaLensWidget::canvasReleased()
{
    refreshTiles();
}

#include "SkissaDetailWidget.moc"
