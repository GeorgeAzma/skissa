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

#include <array>

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
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KisModernStyle.h>
#include <KisValueSteps.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_icon_utils.h>
#include <kis_default_bounds_base.h>
#include <kis_paint_device.h>
#include <kis_sequential_iterator.h>
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
#include "SkissaLight.h"
#include "SkissaNoise.h"
#include "SkissaPaint.h"
#include "SkissaRadial.h"
#include "SkissaShape.h"

using namespace SkissaDetail;

namespace
{

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

/// \p row's angle shown as a dial, pointing \p turn degrees further than the value
void asDial(KisStudioValueRow *row, qreal turn = 0)
{
    KisStudioValueRow::Range range = row->range();
    range.angle = true;
    range.dialTurn = turn;
    row->setRange(range);
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
    // up or down the amount, sideways the size; an effect with one size has it up or down
    for (int i = 0; i < m_sliders.size(); i++) {
        if (m_sliders[i].second->range().size) {
            if (m_horizontal < 0) m_horizontal = i;
        } else if (m_vertical < 0 && qstrcmp(m_sliders[i].first.key, "amount") == 0) {
            m_vertical = i;
        }
    }
    if (m_vertical < 0) {
        m_vertical = m_horizontal >= 0 ? m_horizontal : 0;
        m_horizontal = -1;
    }
}

bool SkissaSliders::canvasPressed(const QPoint &)
{
    if (m_vertical >= 0) m_startVertical = m_sliders[m_vertical].second->value();
    if (m_horizontal >= 0) m_startHorizontal = m_sliders[m_horizontal].second->value();
    return m_vertical >= 0;
}

QString SkissaSliders::canvasDragged(const QPointF &moved, const QPointF &)
{
    // sizes by ratios, the rest across their range
    auto drag = [this](int index, qreal start, qreal by) {
        KisStudioValueRow *row = m_sliders[index].second;
        const KisStudioValueRow::Range range = row->range();
        const qreal value = KisValueSteps::draggedBy(start, by, range.maximum - range.minimum, range.size, range.minimum);
        row->setValue(qBound(range.minimum, value, range.maximum));
    };
    drag(m_vertical, m_startVertical, -moved.y());
    if (m_horizontal >= 0) drag(m_horizontal, m_startHorizontal, moved.x());
    Q_EMIT sigConfigurationItemChanged();
    const bool across = m_horizontal >= 0 && std::abs(moved.x()) > std::abs(moved.y());
    const KisStudioValueRow *row = m_sliders[across ? m_horizontal : m_vertical].second;
    return rowText(row, row->range().decimals, row->range().suffix);
}

void SkissaSliders::canvasReleased()
{
}

Qt::Orientations SkissaSliders::canvasAxes() const
{
    return m_horizontal >= 0 ? Qt::Horizontal | Qt::Vertical : Qt::Vertical;
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
    d->view->setPointY(d->dragPoint, KisValueSteps::draggedBy(d->dragStart, -moved.y(), 1, false));
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
    const float factor = float(KisValueSteps::draggedBy(1, -moved.y(), 0, true));
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
    if (!any) set(SkissaNoise::Lightness, qMax(0.0, KisValueSteps::draggedBy(0, -moved.y(), 100, false)));
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
            // the glow's size only while there is a glow
            d->glowSize->setVisible(d->settings.glow > 0);
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
    d->glowSize->setVisible(d->settings.glow > 0);
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
    /// The layer's width, for pixel art's cells at the size of the pictures
    int layerWidth {0};
    int tileHeight {64};
    QHash<int, QVector<QColor>> imageColors;
    QVector<QPair<QString, QVector<QColor>>> palettes;

    KisStudioChips *sources {nullptr};
    /// The source pictures made again once a drag pauses: three reductions of the layer at every step held it back
    QTimer tilesLater;
    /// The source each tile stands for: Levels only while the settings use it
    QVector<SkissaFewer::Source> shownSources;
    KisStudioValueRow *count {nullptr};
    KisStudioValueRow *pixel {nullptr};
    QWidget *paletteRow {nullptr};
    QComboBox *palette {nullptr};
    KisStudioValueRow *brightness {nullptr};
    KisStudioChips *dithers {nullptr};
    QVector<KisStudioChips::Chip> ditherChips;
    KisStudioValueRow *amount {nullptr};

    /// The drag on the image: what it started from
    int dragCount {0};
    int dragPixel {1};
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
            if (!bounds.isEmpty()) {
                whole = original->createThumbnailUncached(256, 256, bounds);
                layerWidth = bounds.width();
            }
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

    d->pixel = new KisStudioValueRow(i18nc("fewer colors effect setting: how large each pixel of pixel art is", "Pixel size"), this);
    d->pixel->setClickSetsValue(true);
    {
        KisStudioValueRow::Range range;
        range.minimum = 1;
        range.maximum = 64;
        range.size = true;
        range.suffix = i18nc("pixels, after a number", " px");
        d->pixel->setRange(range);
        d->pixel->setDefaultValue(1);
    }
    d->pixel->setToolTip(i18nc("@info:tooltip", "Above 1, pixel art"));
    connect(d->pixel, &KisStudioValueRow::valueChanged, this, [this](qreal value) {
        d->settings.pixel = qRound(value);
        d->tilesLater.start();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->pixel);

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
    d->pixel->setValue(s.pixel);
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
        const qreal scale = d->layerWidth > 0 ? qreal(d->tileWhole.width()) / d->layerWidth : 1.0;
        chip.picture = tilePicture(SkissaFewer::applied(d->tileWhole, QPoint(), d->withSource(source), scale));
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
    d->dragPixel = d->settings.pixel;
    d->dragging = true;
    return true;
}

QString SkissaFewerWidget::canvasDragged(const QPointF &moved, const QPointF &)
{
    if (!d->dragging) return QString();
    // sideways larger or smaller pixels, by ratios as sizes go
    d->settings.pixel = std::clamp(int(qRound(KisValueSteps::draggedBy(d->dragPixel, moved.x(), 0, true))), 1, 64);
    d->pixel->setValue(d->settings.pixel);
    const QString pixels = rowText(d->pixel, 0, i18nc("pixels, after a number", " px"));
    if (d->settings.source != SkissaFewer::Source::Palette) {
        // up more colors, by ratios, so few change slowly and many quickly
        const int most = d->settings.source == SkissaFewer::Source::Levels ? mostLevels : mostColors;
        d->settings.count = std::clamp(int(qRound(KisValueSteps::draggedBy(d->dragCount, -moved.y(), 0, true))), 2, most);
        if (d->settings.source == SkissaFewer::Source::Image) d->settings.colors = d->colorsOfImage(d->settings.count);
        d->count->setValue(d->settings.count);
    }
    d->tilesLater.start();
    Q_EMIT sigConfigurationItemChanged();
    if (d->settings.source == SkissaFewer::Source::Palette) return pixels;
    return QStringLiteral("%1   %2").arg(countText(d->settings.source, d->settings.count), pixels);
}

void SkissaFewerWidget::canvasReleased()
{
    d->dragging = false;
}

namespace
{

using Shading = SkissaInk::Shading;

/// A look: what is drawn, how its shading is drawn (with that way's own settings), where it goes
SkissaInk::Settings drawingOf(float size, float lines, float shaded, float flow, Shading shading, float setting = 0,
                              SkissaInk::Colors colors = SkissaInk::Colors::Paper, float second = -1)
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
    case Shading::Soft:
        settings.softness = setting > 0 ? setting : settings.softness;
        settings.grain = second >= 0 ? second : settings.grain;
        break;
    case Shading::Hatch:
    case Shading::Screen:
    case Shading::Engrave:
        settings.spacing = setting > 0 ? setting : settings.spacing;
        settings.angle = second >= 0 ? second : settings.angle;
        break;
    case Shading::Strokes:
        settings.length = setting > 0 ? setting : settings.length;
        settings.spacing = second > 0 ? second : settings.spacing;
        break;
    case Shading::Dots: settings.dot = setting > 0 ? setting : settings.dot; break;
    case Shading::Contour: settings.levels = setting > 0 ? int(setting) : settings.levels; break;
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
    KisStudioValueRow *grain {nullptr};
    KisStudioValueRow *spacing {nullptr};
    KisStudioValueRow *angle {nullptr};
    KisStudioValueRow *length {nullptr};
    KisStudioValueRow *dot {nullptr};
    KisStudioValueRow *levels {nullptr};
    QWidget *inkColorRow {nullptr};
    QWidget *paperColorRow {nullptr};
    KisStudioColorField *inkColor {nullptr};
    KisStudioColorField *paperColor {nullptr};

    float startShaded {0};
    float startSize {1};

    void makeLooks()
    {
        using C = SkissaInk::Colors;
        // two rows: drawn with lines and tone, then drawn or printed with marks
        looks = {
            {i18nc("drawing effect look", "Lines"), drawingOf(1.0f, 0.7f, 0, 0.5f, Shading::Solid)},
            {i18nc("drawing effect look", "Ink"), SkissaInk::inked()},
            {i18nc("drawing effect look", "Comic"), drawingOf(1.2f, 0.6f, 0.15f, 0.5f, Shading::Solid, 0, C::Over)},
            {i18nc("drawing effect look", "Pencil"), drawingOf(0.8f, 0.4f, 0.7f, 0.3f, Shading::Soft, 0.5f, C::Paper, 0.4f)},
            {i18nc("drawing effect look", "Charcoal"), drawingOf(2.5f, 0.3f, 0.6f, 0.8f, Shading::Soft, 0.45f, C::Paper, 0.8f)},
            {i18nc("drawing effect look", "Wash"), drawingOf(2.0f, 0.3f, 0.08f, 1, Shading::Soft, 0.4f, C::Over)},
            {i18nc("drawing effect look", "Hatching"), drawingOf(1.2f, 0.5f, 0.9f, 0.3f, Shading::Hatch, 6)},
            {i18nc("drawing effect look: fine strokes following the shapes", "Etching"), drawingOf(1.0f, 0.4f, 0.9f, 0.8f, Shading::Strokes, 20, C::Paper, 5)},
            {i18nc("drawing effect look: drawn in dots", "Stipple"), drawingOf(1.0f, 0.5f, 0.85f, 0.3f, Shading::Dots, 2.5f)},
            {i18nc("drawing effect look: lines swelling where dark, as on banknotes", "Engraving"), drawingOf(1.0f, 0.5f, 0.95f, 0.3f, Shading::Engrave, 5)},
            {i18nc("drawing effect look: printed in dots", "Halftone"), drawingOf(1.2f, 0.5f, 0.9f, 0.5f, Shading::Screen, 6)},
            {i18nc("drawing effect look: printed in colored dots", "Pop art"), drawingOf(1.5f, 0.6f, 0.95f, 0.5f, Shading::Screen, 6, C::Colored)},
        };
        shadings = {
            {i18nc("drawing effect: shading as flat ink", "Solid"), Shading::Solid},
            {i18nc("drawing effect: shading as soft gray", "Soft"), Shading::Soft},
            {i18nc("drawing effect: shading as straight strokes", "Hatch"), Shading::Hatch},
            {i18nc("drawing effect: shading as strokes along the shapes", "Strokes"), Shading::Strokes},
            {i18nc("drawing effect: shading as dots", "Dots"), Shading::Dots},
            {i18nc("drawing effect: shading as a printer's grid of dots", "Screen"), Shading::Screen},
            {i18nc("drawing effect: shading as engraved lines", "Engrave"), Shading::Engrave},
            {i18nc("drawing effect: shading as lines of equal lightness", "Contour"), Shading::Contour},
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

    // how the shading is drawn, each way with its own setting, in one row
    d->shadingTiles = new KisStudioChips(this);
    d->shadingTiles->setTileColumns(8, tileHeight);
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
    d->grain = add(i18nc("drawing effect setting: how much the paper's tooth shows", "Grain"), 0, 100, 0, percent, defaults.grain * 100,
                   [this](qreal v) { d->settings.grain = float(v / 100); });
    d->spacing = add(i18nc("drawing effect setting: between strokes or dots", "Spacing"), 2, 60, 1, px, defaults.spacing,
                     [this](qreal v) { d->settings.spacing = float(v); });
    d->angle = add(i18nc("drawing effect setting: of the hatch strokes", "Angle"), -90, 90, 0, QStringLiteral("°"), defaults.angle,
                   [this](qreal v) { d->settings.angle = float(v); });
    // the angle is across the first strokes: the dial shows the strokes
    asDial(d->angle, 90);
    d->length = add(i18nc("drawing effect setting: of the strokes", "Length"), 2, 60, 1, px, defaults.length,
                    [this](qreal v) { d->settings.length = float(v); });
    d->dot = add(i18nc("drawing effect setting: how wide the dots are", "Dot size"), 1, 30, 1, px, defaults.dot,
                 [this](qreal v) { d->settings.dot = float(v); });
    d->levels = add(i18nc("drawing effect setting: how many lines of equal lightness", "Levels"), 2, 64, 0, QString(), defaults.levels,
                    [this](qreal v) { d->settings.levels = qRound(v); });

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
    d->grain->setValue(s.grain * 100.0);
    d->spacing->setValue(s.spacing);
    d->angle->setValue(s.angle);
    d->length->setValue(s.length);
    d->dot->setValue(s.dot);
    d->levels->setValue(s.levels);
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
    d->grain->setVisible(shades && s.shading == Shading::Soft);
    const bool spaced = s.shading == Shading::Hatch || s.shading == Shading::Strokes || s.shading == Shading::Screen || s.shading == Shading::Engrave;
    d->spacing->setVisible(shades && spaced);
    d->angle->setVisible(shades && spaced && s.shading != Shading::Strokes);
    d->length->setVisible(shades && s.shading == Shading::Strokes);
    d->dot->setVisible(shades && s.shading == Shading::Dots);
    d->levels->setVisible(shades && s.shading == Shading::Contour);
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
    d->lookTiles->setChips(chipsOf(names, d->lookPictures, [this](int i) { return d->settings.sameLook(d->looks[i].second); }));
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
    d->settings.ink = float(qBound(0.0, KisValueSteps::draggedBy(d->startShaded, -moved.y(), 1, false), 1.0));
    d->settings.level = levelOf(d->layerLightness, d->settings.ink);
    d->settings.size = float(qBound(0.3, KisValueSteps::draggedBy(d->startSize, moved.x(), 0, true), 40.0));
    d->shaded->setValue(d->settings.ink * 100.0);
    d->size->setValue(d->settings.size);
    Q_EMIT sigConfigurationItemChanged();
    return QStringLiteral("%1   %2").arg(rowText(d->shaded, 0, QStringLiteral("%")), rowText(d->size, 1, i18nc("pixels, after a number", " px")));
}

void SkissaInkWidget::canvasReleased()
{
    refreshTiles();
}

namespace
{

using ShapePlace = SkissaShape::Place;

/// The parts of Shape, as its tabs
enum ShapePart { OutlinePart, GlowPart, ShadowPart, GrowPart };

/// How opaque a layer's paint is where it is solid: all but a hundredth of its painted pixels are at most this opaque;
/// nearly opaque counts as opaque
float solidOf(KisPaintDeviceSP device)
{
    const QRect bounds = device ? device->exactBounds() : QRect();
    if (bounds.isEmpty()) return 1;
    const KoColorSpace *cs = device->colorSpace();
    std::array<qint64, 256> counts {};
    KisSequentialConstIterator it(device, bounds);
    while (it.nextPixel()) counts[cs->opacityU8(it.rawDataConst())]++;
    qint64 painted = 0;
    for (int a = 3; a < 256; a++) painted += counts[a];
    qint64 seen = 0;
    for (int a = 255; a >= 3; a--) {
        seen += counts[a];
        if (seen > painted / 100) return a >= 230 ? 1.0f : a / 255.0f;
    }
    return 1;
}

/// The scale the shape's tiles show the layer at, and their size
constexpr qreal shapeTileScale = 0.5;
const QSize shapeTileSize(120, 75);

/// A shape to show the looks on, for a layer without one: a disc, a bar and letters
QImage sampleShape(const QSize &size)
{
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(232, 92, 70));
    const qreal unit = size.height() / 10.0;
    painter.drawEllipse(QRectF(unit * 2, unit * 2, unit * 6, unit * 6));
    painter.setBrush(QColor(70, 140, 225));
    painter.drawRoundedRect(QRectF(unit * 9, unit * 2.5, unit * 7, unit * 3), unit, unit);
    QFont font = painter.font();
    font.setBold(true);
    font.setPixelSize(qRound(unit * 3.4));
    painter.setFont(font);
    painter.setPen(QColor(250, 200, 50));
    painter.drawText(QRectF(unit * 9, unit * 5.5, unit * 8, unit * 4), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Aa"));
    return image;
}

/// Where \p image's shape has an edge, nearest its middle; its middle if it has none
QPoint edgeOf(const QImage &image, bool *found)
{
    const QImage alpha = image.convertToFormat(QImage::Format_ARGB32);
    const QPointF middle(alpha.width() / 2.0, alpha.height() / 2.0);
    QPoint best = middle.toPoint();
    qreal bestScore = -1;
    for (int y = 1; y + 1 < alpha.height(); y++) {
        for (int x = 1; x + 1 < alpha.width(); x++) {
            const int a = qAlpha(alpha.pixel(x, y));
            const int change = std::abs(qAlpha(alpha.pixel(x + 1, y)) - a) + std::abs(qAlpha(alpha.pixel(x, y + 1)) - a);
            if (change < 64) continue;
            const qreal score = change / (1.0 + QLineF(middle, QPointF(x, y)).length());
            if (score > bestScore) {
                bestScore = score;
                best = QPoint(x, y);
            }
        }
    }
    *found = bestScore > 0;
    return best;
}

} // namespace

struct SkissaShapeWidget::Private {
    KisPaintDeviceSP original;
    SkissaShape::Settings settings;
    /// How opaque the layer's solid paint is (SkissaShape::Settings::solid)
    float solid {1};
    int part {OutlinePart};
    /// The tiles' part of the layer, at their scale, with room around for what the looks reach
    QImage base;
    QPoint corner;
    QVector<QPair<QString, SkissaShape::Settings>> looks;
    QVector<QImage> lookPictures;

    KisStudioChips *lookTiles {nullptr};
    KisStudioChips *parts {nullptr};
    KisStudioValueRow *width {nullptr};
    KisStudioChips *places {nullptr};
    QWidget *outlineColorRow {nullptr};
    KisStudioColorField *outlineColor {nullptr};
    KisStudioValueRow *glow {nullptr};
    KisStudioValueRow *glowStrength {nullptr};
    QWidget *glowColorRow {nullptr};
    KisStudioColorField *glowColor {nullptr};
    KisStudioValueRow *shadow {nullptr};
    KisStudioValueRow *shadowAngle {nullptr};
    KisStudioValueRow *shadowSoftness {nullptr};
    QWidget *shadowColorRow {nullptr};
    KisStudioColorField *shadowColor {nullptr};
    KisStudioValueRow *grow {nullptr};

    /// The drag on the image: where it began and what it began from
    QPoint pressed;
    SkissaShape::Settings start;

    void makeLooks()
    {
        using S = SkissaShape::Settings;
        auto with = [](std::function<void(S &)> change) {
            S s;
            change(s);
            return s;
        };
        looks = {
            {i18nc("shape effect look: nothing added", "None"), with([](S &) {})},
            {i18nc("shape effect look", "Outline"), with([](S &s) { s.outline = 4; })},
            {i18nc("shape effect look: a white border and a soft shadow", "Sticker"), with([](S &s) {
                 s.outline = 10;
                 s.outlineColor = Qt::white;
                 s.shadow = 4;
                 s.shadowSoftness = 12;
                 s.shadowColor = QColor(0, 0, 0, 110);
             })},
            {i18nc("shape effect look", "Glow"), with([](S &s) { s.glow = 30; })},
            {i18nc("shape effect look: a bright outline that glows", "Neon"), with([](S &s) {
                 s.outline = 3;
                 s.place = ShapePlace::Center;
                 s.outlineColor = QColor(120, 250, 255);
                 s.glow = 24;
                 s.glowStrength = 0.9f;
                 s.glowColor = QColor(40, 200, 255);
             })},
            {i18nc("shape effect look", "Shadow"), with([](S &s) {
                 s.shadow = 12;
                 s.shadowSoftness = 14;
                 s.shadowColor = QColor(0, 0, 0, 150);
             })},
            {i18nc("shape effect look: lines made bolder", "Thicker"), with([](S &s) { s.grow = 2; })},
            {i18nc("shape effect look: lines made finer", "Thinner"), with([](S &s) { s.grow = -1; })},
        };
    }

    void makeBase()
    {
        int margin = 0;
        for (const auto &look : looks) margin = std::max(margin, SkissaShape::Shape(look.second, shapeTileScale).reach());
        margin = std::min(margin + 2, 48);
        const QSize whole = shapeTileSize + QSize(2 * margin, 2 * margin);
        bool found = false;
        if (original) {
            const QRect bounds = original->exactBounds();
            if (!bounds.isEmpty()) {
                // an edge of the shape, found on a small picture of it, shown at the tiles' scale
                const qreal small = qMin(1.0, 256.0 / qMax(bounds.width(), bounds.height()));
                const QImage thumbnail = original->createThumbnailUncached(qCeil(bounds.width() * small), qCeil(bounds.height() * small), bounds);
                const QPoint edge = edgeOf(thumbnail, &found);
                if (found) {
                    const QPoint at = bounds.topLeft() + (QPointF(edge) / small).toPoint();
                    const QSize read = whole / shapeTileScale;
                    base = original->convertToQImage(nullptr, QRect(at - QPoint(read.width() / 2, read.height() / 2), read))
                               .scaled(whole, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                }
            }
        }
        if (!found) base = sampleShape(whole);
        corner = QPoint(margin, margin);
        // the looks don't change with the settings: their pictures are made once
        for (const auto &look : looks) {
            SkissaShape::Settings s = look.second;
            s.solid = found ? solid : 1;
            lookPictures << SkissaShape::applied(base, s, shapeTileScale).copy(QRect(corner, shapeTileSize));
        }
    }
};

SkissaShapeWidget::SkissaShapeWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    if (dev) d->original = KisEffectPanels::fullSizeCopy(dev);
    d->solid = solidOf(d->original);
    d->settings.solid = d->solid;
    d->makeLooks();
    d->makeBase();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    const int tileHeight = qMax(64, 2 * fontMetrics().height() + 34);
    d->lookTiles = new KisStudioChips(this);
    d->lookTiles->setTileColumns(4, tileHeight);
    connect(d->lookTiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->looks.size()) return;
        d->settings = d->looks[index].second;
        d->settings.solid = d->solid;
        // the tab of what the look is most about
        const SkissaShape::Settings &s = d->settings;
        d->part = s.outline > 0 ? OutlinePart : s.glow > 0 ? GlowPart : s.shadow > 0 ? ShadowPart : s.grow != 0 ? GrowPart : d->part;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->lookTiles);

    d->parts = new KisStudioChips(this);
    d->parts->setTabs(true);
    connect(d->parts, &KisStudioChips::clicked, this, [this](int index) {
        d->part = index;
        showRows();
    });
    layout->addWidget(d->parts);

    const QString px = i18nc("pixels, after a number", " px");
    const QString percent = QStringLiteral("%");
    const SkissaShape::Settings defaults;
    auto add = [&](const QString &name, qreal low, qreal high, int decimals, const QString &suffix, qreal reset, std::function<void(qreal)> write) {
        KisStudioValueRow *row = valueRow(this, name, low, high, decimals, suffix, reset);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, write](qreal value) {
            write(value);
            edited();
        });
        layout->addWidget(row);
        return row;
    };
    auto colorRow = [&](const QString &name, KisStudioColorField **field, const QColor &reset, std::function<void(const QColor &)> write) {
        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(new QLabel(name, row), 1);
        *field = new KisStudioColorField(row);
        (*field)->setDefaultColor(reset);
        (*field)->setAlphaEnabled(true);
        connect(*field, &KisStudioColorField::colorChanged, this, [this, write](const QColor &color) {
            write(color);
            edited();
        });
        rowLayout->addWidget(*field);
        layout->addWidget(row);
        return row;
    };

    d->width = add(i18nc("shape effect: how wide the outline is", "Width"), 0, 100, 1, px, 4, [this](qreal v) { d->settings.outline = float(v); });
    d->places = new KisStudioChips(this);
    connect(d->places, &KisStudioChips::clicked, this, [this](int index) {
        d->settings.place = ShapePlace(qBound(0, index, 2));
        edited();
    });
    layout->addWidget(d->places);
    d->outlineColorRow = colorRow(i18nc("shape effect: the outline's color", "Color"), &d->outlineColor, defaults.outlineColor,
                                  [this](const QColor &c) { d->settings.outlineColor = c; });

    d->glow = add(i18nc("shape effect: how far the glow reaches", "Size"), 0, 200, 1, px, 30, [this](qreal v) { d->settings.glow = float(v); });
    d->glowStrength = add(i18nc("shape effect: how strong the glow is", "Strength"), 0, 100, 0, percent, defaults.glowStrength * 100,
                          [this](qreal v) { d->settings.glowStrength = float(v / 100); });
    d->glowColorRow = colorRow(i18nc("shape effect: the glow's color", "Color"), &d->glowColor, defaults.glowColor,
                               [this](const QColor &c) { d->settings.glowColor = c; });

    d->shadow = add(i18nc("shape effect: how far the shadow falls", "Distance"), 0, 200, 1, px, 12, [this](qreal v) { d->settings.shadow = float(v); });
    d->shadowAngle = add(i18nc("shape effect: which way the shadow falls", "Angle"), -180, 180, 0, QStringLiteral("°"), defaults.shadowAngle,
                         [this](qreal v) { d->settings.shadowAngle = float(v); });
    asDial(d->shadowAngle);
    d->shadowSoftness = add(i18nc("shape effect: how soft the shadow is", "Softness"), 0, 100, 1, px, defaults.shadowSoftness,
                            [this](qreal v) { d->settings.shadowSoftness = float(v); });
    d->shadowColorRow = colorRow(i18nc("shape effect: the shadow's color", "Color"), &d->shadowColor, defaults.shadowColor,
                                 [this](const QColor &c) { d->settings.shadowColor = c; });

    d->grow = add(i18nc("shape effect: how much the shape grows, or shrinks below 0", "Grow"), -50, 50, 1, px, 0,
                  [this](qreal v) { d->settings.grow = float(v); });
    d->grow->setToolTip(i18nc("@info:tooltip", "Below 0, it shrinks"));

    refresh();
}

SkissaShapeWidget::~SkissaShapeWidget()
{
}

void SkissaShapeWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? SkissaShape::Settings::from(*config) : SkissaShape::Settings();
    // the opacity of this layer's paint, not the one the settings were last used on
    d->settings.solid = d->solid;
    refresh();
}

