/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaDetailWidget.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QComboBox>
#include <QPainter>
#include <QRandomGenerator>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <climits>
#include <cmath>

#include <KisEffectPanels.h>
#include <KisGlobalResourcesInterface.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KoColorSet.h>
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
#include <widgets/KisStudioCurve.h>
#include <widgets/KisStudioValueRow.h>

#include "SkissaDetailFilter.h"
#include "SkissaFewer.h"
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

namespace
{

/// The drag on the image across which the number of colors grows or shrinks by a factor of e
constexpr qreal dragForCount = 150;
/// The drag on the image across which a palette's lightness moves its whole way
constexpr qreal dragForLightness = 600;

/// The most steps of each channel, and the most colors taken from the image or of gray
constexpr int mostLevels = 32;
constexpr int mostColors = 256;

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
    QHash<int, QVector<QColor>> imageColors;
    QVector<QPair<QString, QVector<QColor>>> palettes;

    KisStudioChips *sources {nullptr};
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
        refreshTiles();
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
        refreshTiles();
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
        refreshTiles();
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
        refreshTiles();
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
    const int width = 420;
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaFewerWidget::refresh()
{
    using namespace SkissaFewer;
    const Settings &s = d->settings;
    d->shownSources = {Source::Image, Source::Gray};
    if (!d->palettes.isEmpty()) d->shownSources << Source::Palette;
    if (s.source == Source::Levels) d->shownSources << Source::Levels;
    d->sources->setTileColumns(d->shownSources.size(), qMax(64, 2 * fontMetrics().height() + 34));

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
        chip.picture = tilePicture(SkissaFewer::applied(d->whole, QPoint(), d->withSource(source)));
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
    refreshTiles();
    Q_EMIT sigConfigurationItemChanged();
    if (d->settings.source == SkissaFewer::Source::Palette || std::abs(moved.x()) > std::abs(moved.y())) return brightness;
    return countText(d->settings.source, d->settings.count);
}

void SkissaFewerWidget::canvasReleased()
{
    d->dragging = false;
}
