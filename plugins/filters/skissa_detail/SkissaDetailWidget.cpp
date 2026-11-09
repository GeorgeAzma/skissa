/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaDetailWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QCoreApplication>
#include <QPainter>
#include <QPointer>
#include <QThreadPool>
#include <QRandomGenerator>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <algorithm>
#include <climits>
#include <cmath>

#include <KisColorMath.h>
#include <KisEffectPanels.h>
#include <KisGlobalResourcesInterface.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KoColor.h>
#include <KoColorSet.h>
#include <KoColorSpaceRegistry.h>
#include <KisModernStyle.h>
#include <KisValueSteps.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_icon_utils.h>
#include <kis_default_bounds_base.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioColorField.h>
#include <widgets/KisStudioCurve.h>
#include <widgets/KisStudioPicker.h>
#include <widgets/KisStudioValueRow.h>

#include "KoCompositeOpInLinear.h"
#include "SkissaDetailFilter.h"
#include "SkissaFewer.h"
#include "SkissaInk.h"
#include "SkissaNoise.h"
#include "SkissaRadial.h"

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

/// A setting as one row: its name, its value as a bar and a number; right-click gives \p reset
KisStudioValueRow *valueRow(QWidget *parent, const QString &name, qreal minimum, qreal maximum, int decimals, const QString &suffix, qreal reset)
{
    auto *row = new KisStudioValueRow(name, parent);
    row->setClickSetsValue(true);
    KisStudioValueRow::Range range;
    range.minimum = minimum;
    range.maximum = maximum;
    range.decimals = decimals;
    range.suffix = suffix;
    // sizes move by ratios, as brush sizes do
    range.size = minimum > 0 && suffix.contains(QLatin1String("px"));
    row->setRange(range);
    row->setDefaultValue(reset);
    return row;
}

/// The row's name and value, as a drag on the image shows it
QString rowText(const KisStudioValueRow *row, int decimals, const QString &suffix)
{
    return row->name() + QStringLiteral(" ") + number(row->value(), decimals) + suffix;
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

using KisEffectPanels::tilePicture;

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
    if (KisFilterSP filter = KisFilterRegistry::instance()->value(filterId)) m_loaded = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1);
    Q_FOREACH (const SkissaSlider &spec, sliders) {
        const qreal reset = m_loaded ? m_loaded->getDouble(spec.key) * spec.shownPerValue : spec.minimum;
        KisStudioValueRow *row = valueRow(this, spec.name, spec.minimum, spec.maximum, spec.decimals, spec.suffix, reset);
        row->setToolTip(spec.tip);
        layout->addWidget(row);
        m_sliders << qMakePair(spec, row);
        connect(row, &KisStudioValueRow::valueChanged, this, &KisConfigWidget::sigConfigurationItemChanged);
    }
}

void SkissaSliders::setConfiguration(const KisPropertiesConfigurationSP config)
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(m_filterId);
    KisFilterConfigurationSP defaults = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
    m_loaded = config ? config : KisPropertiesConfigurationSP(defaults);
    for (const auto &slider : m_sliders) {
        const char *key = slider.first.key;
        const qreal value = config && config->hasProperty(key) ? config->getDouble(key) : defaults->getDouble(key);
        slider.second->setValue(value * slider.first.shownPerValue);
    }
}

