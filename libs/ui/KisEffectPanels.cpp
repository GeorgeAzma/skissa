/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisEffectPanels.h"

#include <KisValueSteps.h>

#include <QGuiApplication>
#include <QDomDocument>
#include <QHBoxLayout>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QLabel>
#include <QPainter>
#include <QPolygonF>
#include <QTimer>
#include <QToolButton>
#include <QTransform>
#include <QVBoxLayout>
#include <QtMath>

#include <cmath>
#include <memory>

#include <KisEffectCanvasEditor.h>
#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KoColor.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoSegmentGradient.h>
#include <KoStopGradient.h>
#include <KisGradientConversion.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_config_widget.h>
#include <kis_default_bounds.h>
#include "widgets/KisStudioPicker.h"
#include <kis_icon_utils.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <klocalizedstring.h>
#include <widgets/KisStudioChips.h>
#include <widgets/KisStudioColorField.h>
#include <widgets/KisStudioGradientBar.h>
#include <widgets/KisStudioValueRow.h>

namespace KisEffectPanels
{

namespace
{

/// The part of the layer the pictures show, at its own size so that fine detail shows
const QSize tileSize(96, 60);

using Config = KisFilterConfiguration;
using Edit = std::function<void(Config &)>;
using Test = std::function<bool(const Config &)>;

using Drawing = std::function<void(QPainter &, const QRectF &)>;

/// A choice among a few, each tile a picture of the layer with it, or a drawing of its own
struct Tile {
    QString name;
    Edit edit;
    Test on;
    Drawing drawing {};
};

/// A value of the settings, shown as a row
struct Value {
    QString name;
    KisStudioValueRow::Range range;
    std::function<qreal(const Config &)> read;
    std::function<void(Config &, qreal)> write;
    Test shown;
    /// The range a drag on the image covers, when narrower than the row's
    qreal usualMaximum {0};
};

/// A color of the settings
struct Color {
    QString name;
    std::function<KoColor(const Config &)> read;
    std::function<void(Config &, const KoColor &)> write;
    Test shown;
    /// Whether it may be see-through
    bool alpha {false};
};

/// Bounds of a picture of its own, for effects that read past the edge (Krita's blurs repeat the image's edge)
class PictureBounds : public KisDefaultBounds
{
public:
    explicit PictureBounds(const QRect &rect)
        : m_rect(rect)
    {
    }
    QRect bounds() const override
    {
        return m_rect;
    }

private:
    QRect m_rect;
};

/// Rings of every size from coarse to fine with every hue, for a layer with nothing on it
QImage fallbackPicture(const QSize &size)
{
    return KisStudioPicker::samplePicture(size).convertToFormat(QImage::Format_ARGB32);
}

class Panel : public KisConfigWidget, public KisEffectCanvasEditor
{
    Q_OBJECT
    Q_INTERFACES(KisEffectCanvasEditor)
public:
    Panel(QWidget *parent, const QString &filterId, KisPaintDeviceSP device)
        : KisConfigWidget(parent)
        , m_filter(KisFilterRegistry::instance()->value(filterId))
    {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

        // the pixels as they were before the preview changes them; their tiles are shared until then
        if (device) {
            m_original = fullSizeCopy(device);
            const QRect bounds = m_original->exactBounds();
            if (!bounds.isEmpty()) m_crop = QRect(busiestPoint(m_original, bounds) - QPoint(tileSize.width() / 2, tileSize.height() / 2), tileSize);
            // line art and the like: their effect hardly shows on so little, a painting shows it
            if (!m_crop.isEmpty() && showsLittle(m_original->convertToQImage(nullptr, m_crop))) m_crop = QRect();
        }
        if (m_crop.isEmpty()) {
            m_original = new KisPaintDevice(device ? device->colorSpace() : KoColorSpaceRegistry::instance()->rgb8());
            m_original->convertFromQImage(fallbackPicture(tileSize), nullptr);
            m_crop = QRect(QPoint(), tileSize);
            m_original->setDefaultBounds(new PictureBounds(m_crop));
        }
        m_config = defaults();

        m_pictureTimer.setSingleShot(true);
        m_pictureTimer.setInterval(60);
        connect(&m_pictureTimer, &QTimer::timeout, this, &Panel::refreshPictures);
    }

    KisPaintDeviceSP original() const { return m_original; }

    enum class Pictures {
        /// A part of the layer at its own size: patterns and detail show as they will
        Detail,
        /// All of the layer, small: a look shows
        Whole,
        /// Words, where a picture of the layer wouldn't tell the choices apart
        None,
    };

    void addTiles(const QVector<Tile> &tiles, Test shown = {}, Pictures pictures = Pictures::Detail)
    {
        auto *chips = new KisStudioChips(this);
        if (pictures != Pictures::None) chips->setTileColumns(tiles.size(), qMax(56, 2 * fontMetrics().height() + 26));
        else chips->setTabs(true);
        layout()->addWidget(chips);
        const int index = m_tiles.size();
        m_tiles << TileRow{chips, tiles, shown, {}, pictures};
        connect(chips, &KisStudioChips::clicked, this, [this, index](int tile) {
            const TileRow &row = m_tiles[index];
            if (tile < 0 || tile >= row.tiles.size()) return;
            row.tiles[tile].edit(*m_config);
            changed(true);
        });
    }

    /// A setting that is on or off, as a switch
    void addSwitch(const QString &name, Test on, Edit toggle)
    {
        auto *chips = new KisStudioChips(this);
        chips->setSwitches(true);
        layout()->addWidget(chips);
        m_switches << SwitchRow{chips, name, on};
        connect(chips, &KisStudioChips::clicked, this, [this, toggle](int) {
            toggle(*m_config);
            changed(true);
        });
    }

    KisStudioValueRow *addValue(const Value &value)
    {
        auto *row = new KisStudioValueRow(value.name, this);
        row->setClickSetsValue(true);
        row->setRange(value.range);
        row->setDefaultValue(value.read(*defaults()));
        layout()->addWidget(row);
        const int index = m_values.size();
        m_values << qMakePair(value, row);
        connect(row, &KisStudioValueRow::valueChanged, this, [this, index](qreal v) {
            m_values[index].first.write(*m_config, v);
            changed(false);
        });
        return row;
    }