KisPropertiesConfigurationSP SkissaShapeWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_shape"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaShapeWidget::sizeHint() const
{
    const int width = 460;
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaShapeWidget::edited()
{
    showRows();
    refreshTiles();
    Q_EMIT sigConfigurationItemChanged();
}

void SkissaShapeWidget::refresh()
{
    const SkissaShape::Settings &s = d->settings;
    d->width->setValue(s.outline);
    d->outlineColor->setColor(s.outlineColor);
    d->glow->setValue(s.glow);
    d->glowStrength->setValue(s.glowStrength * 100);
    d->glowColor->setColor(s.glowColor);
    d->shadow->setValue(s.shadow);
    d->shadowAngle->setValue(s.shadowAngle);
    d->shadowSoftness->setValue(s.shadowSoftness);
    d->shadowColor->setColor(s.shadowColor);
    d->grow->setValue(s.grow);
    showRows();
    refreshTiles();
}

void SkissaShapeWidget::showRows()
{
    const SkissaShape::Settings &s = d->settings;
    const bool outlines = s.outline > 0, glows = s.glow > 0 && s.glowStrength > 0, shadows = s.shadowColor.alpha() > 0 && s.shadow > 0;
    QVector<KisStudioChips::Chip> tabs;
    const QString names[] = {i18nc("shape effect part", "Outline"), i18nc("shape effect part", "Glow"), i18nc("shape effect part", "Shadow"),
                             i18nc("shape effect part", "Grow")};
    const bool on[] = {outlines, glows, shadows, s.grow != 0};
    for (int i = 0; i < 4; i++) {
        KisStudioChips::Chip chip;
        chip.text = names[i];
        chip.on = d->part == i;
        chip.dot = on[i];
        tabs << chip;
    }
    d->parts->setChips(tabs);
    d->width->setVisible(d->part == OutlinePart);
    d->places->setVisible(d->part == OutlinePart && outlines);
    d->outlineColorRow->setVisible(d->part == OutlinePart && outlines);
    QVector<KisStudioChips::Chip> places;
    const QString placeNames[] = {i18nc("shape effect: outline outside the shape", "Outside"), i18nc("shape effect: outline on the edge", "Center"),
                                  i18nc("shape effect: outline inside the shape", "Inside")};
    for (int i = 0; i < 3; i++) {
        KisStudioChips::Chip chip;
        chip.text = placeNames[i];
        chip.on = int(s.place) == i;
        places << chip;
    }
    d->places->setChips(places);
    d->glow->setVisible(d->part == GlowPart);
    d->glowStrength->setVisible(d->part == GlowPart && glows);
    d->glowColorRow->setVisible(d->part == GlowPart && glows);
    d->shadow->setVisible(d->part == ShadowPart);
    d->shadowAngle->setVisible(d->part == ShadowPart && shadows);
    d->shadowSoftness->setVisible(d->part == ShadowPart && shadows);
    d->shadowColorRow->setVisible(d->part == ShadowPart && shadows);
    d->grow->setVisible(d->part == GrowPart);
}

void SkissaShapeWidget::refreshTiles()
{
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < d->looks.size(); i++) {
        const auto &look = d->looks[i];
        KisStudioChips::Chip chip;
        chip.text = look.first;
        chip.on = d->settings == look.second || (look.second.changesNothing() && d->settings.changesNothing());
        chip.picture = tilePicture(d->lookPictures.value(i));
        chip.pictureKey = d->lookPictures.value(i).cacheKey();
        chips << chip;
    }
    d->lookTiles->setChips(chips);
}