KisPropertiesConfigurationSP SkissaSliders::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(m_filterId);
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    const QMap<QString, QVariant> kept = m_loaded->getProperties();
    for (auto it = kept.constBegin(); it != kept.constEnd(); ++it) config->setProperty(it.key(), it.value());
    for (const auto &slider : m_sliders) {
        const qreal value = slider.second->value() / slider.first.shownPerValue;
        config->setProperty(slider.first.key, value);
        if (!slider.first.follower) continue;
        const qreal was = m_loaded->getDouble(slider.first.key, value);
        const qreal followed = m_loaded->getDouble(slider.first.follower, was);
        config->setProperty(slider.first.follower, was > 0 ? value * followed / was : value);
    }
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
    KisStudioValueRow *edgesBox {nullptr};

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
                const QPoint detailed = KisEffectPanels::busiestPoint(original, bounds);
                QRect crop(detailed - QPoint(tileSize.width() / 2, tileSize.height() / 2), tileSize);
                crop = crop.adjusted(-tileMargin, -tileMargin, tileMargin, tileMargin);
                base = original->convertToQImage(nullptr, crop);
                corner = QPoint(tileMargin, tileMargin);
            }
        }
        // line art and the like show a blur or a sharpening on too little
        if (base.isNull() || KisEffectPanels::showsLittle(base.copy(QRect(corner, tileSize)))) {
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
    if (dev) d->original = KisEffectPanels::fullSizeCopy(dev);
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
        d->view->glideNextChange();
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

    d->edgesBox = valueRow(this, i18nc("effect setting", "Edges"), 0, 100, 0, QStringLiteral("%"), 0);
    d->edgesBox->setToolTip(i18nc("@info:tooltip", "Keeps edges sharp"));
    connect(d->edgesBox, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        d->edges = value / 100;
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->edgesBox);

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
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaDetailWidget::refresh()
{
    d->view->setCurve(d->curve, flatCurve());
    d->edgesBox->setValue(d->edges * 100);
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
        chip.pictureKey = d->pictures.value(i).cacheKey();
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
    KisStudioValueRow *amounts[SkissaNoise::Targets] {};
    KisStudioValueRow *mids {nullptr};

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
        if (base.isNull() || KisEffectPanels::showsLittle(base)) {
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
    if (dev) d->original = KisEffectPanels::fullSizeCopy(dev);
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
        d->view->glideNextChange();
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

    for (int target = 0; target < SkissaNoise::Targets; target++) {
        const bool displace = target == SkissaNoise::Displace;
        KisStudioValueRow *row = displace ? valueRow(this, targetName(target), 0, 50, 1, i18nc("pixels, after a number", " px"), 0)
                                          : valueRow(this, targetName(target), 0, 200, 0, QStringLiteral("%"), 0);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, target](qreal value) {
            d->settings.amount[target] = float(value / Private::shownPer(target));
            refreshTiles();
            Q_EMIT sigConfigurationItemChanged();
        });
        layout->addWidget(row);
        d->amounts[target] = row;
    }
    d->mids = valueRow(this, i18nc("noise effect setting", "Midtones"), 0, 100, 0, QStringLiteral("%"), 0);
    d->mids->setToolTip(i18nc("@info:tooltip", "Grain shows most in the middle tones, as on film"));
    connect(d->mids, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        d->settings.mids = float(value / 100);
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->mids);

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
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaNoiseWidget::refresh()
{
    d->view->setCurve(d->settings.sizes, SkissaNoise::fineSizes());
    for (int target = 0; target < SkissaNoise::Targets; target++) d->amounts[target]->setValue(d->settings.amount[target] * Private::shownPer(target));
    d->mids->setValue(d->settings.mids * 100);
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
        chip.pictureKey = d->pictures.value(i).cacheKey();
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
    auto set = [this, &shown](int target, qreal value) {
        const bool displace = target == SkissaNoise::Displace;
        value = qMin(value, displace ? 50.0 : 200.0);
        d->settings.amount[target] = float(value / Private::shownPer(target));
        d->amounts[target]->setValue(value);
        shown << rowText(d->amounts[target], displace ? 1 : 0, displace ? i18nc("pixels, after a number", " px") : QStringLiteral("%"));
    };
    for (int target = 0; target < SkissaNoise::Targets; target++) {
        if (d->dragStart[target] <= 0) continue;
        any = true;
        set(target, d->dragStart[target] * factor * Private::shownPer(target));
    }
    // nothing on yet: up brings the lightness in
    if (!any) set(SkissaNoise::Lightness, qMax(0.0, -moved.y() / 3));
    refreshTiles();
    Q_EMIT sigConfigurationItemChanged();
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
    KisStudioValueRow *bulge {nullptr};
    KisStudioValueRow *fringes {nullptr};
    KisStudioValueRow *halo {nullptr};
    KisStudioValueRow *vignette {nullptr};
    KisStudioValueRow *glow {nullptr};
    KisStudioValueRow *glowSize {nullptr};

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
            if (!picture.isNull() && !KisEffectPanels::showsLittle(picture)) {
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
        d->original = KisEffectPanels::fullSizeCopy(dev);
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

    const QString px = i18nc("pixels, after a number", " px");
    const QString percent = QStringLiteral("%");
    // sizes are kept as shares of the half diagonal and shown in this image's pixels
    auto add = [&](const QString &name, float SkissaLens::Settings::*value, qreal low, qreal high, int decimals, const QString &suffix, qreal shownPer,
                   const QString &tip = QString()) {
        KisStudioValueRow *row = valueRow(this, name, low * shownPer, high * shownPer, decimals, suffix, SkissaLens::Settings().*value * shownPer);
        row->setToolTip(tip);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, value, shownPer](qreal shown) {
            d->settings.*value = float(shown / shownPer);
            refreshTiles();
            Q_EMIT sigConfigurationItemChanged();
        });
        layout->addWidget(row);
        return row;
    };
    using S = SkissaLens::Settings;
    const int sizeDecimals = d->unit < 500 ? 1 : 0;
    d->bulge = add(i18nc("lens effect setting", "Bulge"), &S::bulge, -1, 1, 0, percent, 100);
    d->vignette = add(i18nc("lens effect setting", "Vignette"), &S::vignette, 0, 0.95, 0, percent, 100);
    d->fringes = add(i18nc("lens effect setting", "Fringes"), &S::fringes, -0.02, 0.02, 1, px, d->unit);
    d->halo = add(i18nc("lens effect setting", "Halo"), &S::halo, -0.01, 0.01, 1, px, d->unit, i18nc("@info:tooltip", "Purple halos, green below zero"));
    d->glow = add(i18nc("lens effect setting", "Glow"), &S::glow, 0, 1, 0, percent, 100);
    d->glowSize = add(i18nc("lens effect setting", "Glow size"), &S::glowSize, 0.001, 1, sizeDecimals, px, d->unit);

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
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaLensWidget::refresh()
{
    const std::pair<KisStudioValueRow *, qreal> shown[] = {
        {d->bulge, d->settings.bulge * 100.0},  {d->vignette, d->settings.vignette * 100.0}, {d->fringes, d->settings.fringes * d->unit},
        {d->halo, d->settings.halo * d->unit},  {d->glow, d->settings.glow * 100.0},         {d->glowSize, d->settings.glowSize * d->unit},
    };
    for (const auto &row : shown) row.first->setValue(row.second);
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
        chip.pictureKey = d->pictures.value(i).cacheKey();
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

bool SkissaLensWidget::canvasMiddle(QPointF *at) const
{
    *at = QPointF(d->bounds.left() + d->settings.center.x() * d->bounds.width(), d->bounds.top() + d->settings.center.y() * d->bounds.height());
    return true;
}

namespace
{

SkissaRadial::Settings radialOf(float zoom, float spin, float blur = 1, float light = 0, float threshold = 0.8f, float shafts = 0)
{
    SkissaRadial::Settings settings;
    settings.zoom = zoom;
    settings.spin = spin;
    settings.blur = blur;
    settings.light = light;
    settings.threshold = threshold;
    settings.shafts = shafts;
    return settings;
}

/// How bright \p picture's brightest parts look: the cube root of luminance that a two-hundredth of it reaches
float brightestOf(const QImage &picture)
{
    const QImage image = picture.convertToFormat(QImage::Format_ARGB32);
    const float *decode = KoMixing::srgbToLinearTable();
    std::vector<float> seen;
    seen.reserve(size_t(image.width()) * image.height());
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            if (qAlpha(line[x]) < 128) continue;
            seen.push_back(std::cbrt(0.2126f * decode[qRed(line[x])] + 0.7152f * decode[qGreen(line[x])] + 0.0722f * decode[qBlue(line[x])]));
        }
    }
    if (seen.empty()) return 1;
    auto at = seen.begin() + std::ptrdiff_t(seen.size() * 0.995);
    std::nth_element(seen.begin(), at, seen.end());
    return std::max(0.05f, *at);
}

/// The length of the sample streak drawn from the middle's side, in screen pixels
constexpr qreal sampleReach = 110;

} // namespace

struct SkissaRadialWidget::Private {
    KisPaintDeviceSP original;
    QRect bounds;
    qreal unit {1};
    SkissaRadial::Settings settings {SkissaRadial::zoomed()};
    /// How bright the layer's brightest parts look, and the tiles' picture's
    float layerTop {1};
    float frameTop {1};
    QVector<QPair<QString, SkissaRadial::Settings>> presets;
    QImage frame;
    QVector<QImage> pictures;
    SkissaRadial::Settings picturesFor;
    /// Where the sample streak drawn on the image starts
    QPointF sampleStart;

    KisStudioChips *tiles {nullptr};
    KisStudioValueRow *zoom {nullptr};
    KisStudioValueRow *spin {nullptr};
    KisStudioValueRow *picture {nullptr};
    KisStudioValueRow *light {nullptr};
    KisStudioValueRow *threshold {nullptr};
    QWidget *shaftsRow {nullptr};
    KisStudioValueRow *shafts {nullptr};

    void makePresets()
    {
        presets = {
            {i18nc("radial blur preset", "Original"), radialOf(0, 0)},
            {i18nc("radial blur preset", "Zoom"), SkissaRadial::zoomed()},
            {i18nc("radial blur preset", "Spin"), radialOf(0, 16)},
            {i18nc("radial blur preset", "Twist"), radialOf(0.2f, 24)},
            {i18nc("radial blur preset", "Light rays"), radialOf(0.9f, 0, 0, 1, 0.8f, 0.5f)},
            {i18nc("radial blur preset", "Burst"), radialOf(0.5f, 0, 1, 1, 0.7f)},
        };
    }

    void makeFrame()
    {
        if (original && original->exactBounds().intersects(bounds)) {
            const qreal scale = qMin(qreal(lensPictureSize.width()) / bounds.width(), qreal(lensPictureSize.height()) / bounds.height());
            const QImage picture = original->createThumbnailUncached(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale), bounds);
            if (!picture.isNull()) layerTop = brightestOf(picture);
            if (!picture.isNull() && !KisEffectPanels::showsLittle(picture)) {
                frame = picture;
                frameTop = layerTop;
                return;
            }
        }
        frame = KisStudioPicker::samplePicture(lensPictureSize).convertToFormat(QImage::Format_ARGB32);
        frameTop = brightestOf(frame);
    }

    /// The tiles show each look about the middle where it is now, with the beams chosen
    void makePictures()
    {
        SkissaRadial::Settings key;
        key.center = settings.center;
        key.seed = settings.seed;
        key.shafts = settings.light > 0 ? settings.shafts : 0;
        if (pictures.size() == presets.size() && picturesFor == key) return;
        picturesFor = key;
        pictures.clear();
        for (const auto &preset : presets) {
            SkissaRadial::Settings look = preset.second;
            look.center = settings.center;
            look.top = frameTop;
            look.seed = settings.seed;
            if (look.light > 0 && settings.light > 0) look.shafts = settings.shafts;
            pictures << SkissaRadial::applied(frame, look);
        }
    }

    QPointF middleInImage() const
    {
        return QPointF(bounds.left() + settings.center.x() * bounds.width(), bounds.top() + settings.center.y() * bounds.height());
    }

    /// The streak's log of scale and turn, and how much of it lies beyond the pixel, as the effect makes them
    void streakShape(double &logScale, double &turn, double &back) const
    {
        logScale = std::log(1.0 - qBound(0.0, double(settings.zoom), 0.98));
        turn = qDegreesToRadians(double(settings.spin));
        back = std::abs(turn) + std::abs(logScale) > 0 ? 0.5 * std::abs(turn) / (std::abs(turn) + std::abs(logScale)) : 0;
    }
};