    void addColor(const Color &color)
    {
        auto *row = new QWidget(this);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        rowLayout->addWidget(new QLabel(color.name, row), 1);
        auto *field = colorField(row);
        field->setAlphaEnabled(color.alpha);
        field->setDefaultColor(color.read(*defaults()).toQColor());
        rowLayout->addWidget(field);
        layout()->addWidget(row);
        const int index = m_colors.size();
        m_colors << ColorRow{color, row, field};
        connect(field, &KisStudioColorField::colorChanged, this, [this, index](const QColor &c) {
            m_colors[index].color.write(*m_config, KoColor(c, KoColorSpaceRegistry::instance()->rgb8()));
            changed(true);
        });
    }

    /// A color field whose eyedropper takes the layer's own pixels, as they were before the effect
    KisStudioColorField *colorField(QWidget *parent)
    {
        auto *field = new KisStudioColorField(parent);
        field->setSampler([this](const QPoint &pixel, QColor *color) {
            if (!m_original || !m_original->defaultBounds()->bounds().contains(pixel)) return false;
            KoColor taken;
            m_original->pixel(pixel.x(), pixel.y(), &taken);
            *color = taken.toQColor();
            return true;
        });
        return field;
    }

    /// A row of the panel's own, refreshed by \p refresh whenever the settings change
    void addRow(QWidget *row, std::function<void()> refresh)
    {
        layout()->addWidget(row);
        m_ownRows << refresh;
    }

    /// The settings as they are, for rows of the panel's own; edited() after changing them
    Config &config()
    {
        return *m_config;
    }

    void edited()
    {
        changed(true);
    }

    /// A drag on the image up or down changes value \p vertical, sideways value \p horizontal (-1: none)
    void dragValues(int vertical, int horizontal)
    {
        m_vertical = vertical;
        m_horizontal = horizontal;
    }

    /// A drag on the image draws a line whose length is value \p length and whose direction is value \p angle
    void dragLine(int length, int angle)
    {
        m_lineLength = length;
        m_lineAngle = angle;
    }

    /// A press on the image without a drag hands over its pixel's color
    void pickColor(std::function<void(Config &, const KoColor &)> pick)
    {
        m_pick = pick;
    }

    /// What the tiles' pictures change from the settings in use, so each shows its kind on so small a picture
    void forPictures(Edit edit)
    {
        m_forPictures = edit;
    }

    /// Settings that follow from the others, made before every change goes out
    void settle(Edit settle)
    {
        m_settle = settle;
    }

    /// Settings a panel needs made right from the layer (its color model)
    void prepare(Edit prepare)
    {
        m_prepare = prepare;
        m_prepare(*m_config);
    }

    void finish()
    {
        static_cast<QVBoxLayout *>(layout())->addStretch();
        refresh();
    }

    void setConfiguration(const KisPropertiesConfigurationSP config) override
    {
        m_config = defaults();
        if (config) {
            const QMap<QString, QVariant> properties = config->getProperties();
            for (auto it = properties.constBegin(); it != properties.constEnd(); ++it) m_config->setProperty(it.key(), it.value());
        }
        if (m_prepare) m_prepare(*m_config);
        refresh();
    }

    KisPropertiesConfigurationSP configuration() const override
    {
        KisFilterConfigurationSP config = m_config->clone();
        if (m_settle) m_settle(*config);
        return config;
    }

    QSize sizeHint() const override
    {
        const int width = 380;
        return QSize(width, qMax(layout()->heightForWidth(width), layout()->sizeHint().height()));
    }

    void canvasHovered(const QPoint &) override
    {
    }

    Qt::Orientations canvasAxes() const override
    {
        if (m_lineLength >= 0 && m_lineAngle >= 0) return Qt::Horizontal | Qt::Vertical;
        Qt::Orientations axes;
        if (m_vertical >= 0) axes |= Qt::Vertical;
        if (m_horizontal >= 0) axes |= Qt::Horizontal;
        return axes ? axes : Qt::Horizontal | Qt::Vertical;
    }

    bool canvasPressed(const QPoint &pixel) override
    {
        m_dragging = true;
        m_moved = false;
        m_pressedAt = pixel;
        m_startValues.clear();
        for (const auto &value : m_values) m_startValues << value.second->value();
        return true;
    }

    QString canvasDragged(const QPointF &moved, const QPointF &) override
    {
        if (!m_dragging) return QString();
        // the overlay calls with nothing moved at the press, to show the value
        if (!m_moved && moved.isNull()) return QString();
        m_moved = true;
        if (m_lineLength >= 0 && m_lineAngle >= 0) {
            setValue(m_lineLength, std::hypot(moved.x(), moved.y()));
            // clockwise from the right, as the image's y grows down
            qreal angle = qRadiansToDegrees(std::atan2(moved.y(), moved.x()));
            if (angle < 0) angle += 360;
            setValue(m_lineAngle, angle);
            return shown(m_lineLength) + QStringLiteral("  ") + shown(m_lineAngle);
        }
        QString text;
        auto drag = [&](int index, qreal by, bool up) {
            if (index < 0) return;
            const Value &value = m_values[index].first;
            const qreal start = m_startValues.value(index);
            const qreal span = (value.usualMaximum > value.range.minimum ? value.usualMaximum : value.range.maximum) - value.range.minimum;
            setValue(index, KisValueSteps::draggedBy(start, up ? -by : by, span, value.range.size, value.range.minimum));
        };
        drag(m_vertical, moved.y(), true);
        drag(m_horizontal, moved.x(), false);
        const bool across = m_horizontal >= 0 && (m_vertical < 0 || std::abs(moved.x()) > std::abs(moved.y()));
        const int named = across ? m_horizontal : m_vertical;
        if (named >= 0) text = shown(named);
        return text;
    }

    void canvasReleased() override
    {
        if (m_dragging && !m_moved && m_pick) {
            KoColor color;
            if (m_original->pixel(m_pressedAt.x(), m_pressedAt.y(), &color)) {
                m_pick(*m_config, color);
                changed(true);
            }
        }
        m_dragging = false;
    }

private:
    struct TileRow {
        KisStudioChips *chips;
        QVector<Tile> tiles;
        Test shown;
        QVector<QImage> pictures;
        Pictures kind;
        /// Which tiles were lit when the tiles were last set, and whether their pictures changed since
        QVector<bool> lit;
        bool newPictures {true};
    };
    struct SwitchRow {
        KisStudioChips *chips;
        QString name;
        Test on;
    };
    struct ColorRow {
        Color color;
        QWidget *row;
        KisStudioColorField *field;
    };

    KisFilterConfigurationSP defaults() const
    {
        KisFilterConfigurationSP config = m_filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
        if (m_prepare) m_prepare(*config);
        return config;
    }