void SkissaShapeWidget::canvasHovered(const QPoint &)
{
}

bool SkissaShapeWidget::canvasPressed(const QPoint &pixel)
{
    d->pressed = pixel;
    d->start = d->settings;
    return true;
}

QString SkissaShapeWidget::canvasDragged(const QPointF &moved, const QPointF &at)
{
    SkissaShape::Settings &s = d->settings;
    const QString px = i18nc("pixels, after a number", " px");
    switch (d->part) {
    case ShadowPart: {
        // the shadow follows the pointer: where it was pressed is where the shape's shadow was
        const double angle = qDegreesToRadians(double(d->start.shadowAngle));
        const QPointF began(std::cos(angle) * d->start.shadow, std::sin(angle) * d->start.shadow);
        const QPointF now = began + (at - QPointF(d->pressed));
        s.shadow = float(qBound(0.0, std::hypot(now.x(), now.y()), 200.0));
        if (s.shadow > 0.5f) s.shadowAngle = float(qRadiansToDegrees(std::atan2(now.y(), now.x())));
        break;
    }
    case OutlinePart:
        // by ratios, as sizes go, from a little when there was none
        s.outline = float(qBound(0.0, KisValueSteps::draggedBy(d->start.outline, moved.x(), 0, true, 1), 100.0));
        break;
    case GlowPart:
        s.glow = float(qBound(0.0, KisValueSteps::draggedBy(d->start.glow, moved.x(), 0, true, 2), 200.0));
        s.glowStrength = float(qBound(0.0, KisValueSteps::draggedBy(d->start.glowStrength, -moved.y(), 1, false), 1.0));
        break;
    default:
        // pixels, evenly: across the drag's range as far as the usual grow goes
        s.grow = float(qBound(-50.0, KisValueSteps::draggedBy(d->start.grow, moved.x(), 64, false), 50.0));
        break;
    }
    refresh();
    Q_EMIT sigConfigurationItemChanged();
    switch (d->part) {
    case ShadowPart: return rowText(d->shadow, 0, px) + QStringLiteral("   ") + rowText(d->shadowAngle, 0, QStringLiteral("°"));
    case OutlinePart: return rowText(d->width, 1, px);
    case GlowPart: return rowText(d->glow, 0, px) + QStringLiteral("   ") + rowText(d->glowStrength, 0, QStringLiteral("%"));
    default: return rowText(d->grow, 1, px);
    }
}