SkissaRadialWidget::SkissaRadialWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    if (dev) {
        d->original = KisEffectPanels::fullSizeCopy(dev);
        d->bounds = dev->defaultBounds()->bounds();
    }
    if (d->bounds.isEmpty()) d->bounds = QRect(0, 0, 1600, 1000);
    d->unit = std::hypot(d->bounds.width(), d->bounds.height()) / 2;
    d->makePresets();
    d->makeFrame();
    d->settings.top = d->layerTop;

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    d->tiles = new KisStudioChips(this);
    d->tiles->setTileColumns(6, qMax(64, 2 * fontMetrics().height() + 34));
    connect(d->tiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->presets.size()) return;
        const SkissaRadial::Settings before = d->settings;
        d->settings = d->presets[index].second;
        d->settings.center = before.center;
        d->settings.top = before.top;
        d->settings.seed = before.seed;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->tiles);

    const QString percent = QStringLiteral("%");
    const QString degrees = QStringLiteral("°");
    auto make = [&](const QString &name, float SkissaRadial::Settings::*value, qreal low, qreal high, const QString &suffix, qreal shownPer,
                    qreal reset, const QString &tip) {
        KisStudioValueRow *row = valueRow(this, name, low * shownPer, high * shownPer, 0, suffix, reset * shownPer);
        row->setToolTip(tip);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, value, shownPer](qreal shown) {
            d->settings.*value = float(shown / shownPer);
            showLightRows();
            refreshTiles();
            Q_EMIT sigConfigurationItemChanged();
        });
        return row;
    };
    auto add = [&](const QString &name, float SkissaRadial::Settings::*value, qreal low, qreal high, const QString &suffix, qreal shownPer,
                   qreal reset, const QString &tip = QString()) {
        KisStudioValueRow *row = make(name, value, low, high, suffix, shownPer, reset, tip);
        layout->addWidget(row);
        return row;
    };
    using S = SkissaRadial::Settings;
    d->zoom = add(i18nc("radial blur setting", "Zoom"), &S::zoom, 0, 0.98, percent, 100, 0);
    d->spin = add(i18nc("radial blur setting", "Spin"), &S::spin, -180, 180, degrees, 1, 0);
    d->picture = add(i18nc("radial blur setting: how much the picture itself streaks", "Picture"), &S::blur, 0, 1, percent, 100, 1,
                     i18nc("@info:tooltip", "How much the picture itself streaks"));
    d->light = add(i18nc("radial blur setting", "Light"), &S::light, 0, 4, percent, 100, 0, i18nc("@info:tooltip", "Rays from the bright parts"));
    d->threshold = add(i18nc("radial blur setting", "Threshold"), &S::threshold, 0, 0.99, percent, 100, 0.8,
                       i18nc("@info:tooltip", "Only the brightest parts shine"));

    // beams, with the dice for others
    d->shaftsRow = new QWidget(this);
    auto *shaftsLayout = new QHBoxLayout(d->shaftsRow);
    shaftsLayout->setContentsMargins(0, 0, 0, 0);
    shaftsLayout->setSpacing(KisModernStyle::space1);
    d->shafts = make(i18nc("radial blur setting: light broken into beams", "Shafts"), &S::shafts, 0, 1, percent, 100, 0,
                     i18nc("@info:tooltip", "Light broken into beams"));
    d->shafts->setParent(d->shaftsRow);
    shaftsLayout->addWidget(d->shafts, 1);
    auto *shuffle = new QToolButton(d->shaftsRow);
    shuffle->setAutoRaise(true);
    shuffle->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_dice")));
    shuffle->setToolTip(i18nc("@info:tooltip", "Other beams"));
    connect(shuffle, &QToolButton::clicked, this, [this]() {
        d->settings.seed = QRandomGenerator::global()->bounded(1, 0x7fffffff);
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    });
    shaftsLayout->addWidget(shuffle);
    layout->addWidget(d->shaftsRow);

    refresh();
}

SkissaRadialWidget::~SkissaRadialWidget()
{
}

void SkissaRadialWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? SkissaRadial::Settings::from(*config) : SkissaRadial::zoomed();
    // the brightest parts are this layer's, not those of the layer the settings were last used on
    d->settings.top = d->layerTop;
    refresh();
}

KisPropertiesConfigurationSP SkissaRadialWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_radial"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaRadialWidget::sizeHint() const
{
    const int width = 500;
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaRadialWidget::refresh()
{
    d->zoom->setValue(d->settings.zoom * 100.0);
    d->spin->setValue(d->settings.spin);
    d->picture->setValue(d->settings.blur * 100.0);
    d->light->setValue(d->settings.light * 100.0);
    d->threshold->setValue(d->settings.threshold * 100.0);
    d->shafts->setValue(d->settings.shafts * 100.0);
    showLightRows();
    refreshTiles();
}

void SkissaRadialWidget::showLightRows()
{
    d->threshold->setVisible(d->settings.light > 0);
    d->shaftsRow->setVisible(d->settings.light > 0);
}

void SkissaRadialWidget::refreshTiles()
{
    d->makePictures();
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < d->presets.size(); i++) {
        KisStudioChips::Chip chip;
        chip.text = d->presets[i].first;
        chip.on = d->settings.sameLook(d->presets[i].second) || (i == 0 && d->settings.changesNothing());
        chip.picture = tilePicture(d->pictures.value(i), true);
        chip.pictureKey = d->pictures.value(i).cacheKey();
        chips << chip;
    }
    d->tiles->setChips(chips);
}

void SkissaRadialWidget::placeCenter(const QPointF &at)
{
    // a little past the edges too: a light just out of view still shines in
    const QPointF center(qBound(-1.0, (at.x() - d->bounds.left()) / d->bounds.width(), 2.0),
                         qBound(-1.0, (at.y() - d->bounds.top()) / d->bounds.height(), 2.0));
    if (center == d->settings.center) return;
    d->settings.center = center;
    Q_EMIT sigConfigurationItemChanged();
}

void SkissaRadialWidget::canvasHovered(const QPoint &)
{
}

bool SkissaRadialWidget::canvasPressed(const QPoint &pixel)
{
    placeCenter(QPointF(pixel) + QPointF(0.5, 0.5));
    return true;
}

QString SkissaRadialWidget::canvasDragged(const QPointF &, const QPointF &at)
{
    placeCenter(at);
    return QString();
}

void SkissaRadialWidget::canvasReleased()
{
    refreshTiles();
}

bool SkissaRadialWidget::canvasMiddle(QPointF *at) const
{
    *at = d->middleInImage();
    return true;
}

bool SkissaRadialWidget::canvasStreak(qreal pixelsPerScreen, QPolygonF *path) const
{
    // the sample starts up and to the right of the middle, turning toward the image's middle when it is far off it
    const QPointF middle = d->middleInImage();
    const qreal reach = sampleReach * pixelsPerScreen;
    QPointF toward = QRectF(d->bounds).center() - middle + QPointF(0.7, -0.7) * reach;
    const qreal length = std::hypot(toward.x(), toward.y());
    toward = length > 0 ? toward / length : QPointF(0.7071, -0.7071);
    d->sampleStart = middle + toward * reach;

    double logScale, turn, back;
    d->streakShape(logScale, turn, back);
    const QPointF from = d->sampleStart - middle;
    path->clear();
    constexpr int steps = 48;
    for (int i = 0; i <= steps; i++) {
        const double along = -back + double(i) / steps;
        const double scale = std::exp(along * logScale), angle = along * turn;
        const double c = scale * std::cos(angle), s = scale * std::sin(angle);
        *path << middle + QPointF(c * from.x() - s * from.y(), s * from.x() + c * from.y());
    }
    return true;
}