    QString shown(int index) const
    {
        const KisStudioValueRow *row = m_values[index].second;
        const KisStudioValueRow::Range &range = m_values[index].first.range;
        return row->name() + QStringLiteral(" ") + QString::number(row->value(), 'f', range.decimals) + range.suffix;
    }

    void setValue(int index, qreal value)
    {
        const Value &spec = m_values[index].first;
        value = qBound(spec.range.minimum, value, spec.range.maximum);
        spec.write(*m_config, value);
        m_values[index].second->setValue(value);
        changed(false);
    }

    void changed(bool refreshRows)
    {
        if (refreshRows) refresh();
        else {
            refreshShown();
            refreshTiles();
        }
        m_pictureTimer.start();
        Q_EMIT sigConfigurationItemChanged();
    }

    /// Which rows show, as the settings decide
    void refreshShown()
    {
        for (const auto &value : m_values) value.second->setVisible(!value.first.shown || value.first.shown(*m_config));
        for (const ColorRow &color : m_colors) color.row->setVisible(!color.color.shown || color.color.shown(*m_config));
    }

    void refresh()
    {
        for (const auto &value : m_values) {
            value.second->setVisible(!value.first.shown || value.first.shown(*m_config));
            value.second->setValue(value.first.read(*m_config));
        }
        for (const ColorRow &color : m_colors) {
            color.row->setVisible(!color.color.shown || color.color.shown(*m_config));
            color.field->setColor(color.color.read(*m_config).toQColor());
        }
        for (const SwitchRow &row : m_switches) {
            KisStudioChips::Chip chip;
            chip.text = row.name;
            chip.on = row.on(*m_config);
            row.chips->setChips({chip});
        }
        Q_FOREACH (const std::function<void()> &refreshRow, m_ownRows) {
            if (refreshRow) refreshRow();
        }
        refreshTiles();
        if (!m_pictured) refreshPictures();
    }

    void refreshTiles()
    {
        for (TileRow &row : m_tiles) {
            row.chips->setVisible(!row.shown || row.shown(*m_config));
            QVector<bool> lit;
            for (const Tile &tile : row.tiles) lit << tile.on(*m_config);
            if (lit == row.lit && !row.newPictures) continue;
            row.lit = lit;
            row.newPictures = false;
            QVector<KisStudioChips::Chip> chips;
            for (int i = 0; i < row.tiles.size(); i++) {
                KisStudioChips::Chip chip;
                chip.text = row.tiles[i].name;
                chip.on = lit[i];
                if (row.tiles[i].drawing) chip.picture = row.tiles[i].drawing;
                // patterns at their own size, not smoothed
                else if (i < row.pictures.size()) chip.picture = tilePicture(row.pictures[i], false, row.kind == Pictures::Detail);
                chips << chip;
            }
            row.chips->setChips(chips);
        }
    }

    /// Each tile as the layer would look with it
    void refreshPictures()
    {
        m_pictured = true;
        for (TileRow &row : m_tiles) {
            row.pictures.clear();
            row.newPictures = true;
            if (row.chips->isHidden() || row.kind == Pictures::None) continue;
            for (const Tile &tile : row.tiles) {
                if (tile.drawing) {
                    row.pictures << QImage();
                    continue;
                }
                KisFilterConfigurationSP config = m_config->clone();
                tile.edit(*config);
                if (m_forPictures) m_forPictures(*config);
                if (m_settle) m_settle(*config);
                row.pictures << applied(config, row.kind == Pictures::Whole);
            }
        }
        refreshTiles();
    }

    QImage applied(KisFilterConfigurationSP config, bool whole) const
    {
        config = config->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance());
        if (whole) {
            if (!m_whole) {
                const QRect bounds = m_original->exactBounds().isEmpty() ? m_crop : m_original->exactBounds();
                const QSize size = bounds.size().scaled(192, 192, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
                m_whole = m_original->createThumbnailDevice(size.width(), size.height(), bounds);
                m_whole->setDefaultBounds(new PictureBounds(QRect(QPoint(), size)));
            }
            KisPaintDeviceSP work = new KisPaintDevice(*m_whole);
            const QRect rect = work->defaultBounds()->bounds();
            m_filter->process(work, rect, config);
            return work->convertToQImage(nullptr, rect);
        }
        const QRect needed = m_filter->neededRect(m_crop, config, 0) | m_crop;
        KisPaintDeviceSP work = new KisPaintDevice(m_original->colorSpace());
        work->setDefaultBounds(m_original->defaultBounds());
        KisPainter::copyAreaOptimized(needed.topLeft(), m_original, work, needed);
        m_filter->process(work, m_crop, config);
        return work->convertToQImage(nullptr, m_crop);
    }

    KisFilterSP m_filter;
    KisPaintDeviceSP m_original;
    QRect m_crop;
    KisFilterConfigurationSP m_config;
    QVector<TileRow> m_tiles;
    QVector<QPair<Value, KisStudioValueRow *>> m_values;
    QVector<ColorRow> m_colors;
    /// The wheel whose eyedropper is on, -1: none
    QVector<SwitchRow> m_switches;
    QVector<std::function<void()>> m_ownRows;
    /// All of the layer, small, made when a row first needs it
    mutable KisPaintDeviceSP m_whole;
    Edit m_settle;
    Edit m_forPictures;
    Edit m_prepare;
    std::function<void(Config &, const KoColor &)> m_pick;
    QTimer m_pictureTimer;
    bool m_pictured {false};