void SkissaShapeWidget::canvasReleased()
{
}

Qt::Orientations SkissaShapeWidget::canvasAxes() const
{
    return d->part == OutlinePart || d->part == GrowPart ? Qt::Horizontal : Qt::Horizontal | Qt::Vertical;
}

namespace
{

/// The ways Light spreads, as its tabs
enum LightPart { GlowLight, StarLight, StreakLight };

} // namespace

struct SkissaLightWidget::Private {
    KisPaintDeviceSP original;
    SkissaLight::Settings settings;
    int part {GlowLight};
    /// The layer small, how bright its brightest parts look, and the tiles' pictures of it at that scale
    QImage frame;
    qreal frameScale {1};
    float layerTop {1};
    QVector<QPair<QString, SkissaLight::Settings>> looks;
    QVector<QImage> lookPictures;

    KisStudioChips *lookTiles {nullptr};
    KisStudioValueRow *threshold {nullptr};
    KisStudioChips *parts {nullptr};
    KisStudioValueRow *glow {nullptr};
    KisStudioValueRow *size {nullptr};
    KisStudioValueRow *star {nullptr};
    KisStudioValueRow *length {nullptr};
    KisStudioValueRow *points {nullptr};
    KisStudioValueRow *angle {nullptr};
    KisStudioValueRow *streak {nullptr};
    KisStudioValueRow *streakLength {nullptr};
    KisStudioValueRow *streakAngle {nullptr};
    QWidget *streakColorRow {nullptr};
    KisStudioColorField *streakColor {nullptr};