QString SkissaRadialWidget::canvasStreakDragged(const QPointF &end)
{
    // the streak's end follows the pointer: how far in is the zoom, how far around the spin, shared between the two
    // sides of a pixel as the effect shares them
    const QPointF middle = d->middleInImage();
    const QPointF from = d->sampleStart - middle, to = end - middle;
    const qreal fromLength = std::hypot(from.x(), from.y()), toLength = std::hypot(to.x(), to.y());
    if (fromLength < 1e-6 || toLength < 1e-6) return QString();
    const double inward = std::min(0.0, std::log(toLength / fromLength));
    const double around = std::remainder(std::atan2(to.y(), to.x()) - std::atan2(from.y(), from.x()), 2 * M_PI);
    const double back = std::abs(around) + std::abs(inward) > 0 ? 0.5 * std::abs(around) / (std::abs(around) + std::abs(inward)) : 0;
    d->settings.zoom = float(qBound(0.0, 1 - std::exp(inward / (1 - back)), 0.98));
    d->settings.spin = float(qBound(-180.0, qRadiansToDegrees(around / (1 - back)), 180.0));
    if (d->settings.blur == 0 && d->settings.light == 0) d->settings.blur = 1;
    d->zoom->setValue(d->settings.zoom * 100.0);
    d->spin->setValue(d->settings.spin);
    d->picture->setValue(d->settings.blur * 100.0);
    Q_EMIT sigConfigurationItemChanged();
    return QStringLiteral("%1   %2").arg(rowText(d->zoom, 0, QStringLiteral("%")), rowText(d->spin, 0, QStringLiteral("°")));
}

namespace
{

/// The drag on the image across which the number of colors grows or shrinks by a factor of e
constexpr qreal dragForCount = 150;
/// The drag on the image across which a palette's lightness moves its whole way
constexpr qreal dragForLightness = 600;

/// The most steps of each channel, and the most colors taken from the image or of gray
constexpr int mostLevels = 32;
constexpr int mostColors = 256;
/// The panel's width, unless it is shown wider
constexpr int fewerWidth = 420;

SkissaFewer::Settings fewerOf(SkissaFewer::Source source, int count, SkissaFewer::Dither dither = SkissaFewer::Dither::None)
{
    SkissaFewer::Settings settings;
    settings.source = source;
    settings.count = count;
    settings.dither = dither;
    return settings;
}

QString countText(SkissaFewer::Source source, int count)
{
    return source == SkissaFewer::Source::Levels ? i18ncp("posterize steps of each channel", "%1 level", "%1 levels", count)
                                                 : i18ncp("number of colors", "%1 color", "%1 colors", count);
}

/// A palette's colors, as a strip
QIcon stripOf(const QVector<QColor> &colors)
{
    QPixmap pixmap(64, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    const int shown = std::min(int(colors.size()), 16);
    for (int i = 0; i < shown; i++) {
        const int index = int(qint64(i) * colors.size() / shown);
        painter.fillRect(QRectF(64.0 * i / shown, 0, 64.0 / shown + 0.5, 16), colors[index]);
    }
    return QIcon(pixmap);
}

} // namespace

struct SkissaFewerWidget::Private {
    KisPaintDeviceSP original;
    SkissaFewer::Settings settings;
    /// All of the layer, small: its colors, and the pictures of where they come from
    QImage whole;
    /// The layer at the size the source tiles show it, for their pictures: tiles of that size covered by it
    QImage tileWhole;
    QSize tileCover;
    int tileHeight {64};
    QHash<int, QVector<QColor>> imageColors;
    QVector<QPair<QString, QVector<QColor>>> palettes;

    KisStudioChips *sources {nullptr};
    /// The source pictures made again once a drag pauses: three reductions of the layer at every step held it back
    QTimer tilesLater;
    /// The source each tile stands for: Levels only while the settings use it
    QVector<SkissaFewer::Source> shownSources;
    KisStudioValueRow *count {nullptr};
    QWidget *paletteRow {nullptr};
    QComboBox *palette {nullptr};
    KisStudioValueRow *brightness {nullptr};
    KisStudioChips *dithers {nullptr};
    QVector<KisStudioChips::Chip> ditherChips;
    KisStudioValueRow *amount {nullptr};

    /// The drag on the image: what it started from
    int dragCount {0};
    float dragLightness {0};
    bool dragging {false};

    QVector<QColor> colorsOfImage(int count)
    {
        auto it = imageColors.constFind(count);
        if (it != imageColors.constEnd()) return *it;
        const QVector<QColor> colors = SkissaFewer::colorsOf(whole, count);
        imageColors.insert(count, colors);
        return colors;
    }

    /// The colors the settings choose from, found again where they aren't kept
    void fillColors(SkissaFewer::Settings &s)
    {
        if (s.source == SkissaFewer::Source::Image && s.colors.isEmpty()) s.colors = colorsOfImage(s.count);
        if (s.source == SkissaFewer::Source::Palette && s.colors.isEmpty() && !palettes.isEmpty()) {
            int index = 0;
            for (int i = 0; i < palettes.size(); i++) {
                if (palettes[i].first == s.palette) index = i;
            }
            s.palette = palettes[index].first;
            s.colors = palettes[index].second;
        }
    }

    /// The settings with their colors taken from \p source instead, the count kept within what it allows
    SkissaFewer::Settings withSource(SkissaFewer::Source source)
    {
        SkissaFewer::Settings s = settings;
        if (s.source == source) return s;
        s.source = source;
        s.count = std::clamp(s.count, 2, source == SkissaFewer::Source::Levels ? mostLevels : mostColors);
        s.colors.clear();
        fillColors(s);
        return s;
    }

    void loadPalettes()
    {
        KisResourceModel model(ResourceType::Palettes);
        for (int row = 0; row < model.rowCount(); row++) {
            KoColorSetSP set = model.resourceForIndex(model.index(row, 0)).dynamicCast<KoColorSet>();
            if (!set) continue;
            QVector<QColor> colors;
            for (int y = 0; y < set->rowCount(); y++) {
                for (int x = 0; x < set->columnCount(); x++) {
                    const KisSwatch swatch = set->getColorGlobal(x, y);
                    if (swatch.isValid() && colors.size() < 0xfffe) colors << swatch.color().toQColor();
                }
            }
            if (!colors.isEmpty()) palettes << qMakePair(set->name(), colors);
        }
    }

