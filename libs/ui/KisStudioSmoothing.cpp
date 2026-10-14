/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSmoothing.h"

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHash>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QQueue>
#include <QVBoxLayout>
#include <QtMath>

#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <KoToolManager.h>

#include "KisMainWindow.h"
#include "KisViewManager.h"
#include "kis_canvas2.h"
#include "kis_config.h"
#include "tool/kis_tool_freehand.h"
#include "widgets/KisStudioChips.h"
#include "widgets/KisStudioValueRow.h"

namespace {

constexpr qreal minDistance = 3.0;
constexpr qreal maxDistance = 1000.0;
const QString lastTypeKey = QStringLiteral("StudioSmoothingLastType");

KisToolFreehand *brushTool(KisMainWindow *mainWindow)
{
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    if (!view || !view->canvasBase()) return nullptr;
    const QString id = KoToolManager::instance()->activeToolId();
    // the tools with smoothing in their options
    if (id != QLatin1String("KritaShape/KisToolBrush") && id != QLatin1String("KritaShape/KisToolMultiBrush")) return nullptr;
    return dynamic_cast<KisToolFreehand *>(KoToolManager::instance()->toolById(view->canvasBase(), id));
}

/// Both distances (fast and slow strokes) to \p distance, in the proportion they have
void setDistance(KisSmoothingOptions &options, qreal distance)
{
    const qreal ratio = options.smoothnessDistanceMin() > 0 ? options.smoothnessDistanceMax() / options.smoothnessDistanceMin() : 1.0;
    options.setSmoothnessDistanceMin(distance);
    options.setSmoothnessDistanceMax(qBound(minDistance, distance * ratio, maxDistance));
}

bool smooths(KisSmoothingOptions::SmoothingType type)
{
    return type == KisSmoothingOptions::WEIGHTED_SMOOTHING || type == KisSmoothingOptions::STABILIZER;
}

/// A pen sample of the line drawn in the preview
struct Sample {
    QPointF pos;
    qreal pressure {1.0};
    qreal time {0.0};
};

constexpr qreal previewWidth = 640;
constexpr qreal previewHeight = 170;

/// A quick wavy line as a hand draws it, with its tremor: slow at its ends, lighter where it starts and ends
QVector<Sample> handLine()
{
    QVector<Sample> line;
    const int count = 200;
    const qreal duration = 1000.0;
    for (int i = 0; i < count; i++) {
        const qreal t = i / qreal(count - 1);
        const qreal u = t * t * (3 - 2 * t);
        const qreal tremor = 5.5 * (std::sin(t * 2 * M_PI * 11) + 0.6 * std::sin(t * 2 * M_PI * 17 + 1.3) + 0.4 * std::sin(t * 2 * M_PI * 29 + 0.7));
        Sample sample;
        sample.pos = QPointF(30 + 580 * u + 2.5 * std::sin(t * 2 * M_PI * 13 + 2), 85 - 48 * std::sin(u * 2 * M_PI * 1.15 + 0.3) + tremor);
        const qreal in = qMin(1.0, t / 0.12);
        const qreal out = qMin(1.0, (1 - t) / 0.2);
        sample.pressure = qMax(0.05, in * in * (3 - 2 * in) * out * out * (3 - 2 * out));
        sample.time = t * duration;
        line << sample;
    }
    return line;
}

/// The smoothing distance at a drawing speed (0 to 1), as the freehand helper takes it
qreal distanceAt(const KisSmoothingOptions &options, qreal speed)
{
    return (1.0 - speed) * options.smoothnessDistanceMax() + speed * options.smoothnessDistanceMin();
}

/// Krita's weighted smoothing (KisToolFreehandHelper::paint) run over \p raw
QVector<Sample> weighted(const QVector<Sample> &raw, const KisSmoothingOptions &options)
{
    QVector<Sample> out {raw.first()};
    QVector<Sample> history;
    QVector<qreal> distances;
    Sample previous = raw.first();
    for (int n = 1; n < raw.size(); n++) {
        Sample info = raw[n];
        const QPointF prevPos = history.isEmpty() ? previous.pos : history.last().pos;
        distances << QLineF(prevPos, info.pos).length();
        history << info;
        if (history.size() > 3) {
            // the speed as the painting information builder measures it: view pixels per millisecond, of 30 at most
            const qreal speed = qMin(1.0, QLineF(raw[n - 1].pos, raw[n].pos).length() / qMax(1.0, raw[n].time - raw[n - 1].time) / 30.0);
            const qreal sigma = distanceAt(options, speed) / 3.0;
            const qreal gaussianWeight = 1 / (std::sqrt(2 * M_PI) * sigma);
            const qreal gaussianWeight2 = sigma * sigma;
            qreal distanceSum = 0.0;
            qreal scaleSum = 0.0;
            qreal pressure = 0.0;
            qreal baseRate = 0.0;
            qreal x = 0.0;
            qreal y = 0.0;
            for (int i = history.size() - 1; i >= 0; i--) {
                qreal rate = 0.0;
                const Sample &next = history[i];
                qreal distance = distances[i];
                if (i < history.size() - 1) {
                    qreal pressureGrad = next.pressure - history[i + 1].pressure;
                    if (pressureGrad > 0.0) {
                        pressureGrad *= 40.0 * options.tailAggressiveness() * (1.0 - next.pressure);
                        distance += pressureGrad * 3.0 * sigma;
                    }
                }
                if (gaussianWeight2 != 0.0) {
                    distanceSum += distance;
                    rate = gaussianWeight * std::exp(-distanceSum * distanceSum / (2 * gaussianWeight2));
                }
                if (history.size() - i == 1) {
                    baseRate = rate;
                } else if (baseRate / rate > 100) {
                    break;
                }
                scaleSum += rate;
                x += rate * next.pos.x();
                y += rate * next.pos.y();
                pressure += rate * next.pressure;
            }
            if (scaleSum != 0.0) {
                info.pos = QPointF(x / scaleSum, y / scaleSum);
                if (options.smoothPressure()) info.pressure = pressure / scaleSum;
                history.last() = info;
            }
        }
        out << info;
        previous = info;
    }
    return out;
}

/// Krita's stabilizer (KisToolFreehandHelper::stabilizerPollAndPaint): the pen sampled every millisecond, averaged over the last samples
QVector<Sample> stabilized(const QVector<Sample> &raw, const KisSmoothingOptions &options)
{
    const int count = qMax(3, qRound(distanceAt(options, 0.0)));
    QQueue<Sample> queue;
    for (int i = 0; i < count; i++) queue.enqueue(raw.first());
    QVector<Sample> out {raw.first()};
    Sample previous = raw.first();
    QVector<Sample> samples;
    int at = 0;
    for (qreal time = 0; time <= raw.last().time; time += 1.0) {
        while (at + 1 < raw.size() && raw[at + 1].time <= time) at++;
        samples << raw[at];
    }
    if (options.finishStabilizedCurve()) {
        for (int i = 0; i < count; i++) samples << raw.last();
    }
    Q_FOREACH (const Sample &sample, samples) {
        if (options.useDelayDistance() && QLineF(sample.pos, previous.pos).length() <= options.delayDistance()) {
            if (queue.head().pos != previous.pos) {
                for (Sample &queued : queue) queued = previous;
            }
            continue;
        }
        Sample result = sample;
        for (int i = 1; i < queue.size(); i++) {
            const qreal k = qreal(i) / (i + 1);
            result.pos = k * result.pos + (1 - k) * queue[i].pos;
            if (options.stabilizeSensors()) result.pressure = k * result.pressure + (1 - k) * queue[i].pressure;
        }
        out << result;
        previous = result;
        queue.dequeue();
        queue.enqueue(sample);
    }
    return out;
}

/// The line drawn with the smoothing as it is set: the pen's own path faint under it, where the pen let go a dot
class SmoothingPreview : public QWidget
{
    Q_OBJECT
public:
    explicit SmoothingPreview(QPointer<KisMainWindow> mainWindow, QWidget *parent)
        : QWidget(parent)
        , m_mainWindow(mainWindow)
        , m_raw(handLine())
    {
        setFixedHeight(84);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        QColor ground = ink;
        ground.setAlphaF(0.05);
        const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        painter.fillPath(KisModernStyle::squircle(box, KisModernStyle::tileRadius), ground);

        const qreal scale = qMin(width() / previewWidth, height() / previewHeight);
        const QPointF offset((width() - previewWidth * scale) / 2, (height() - previewHeight * scale) / 2);
        auto map = [&](const QPointF &p) { return offset + p * scale; };

        QVector<Sample> line = m_raw;
        KisSmoothingOptionsSP options = KisStudioSmoothing::options(m_mainWindow);
        if (options && options->smoothingType() == KisSmoothingOptions::WEIGHTED_SMOOTHING) {
            line = weighted(m_raw, *options);
        } else if (options && options->smoothingType() == KisSmoothingOptions::STABILIZER) {
            line = stabilized(m_raw, *options);
        }

        QPainterPath pen;
        pen.moveTo(map(m_raw.first().pos));
        for (int i = 1; i < m_raw.size(); i++) pen.lineTo(map(m_raw[i].pos));
        painter.setPen(QPen(KisModernStyle::secondaryInk(ink), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(pen);

        // the stroke, as thick as the pen presses
        for (int i = 1; i < line.size(); i++) {
            painter.setPen(QPen(ink, 0.8 + 2.4 * line[i].pressure, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(map(line[i - 1].pos), map(line[i].pos));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(KisModernStyle::secondaryInk(ink));
        painter.drawEllipse(map(m_raw.last().pos), 2.5, 2.5);
    }

private:
    QPointer<KisMainWindow> m_mainWindow;
    const QVector<Sample> m_raw;
};

QHash<KisMainWindow *, QPointer<KisStudioSmoothingPopup>> &popups()
{
    static QHash<KisMainWindow *, QPointer<KisStudioSmoothingPopup>> all;
    return all;
}

} // namespace

KisStudioSmoothing *KisStudioSmoothing::instance()
{
    static KisStudioSmoothing *smoothing = new KisStudioSmoothing();
    return smoothing;
}

KisSmoothingOptionsSP KisStudioSmoothing::options(KisMainWindow *mainWindow)
{
    KisToolFreehand *tool = brushTool(mainWindow);
    return tool ? tool->smoothingOptions() : KisSmoothingOptionsSP();
}

qreal KisStudioSmoothing::distanceToAmount(qreal distance)
{
    return qBound(0.0, std::log(qMax(minDistance, distance) / minDistance) / std::log(maxDistance / minDistance), 1.0);
}

qreal KisStudioSmoothing::amountToDistance(qreal amount)
{
    return minDistance * std::pow(maxDistance / minDistance, qBound(0.0, amount, 1.0));
}

qreal KisStudioSmoothing::amount(KisMainWindow *mainWindow)
{
    KisSmoothingOptionsSP options = KisStudioSmoothing::options(mainWindow);
    if (!options) return -1;
    if (!smooths(options->smoothingType())) return 0;
    // at least a sliver while on, so on is never shown as off
    return qMax(0.02, distanceToAmount(options->smoothnessDistanceMin()));
}

void KisStudioSmoothing::setAmount(KisMainWindow *mainWindow, qreal amount)
{
    KisSmoothingOptionsSP options = KisStudioSmoothing::options(mainWindow);
    if (!options) return;
    if (amount <= 0.0) {
        if (smooths(options->smoothingType())) {
            KisConfig(false).writeEntry(lastTypeKey, int(options->smoothingType()));
            setType(mainWindow, KisSmoothingOptions::SIMPLE_SMOOTHING);
        }
        return;
    }
    setDistance(*options, amountToDistance(amount));
    if (!smooths(options->smoothingType())) {
        const int last = KisConfig(true).readEntry(lastTypeKey, int(KisSmoothingOptions::WEIGHTED_SMOOTHING));
        options->setSmoothingType(last == KisSmoothingOptions::STABILIZER ? KisSmoothingOptions::STABILIZER
                                                                          : KisSmoothingOptions::WEIGHTED_SMOOTHING);
    }
    changed(mainWindow);
}

QString KisStudioSmoothing::text(KisMainWindow *mainWindow)
{
    KisSmoothingOptionsSP options = KisStudioSmoothing::options(mainWindow);
    if (!options || !smooths(options->smoothingType())) return i18nc("smoothing is off", "Off");
    return QString::number(qRound(options->smoothnessDistanceMin()));
}

void KisStudioSmoothing::setType(KisMainWindow *mainWindow, KisSmoothingOptions::SmoothingType type)
{
    KisSmoothingOptionsSP options = KisStudioSmoothing::options(mainWindow);
    if (!options) return;
    if (smooths(type)) KisConfig(false).writeEntry(lastTypeKey, int(type));
    options->setSmoothingType(type);
    changed(mainWindow);
}

void KisStudioSmoothing::changed(KisMainWindow *mainWindow)
{
    if (KisToolFreehand *tool = brushTool(mainWindow)) {
        // its options are made on first use; they read the smoothing back
        tool->optionWidgets();
        tool->updateSettingsViews();
    }
    Q_EMIT instance()->sigChanged();
}

struct KisStudioSmoothingPopup::Private
{
    QPointer<KisMainWindow> mainWindow;
    std::function<void()> everything;
    SmoothingPreview *preview {nullptr};
    KisStudioChips *kinds {nullptr};
    QWidget *settings {nullptr};
    QVBoxLayout *settingsLayout {nullptr};
    KisSmoothingOptions::SmoothingType shownType {KisSmoothingOptions::NO_SMOOTHING};
    /// While a row is being dragged, its own change doesn't rebuild the rows
    bool changing {false};
};

void KisStudioSmoothingPopup::toggle(KisMainWindow *mainWindow, QWidget *anchor, std::function<void()> everything)
{
    QPointer<KisStudioSmoothingPopup> &popup = popups()[mainWindow];
    if (popup) {
        popup->close();
        return;
    }
    popup = new KisStudioSmoothingPopup(mainWindow, everything);
    popup->popupBeside(anchor, Qt::BottomEdge);
}

KisStudioSmoothingPopup::KisStudioSmoothingPopup(KisMainWindow *mainWindow, std::function<void()> everything)
    : KisStudioPopup(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->everything = everything;
    setObjectName(QStringLiteral("studioSmoothing"));

    auto *layout = new QVBoxLayout(this);
    fitLayout(layout, KisModernStyle::space3);
    layout->setSpacing(KisModernStyle::space2);

    auto *title = new QLabel(i18nc("brush line smoothing", "Smoothing"), this);
    title->setFont(KisModernStyle::textFont(font(), 1.1, QFont::DemiBold));
    title->setContentsMargins(KisModernStyle::space1, 0, 0, 0);
    layout->addWidget(title);

    d->preview = new SmoothingPreview(d->mainWindow, this);
    layout->addWidget(d->preview);

    d->kinds = new KisStudioChips(this);
    d->kinds->setTabs(true);
    connect(d->kinds, &KisStudioChips::clicked, this, [this](int index) {
        static const KisSmoothingOptions::SmoothingType types[] = {KisSmoothingOptions::SIMPLE_SMOOTHING, KisSmoothingOptions::WEIGHTED_SMOOTHING,
                                                                  KisSmoothingOptions::STABILIZER, KisSmoothingOptions::PIXEL_PERFECT};
        if (index >= 0 && index < 4) KisStudioSmoothing::setType(d->mainWindow, types[index]);
    });
    layout->addWidget(d->kinds);

    d->settings = new QWidget(this);
    d->settingsLayout = new QVBoxLayout(d->settings);
    d->settingsLayout->setContentsMargins(0, 0, 0, 0);
    d->settingsLayout->setSpacing(KisModernStyle::space1);
    layout->addWidget(d->settings);

    auto *everythingRow = new KisStudioValueRow(i18nc("all of the tool's options", "Everything"), this);
    everythingRow->setDraggable(false);
    everythingRow->setValueText(QStringLiteral("›"));
    connect(everythingRow, &KisStudioValueRow::clicked, this, [this]() {
        std::function<void()> everything = d->everything;
        close();
        if (everything) everything();
    });
    layout->addWidget(everythingRow);

    setFixedWidth(368 + 2 * (shadowMargin() + KisModernStyle::space3));
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigChanged, this, &KisStudioSmoothingPopup::rebuild);
    rebuild();
}

KisStudioSmoothingPopup::~KisStudioSmoothingPopup()
{
    delete d;
}

void KisStudioSmoothingPopup::rebuild()
{
    KisSmoothingOptionsSP options = KisStudioSmoothing::options(d->mainWindow);
    if (!options) {
        close();
        return;
    }
    d->preview->update();
    KisSmoothingOptions::SmoothingType type = options->smoothingType();
    if (type == KisSmoothingOptions::NO_SMOOTHING) type = KisSmoothingOptions::SIMPLE_SMOOTHING;

    struct Kind {
        KisSmoothingOptions::SmoothingType type;
        QString text;
        QString tip;
    };
    const QVector<Kind> kinds = {
        {KisSmoothingOptions::SIMPLE_SMOOTHING, i18nc("smoothing is off", "Off"), i18nc("@info:tooltip", "Your line, exactly")},
        {KisSmoothingOptions::WEIGHTED_SMOOTHING, i18nc("@item:inlistbox Brush Smoothing", "Weighted"), i18nc("@info:tooltip", "Trails behind the pen and evens out")},
        {KisSmoothingOptions::STABILIZER, i18nc("@item:inlistbox Brush Smoothing", "Stabilizer"),
         i18nc("@info:tooltip", "Follows the pen on a string: steadiest, with some lag")},
        {KisSmoothingOptions::PIXEL_PERFECT, i18nc("@item:inlistbox Brush Smoothing", "Pixel"), i18nc("@info:tooltip", "Pixel art: no doubled pixels at corners")},
    };
    QVector<KisStudioChips::Chip> chips;
    for (int i = 0; i < kinds.size(); i++) {
        KisStudioChips::Chip chip;
        chip.text = kinds[i].text;
        chip.toolTip = kinds[i].tip;
        chips << chip;
        if (kinds[i].type == type) d->kinds->setCurrent(i);
    }
    d->kinds->setChips(chips);

    // a row's own change keeps the rows, so the drag goes on
    if (d->changing || (type == d->shownType && d->settingsLayout->count() > 0)) {
        Q_FOREACH (KisStudioValueRow *row, d->settings->findChildren<KisStudioValueRow *>()) {
            if (row->property("amount").toBool()) row->setValue(options->smoothnessDistanceMin());
            if (row->property("delay").toBool()) row->setValue(options->useDelayDistance() ? options->delayDistance() : 0);
        }
        return;
    }
    d->shownType = type;
    while (QLayoutItem *item = d->settingsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }

    const QPointer<KisMainWindow> mainWindow = d->mainWindow;
    auto change = [this, mainWindow](std::function<void(KisSmoothingOptions &)> apply) {
        KisSmoothingOptionsSP options = KisStudioSmoothing::options(mainWindow);
        if (!options) return;
        apply(*options);
        d->changing = true;
        KisStudioSmoothing::changed(mainWindow);
        d->changing = false;
    };
    auto addRow = [this](const QString &name, const KisStudioValueRow::Range &range, qreal value, qreal reset) {
        auto *row = new KisStudioValueRow(name, d->settings);
        row->setRange(range);
        row->setValue(value);
        row->setDefaultValue(reset);
        d->settingsLayout->addWidget(row);
        return row;
    };
    auto addSwitch = [this, change](const QString &name, bool on, std::function<void(KisSmoothingOptions &, bool)> apply) {
        auto *box = new QCheckBox(name, d->settings);
        box->setChecked(on);
        connect(box, &QCheckBox::toggled, this, [change, apply](bool checked) {
            change([apply, checked](KisSmoothingOptions &options) { apply(options, checked); });
        });
        d->settingsLayout->addWidget(box);
    };

    if (type == KisSmoothingOptions::WEIGHTED_SMOOTHING || type == KisSmoothingOptions::STABILIZER) {
        KisStudioValueRow::Range strength;
        strength.minimum = minDistance;
        strength.maximum = maxDistance;
        strength.size = true;
        auto *amount = addRow(i18nc("how much a line is smoothed", "Strength"), strength, options->smoothnessDistanceMin(), KisConfig(true).lineSmoothingDistanceMin(true));
        amount->setProperty("amount", true);
        connect(amount, &KisStudioValueRow::valueChanged, this, [change](qreal value) {
            change([value](KisSmoothingOptions &options) { setDistance(options, value); });
        });
    }
    if (type == KisSmoothingOptions::WEIGHTED_SMOOTHING) {
        KisStudioValueRow::Range percent;
        percent.maximum = 100;
        percent.suffix = QStringLiteral("%");
        auto *ending = addRow(i18n("Stroke Ending"), percent, options->tailAggressiveness() * 100, KisConfig(true).lineSmoothingTailAggressiveness(true) * 100);
        ending->setToolTip(i18nc("@info:tooltip", "How fast the line's end catches up with the pen"));
        connect(ending, &KisStudioValueRow::valueChanged, this, [change](qreal value) {
            change([value](KisSmoothingOptions &options) { options.setTailAggressiveness(value / 100); });
        });
        addSwitch(i18n("Smooth Pressure"), options->smoothPressure(), [](KisSmoothingOptions &options, bool on) { options.setSmoothPressure(on); });
    }
    if (type == KisSmoothingOptions::STABILIZER) {
        KisStudioValueRow::Range pixels;
        pixels.maximum = 500;
        pixels.suffix = i18n(" px");
        auto *delay = addRow(i18n("Delay"), pixels, options->useDelayDistance() ? options->delayDistance() : 0, 0);
        delay->setProperty("delay", true);
        delay->setToolTip(i18nc("@info:tooltip", "The line waits until the pen is this far away"));
        connect(delay, &KisStudioValueRow::valueChanged, this, [change](qreal value) {
            change([value](KisSmoothingOptions &options) {
                options.setUseDelayDistance(value > 0);
                if (value > 0) options.setDelayDistance(value);
            });
        });
        addSwitch(i18nc("the stabilized line goes on to where the pen let go", "Finish line"), options->finishStabilizedCurve(),
                  [](KisSmoothingOptions &options, bool on) { options.setFinishStabilizedCurve(on); });
        addSwitch(i18n("Smooth Pressure"), options->stabilizeSensors(), [](KisSmoothingOptions &options, bool on) { options.setStabilizeSensors(on); });
    }
    d->settings->setVisible(d->settingsLayout->count() > 0);

    layout()->activate();
    adjustSize();
    if (isVisible()) keepOnScreen();
}

#include "KisStudioSmoothing.moc"