    int m_vertical {-1};
    int m_horizontal {-1};
    int m_lineLength {-1};
    int m_lineAngle {-1};
    bool m_dragging {false};
    bool m_moved {false};
    QPoint m_pressedAt;
    QVector<qreal> m_startValues;
};

KisStudioValueRow::Range range(qreal minimum, qreal maximum, int decimals, const QString &suffix, bool size = false)
{
    KisStudioValueRow::Range r;
    r.minimum = minimum;
    r.maximum = maximum;
    r.decimals = decimals;
    r.suffix = suffix;
    r.size = size;
    // a direction is shown as a dial
    r.angle = suffix == QStringLiteral("°");
    return r;
}

std::function<qreal(const Config &)> reads(const QString &key, qreal fallback)
{
    return [key, fallback](const Config &c) { return c.getDouble(key, fallback); };
}

std::function<void(Config &, qreal)> writes(const QString &key, bool whole = false)
{
    return [key, whole](Config &c, qreal v) { c.setProperty(key, whole ? QVariant(qRound(v)) : QVariant(v)); };
}

const QString px = QStringLiteral(" px");
const QString degrees = QStringLiteral("°");
const QString percent = QStringLiteral("%");

// Halftone

/// The screens of the settings: one for the lightness or transparency, or one a color channel
QStringList screensOf(const Config &c)
{
    const QString mode = c.getString("mode", QStringLiteral("intensity"));
    if (mode != QLatin1String("independent_channels")) return {mode + QStringLiteral("_")};
    const QString model = c.getString("color_model_id");
    const int channels = model == CMYKAColorModelID.id() ? 4 : 3;
    QStringList screens;
    for (int i = 0; i < channels; i++) screens << model + QStringLiteral("_channel") + QString::number(i) + QStringLiteral("_");
    return screens;
}

QString screenKey(const QString &screen, const char *key)
{
    return screen + QStringLiteral("generator_screentone_") + QLatin1String(key);
}

/// Each color channel's screen turned from the others, as print screens are, around the angle chosen
qreal channelTurn(const QString &screen)
{
    static const QHash<QString, qreal> turns = {
        {QStringLiteral("RGBA_channel0_"), -30}, {QStringLiteral("RGBA_channel1_"), 0}, {QStringLiteral("RGBA_channel2_"), 30},
        {QStringLiteral("CMYKA_channel0_"), -30}, {QStringLiteral("CMYKA_channel1_"), 30}, {QStringLiteral("CMYKA_channel2_"), -45},
        {QStringLiteral("CMYKA_channel3_"), 0},
    };
    return turns.value(screen);
}

qreal screenSize(const Config &c)
{
    const QString screen = screensOf(c).first();
    if (c.getInt(screenKey(screen, "size_mode"), 0) == 1) return c.getDouble(screenKey(screen, "size_x"), 10);
    return c.getDouble(screenKey(screen, "resolution"), 300) / std::max(0.01, c.getDouble(screenKey(screen, "frequency_x"), 30));
}

void setOnScreens(Config &c, const char *key, const QVariant &value)
{
    Q_FOREACH (const QString &screen, screensOf(c)) {
        c.setProperty(screen + QStringLiteral("generator"), QStringLiteral("screentone"));
        c.setProperty(screenKey(screen, key), value);
    }
}

void setScreenSize(Config &c, qreal size)
{
    setOnScreens(c, "size_mode", 1);
    setOnScreens(c, "keep_size_square", true);
    setOnScreens(c, "size_x", size);
    setOnScreens(c, "size_y", size);
}

qreal screenAngle(const Config &c)
{
    const QString screen = screensOf(c).first();
    return c.getDouble(screenKey(screen, "rotation"), 45) - channelTurn(screen);
}

void setScreenAngle(Config &c, qreal angle)
{
    Q_FOREACH (const QString &screen, screensOf(c)) {
        c.setProperty(screen + QStringLiteral("generator"), QStringLiteral("screentone"));
        c.setProperty(screenKey(screen, "rotation"), angle + channelTurn(screen));
    }
}

void setHardness(Config &c, qreal hardness)
{
    Q_FOREACH (const QString &screen, screensOf(c)) c.setProperty(screen + QStringLiteral("hardness"), hardness);
}

/// Switches to \p mode, carrying the shape, size, angle and hardness over to its screens
void setHalftoneMode(Config &c, const QString &mode)
{
    if (c.getString("mode") == mode) return;
    const QString from = screensOf(c).first();
    const int pattern = c.getInt(screenKey(from, "pattern"), 0);
    const int shape = c.getInt(screenKey(from, "shape"), 0);
    const qreal size = screenSize(c);
    const qreal angle = screenAngle(c);
    const qreal hardness = c.getDouble(from + QStringLiteral("hardness"), 80);
    c.setProperty("mode", mode);
    setOnScreens(c, "pattern", pattern);
    setOnScreens(c, "shape", shape);
    setScreenSize(c, size);
    setScreenAngle(c, angle);
    setHardness(c, hardness);
}

KisConfigWidget *halftone(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("halftone"), device);
    const QString model = panel->original()->colorSpace()->colorModelId().id();
    panel->prepare([model](Config &c) { c.setProperty("color_model_id", model); });

    auto shapeTile = [](const QString &name, int pattern, int shape) {
        return Tile{name,
                    [pattern, shape](Config &c) {
                        setOnScreens(c, "pattern", pattern);
                        setOnScreens(c, "shape", shape);
                    },
                    [pattern, shape](const Config &c) {
                        const QString screen = screensOf(c).first();
                        return c.getInt(screenKey(screen, "pattern"), 0) == pattern && c.getInt(screenKey(screen, "shape"), 0) == shape;
                    }};
    };
    panel->addTiles({shapeTile(i18nc("halftone shape", "Dots"), 0, 0), shapeTile(i18nc("halftone shape", "Diamonds"), 0, 2),
                     shapeTile(i18nc("halftone shape", "Squares"), 0, 3), shapeTile(i18nc("halftone shape", "Lines"), 1, 0),
                     shapeTile(i18nc("halftone shape", "Waves"), 1, 1)});

    // what the screens make: ink on paper, ink alone, or each color channel screened
    const bool colors = model == RGBAColorModelID.id() || model == CMYKAColorModelID.id();
    auto inkTile = [](const QString &name, int paper) {
        return Tile{name,
                    [paper](Config &c) {
                        setHalftoneMode(c, QStringLiteral("intensity"));
                        c.setProperty("intensity_background_opacity", paper);
                    },
                    [paper](const Config &c) {
                        return c.getString("mode") == QLatin1String("intensity") && (c.getInt("intensity_background_opacity", 100) > 0) == (paper > 0);
                    }};
    };
    QVector<Tile> inks = {inkTile(i18nc("halftone: ink on paper", "Paper"), 100), inkTile(i18nc("halftone: ink alone, see-through between", "Clear"), 0)};
    if (colors) {
        inks << Tile{i18nc("halftone: each color channel screened", "Color"),
                     [](Config &c) { setHalftoneMode(c, QStringLiteral("independent_channels")); },
                     [](const Config &c) { return c.getString("mode") == QLatin1String("independent_channels"); }};
    }
    panel->addTiles(inks);
    // a screen larger than a third of a tile shows one blot, not dots or lines
    panel->forPictures([](Config &c) { setScreenSize(c, qMin(screenSize(c), tileSize.height() / 3.0)); });