    void makeImages()
    {
        if (original) {
            const QRect bounds = original->exactBounds();
            if (!bounds.isEmpty()) whole = original->createThumbnailUncached(256, 256, bounds);
        }
        if (whole.isNull()) {
            // a ramp of every hue and lightness, for a layer with nothing on it
            whole = QImage(tileSize, QImage::Format_ARGB32);
            for (int y = 0; y < whole.height(); y++) {
                for (int x = 0; x < whole.width(); x++) {
                    whole.setPixelColor(x, y, QColor::fromHsvF(qreal(x) / whole.width(), 0.7, 1.0 - qreal(y) / whole.height()));
                }
            }
        }
    }
};

SkissaFewerWidget::SkissaFewerWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    using namespace SkissaFewer;
    // the pixels as they were before the preview changes them; their tiles are shared until then
    if (dev) d->original = KisEffectPanels::fullSizeCopy(dev);
    d->makeImages();
    d->loadPalettes();
    d->tilesLater.setSingleShot(true);
    d->tilesLater.setInterval(60);
    connect(&d->tilesLater, &QTimer::timeout, this, &SkissaFewerWidget::refreshTiles);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    // where the colors come from, each as the layer would look
    d->sources = new KisStudioChips(this);
    connect(d->sources, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->shownSources.size() || d->shownSources[index] == d->settings.source) return;
        d->settings = d->withSource(d->shownSources[index]);
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->sources);

    d->count = new KisStudioValueRow(i18nc("fewer colors effect setting", "Colors"), this);
    d->count->setClickSetsValue(true);
    connect(d->count, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        d->settings.count = qRound(value);
        if (d->settings.source == Source::Image) d->settings.colors = d->colorsOfImage(d->settings.count);
        d->tilesLater.start();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->count);

    d->paletteRow = new QWidget(this);
    auto *paletteLayout = new QHBoxLayout(d->paletteRow);
    paletteLayout->setContentsMargins(0, 0, 0, 0);
    paletteLayout->addWidget(new QLabel(i18nc("fewer colors effect setting", "Palette"), d->paletteRow));
    d->palette = new QComboBox(d->paletteRow);
    d->palette->setIconSize(QSize(32, 8));
    for (const auto &palette : d->palettes) d->palette->addItem(stripOf(palette.second), palette.first);
    connect(d->palette, qOverload<int>(&QComboBox::activated), this, [this](int index) {
        if (index < 0 || index >= d->palettes.size()) return;
        d->settings.palette = d->palettes[index].first;
        d->settings.colors = d->palettes[index].second;
        d->tilesLater.start();
        Q_EMIT sigConfigurationItemChanged();
    });
    paletteLayout->addWidget(d->palette, 1);
    layout->addWidget(d->paletteRow);

    d->brightness = new KisStudioValueRow(i18nc("fewer colors effect setting", "Brightness"), this);
    d->brightness->setClickSetsValue(true);
    {
        KisStudioValueRow::Range range;
        range.minimum = -100;
        range.maximum = 100;
        range.suffix = QStringLiteral("%");
        d->brightness->setRange(range);
        d->brightness->setDefaultValue(0);
    }
    connect(d->brightness, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        d->settings.lightness = float(value / 200);
        d->tilesLater.start();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->brightness);

    d->dithers = new KisStudioChips(this);
    d->dithers->setTileColumns(4, qMax(40, fontMetrics().height() + 26));
    connect(d->dithers, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index > 3) return;
        d->settings.dither = Dither(index);
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->dithers);

    d->amount = new KisStudioValueRow(i18nc("dithering strength", "Amount"), this);
    d->amount->setClickSetsValue(true);
    {
        KisStudioValueRow::Range range;
        range.maximum = 100;
        range.suffix = QStringLiteral("%");
        d->amount->setRange(range);
        d->amount->setDefaultValue(100);
    }
    connect(d->amount, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        d->settings.amount = float(value / 100);
        d->tilesLater.start();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->amount);

    // each way of dithering on a ramp from black to white in two colors
    QImage ramp(64, 16, QImage::Format_ARGB32);
    for (int y = 0; y < ramp.height(); y++) {
        for (int x = 0; x < ramp.width(); x++) {
            const int v = qRound(255.0 * x / (ramp.width() - 1));
            ramp.setPixel(x, y, qRgb(v, v, v));
        }
    }
    const QString names[] = {i18nc("no dithering", "None"), i18nc("dithering by a pattern", "Pattern"), i18nc("dithering by noise", "Noise"),
                             i18nc("error diffusion dithering", "Diffuse")};
    for (int i = 0; i < 4; i++) {
        KisStudioChips::Chip chip;
        chip.text = names[i];
        chip.picture = tilePicture(SkissaFewer::applied(ramp, QPoint(), fewerOf(Source::Gray, 2, Dither(i))), true, true);
        d->ditherChips << chip;
    }

    refresh();
}

SkissaFewerWidget::~SkissaFewerWidget()
{
}

void SkissaFewerWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? SkissaFewer::Settings::from(*config) : SkissaFewer::Settings();
    d->fillColors(d->settings);
    refresh();
}

KisPropertiesConfigurationSP SkissaFewerWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_fewer_colors"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaFewerWidget::sizeHint() const
{
    const int width = fewerWidth;
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaFewerWidget::refresh()
{
    using namespace SkissaFewer;
    const Settings &s = d->settings;
    d->shownSources = {Source::Image, Source::Gray};
    if (!d->palettes.isEmpty()) d->shownSources << Source::Palette;
    if (s.source == Source::Levels) d->shownSources << Source::Levels;
    d->tileHeight = qMax(64, 2 * fontMetrics().height() + 34);
    d->sources->setTileColumns(d->shownSources.size(), d->tileHeight);

    const bool palette = s.source == Source::Palette;
    d->count->setName(s.source == Source::Levels ? i18nc("fewer colors effect setting: steps of each channel", "Levels")
                                                 : i18nc("fewer colors effect setting", "Colors"));
    d->count->setVisible(!palette);
    d->paletteRow->setVisible(palette);
    {
        KisStudioValueRow::Range range;
        range.minimum = 2;
        range.maximum = s.source == Source::Levels ? mostLevels : mostColors;
        range.size = true;
        d->count->setRange(range);
        d->count->setDefaultValue(s.source == Source::Levels ? 4 : 8);
        d->count->setValue(s.count);
    }
    {
        QSignalBlocker blocker(d->palette);
        const int index = d->palette->findText(s.palette);
        if (index >= 0) d->palette->setCurrentIndex(index);
    }
    d->brightness->setValue(s.lightness * 200);
    d->amount->setValue(s.amount * 100);
    d->amount->setVisible(s.dither != Dither::None);
    for (int i = 0; i < d->ditherChips.size(); i++) d->ditherChips[i].on = int(s.dither) == i;
    d->dithers->setChips(d->ditherChips);
    refreshTiles();
}

void SkissaFewerWidget::refreshTiles()
{
    using namespace SkissaFewer;
    // the pictures made at the size they are shown: a larger one is only scaled down
    const qreal across = qreal(qMax(d->sources->width(), fewerWidth)) / qMax(1, d->shownSources.size());
    const QSize cover = QSizeF(across, d->tileHeight).toSize() * devicePixelRatioF();
    if (d->tileWhole.isNull() || d->tileCover != cover) {
        d->tileCover = cover;
        const QSize size = d->whole.size().scaled(cover, Qt::KeepAspectRatioByExpanding);
        d->tileWhole = size.width() < d->whole.width() ? d->whole.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation) : d->whole;
    }
    QVector<KisStudioChips::Chip> chips;
    Q_FOREACH (Source source, d->shownSources) {
        KisStudioChips::Chip chip;
        switch (source) {
        case Source::Image: chip.text = i18nc("colors taken from", "Picture"); break;
        case Source::Gray: chip.text = i18nc("colors taken from", "Gray"); break;
        case Source::Palette: chip.text = i18nc("colors taken from", "Palette"); break;
        case Source::Levels: chip.text = i18nc("colors taken from: steps of each channel", "Levels"); break;
        }
        chip.on = source == d->settings.source;
        // the whole layer, small: where the colors come from shows in all of it
        chip.picture = tilePicture(SkissaFewer::applied(d->tileWhole, QPoint(), d->withSource(source)));
        chips << chip;
    }
    d->sources->setChips(chips);
}

void SkissaFewerWidget::canvasHovered(const QPoint &)
{
}

bool SkissaFewerWidget::canvasPressed(const QPoint &)
{
    d->dragCount = d->settings.count;
    d->dragLightness = d->settings.lightness;
    d->dragging = true;
    return true;
}