    SkissaLight::Settings start;

    void makeLooks()
    {
        using S = SkissaLight::Settings;
        auto with = [](std::function<void(S &)> change) {
            S s;
            s.glow = 0;
            change(s);
            return s;
        };
        looks = {
            {i18nc("light effect look: nothing added", "None"), with([](S &) {})},
            {i18nc("light effect look", "Bloom"), with([](S &s) { s.glow = 1; })},
            {i18nc("light effect look: a soft, wide glow", "Dreamy"), with([](S &s) {
                 s.glow = 2;
                 s.size = 160;
                 s.threshold = 0.55f;
             })},
            {i18nc("light effect look: diffraction spikes", "Star"), with([](S &s) {
                 s.star = 1.2f;
                 s.glow = 0.3f;
             })},
            {i18nc("light effect look: small four-pointed stars", "Sparkle"), with([](S &s) {
                 s.star = 2;
                 s.points = 4;
                 s.angle = 45;
                 s.length = 40;
                 s.threshold = 0.9f;
             })},
            {i18nc("light effect look: the long streak of an anamorphic lens", "Anamorphic"), with([](S &s) {
                 s.streak = 1.5f;
                 s.glow = 0.3f;
             })},
        };
    }

    void makeFrame()
    {
        if (original) {
            const QRect bounds = original->exactBounds();
            if (!bounds.isEmpty()) {
                frameScale = qMin(qreal(lensPictureSize.width()) / bounds.width(), qreal(lensPictureSize.height()) / bounds.height());
                const QImage picture = original->createThumbnailUncached(qCeil(bounds.width() * frameScale), qCeil(bounds.height() * frameScale), bounds);
                if (!picture.isNull()) layerTop = brightestOf(picture);
                if (!picture.isNull() && !KisEffectPanels::showsLittle(picture)) frame = picture;
            }
        }
        float frameTop = layerTop;
        if (frame.isNull()) {
            frame = KisStudioPicker::samplePicture(lensPictureSize).convertToFormat(QImage::Format_ARGB32);
            frameTop = brightestOf(frame);
            frameScale = 0.25;
        }
        for (const auto &look : looks) {
            SkissaLight::Settings s = look.second;
            s.top = frameTop;
            // at least a few pixels of each spread, so the tiles show its shape
            lookPictures << SkissaLight::applied(frame, s, std::max(frameScale, 0.15));
        }
    }
};

