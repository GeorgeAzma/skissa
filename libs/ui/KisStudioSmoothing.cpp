/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSmoothing.h"

#include <QAction>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QHash>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QtMath>

#include <klocalizedstring.h>

#include <KConfigGroup>
#include <KSharedConfig>
#include <KisModernStyle.h>
#include <KoToolBase.h>
#include <KoCanvasController.h>
#include <KoToolManager.h>
#include <brushengine/kis_paintop_preset.h>
#include <kactioncollection.h>

#include "KisMainWindow.h"
#include "KisViewManager.h"
#include "kis_canvas2.h"
#include "kis_canvas_resource_provider.h"
#include "kis_painting_assistants_decoration.h"
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

/// The settings a brush keeps of its smoothing, as one line of text
QString remembered(const KisSmoothingOptions &options)
{
    return QStringList({QString::number(int(options.smoothingType())), QString::number(options.smoothnessDistanceMin()),
                        QString::number(options.smoothnessDistanceMax()), QString::number(options.tailAggressiveness()),
                        QString::number(int(options.smoothPressure())), QString::number(int(options.useScalableDistance())),
                        QString::number(options.delayDistance()), QString::number(int(options.useDelayDistance())),
                        QString::number(int(options.finishStabilizedCurve())), QString::number(int(options.stabilizeSensors()))})
        .join(QLatin1Char(';'));
}

bool recall(const QString &text, KisSmoothingOptions &options)
{
    const QStringList parts = text.split(QLatin1Char(';'));
    if (parts.size() != 10) return false;
    options.setSmoothingType(KisSmoothingOptions::SmoothingType(qBound(0, parts[0].toInt(), 4)));
    options.setSmoothnessDistanceMin(parts[1].toDouble());
    options.setSmoothnessDistanceMax(parts[2].toDouble());
    options.setTailAggressiveness(parts[3].toDouble());
    options.setSmoothPressure(parts[4].toInt());
    options.setUseScalableDistance(parts[5].toInt());
    options.setDelayDistance(parts[6].toDouble());
    options.setUseDelayDistance(parts[7].toInt());
    options.setFinishStabilizedCurve(parts[8].toInt());
    options.setStabilizeSensors(parts[9].toInt());
    return true;
}

KConfigGroup memory()
{
    return KSharedConfig::openConfig()->group("StudioBrushSmoothing");
}

/// Remembers each brush's smoothing, and gives a brush its own when it is picked
class SmoothingMemory : public QObject
{
public:
    explicit SmoothingMemory(KisMainWindow *mainWindow)
        : QObject(mainWindow)
        , m_mainWindow(mainWindow)
    {
        connect(KoToolManager::instance(), &KoToolManager::changedTool, this, [this]() { follow(); });
        connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigChanged, this, [this]() { save(); });
        QTimer::singleShot(0, this, [this]() { follow(); });
    }

    /// The brush in use, once the view has its resources
    void watchResources()
    {
        KisViewManager *view = m_mainWindow ? m_mainWindow->viewManager() : nullptr;
        KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
        if (!resources || resources == m_resources) return;
        m_resources = resources;
        connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, this, [this]() { follow(); });
    }

private:
    QString presetName() const
    {
        KisViewManager *view = m_mainWindow ? m_mainWindow->viewManager() : nullptr;
        KisPaintOpPresetSP preset = view && view->canvasResourceProvider() ? view->canvasResourceProvider()->currentPreset() : nullptr;
        return preset ? preset->name() : QString();
    }

    void save()
    {
        if (m_applying || m_preset.isEmpty() || !m_options) return;
        memory().writeEntry(m_preset, remembered(*m_options));
    }

    /// A brush or tool picked: the smoothing goes with its brush
    void follow()
    {
        watchResources();
        // what the last brush had, kept for it
        save();
        m_options = KisStudioSmoothing::options(m_mainWindow);
        m_preset = presetName();
        if (!m_options || m_preset.isEmpty()) return;
        if (KoToolBase *tool = KoToolManager::instance()->toolById(m_mainWindow->viewManager()->canvasBase(), KoToolManager::instance()->activeToolId())) {
            // changed in the tool's own options: kept as well
            for (const char *name : {"smoothingTypeChanged()", "smoothnessQualityChanged()", "smoothnessFactorChanged()", "smoothPressureChanged()",
                                     "useDelayDistanceChanged()", "delayDistanceChanged()", "finishStabilizedCurveChanged()",
                                     "stabilizeSensorsChanged()", "useScalableDistanceChanged()"}) {
                if (tool->metaObject()->indexOfSignal(name) < 0) continue;
                // as SIGNAL() writes it
                const QByteArray signal = QByteArray("2") + name;
                connect(tool, signal.constData(), KisStudioSmoothing::instance(), SIGNAL(sigToolChanged()), Qt::UniqueConnection);
            }
        }
        const QString kept = memory().readEntry(m_preset, QString());
        if (kept.isEmpty()) return;
        m_applying = true;
        if (recall(kept, *m_options)) KisStudioSmoothing::changed(m_mainWindow);
        m_applying = false;
    }

    QPointer<KisMainWindow> m_mainWindow;
    QPointer<KisCanvasResourceProvider> m_resources;
    KisSmoothingOptionsSP m_options;
    QString m_preset;
    bool m_applying {false};
};