    const int size = 0;
    panel->addValue({i18nc("halftone setting", "Size"), range(2, 400, 1, px, true), screenSize, setScreenSize, {}, 60});
    const int angle = 1;
    panel->addValue({i18nc("halftone setting", "Angle"), range(-90, 90, 0, degrees), screenAngle, setScreenAngle, {}, 90});
    panel->addValue({i18nc("halftone setting", "Softness"), range(0, 100, 0, percent),
                     [](const Config &c) { return 100 - c.getDouble(screensOf(c).first() + QStringLiteral("hardness"), 80); },
                     [](Config &c, qreal v) { setHardness(c, 100 - v); }, {}, 100});

    auto intensity = [](const Config &c) { return c.getString("mode", QStringLiteral("intensity")) == QLatin1String("intensity"); };
    panel->addColor({i18nc("halftone: the color of the dots", "Ink"),
                     [](const Config &c) { return c.getColor("intensity_foreground_color", KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8())); },
                     [](Config &c, const KoColor &color) {
                         QVariant v;
                         v.setValue(color);
                         c.setProperty("intensity_foreground_color", v);
                     },
                     intensity});
    panel->addColor({i18nc("halftone: the color between the dots", "Paper"),
                     [](const Config &c) { return c.getColor("intensity_background_color", KoColor(Qt::white, KoColorSpaceRegistry::instance()->rgb8())); },
                     [](Config &c, const KoColor &color) {
                         QVariant v;
                         v.setValue(color);
                         c.setProperty("intensity_background_color", v);
                     },
                     [intensity](const Config &c) { return intensity(c) && c.getInt("intensity_background_opacity", 100) > 0; }});
    panel->dragValues(size, angle);
    panel->finish();
    return panel;
}

// Blurs

KisConfigWidget *motionBlur(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("motion blur"), device);
    panel->addValue({i18nc("motion blur setting", "Length"), range(0, 256, 0, px, true), reads("blurLength", 5), writes("blurLength", true), {}, 64});
    panel->addValue({i18nc("motion blur setting", "Angle"), range(0, 360, 0, degrees), reads("blurAngle", 0), writes("blurAngle", true), {}, 360});
    panel->dragLine(0, 1);
    panel->finish();
    return panel;
}

KisConfigWidget *lensBlur(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("lens blur"), device);
    auto shape = [](const QString &name, const char *key, int sides) {
        const QString id = QLatin1String(key);
        // the iris, as the blur spreads a bright point
        Drawing drawing = [sides](QPainter &painter, const QRectF &rect) {
            QPolygonF iris;
            const qreal radius = 0.36 * std::min(rect.width(), rect.height());
            for (int i = 0; i < sides; i++) {
                const qreal angle = 2 * M_PI * i / sides - M_PI / 2;
                iris << rect.center() + radius * QPointF(std::cos(angle), std::sin(angle));
            }
            QColor ink = QGuiApplication::palette().color(QPalette::WindowText);
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            ink.setAlphaF(0.85);
            painter.setBrush(ink);
            painter.drawPolygon(iris);
            painter.restore();
        };
        return Tile{name, [id](Config &c) { c.setProperty("irisShape", id); }, [id](const Config &c) { return c.getString("irisShape") == id; }, drawing};
    };
    panel->addTiles({shape(QString(), "Triangle", 3), shape(QString(), "Quadrilateral (4)", 4), shape(QString(), "Pentagon (5)", 5),
                     shape(QString(), "Hexagon (6)", 6), shape(QString(), "Octagon (8)", 8)});
    panel->addValue({i18nc("lens blur setting", "Size"), range(1, 256, 0, px, true), reads("irisRadius", 5), writes("irisRadius", true), {}, 64});
    panel->addValue({i18nc("lens blur setting", "Angle"), range(0, 360, 0, degrees), reads("irisRotation", 0), writes("irisRotation", true), {}, 360});
    // the kernel's half size, as Krita's panel writes it: half the iris polygon's bounds
    panel->settle([](Config &c) {
        const QString shape = c.getString("irisShape");
        const int sides = shape == QLatin1String("Triangle") ? 3 : shape.mid(shape.indexOf(QLatin1Char('(')) + 1, 1).toInt();
        if (sides < 3) return;
        QPolygonF iris;
        qreal angle = 0;
        for (int i = 0; i < sides; i++) {
            iris << QPointF(0.5 * std::cos(angle), 0.5 * std::sin(angle));
            angle += 2 * M_PI / sides;
        }
        QTransform transform;
        transform.rotate(c.getInt("irisRotation", 0));
        const int radius = c.getInt("irisRadius", 5);
        transform.scale(radius * 2, radius * 2);
        const QRect rect = transform.map(iris).boundingRect().toAlignedRect();
        c.setProperty("halfWidth", int(std::ceil(rect.width() / 2.0)));
        c.setProperty("halfHeight", int(std::ceil(rect.height() / 2.0)));
    });
    panel->dragValues(0, 1);
    panel->finish();
    return panel;
}

KisConfigWidget *pixelize(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("pixelize"), device);
    panel->addValue({i18nc("pixelize setting", "Size"), range(1, 512, 0, px, true), reads("pixelWidth", 10),
                     [](Config &c, qreal v) {
                         c.setProperty("pixelWidth", qRound(v));
                         c.setProperty("pixelHeight", qRound(v));
                         c.setProperty("keepAspect", true);
                     },
                     {}, 64});
    panel->dragValues(0, -1);
    panel->finish();
    return panel;
}

// Colors

KisConfigWidget *colorToAlpha(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("colortoalpha"), device);
    panel->addColor({i18nc("color to alpha: the color made see-through", "Color"),
                     [](const Config &c) {
                         const QColor color = c.getProperty("targetcolor").value<QColor>();
                         return KoColor(color.isValid() ? color : QColor(Qt::white), KoColorSpaceRegistry::instance()->rgb8());
                     },
                     [](Config &c, const KoColor &color) { c.setProperty("targetcolor", color.toQColor()); },
                     {}});
    panel->addValue({i18nc("color to alpha: how far a color may be from it", "Range"), range(0, 255, 0, QString()), reads("threshold", 100),
                     writes("threshold", true), {}, 255});
    panel->pickColor([](Config &c, const KoColor &color) { c.setProperty("targetcolor", color.toQColor()); });
    panel->dragValues(0, -1);
    panel->finish();
    return panel;
}