SkissaLightWidget::SkissaLightWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    if (dev) d->original = KisEffectPanels::fullSizeCopy(dev);
    d->makeLooks();
    d->makeFrame();
    d->settings.top = d->layerTop;

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    const int tileHeight = qMax(64, 2 * fontMetrics().height() + 34);
    d->lookTiles = new KisStudioChips(this);
    d->lookTiles->setTileColumns(3, tileHeight);
    connect(d->lookTiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->looks.size()) return;
        d->settings = d->looks[index].second;
        d->settings.top = d->layerTop;
        const SkissaLight::Settings &s = d->settings;
        d->part = s.streak > 0 ? StreakLight : s.star > 0 ? StarLight : GlowLight;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->lookTiles);

    const QString px = i18nc("pixels, after a number", " px");
    const QString percent = QStringLiteral("%");
    const SkissaLight::Settings defaults;
    auto add = [&](const QString &name, qreal low, qreal high, int decimals, const QString &suffix, qreal reset, std::function<void(qreal)> write) {
        KisStudioValueRow *row = valueRow(this, name, low, high, decimals, suffix, reset);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, write](qreal value) {
            write(value);
            edited();
        });
        layout->addWidget(row);
        return row;
    };
    d->threshold = add(i18nc("light effect: how bright a part must be to shine", "Threshold"), 0, 100, 0, percent, defaults.threshold * 100,
                       [this](qreal v) { d->settings.threshold = float(v / 100); });
    d->threshold->setToolTip(i18nc("@info:tooltip", "Lower, more of the picture shines"));

    d->parts = new KisStudioChips(this);
    d->parts->setTabs(true);
    connect(d->parts, &KisStudioChips::clicked, this, [this](int index) {
        d->part = index;
        showRows();
    });
    layout->addWidget(d->parts);

    d->glow = add(i18nc("light effect: how strong the glow is", "Strength"), 0, 400, 0, percent, 100, [this](qreal v) { d->settings.glow = float(v / 100); });
    d->size = add(i18nc("light effect: how far the glow reaches", "Size"), 2, 1000, 0, px, defaults.size, [this](qreal v) { d->settings.size = float(v); });
    d->star = add(i18nc("light effect: how strong the star is", "Strength"), 0, 400, 0, percent, 100, [this](qreal v) { d->settings.star = float(v / 100); });
    d->length = add(i18nc("light effect: how long the star's spikes are", "Length"), 2, 2000, 0, px, defaults.length,
                    [this](qreal v) { d->settings.length = float(v); });
    d->points = add(i18nc("light effect: how many spikes the star has", "Points"), 2, 16, 0, QString(), defaults.points,
                    [this](qreal v) {
                        // a star's spikes come in pairs: the row shows the number it draws
                        d->settings.points = qRound(v / 2) * 2;
                        d->points->setValueText(QString::number(d->settings.points));
                    });
    d->angle = add(i18nc("light effect: how the star is turned", "Angle"), -180, 180, 0, QStringLiteral("°"), defaults.angle,
                   [this](qreal v) { d->settings.angle = float(v); });
    asDial(d->angle);
    d->streak = add(i18nc("light effect: how strong the streak is", "Strength"), 0, 400, 0, percent, 100, [this](qreal v) { d->settings.streak = float(v / 100); });
    d->streakLength = add(i18nc("light effect: how long the streak is", "Length"), 2, 4000, 0, px, defaults.streakLength,
                          [this](qreal v) { d->settings.streakLength = float(v); });
    d->streakAngle = add(i18nc("light effect: the streak's direction", "Angle"), -180, 180, 0, QStringLiteral("°"), defaults.streakAngle,
                         [this](qreal v) { d->settings.streakAngle = float(v); });
    asDial(d->streakAngle);
    d->streakColorRow = new QWidget(this);
    auto *colorLayout = new QHBoxLayout(d->streakColorRow);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->addWidget(new QLabel(i18nc("light effect: the streak's color", "Color"), d->streakColorRow), 1);
    d->streakColor = new KisStudioColorField(d->streakColorRow);
    d->streakColor->setDefaultColor(defaults.streakColor);
    connect(d->streakColor, &KisStudioColorField::colorChanged, this, [this](const QColor &color) {
        d->settings.streakColor = color;
        edited();
    });
    colorLayout->addWidget(d->streakColor);
    layout->addWidget(d->streakColorRow);

    refresh();
}