QStringList brushTools()
{
    return {QStringLiteral("KritaShape/KisToolBrush"), QStringLiteral("KritaShape/KisToolDyna"), QStringLiteral("KritaShape/KisToolMultiBrush")};
}

QObject *activeTool(KisMainWindow *mainWindow)
{
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    if (!view || !view->canvasBase()) return nullptr;
    return KoToolManager::instance()->toolById(view->canvasBase(), KoToolManager::instance()->activeToolId());
}

} // namespace

KisStudioSmoothing *KisStudioSmoothing::instance()
{
    static KisStudioSmoothing *smoothing = new KisStudioSmoothing();
    return smoothing;
}

void KisStudioSmoothing::watch(KisMainWindow *mainWindow)
{
    new SmoothingMemory(mainWindow);
    // a change made in the tool's own options shows on the chip and in the editor too
    connect(instance(), &KisStudioSmoothing::sigToolChanged, instance(), &KisStudioSmoothing::sigChanged, Qt::UniqueConnection);
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

bool KisStudioSmoothing::pixelBrush(KisMainWindow *mainWindow)
{
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    KisPaintOpPresetSP preset = view && view->canvasResourceProvider() ? view->canvasResourceProvider()->currentPreset() : nullptr;
    if (!preset) return false;
    if (preset->name().contains(QLatin1String("pixel"), Qt::CaseInsensitive)) return true;
    // a hard tip without anti-aliasing paints whole pixels
    const QString tip = preset->settings() ? preset->settings()->getString(QStringLiteral("brush_definition")) : QString();
    return tip.contains(QLatin1String("type=\"auto_brush\"")) && tip.contains(QLatin1String("antialiasEdges=\"0\""))
        && tip.contains(QLatin1String("hfade=\"1\"")) && tip.contains(QLatin1String("vfade=\"1\""));
}

struct KisStudioStrokePage::Private
{
    QPointer<KisMainWindow> mainWindow;
    QVBoxLayout *layout {nullptr};
    /// What the rows were built for, to rebuild only when that changes
    QString shown;
    /// While a row is being dragged, its own change doesn't rebuild the rows
    bool changing {false};
    SmoothingPreview *preview {nullptr};
};

bool KisStudioStrokePage::applies(KisMainWindow *mainWindow)
{
    return mainWindow && brushTools().contains(KoToolManager::instance()->activeToolId());
}

KisStudioStrokePage::KisStudioStrokePage(KisMainWindow *mainWindow, QWidget *parent)
    : QWidget(parent)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->layout = new QVBoxLayout(this);
    d->layout->setContentsMargins(0, 0, 0, 0);
    d->layout->setSpacing(KisModernStyle::space1);
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigChanged, this, &KisStudioStrokePage::rebuild);
    rebuild();
}

KisStudioStrokePage::~KisStudioStrokePage()
{
    delete d;
}