KisConfigWidget *replaceColor(QWidget *parent, KisPaintDeviceSP device)
{
    auto colorOf = [](const char *key, const QColor &fallback) {
        return [key, fallback](const Config &c) {
            const QColor color(c.getString(key, fallback.name(QColor::HexArgb)));
            return KoColor(color.isValid() ? color : fallback, KoColorSpaceRegistry::instance()->rgb8());
        };
    };
    auto writesColor = [](const char *key) {
        return [key](Config &c, const KoColor &color) { c.setProperty(key, color.toQColor().name(QColor::HexArgb)); };
    };
    auto *panel = new Panel(parent, QStringLiteral("skissa_replace"), device);
    auto mode = [](const QString &name, bool background) {
        return Tile{name, [background](Config &c) { c.setProperty("background", background); },
                    [background](const Config &c) { return c.getBool("background", true) == background; }};
    };
    panel->addTiles({mode(i18nc("replace color: the color is under everything, as paper is", "Background"), true),
                     mode(i18nc("replace color: only the color and its soft edges", "Color"), false)},
                    {}, Panel::Pictures::Whole);
    panel->addColor({i18nc("replace color: the color replaced", "Replace"), colorOf("color", Qt::white), [](Config &c, const KoColor &color) {
                         QColor opaque = color.toQColor();
                         opaque.setAlpha(255);
                         c.setProperty("color", opaque.name(QColor::HexArgb));
                     },
                     {}});
    panel->addColor({i18nc("replace color: what takes its place", "With"), colorOf("with", Qt::transparent), writesColor("with"), {}, true});
    panel->addValue({i18nc("replace color: how far a color may be from it and still count", "Range"), range(0, 100, 0, QStringLiteral("%")),
                     [](const Config &c) { return c.getDouble("range", 0.02) * 100; }, [](Config &c, qreal v) { c.setProperty("range", v / 100); },
                     {}, 30});
    KisPaintDeviceSP original = panel->original();
    auto picture = std::make_shared<QImage>();
    panel->pickColor([original, picture](Config &c, const KoColor &color) mutable {
        if (color.opacityU8() == OPACITY_TRANSPARENT_U8) return;
        QColor opaque = color.toQColor();
        opaque.setAlpha(255);
        c.setProperty("color", opaque.name(QColor::HexArgb));
        // a color most of the layer is lies under the rest, as paper does
        if (picture->isNull()) {
            const QRect bounds = original->exactBounds();
            if (bounds.isEmpty()) return;
            const QSize size = bounds.size().scaled(256, 256, Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
            *picture = original->createThumbnailUncached(size.width(), size.height(), bounds).convertToFormat(QImage::Format_ARGB32);
        }
        const int near = qMax(10, qRound(c.getDouble("range", 0.02) * 255));
        qint64 seen = 0;
        qint64 same = 0;
        for (int y = 0; y < picture->height(); y++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(picture->constScanLine(y));
            for (int x = 0; x < picture->width(); x++) {
                if (qAlpha(line[x]) < 128) continue;
                seen++;
                if (qAbs(qRed(line[x]) - opaque.red()) <= near && qAbs(qGreen(line[x]) - opaque.green()) <= near
                    && qAbs(qBlue(line[x]) - opaque.blue()) <= near) {
                    same++;
                }
            }
        }
        c.setProperty("background", seen > 0 && same * 10 >= seen * 3);
    });
    panel->dragValues(0, -1);
    panel->finish();
    return panel;
}

KisConfigWidget *desaturate(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("desaturate"), device);
    // Krita's types: lightness, luminance (Rec. 709), luma (Rec. 601, left out: it looks the same as 709), average, darkest
    // channel, brightest channel
    auto way = [](const QString &name, int type) {
        return Tile{name, [type](Config &c) { c.setProperty("type", type); }, [type](const Config &c) { return c.getInt("type", 0) == type; }};
    };
    panel->addTiles({way(i18nc("desaturate: gray halfway between the brightest and darkest channels (HSL lightness)", "Lightness"), 0),
                     way(i18nc("desaturate: gray by the light given off (Rec. 709)", "Luminance"), 1),
                     way(i18nc("desaturate: gray from the channels' average", "Average"), 3),
                     way(i18nc("desaturate: gray from the darkest channel", "Darkest"), 4),
                     way(i18nc("desaturate: gray from the brightest channel", "Brightest"), 5)},
                    {}, Panel::Pictures::Whole);
    panel->finish();
    return panel;
}

KisConfigWidget *propagateColors(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("propagatecolors"), device);
    auto bounded = [](const Config &c) { return c.getString("expansionMode", QStringLiteral("unbounded")) == QLatin1String("bounded"); };
    panel->addTiles({Tile{i18nc("propagate colors: fill all the way", "Everywhere"), [](Config &c) { c.setProperty("expansionMode", "unbounded"); },
                          [bounded](const Config &c) { return !bounded(c); }},
                     Tile{i18nc("propagate colors: fill only so far", "Distance"), [](Config &c) { c.setProperty("expansionMode", "bounded"); }, bounded}},
                    {}, Panel::Pictures::None);
    panel->addValue({i18nc("propagate colors setting", "Distance"), range(1, 1000, 0, px, true), reads("expansionAmount", 10),
                     writes("expansionAmount"), bounded, 200});
    panel->addSwitch(i18nc("propagate colors: keep what is see-through as it is", "Keep transparency"),
                     [](const Config &c) { return c.getString("alphaChannelMode", QStringLiteral("expand")) == QLatin1String("preserve"); },
                     [](Config &c) {
                         const bool preserve = c.getString("alphaChannelMode", QStringLiteral("expand")) == QLatin1String("preserve");
                         c.setProperty("alphaChannelMode", preserve ? "expand" : "preserve");
                     });
    panel->dragValues(0, -1);
    panel->finish();
    return panel;
}

KisConfigWidget *heightToNormal(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("height to normal"), device);
    // green up (OpenGL, Blender) or down (DirectX, Unreal)
    const int yPlus = 2, yMinus = 3;
    panel->addTiles({Tile{i18nc("normal map convention", "OpenGL"), [yPlus](Config &c) { c.setProperty("greenSwizzle", yPlus); },
                          [yPlus](const Config &c) { return c.getInt("greenSwizzle", yPlus) == yPlus; }},
                     Tile{i18nc("normal map convention", "DirectX"), [yMinus](Config &c) { c.setProperty("greenSwizzle", yMinus); },
                          [yMinus](const Config &c) { return c.getInt("greenSwizzle", yPlus) == yMinus; }}},
                    {}, Panel::Pictures::None);
    panel->addValue({i18nc("height to normal map setting", "Size"), range(1, 100, 0, px, true), reads("horizRadius", 1),
                     [](Config &c, qreal v) {
                         c.setProperty("horizRadius", qRound(v));
                         c.setProperty("vertRadius", qRound(v));
                         c.setProperty("lockAspect", true);
                     },
                     {}, 20});
    panel->dragValues(0, -1);
    panel->finish();
    return panel;
}