SkissaLightWidget::~SkissaLightWidget()
{
}

void SkissaLightWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? SkissaLight::Settings::from(*config) : SkissaLight::Settings();
    // the brightest parts are this layer's, not those of the one the settings were last used on
    d->settings.top = d->layerTop;
    refresh();
}

KisPropertiesConfigurationSP SkissaLightWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_light"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaLightWidget::sizeHint() const
{
    const int width = 460;
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaLightWidget::edited()
{
    showRows();
    Q_EMIT sigConfigurationItemChanged();
}

void SkissaLightWidget::refresh()
{
    const SkissaLight::Settings &s = d->settings;
    d->threshold->setValue(s.threshold * 100);
    d->glow->setValue(s.glow * 100);
    d->size->setValue(s.size);
    d->star->setValue(s.star * 100);
    d->length->setValue(s.length);
    d->points->setValue(s.points);
    d->points->setValueText(QString::number(s.points));
    d->angle->setValue(s.angle);
    d->streak->setValue(s.streak * 100);
    d->streakLength->setValue(s.streakLength);
    d->streakAngle->setValue(s.streakAngle);
    d->streakColor->setColor(s.streakColor);
    showRows();
}

void SkissaLightWidget::showRows()
{
    const SkissaLight::Settings &s = d->settings;
    QVector<KisStudioChips::Chip> tabs;
    const QString names[] = {i18nc("light effect part", "Glow"), i18nc("light effect part: diffraction spikes", "Star"),
                             i18nc("light effect part: an anamorphic lens's streak", "Streak")};
    const bool on[] = {s.glow > 0, s.star > 0, s.streak > 0};
    for (int i = 0; i < 3; i++) {
        KisStudioChips::Chip chip;
        chip.text = names[i];
        chip.on = d->part == i;
        chip.dot = on[i];
        tabs << chip;
    }
    d->parts->setChips(tabs);
    d->glow->setVisible(d->part == GlowLight);
    d->size->setVisible(d->part == GlowLight && s.glow > 0);
    d->star->setVisible(d->part == StarLight);
    d->length->setVisible(d->part == StarLight && s.star > 0);
    d->points->setVisible(d->part == StarLight && s.star > 0);
    d->angle->setVisible(d->part == StarLight && s.star > 0);
    d->streak->setVisible(d->part == StreakLight);
    d->streakLength->setVisible(d->part == StreakLight && s.streak > 0);
    d->streakAngle->setVisible(d->part == StreakLight && s.streak > 0);
    d->streakColorRow->setVisible(d->part == StreakLight && s.streak > 0);

    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < d->looks.size(); i++) {
        SkissaLight::Settings look = d->looks[i].second;
        look.top = s.top;
        KisStudioChips::Chip chip;
        chip.text = d->looks[i].first;
        chip.on = s == look || (look.changesNothing() && s.changesNothing());
        chip.picture = tilePicture(d->lookPictures.value(i));
        chip.pictureKey = d->lookPictures.value(i).cacheKey();
        chips << chip;
    }
    d->lookTiles->setChips(chips);
}

void SkissaLightWidget::canvasHovered(const QPoint &)
{
}

bool SkissaLightWidget::canvasPressed(const QPoint &)
{
    d->start = d->settings;
    return true;
}

QString SkissaLightWidget::canvasDragged(const QPointF &moved, const QPointF &)
{
    // up stronger, sideways larger by ratios, for the part shown
    SkissaLight::Settings &s = d->settings;
    const qreal stronger = KisValueSteps::draggedBy(0, -moved.y(), 1, false), larger = KisValueSteps::draggedBy(1, moved.x(), 0, true);
    const QString px = i18nc("pixels, after a number", " px");
    switch (d->part) {
    case StarLight:
        s.star = float(qBound(0.0, d->start.star + stronger, 4.0));
        s.length = float(qBound(2.0, d->start.length * larger, 2000.0));
        break;
    case StreakLight:
        s.streak = float(qBound(0.0, d->start.streak + stronger, 4.0));
        s.streakLength = float(qBound(2.0, d->start.streakLength * larger, 4000.0));
        break;
    default:
        s.glow = float(qBound(0.0, d->start.glow + stronger, 4.0));
        s.size = float(qBound(2.0, d->start.size * larger, 1000.0));
        break;
    }
    refresh();
    Q_EMIT sigConfigurationItemChanged();
    switch (d->part) {
    case StarLight: return rowText(d->star, 0, QStringLiteral("%")) + QStringLiteral("   ") + rowText(d->length, 0, px);
    case StreakLight: return rowText(d->streak, 0, QStringLiteral("%")) + QStringLiteral("   ") + rowText(d->streakLength, 0, px);
    default: return rowText(d->glow, 0, QStringLiteral("%")) + QStringLiteral("   ") + rowText(d->size, 0, px);
    }
}