QString SkissaFewerWidget::canvasDragged(const QPointF &moved, const QPointF &)
{
    if (!d->dragging) return QString();
    // sideways lighter or darker
    d->settings.lightness = float(qBound(-0.5, d->dragLightness + moved.x() / dragForLightness, 0.5));
    d->brightness->setValue(d->settings.lightness * 200);
    const QString brightness = i18nc("fewer colors effect setting", "Brightness") + QStringLiteral(" %1%").arg(qRound(d->settings.lightness * 200));
    if (d->settings.source != SkissaFewer::Source::Palette) {
        // up more colors, by ratios, so few change slowly and many quickly
        const int most = d->settings.source == SkissaFewer::Source::Levels ? mostLevels : mostColors;
        d->settings.count = std::clamp(int(qRound(d->dragCount * std::exp(-moved.y() / dragForCount))), 2, most);
        if (d->settings.source == SkissaFewer::Source::Image) d->settings.colors = d->colorsOfImage(d->settings.count);
        d->count->setValue(d->settings.count);
    }
    d->tilesLater.start();
    Q_EMIT sigConfigurationItemChanged();
    if (d->settings.source == SkissaFewer::Source::Palette || std::abs(moved.x()) > std::abs(moved.y())) return brightness;
    return countText(d->settings.source, d->settings.count);
}

void SkissaFewerWidget::canvasReleased()
{
    d->dragging = false;
}

namespace
{

using Shading = SkissaInk::Shading;

/// A look: what is drawn, how its shading is drawn (with that way's own setting), where it goes
SkissaInk::Settings drawingOf(float size, float lines, float shaded, float flow, Shading shading, float setting = 0,
                              SkissaInk::Colors colors = SkissaInk::Colors::Paper)
{
    SkissaInk::Settings settings;
    settings.size = size;
    settings.lines = lines;
    settings.ink = shaded;
    settings.flow = flow;
    settings.shading = shading;
    settings.colors = colors;
    switch (shading) {
    case Shading::Solid: settings.steps = setting > 0 ? int(setting) : settings.steps; break;
    case Shading::Soft: settings.softness = setting > 0 ? setting : settings.softness; break;
    case Shading::Hatch: settings.spacing = setting > 0 ? setting : settings.spacing; break;
    case Shading::Strokes: settings.length = setting > 0 ? setting : settings.length; break;
    case Shading::Dots: settings.dot = setting > 0 ? setting : settings.dot; break;
    }
    return settings;
}

/// Each pixel's Oklab lightness as on white paper, sorted: how much of \p picture is darker than a lightness
std::vector<float> lightnessOf(const QImage &picture)
{
    const QImage image = picture.convertToFormat(QImage::Format_ARGB32);
    const float *decode = KoMixing::srgbToLinearTable();
    std::vector<float> seen;
    seen.reserve(size_t(image.width()) * image.height());
    for (int y = 0; y < image.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < image.width(); x++) {
            const float alpha = qAlpha(line[x]) / 255.0f;
            const float r = decode[qRed(line[x])] * alpha + 1 - alpha;
            const float g = decode[qGreen(line[x])] * alpha + 1 - alpha;
            const float b = decode[qBlue(line[x])] * alpha + 1 - alpha;
            seen.push_back(float(KisColorMath::oklabFromLinear(r, g, b).l));
        }
    }
    std::sort(seen.begin(), seen.end());
    return seen;
}

/// The lightness below which \p share of a picture lies (its sorted lightness \p sorted); none for no shading
float levelOf(const std::vector<float> &sorted, float share)
{
    if (share <= 0) return 0;
    if (sorted.empty()) return -1;
    if (share >= 1) return sorted.back() + 0.01f;
    return sorted[size_t(share * (sorted.size() - 1))];
}

/// A tile's picture, and the scale it shows the layer at: large enough that lines show as they will, small enough
/// that a tile holds more than one thing
const QSize drawingTileSize(120, 75);
constexpr qreal drawingTileScale = 0.5;

} // namespace

struct SkissaInkWidget::Private {
    KisPaintDeviceSP original;
    SkissaInk::Settings settings {SkissaInk::inked()};
    /// The layer's lightness, sorted, and the tiles' picture's
    std::vector<float> layerLightness;
    std::vector<float> baseLightness;
    QVector<QPair<QString, SkissaInk::Settings>> looks;
    QVector<QPair<QString, Shading>> shadings;
    QVector<QPair<QString, SkissaInk::Colors>> colorings;
    /// The tiles' part of the layer, with room around it for what the looks read
    QImage base;
    QPoint corner;
    QVector<QImage> lookPictures;
    QVector<QImage> shadingPictures;
    QVector<QImage> colorPictures;
    /// Pictures are made on another thread after a pause; only the latest asked for are shown (a widget gone by then
    /// drops them)
    QTimer *later {nullptr};
    int generation {0};

    KisStudioChips *lookTiles {nullptr};
    KisStudioChips *shadingTiles {nullptr};
    KisStudioChips *colorTiles {nullptr};
    KisStudioValueRow *size {nullptr};
    KisStudioValueRow *lines {nullptr};
    KisStudioValueRow *shaded {nullptr};
    KisStudioValueRow *flow {nullptr};
    KisStudioValueRow *steps {nullptr};
    KisStudioValueRow *softness {nullptr};
    KisStudioValueRow *spacing {nullptr};
    KisStudioValueRow *angle {nullptr};
    KisStudioValueRow *length {nullptr};
    KisStudioValueRow *dot {nullptr};
    QWidget *inkColorRow {nullptr};
    QWidget *paperColorRow {nullptr};
    KisStudioColorField *inkColor {nullptr};
    KisStudioColorField *paperColor {nullptr};

    float startShaded {0};
    float startSize {1};

    void makeLooks()
    {
        using C = SkissaInk::Colors;
        looks = {
            {i18nc("drawing effect look", "Original"), drawingOf(1.5f, 0, 0, 0, Shading::Solid, 0, C::Over)},
            {i18nc("drawing effect look", "Lines"), drawingOf(1.0f, 0.7f, 0, 0.5f, Shading::Solid)},
            {i18nc("drawing effect look", "Ink"), SkissaInk::inked()},
            {i18nc("drawing effect look", "Pencil"), drawingOf(0.8f, 0.4f, 0.7f, 0.3f, Shading::Soft, 0.5f)},
            {i18nc("drawing effect look", "Charcoal"), drawingOf(2.5f, 0.3f, 0.6f, 0.8f, Shading::Soft, 0.45f)},
            {i18nc("drawing effect look", "Hatching"), drawingOf(1.2f, 0.5f, 0.9f, 0, Shading::Hatch, 8)},
            {i18nc("drawing effect look: fine strokes following the shapes", "Etching"), drawingOf(1.0f, 0.4f, 1.0f, 0.8f, Shading::Strokes, 8)},
            {i18nc("drawing effect look: drawn in dots", "Stipple"), drawingOf(1.0f, 0.5f, 0.85f, 0.3f, Shading::Dots, 2.5f)},
            {i18nc("drawing effect look", "Woodcut"), drawingOf(2.0f, 0.8f, 0.45f, 1, Shading::Solid)},
            {i18nc("drawing effect look", "Comic"), drawingOf(1.2f, 0.6f, 0.15f, 0.5f, Shading::Solid, 0, C::Over)},
            {i18nc("drawing effect look: colored pencils", "Crayon"), drawingOf(1.0f, 0.4f, 1.0f, 0.3f, Shading::Hatch, 5, C::Colored)},
            {i18nc("drawing effect look", "Wash"), drawingOf(2.0f, 0.3f, 0.08f, 1, Shading::Soft, 0.4f, C::Over)},
        };
        shadings = {
            {i18nc("drawing effect: shading as flat ink", "Solid"), Shading::Solid},
            {i18nc("drawing effect: shading as soft gray", "Soft"), Shading::Soft},
            {i18nc("drawing effect: shading as straight strokes", "Hatch"), Shading::Hatch},
            {i18nc("drawing effect: shading as strokes along the shapes", "Strokes"), Shading::Strokes},
            {i18nc("drawing effect: shading as dots", "Dots"), Shading::Dots},
        };
        colorings = {
            {i18nc("drawing effect: ink on paper", "Paper"), C::Paper},
            {i18nc("drawing effect: ink alone, see-through between", "Clear"), C::Clear},
            {i18nc("drawing effect: ink over the picture", "Picture"), C::Over},
            {i18nc("drawing effect: the picture's own colors as ink", "Colors"), C::Colored},
        };
    }