// Gradient map

using Stops = QVector<QPair<qreal, QColor>>;

/// The stops of the settings' gradient; black to white when it has none
Stops stopsOf(const Config &c)
{
    Stops stops;
    QDomDocument document;
    if (document.setContent(c.getString("gradientXML"))) {
        const QDomElement element = document.firstChildElement();
        QGradientStops colors;
        if (element.attribute("type") == QLatin1String("stop")) {
            Q_FOREACH (const KoGradientStop &stop, KoStopGradient::fromXML(element).stops()) stops << qMakePair(stop.position, stop.color.toQColor());
        } else if (element.attribute("type") == QLatin1String("segment")) {
            KoSegmentGradientSP segments(new KoSegmentGradient(KoSegmentGradient::fromXML(element)));
            Q_FOREACH (const QGradientStop &stop, KisGradientConversion::toQGradientStops(segments)) stops << qMakePair(stop.first, stop.second);
        }
    }
    if (stops.size() < 2) stops = {qMakePair(0.0, QColor(Qt::black)), qMakePair(1.0, QColor(Qt::white))};
    std::sort(stops.begin(), stops.end(), [](const QPair<qreal, QColor> &a, const QPair<qreal, QColor> &b) { return a.first < b.first; });
    return stops;
}

/// Writes \p stops as the settings' gradient, as Krita's panel does
void setStops(Config &c, const Stops &stops)
{
    KoStopGradientSP gradient(new KoStopGradient());
    QList<KoGradientStop> list;
    for (const auto &stop : stops) list << KoGradientStop(stop.first, KoColor(stop.second, KoColorSpaceRegistry::instance()->rgb8()), COLORSTOP);
    gradient->setStops(list);
    gradient->setName(i18nc("the gradient of a gradient map effect", "Gradient map"));
    gradient->updatePreview();
    QDomDocument document;
    QDomElement element = document.createElement("gradient");
    element.setAttribute("name", gradient->name());
    element.setAttribute("md5sum", gradient->md5Sum());
    gradient->toXML(document, element);
    document.appendChild(element);
    c.setProperty("gradientXML", document.toString());
}

KisConfigWidget *gradientMap(QWidget *parent, KisPaintDeviceSP device)
{
    auto *panel = new Panel(parent, QStringLiteral("gradientmap"), device);
    // without a gradient of its own Krita maps through one made from the canvas colors: start from black to white
    panel->prepare([](Config &c) {
        if (c.getString("gradientXML").isEmpty()) setStops(c, stopsOf(c));
    });
    auto look = [](const QString &name, const Stops &stops) {
        return Tile{name, [stops](Config &c) { setStops(c, stops); },
                    [stops](const Config &c) {
                        const Stops current = stopsOf(c);
                        if (current.size() != stops.size()) return false;
                        for (int i = 0; i < stops.size(); i++) {
                            if (std::abs(current[i].first - stops[i].first) > 1e-3 || current[i].second.rgb() != stops[i].second.rgb()) return false;
                        }
                        return true;
                    }};
    };
    auto rgb = [](const char *hex) { return QColor(QLatin1String(hex)); };
    panel->addTiles({look(i18nc("gradient map look", "Noir"), {{0, rgb("#000000")}, {1, rgb("#ffffff")}}),
                     look(i18nc("gradient map look", "Sepia"), {{0, rgb("#1e130b")}, {0.55, rgb("#9c7a52")}, {1, rgb("#f6ead2")}}),
                     look(i18nc("gradient map look", "Duotone"), {{0, rgb("#1b1f4b")}, {1, rgb("#f9c784")}}),
                     look(i18nc("gradient map look", "Sunset"), {{0, rgb("#1a0b2e")}, {0.4, rgb("#9b2d5e")}, {0.75, rgb("#f08a4b")}, {1, rgb("#fde9a9")}}),
                     look(i18nc("gradient map look", "Night"), {{0, rgb("#05070d")}, {0.5, rgb("#1f4e6b")}, {1, rgb("#c9f2ff")}}),
                     look(i18nc("gradient map look", "Thermal"), {{0, rgb("#000000")}, {0.3, rgb("#4b0f8a")}, {0.6, rgb("#e5432d")}, {0.85, rgb("#ffc93c")}, {1, rgb("#ffffff")}})},
                    {}, Panel::Pictures::Whole);

    auto *bar = new KisStudioGradientBar(panel);
    auto *colorRow = new QWidget(panel);
    auto *colorLayout = new QHBoxLayout(colorRow);
    colorLayout->setContentsMargins(0, 0, 0, 0);
    colorLayout->addWidget(new QLabel(i18nc("gradient map: the color of the chosen stop", "Color"), colorRow), 1);
    auto *button = panel->colorField(colorRow);
    colorLayout->addWidget(button);
    auto showColor = [bar, button]() { button->setColor(bar->stops().value(bar->selected()).second); };
    panel->addRow(bar, [panel, bar, showColor]() {
        bar->setStops(stopsOf(panel->config()));
        showColor();
    });
    panel->addRow(colorRow, {});
    QObject::connect(bar, &KisStudioGradientBar::stopsChanged, panel, [panel, bar]() {
        setStops(panel->config(), bar->stops());
        panel->edited();
    });
    QObject::connect(bar, &KisStudioGradientBar::selectedChanged, panel, showColor);
    QObject::connect(button, &KisStudioColorField::colorChanged, panel, [panel, bar](const QColor &color) {
        Stops stops = bar->stops();
        if (bar->selected() < 0 || bar->selected() >= stops.size()) return;
        stops[bar->selected()].second = color;
        setStops(panel->config(), stops);
        panel->edited();
    });

    // how the colors meet: blended, in steps, or dithered between steps
    auto mode = [](const QString &name, int value) {
        return Tile{name, [value](Config &c) { c.setProperty("colorMode", value); }, [value](const Config &c) { return c.getInt("colorMode", 0) == value; }};
    };
    panel->addTiles({mode(i18nc("gradient map: colors blended", "Smooth"), 0), mode(i18nc("gradient map: the nearest stop's color", "Steps"), 1),
                     mode(i18nc("gradient map: dithered between stops", "Dither"), 2)},
                    {}, Panel::Pictures::None);
    panel->finish();
    return panel;
}

} // namespace