void KisStudioStrokePage::rebuild()
{
    KisSmoothingOptionsSP options = KisStudioSmoothing::options(d->mainWindow);
    QObject *tool = activeTool(d->mainWindow);
    KisViewManager *view = d->mainWindow ? d->mainWindow->viewManager() : nullptr;
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    const bool guides = tool && tool->property("assistantSnapping").isValid() && canvas && canvas->paintingAssistantsDecoration()
        && !canvas->paintingAssistantsDecoration()->assistants().isEmpty();
    KisSmoothingOptions::SmoothingType type = options ? options->smoothingType() : KisSmoothingOptions::NO_SMOOTHING;
    if (type == KisSmoothingOptions::NO_SMOOTHING) type = KisSmoothingOptions::SIMPLE_SMOOTHING;
    const bool pixel = options && (type == KisSmoothingOptions::PIXEL_PERFECT || KisStudioSmoothing::pixelBrush(d->mainWindow));

    if (d->preview) d->preview->update();
    // a row's own change keeps the rows, so the drag goes on
    const QString shown = QStringLiteral("%1 %2 %3 %4").arg(bool(options)).arg(int(type)).arg(guides).arg(pixel);
    if (d->changing || shown == d->shown) {
        Q_FOREACH (KisStudioValueRow *row, findChildren<KisStudioValueRow *>()) {
            if (options && row->property("amount").toBool()) row->setValue(options->smoothnessDistanceMin());
            if (options && row->property("delay").toBool()) row->setValue(options->useDelayDistance() ? options->delayDistance() : 0);
        }
        return;
    }
    d->shown = shown;
    d->preview = nullptr;
    while (QLayoutItem *item = d->layout->takeAt(0)) {
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
        auto *row = new KisStudioValueRow(name, this);
        row->setRange(range);
        row->setValue(value);
        row->setDefaultValue(reset);
        d->layout->addWidget(row);
        return row;
    };
    auto addSwitch = [this](const QString &name, const QString &tip, bool on, std::function<void(bool)> apply) {
        auto *box = new QCheckBox(name, this);
        box->setChecked(on);
        box->setToolTip(tip);
        connect(box, &QCheckBox::toggled, this, apply);
        d->layout->addWidget(box);
        return box;
    };

    if (options) {
        d->preview = new SmoothingPreview(d->mainWindow, this);
        d->layout->addWidget(d->preview);

        // the kinds of smoothing; pixel perfect is a switch of its own, for pixel brushes
        auto *kinds = new KisStudioChips(this);
        kinds->setTabs(true);
        static const KisSmoothingOptions::SmoothingType types[] = {KisSmoothingOptions::SIMPLE_SMOOTHING, KisSmoothingOptions::WEIGHTED_SMOOTHING,
                                                                  KisSmoothingOptions::STABILIZER};
        QVector<KisStudioChips::Chip> chips;
        const QStringList names = {i18nc("smoothing is off", "Off"), i18nc("@item:inlistbox Brush Smoothing", "Weighted"),
                                   i18nc("@item:inlistbox Brush Smoothing", "Stabilizer")};
        const QStringList tips = {i18nc("@info:tooltip", "Your line, exactly"), i18nc("@info:tooltip", "Trails behind the pen and evens out"),
                                  i18nc("@info:tooltip", "Follows the pen on a string: steadiest, with some lag")};
        for (int i = 0; i < 3; i++) {
            KisStudioChips::Chip chip;
            chip.text = names[i];
            chip.toolTip = tips[i];
            chips << chip;
            if (types[i] == type) kinds->setCurrent(i);
        }
        kinds->setChips(chips);
        connect(kinds, &KisStudioChips::clicked, this, [mainWindow](int index) {
            if (index >= 0 && index < 3) KisStudioSmoothing::setType(mainWindow, types[index]);
        });
        d->layout->addWidget(kinds);

        if (smooths(type)) {
            KisStudioValueRow::Range strength;
            strength.minimum = minDistance;
            strength.maximum = maxDistance;
            strength.size = true;
            auto *amount = addRow(i18nc("how much a line is smoothed", "Strength"), strength, options->smoothnessDistanceMin(),
                                  KisConfig(true).lineSmoothingDistanceMin(true));
            amount->setProperty("amount", true);
            amount->setToolTip(i18nc("@info:tooltip line smoothing", "More: a steadier line, more lag"));
            connect(amount, &KisStudioValueRow::valueChanged, this, [change](qreal value) {
                change([value](KisSmoothingOptions &options) { setDistance(options, value); });
            });
        }
        if (type == KisSmoothingOptions::WEIGHTED_SMOOTHING) {
            KisStudioValueRow::Range percent;
            percent.maximum = 100;
            percent.suffix = QStringLiteral("%");
            auto *ending = addRow(i18n("Stroke Ending"), percent, options->tailAggressiveness() * 100,
                                  KisConfig(true).lineSmoothingTailAggressiveness(true) * 100);
            ending->setToolTip(i18nc("@info:tooltip", "0% the end lags → 100% it ends at the pen"));
            connect(ending, &KisStudioValueRow::valueChanged, this, [change](qreal value) {
                change([value](KisSmoothingOptions &options) { options.setTailAggressiveness(value / 100); });
            });
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
            addSwitch(i18nc("the stabilized line goes on to where the pen let go", "Finish line"),
                      i18nc("@info:tooltip", "Letting go, the line goes on to the pen"), options->finishStabilizedCurve(),
                      [change](bool on) { change([on](KisSmoothingOptions &options) { options.setFinishStabilizedCurve(on); }); });
        }
        if (smooths(type)) {
            // one switch for both kinds' pressure smoothing
            const bool smoothPressure = type == KisSmoothingOptions::STABILIZER ? options->stabilizeSensors() : options->smoothPressure();
            addSwitch(i18n("Smooth Pressure"), i18nc("@info:tooltip", "Pressure, tilt and the rest are evened out too"), smoothPressure,
                      [change](bool on) {
                          change([on](KisSmoothingOptions &options) {
                              options.setSmoothPressure(on);
                              options.setStabilizeSensors(on);
                          });
                      });
        }
        if (pixel) {
            addSwitch(i18nc("pixel art lines without doubled corners", "Pixel perfect"), i18nc("@info:tooltip", "No doubled pixels at corners"),
                      type == KisSmoothingOptions::PIXEL_PERFECT, [mainWindow](bool on) {
                          KisStudioSmoothing::setType(mainWindow, on ? KisSmoothingOptions::PIXEL_PERFECT : KisSmoothingOptions::SIMPLE_SMOOTHING);
                      });
        }
    }

    // a stroke held still at its end becomes a clean shape
    if (QAction *shapes = d->mainWindow ? d->mainWindow->actionCollection()->action(QStringLiteral("stroke_shapes")) : nullptr) {
        QCheckBox *box = addSwitch(i18nc("strokes become clean shapes", "Shapes"), shapes->toolTip(), shapes->isChecked(),
                                   [shapes](bool on) { shapes->setChecked(on); });
        connect(shapes, &QAction::toggled, box, &QCheckBox::setChecked);
    }

    if (guides) {
        // snapping to the image's guides: off, or how strongly
        auto *line = new QWidget(this);
        auto *lineLayout = new QHBoxLayout(line);
        lineLayout->setContentsMargins(0, 0, 0, 0);
        lineLayout->setSpacing(KisModernStyle::hairline);
        KisStudioValueRow::Range percent;
        percent.maximum = 100;
        percent.suffix = QStringLiteral("%");
        const bool snapping = tool->property("assistantSnapping").toBool();
        auto *snap = new KisStudioValueRow(i18nc("lines drawn are pulled onto the image's guides", "Snap to guides"), line);
        snap->setRange(percent);
        snap->setValue(snapping ? tool->property("magnetism").toInt() / 10.0 : 0);
        snap->setValueText(snapping ? QString() : i18nc("snapping is off", "Off"));
        snap->setDefaultValue(0);
        snap->setToolTip(i18nc("@info:tooltip", "0% off → 100% on the guide"));
        const QPointer<QObject> target(tool);
        connect(snap, &KisStudioValueRow::valueChanged, this, [target, snap](qreal value) {
            if (!target) return;
            if (value > 0) target->setProperty("magnetism", qRound(value * 10));
            target->setProperty("assistantSnapping", value > 0);
            snap->setValueText(value > 0 ? QString() : i18nc("snapping is off", "Off"));
        });
        lineLayout->addWidget(snap, 1);
        auto *more = new QToolButton(line);
        more->setAutoRaise(true);
        more->setText(QStringLiteral("⋯"));
        more->setToolTip(i18nc("@info:tooltip", "More snapping"));
        more->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(more);
        QAction *single = menu->addAction(i18n("Snap to Single Line"));
        single->setCheckable(true);
        single->setChecked(tool->property("onlyOneAssistant").toBool());
        connect(single, &QAction::toggled, this, [target](bool on) {
            if (target) target->setProperty("onlyOneAssistant", on);
        });
        QAction *eraser = menu->addAction(i18n("Snap Eraser"));
        eraser->setCheckable(true);
        eraser->setChecked(tool->property("snapEraser").toBool());
        connect(eraser, &QAction::toggled, this, [target](bool on) {
            if (target) target->setProperty("snapEraser", on);
        });
        more->setMenu(menu);
        lineLayout->addWidget(more);
        d->layout->addWidget(line);
    }

    auto *rest = new KisStudioValueRow(i18nc("the rest of the tool's options", "Tool options"), this);
    rest->setDraggable(false);
    rest->setValueText(QStringLiteral("›"));
    connect(rest, &KisStudioValueRow::clicked, this, [mainWindow]() { Q_EMIT KisStudioSmoothing::instance()->sigToolOptionsRequested(mainWindow); });
    d->layout->addSpacing(KisModernStyle::space1);
    d->layout->addWidget(rest);

    d->layout->activate();
    Q_EMIT sigResized();
}

#include "KisStudioSmoothing.moc"