    void makeBase()
    {
        int margin = 0;
        for (const auto &look : looks) margin = std::max(margin, SkissaInk::Ink(look.second, drawingTileScale).reach());
        margin = std::min(margin + 4, 64);
        const QSize whole = drawingTileSize + QSize(2 * margin, 2 * margin);
        if (original) {
            const QRect bounds = original->exactBounds();
            if (!bounds.isEmpty()) {
                const qreal scale = qMin(1.0, 256.0 / qMax(bounds.width(), bounds.height()));
                layerLightness = lightnessOf(original->createThumbnailUncached(qCeil(bounds.width() * scale), qCeil(bounds.height() * scale), bounds));
                const QPoint busiest = KisEffectPanels::busiestPoint(original, bounds);
                const QSize read = whole / drawingTileScale;
                const QRect crop(busiest - QPoint(read.width() / 2, read.height() / 2), read);
                base = original->convertToQImage(nullptr, crop).scaled(whole, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                corner = QPoint(margin, margin);
                baseLightness = layerLightness;
            }
        }
        if (base.isNull() || KisEffectPanels::showsLittle(base.copy(QRect(corner, drawingTileSize)))) {
            base = KisStudioPicker::samplePicture(whole * 3).convertToFormat(QImage::Format_ARGB32).copy(QRect(QPoint(whole.width(), whole.height()), whole));
            corner = QPoint(margin, margin);
            baseLightness = lightnessOf(base);
        }
    }

    /// \p look as the layer has it: its share of shading at the layer's own lightness
    SkissaInk::Settings onLayer(SkissaInk::Settings look) const
    {
        look.level = levelOf(layerLightness, look.ink);
        return look;
    }
};

SkissaInkWidget::SkissaInkWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    if (dev) d->original = KisEffectPanels::fullSizeCopy(dev);
    d->makeLooks();
    d->makeBase();
    d->settings = d->onLayer(d->settings);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    const int tileHeight = qMax(64, 2 * fontMetrics().height() + 34);
    d->lookTiles = new KisStudioChips(this);
    d->lookTiles->setTileColumns(6, tileHeight);
    connect(d->lookTiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->looks.size()) return;
        const SkissaInk::Settings before = d->settings;
        d->settings = d->onLayer(d->looks[index].second);
        d->settings.inkColor = before.inkColor;
        d->settings.paperColor = before.paperColor;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->lookTiles);

    const QString percent = QStringLiteral("%");
    const QString px = i18nc("pixels, after a number", " px");
    auto add = [&](const QString &name, qreal low, qreal high, int decimals, const QString &suffix, qreal reset, std::function<void(qreal)> write,
                   const QString &tip = QString()) {
        KisStudioValueRow *row = valueRow(this, name, low, high, decimals, suffix, reset);
        row->setToolTip(tip);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, write](qreal value) {
            write(value);
            showRows();
            refreshTiles();
            Q_EMIT sigConfigurationItemChanged();
        });
        layout->addWidget(row);
        return row;
    };
    const SkissaInk::Settings defaults = SkissaInk::inked();

    // what is drawn
    d->size = add(i18nc("drawing effect setting", "Size"), 0.3, 40, 1, px, defaults.size, [this](qreal v) { d->settings.size = float(v); },
                  i18nc("@info:tooltip", "How fine or bold the lines are"));
    d->lines = add(i18nc("drawing effect setting", "Lines"), 0, 100, 0, percent, defaults.lines * 100,
                   [this](qreal v) { d->settings.lines = float(v / 100); });
    d->shaded = add(i18nc("drawing effect setting: how much of the picture is shaded", "Shading"), 0, 100, 0, percent, defaults.ink * 100,
                    [this](qreal v) {
                        d->settings.ink = float(v / 100);
                        d->settings.level = levelOf(d->layerLightness, d->settings.ink);
                    },
                    i18nc("@info:tooltip", "How much of the picture is shaded, darkest first"));
    d->flow = add(i18nc("drawing effect setting", "Flow"), 0, 100, 0, percent, defaults.flow * 100, [this](qreal v) { d->settings.flow = float(v / 100); },
                  i18nc("@info:tooltip", "How much lines follow the shapes"));

    // how the shading is drawn, each way with its own setting
    d->shadingTiles = new KisStudioChips(this);
    d->shadingTiles->setTileColumns(5, tileHeight);
    connect(d->shadingTiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->shadings.size()) return;
        d->settings.shading = d->shadings[index].second;
        showRows();
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->shadingTiles);
    d->steps = add(i18nc("drawing effect setting: steps of darkness", "Steps"), 1, 8, 0, QString(), defaults.steps,
                   [this](qreal v) { d->settings.steps = qRound(v); });
    d->softness = add(i18nc("drawing effect setting", "Softness"), 0, 100, 0, percent, defaults.softness * 100,
                      [this](qreal v) { d->settings.softness = float(v / 100); });
    d->spacing = add(i18nc("drawing effect setting: between hatch strokes", "Spacing"), 2, 60, 1, px, defaults.spacing,
                     [this](qreal v) { d->settings.spacing = float(v); });
    d->angle = add(i18nc("drawing effect setting: of the hatch strokes", "Angle"), -90, 90, 0, QStringLiteral("°"), defaults.angle,
                   [this](qreal v) { d->settings.angle = float(v); });
    d->length = add(i18nc("drawing effect setting: of the strokes", "Length"), 2, 60, 1, px, defaults.length,
                    [this](qreal v) { d->settings.length = float(v); });
    d->dot = add(i18nc("drawing effect setting: how wide the dots are", "Dot size"), 1, 30, 1, px, defaults.dot,
                 [this](qreal v) { d->settings.dot = float(v); });

    // where the ink goes
    d->colorTiles = new KisStudioChips(this);
    d->colorTiles->setTileColumns(4, tileHeight);
    connect(d->colorTiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->colorings.size()) return;
        d->settings.colors = d->colorings[index].second;
        showRows();
        refreshTiles();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->colorTiles);

    // the ink's and the paper's colors side by side, each shown while it is used
    auto *colors = new QWidget(this);
    auto *colorsLayout = new QHBoxLayout(colors);
    colorsLayout->setContentsMargins(0, 0, 0, 0);
    colorsLayout->setSpacing(KisModernStyle::space4);
    auto colorPart = [&](const QString &name, const QColor &color, const QColor &reset, KisStudioColorField **field,
                         std::function<void(const QColor &)> write) {
        auto *part = new QWidget(colors);
        auto *partLayout = new QHBoxLayout(part);
        partLayout->setContentsMargins(0, 0, 0, 0);
        partLayout->addWidget(new QLabel(name, part), 1);
        *field = new KisStudioColorField(part);
        (*field)->setColor(color);
        (*field)->setDefaultColor(reset);
        (*field)->setAlphaEnabled(true);
        // the eyedropper takes the layer's own colors, as they were before the effect
        (*field)->setSampler([this](const QPoint &pixel, QColor *taken) {
            if (!d->original || !d->original->defaultBounds()->bounds().contains(pixel)) return false;
            KoColor color;
            d->original->pixel(pixel.x(), pixel.y(), &color);
            *taken = color.toQColor();
            return true;
        });
        partLayout->addWidget(*field);
        connect(*field, &KisStudioColorField::colorChanged, this, [this, write](const QColor &picked) {
            write(picked);
            refreshTiles();
            Q_EMIT sigConfigurationItemChanged();
        });
        colorsLayout->addWidget(part, 1);
        return part;
    };
    d->inkColorRow = colorPart(i18nc("drawing effect: the color of the ink", "Ink"), d->settings.inkColor, defaults.inkColor, &d->inkColor,
                               [this](const QColor &c) { d->settings.inkColor = c; });
    d->paperColorRow = colorPart(i18nc("drawing effect: the color of the paper", "Paper"), d->settings.paperColor, defaults.paperColor, &d->paperColor,
                                 [this](const QColor &c) { d->settings.paperColor = c; });
    layout->addWidget(colors);

    d->later = new QTimer(this);
    d->later->setSingleShot(true);
    d->later->setInterval(120);
    connect(d->later, &QTimer::timeout, this, &SkissaInkWidget::makePictures);

    refresh();
    makePictures();
}