KisConfigWidget *create(const QString &filterId, QWidget *parent, KisPaintDeviceSP device)
{
    if (!KisFilterRegistry::instance()->value(filterId)) return nullptr;
    if (filterId == QLatin1String("halftone")) return halftone(parent, device);
    if (filterId == QLatin1String("motion blur")) return motionBlur(parent, device);
    if (filterId == QLatin1String("lens blur")) return lensBlur(parent, device);
    if (filterId == QLatin1String("pixelize")) return pixelize(parent, device);
    if (filterId == QLatin1String("colortoalpha")) return colorToAlpha(parent, device);
    if (filterId == QLatin1String("skissa_replace")) return replaceColor(parent, device);
    if (filterId == QLatin1String("propagatecolors")) return propagateColors(parent, device);
    if (filterId == QLatin1String("desaturate")) return desaturate(parent, device);
    if (filterId == QLatin1String("height to normal")) return heightToNormal(parent, device);
    if (filterId == QLatin1String("gradientmap")) return gradientMap(parent, device);
    return nullptr;
}

KisPaintDeviceSP fullSizeCopy(KisPaintDeviceSP device)
{
    KisPaintDeviceSP copy = new KisPaintDevice(*device);
    copy->setDefaultBounds(new PictureBounds(device->defaultBounds()->bounds()));
    return copy;
}

bool showsLittle(const QImage &picture)
{
    if (picture.isNull()) return true;
    const QImage image = picture.width() * picture.height() > 128 * 128
        ? picture.scaled(128, 128, Qt::KeepAspectRatio, Qt::FastTransformation).convertToFormat(QImage::Format_ARGB32)
        : picture.convertToFormat(QImage::Format_ARGB32);
    const int w = image.width(), h = image.height();
    if (w < 8 || h < 8) return true;
    // colors in coarse bins, all clear pixels in one: what isn't in the commonest bin is what there is to see
    QVector<int> binOf(w * h);
    QHash<int, int> bins;
    int common = 0, most = 0;
    for (int y = 0; y < h; y++) {
        const QRgb *row = reinterpret_cast<const QRgb *>(image.constScanLine(y));
        for (int x = 0; x < w; x++) {
            const QRgb pixel = row[x];
            const int bin = qAlpha(pixel) < 64 ? -1 : (qRed(pixel) >> 5) << 6 | (qGreen(pixel) >> 5) << 3 | (qBlue(pixel) >> 5);
            binOf[y * w + x] = bin;
            const int n = ++bins[bin];
            if (n > most) {
                most = n;
                common = bin;
            }
        }
    }
    // what is there, thinned by two pixels from every side: lines vanish, painted areas stay
    QVector<quint8> there(w * h);
    for (int i = 0; i < w * h; i++) there[i] = binOf[i] != common;
    for (int pass = 0; pass < 2; pass++) {
        QVector<quint8> thinned(w * h, 0);
        for (int y = 1; y < h - 1; y++) {
            for (int x = 1; x < w - 1; x++) {
                const int i = y * w + x;
                thinned[i] = there[i] && there[i - 1] && there[i + 1] && there[i - w] && there[i + w];
            }
        }
        there = thinned;
    }
    int left = 0;
    for (quint8 v : there) left += v;
    return left < w * h / 12;
}

QPoint busiestPoint(KisPaintDeviceSP device, const QRect &bounds)
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
    QRect bestBlock;
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
                bestBlock = QRect(bounds.topLeft() + QPoint(bx * bounds.width() / thumb.width(), by * bounds.height() / thumb.height()),
                                  QSize(bw * bounds.width() / thumb.width() + 1, bh * bounds.height() / thumb.height() + 1));
            }
        }
    }
    // a large layer's block is many times a tile: the busiest tile-sized window in it, at its own size
    const QSize window(96, 60);
    if (bestBlock.width() <= 2 * window.width() && bestBlock.height() <= 2 * window.height()) return best;
    const QRect area = QRect(best - QPoint(512, 512), QSize(1024, 1024)) & bestBlock.adjusted(-window.width() / 2, -window.height() / 2,
                                                                                              window.width() / 2, window.height() / 2);
    const QImage pixels = device->convertToQImage(nullptr, area).convertToFormat(QImage::Format_ARGB32);
    // each pixel's difference from its neighbours, summed over windows with a table of running sums
    const int w = pixels.width(), h = pixels.height();
    if (w < window.width() || h < window.height()) return best;
    QVector<double> table((w + 1) * (h + 1), 0.0);
    auto at = [&pixels](int x, int y) {
        const QRgb pixel = pixels.pixel(x, y);
        return qGray(pixel) * qAlpha(pixel) / 255.0;
    };
    for (int y = 0; y < h; y++) {
        double row = 0;
        for (int x = 0; x < w; x++) {
            if (x > 0 && y > 0 && x + 1 < w && y + 1 < h) {
                row += std::abs(4 * at(x, y) - at(x - 1, y) - at(x + 1, y) - at(x, y - 1) - at(x, y + 1));
            }
            table[(y + 1) * (w + 1) + x + 1] = table[y * (w + 1) + x + 1] + row;
        }
    }
    double bestSum = -1;
    QPoint corner;
    for (int y = 0; y + window.height() <= h; y += 4) {
        for (int x = 0; x + window.width() <= w; x += 4) {
            const int x2 = x + window.width(), y2 = y + window.height();
            const double sum = table[y2 * (w + 1) + x2] - table[y * (w + 1) + x2] - table[y2 * (w + 1) + x] + table[y * (w + 1) + x];
            if (sum > bestSum) {
                bestSum = sum;
                corner = QPoint(x, y);
            }
        }
    }
    return area.topLeft() + corner + QPoint(window.width() / 2, window.height() / 2);
}

std::function<void(QPainter &, const QRectF &)> tilePicture(const QImage &picture, bool whole, bool crisp)
{
    return [picture, whole, crisp](QPainter &painter, const QRectF &rect) {
        if (picture.isNull()) return;
        painter.save();
        painter.setClipPath(KisModernStyle::squircle(rect, KisModernStyle::cornerRadius));
        const QSizeF size = QSizeF(picture.size()).scaled(rect.size(), whole ? Qt::KeepAspectRatio : Qt::KeepAspectRatioByExpanding);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, !crisp);
        painter.drawImage(QRectF(rect.center() - QPointF(size.width(), size.height()) / 2, size), picture);
        painter.restore();
    };
}

} // namespace KisEffectPanels

#include "KisEffectPanels.moc"