void SkissaLightWidget::canvasReleased()
{
}

namespace
{

/// The scale Painting's tiles show the layer at, and their size
constexpr qreal paintTileScale = 0.5;
const QSize paintTileSize(120, 75);

} // namespace

struct SkissaPaintWidget::Private {
    KisPaintDeviceSP original;
    SkissaPaint::Settings settings;
    QVector<QPair<QString, SkissaPaint::Settings>> looks;
    QVector<QImage> lookPictures;

    KisStudioChips *lookTiles {nullptr};
    KisStudioValueRow *size {nullptr};
    KisStudioValueRow *sharpness {nullptr};
    KisStudioValueRow *flow {nullptr};
    SkissaPaint::Settings start;

    void makeLooks()
    {
        auto look = [](float size, float sharpness, float flow) {
            SkissaPaint::Settings s;
            s.size = size;
            s.sharpness = sharpness;
            s.flow = flow;
            return s;
        };
        looks = {
            {i18nc("painting effect look", "Oil"), look(6, 0.6f, 0.7f)},
            {i18nc("painting effect look", "Gouache"), look(4, 0.9f, 0.9f)},
            {i18nc("painting effect look: large, bold strokes", "Bold"), look(12, 0.6f, 0.8f)},
            {i18nc("painting effect look: soft, blended strokes", "Soft"), look(8, 0.2f, 0.5f)},
        };
        // a busy part of the layer at the tiles' scale, with room around for the largest brush
        int margin = 0;
        for (const auto &l : looks) margin = std::max(margin, SkissaPaint::Paint(l.second, paintTileScale).reach());
        const QSize whole = paintTileSize + QSize(2 * margin, 2 * margin);
        QImage base;
        if (original) {
            const QRect bounds = original->exactBounds();
            if (!bounds.isEmpty()) {
                const QPoint busiest = KisEffectPanels::busiestPoint(original, bounds);
                const QSize read = whole / paintTileScale;
                base = original->convertToQImage(nullptr, QRect(busiest - QPoint(read.width() / 2, read.height() / 2), read))
                           .scaled(whole, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            }
        }
        if (base.isNull() || KisEffectPanels::showsLittle(base.copy(QRect(QPoint(margin, margin), paintTileSize)))) {
            base = KisStudioPicker::samplePicture(whole * 3).convertToFormat(QImage::Format_ARGB32).copy(QRect(QPoint(whole.width(), whole.height()), whole));
        }
        for (const auto &l : looks) lookPictures << SkissaPaint::applied(base, l.second, paintTileScale).copy(QRect(QPoint(margin, margin), paintTileSize));
    }
};

SkissaPaintWidget::SkissaPaintWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    if (dev) d->original = KisEffectPanels::fullSizeCopy(dev);
    d->makeLooks();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    d->lookTiles = new KisStudioChips(this);
    d->lookTiles->setTileColumns(4, qMax(64, 2 * fontMetrics().height() + 34));
    connect(d->lookTiles, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->looks.size()) return;
        d->settings = d->looks[index].second;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->lookTiles);

    const SkissaPaint::Settings defaults;
    auto add = [&](const QString &name, qreal low, qreal high, int decimals, const QString &suffix, qreal reset, std::function<void(qreal)> write,
                   const QString &tip = QString()) {
        KisStudioValueRow *row = valueRow(this, name, low, high, decimals, suffix, reset);
        row->setToolTip(tip);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, write](qreal value) {
            write(value);
            refresh();
            Q_EMIT sigConfigurationItemChanged();
        });
        layout->addWidget(row);
        return row;
    };
    d->size = add(i18nc("painting effect: the brush's size", "Size"), 1, 100, 1, i18nc("pixels, after a number", " px"), defaults.size,
                  [this](qreal v) { d->settings.size = float(v); });
    d->sharpness = add(i18nc("painting effect: how crisp the strokes' edges are", "Sharpness"), 0, 100, 0, QStringLiteral("%"), defaults.sharpness * 100,
                       [this](qreal v) { d->settings.sharpness = float(v / 100); });
    d->flow = add(i18nc("painting effect: how much strokes follow the shapes", "Flow"), 0, 100, 0, QStringLiteral("%"), defaults.flow * 100,
                  [this](qreal v) { d->settings.flow = float(v / 100); }, i18nc("@info:tooltip", "How much strokes follow the shapes"));
    refresh();
}

SkissaPaintWidget::~SkissaPaintWidget()
{
}

void SkissaPaintWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? SkissaPaint::Settings::from(*config) : SkissaPaint::Settings();
    refresh();
}

KisPropertiesConfigurationSP SkissaPaintWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_paint"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaPaintWidget::sizeHint() const
{
    const int width = 460;
    return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
}

void SkissaPaintWidget::refresh()
{
    const SkissaPaint::Settings &s = d->settings;
    d->size->setValue(s.size);
    d->sharpness->setValue(s.sharpness * 100);
    d->flow->setValue(s.flow * 100);
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < d->looks.size(); i++) {
        KisStudioChips::Chip chip;
        chip.text = d->looks[i].first;
        chip.on = s == d->looks[i].second;
        chip.picture = tilePicture(d->lookPictures.value(i));
        chip.pictureKey = d->lookPictures.value(i).cacheKey();
        chips << chip;
    }
    d->lookTiles->setChips(chips);
}

void SkissaPaintWidget::canvasHovered(const QPoint &)
{
}

bool SkissaPaintWidget::canvasPressed(const QPoint &)
{
    d->start = d->settings;
    return true;
}

QString SkissaPaintWidget::canvasDragged(const QPointF &moved, const QPointF &)
{
    // sideways a larger or smaller brush, by ratios; up crisper
    d->settings.size = float(qBound(1.0, KisValueSteps::draggedBy(d->start.size, moved.x(), 0, true), 100.0));
    d->settings.sharpness = float(qBound(0.0, KisValueSteps::draggedBy(d->start.sharpness, -moved.y(), 1, false), 1.0));
    refresh();
    Q_EMIT sigConfigurationItemChanged();
    return rowText(d->size, 1, i18nc("pixels, after a number", " px")) + QStringLiteral("   ") + rowText(d->sharpness, 0, QStringLiteral("%"));
}

void SkissaPaintWidget::canvasReleased()
{
}