SkissaInkWidget::~SkissaInkWidget()
{
}

void SkissaInkWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    // the share of shading is read on this layer, not the one the settings were last used on
    d->settings = d->onLayer(config ? SkissaInk::Settings::from(*config) : SkissaInk::inked());
    refresh();
}

KisPropertiesConfigurationSP SkissaInkWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_ink"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaInkWidget::sizeHint() const
{
    const int width = 500;
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaInkWidget::refresh()
{
    const SkissaInk::Settings &s = d->settings;
    d->size->setValue(s.size);
    d->lines->setValue(s.lines * 100.0);
    d->shaded->setValue(s.ink * 100.0);
    d->flow->setValue(s.flow * 100.0);
    d->steps->setValue(s.steps);
    d->softness->setValue(s.softness * 100.0);
    d->spacing->setValue(s.spacing);
    d->angle->setValue(s.angle);
    d->length->setValue(s.length);
    d->dot->setValue(s.dot);
    d->inkColor->setColor(s.inkColor);
    d->paperColor->setColor(s.paperColor);
    showRows();
    refreshTiles();
}

void SkissaInkWidget::showRows()
{
    // the four rows of what is drawn always show; how the shading is drawn while there is shading, with only its own
    // way's setting; where the ink goes while anything is drawn, with each color where it shows
    const SkissaInk::Settings &s = d->settings;
    const bool draws = s.lines > 0 || s.ink > 0;
    const bool shades = s.ink > 0;
    d->shadingTiles->setVisible(shades);
    d->steps->setVisible(shades && s.shading == Shading::Solid);
    d->softness->setVisible(shades && s.shading == Shading::Soft);
    d->spacing->setVisible(shades && s.shading == Shading::Hatch);
    d->angle->setVisible(shades && s.shading == Shading::Hatch);
    d->length->setVisible(shades && s.shading == Shading::Strokes);
    d->dot->setVisible(shades && s.shading == Shading::Dots);
    d->colorTiles->setVisible(draws);
    d->inkColorRow->setVisible(draws && s.colors != SkissaInk::Colors::Colored);
    d->paperColorRow->setVisible(draws && (s.colors == SkissaInk::Colors::Paper || s.colors == SkissaInk::Colors::Colored));
    d->inkColorRow->parentWidget()->setVisible(!d->inkColorRow->isHidden() || !d->paperColorRow->isHidden());
}

void SkissaInkWidget::refreshTiles()
{
    showTiles();
    d->later->start();
}

void SkissaInkWidget::showTiles()
{
    auto chipsOf = [](const QVector<QString> &names, const QVector<QImage> &pictures, std::function<bool(int)> on) {
        QVector<KisStudioChips::Chip> chips;
        for (int i = 0; i < names.size(); i++) {
            KisStudioChips::Chip chip;
            chip.text = names[i];
            chip.on = on(i);
            chip.picture = tilePicture(pictures.value(i));
            chip.pictureKey = pictures.value(i).cacheKey();
            chips << chip;
        }
        return chips;
    };
    QVector<QString> names;
    for (const auto &look : d->looks) names << look.first;
    d->lookTiles->setChips(chipsOf(names, d->lookPictures, [this](int i) {
        return d->settings.sameLook(d->looks[i].second) || (i == 0 && d->settings.changesNothing());
    }));
    names.clear();
    for (const auto &shading : d->shadings) names << shading.first;
    d->shadingTiles->setChips(chipsOf(names, d->shadingPictures, [this](int i) { return d->settings.shading == d->shadings[i].second; }));
    names.clear();
    for (const auto &coloring : d->colorings) names << coloring.first;
    d->colorTiles->setChips(chipsOf(names, d->colorPictures, [this](int i) { return d->settings.colors == d->colorings[i].second; }));
}

void SkissaInkWidget::makePictures()
{
    // the looks with the colors chosen; the drawing as it is with each way of shading, then in each place; made away
    // from the window's thread
    QVector<SkissaInk::Settings> asked;
    for (const auto &look : d->looks) {
        SkissaInk::Settings s = look.second;
        s.inkColor = d->settings.inkColor;
        s.paperColor = d->settings.paperColor;
        asked << s;
    }
    for (const auto &shading : d->shadings) {
        SkissaInk::Settings s = d->settings;
        s.shading = shading.second;
        asked << s;
    }
    for (const auto &coloring : d->colorings) {
        SkissaInk::Settings s = d->settings;
        s.colors = coloring.second;
        asked << s;
    }
    for (SkissaInk::Settings &s : asked) s.level = levelOf(d->baseLightness, s.ink);
    const int generation = ++d->generation;
    const QImage base = d->base;
    const QRect shown(d->corner, drawingTileSize);
    const int looks = d->looks.size(), shadings = d->shadings.size();
    QPointer<SkissaInkWidget> self(this);
    QThreadPool::globalInstance()->start([self, generation, base, shown, asked, looks, shadings]() {
        QVector<QImage> pictures;
        for (const SkissaInk::Settings &s : asked) pictures << SkissaInk::applied(base, s, drawingTileScale).copy(shown);
        QMetaObject::invokeMethod(qApp, [self, generation, pictures, looks, shadings]() {
            if (!self || self->d->generation != generation) return;
            self->d->lookPictures = pictures.mid(0, looks);
            self->d->shadingPictures = pictures.mid(looks, shadings);
            self->d->colorPictures = pictures.mid(looks + shadings);
            self->showTiles();
        }, Qt::QueuedConnection);
    });
}

void SkissaInkWidget::canvasHovered(const QPoint &)
{
}

bool SkissaInkWidget::canvasPressed(const QPoint &)
{
    d->startShaded = d->settings.ink;
    d->startSize = d->settings.size;
    return true;
}

QString SkissaInkWidget::canvasDragged(const QPointF &moved, const QPointF &)
{
    // up for more shading, sideways for bolder or finer lines, by ratios as brush sizes go
    d->settings.ink = float(qBound(0.0, d->startShaded - moved.y() / 300.0, 1.0));
    d->settings.level = levelOf(d->layerLightness, d->settings.ink);
    d->settings.size = float(qBound(0.3, d->startSize * std::exp(moved.x() / 250.0), 40.0));
    d->shaded->setValue(d->settings.ink * 100.0);
    d->size->setValue(d->settings.size);
    Q_EMIT sigConfigurationItemChanged();
    return QStringLiteral("%1   %2").arg(rowText(d->shaded, 0, QStringLiteral("%")), rowText(d->size, 1, i18nc("pixels, after a number", " px")));
}

void SkissaInkWidget::canvasReleased()
{
    refreshTiles();
}
