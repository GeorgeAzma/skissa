/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPresetStrokeThumbnails.h"

#include <QHash>
#include <QList>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>
#include <QSet>
#include <QTimer>

#include <memory>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>

#include "KisAsynchronousStrokeUpdateHelper.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_information.h"
#include "kis_paint_layer.h"
#include "kis_paintop_preset.h"
#include "kis_paintop_settings.h"
#include "kis_resources_snapshot.h"
#include "kis_simple_stroke_strategy.h"
#include "strokes/KisFreehandStrokeInfo.h"
#include "strokes/freehand_stroke.h"

namespace
{
const QString settingKey = QStringLiteral("PresetStrokeThumbnails");

/// Rendered as a wide strip and scaled down for display
constexpr int strokeWidth = 320;
constexpr int strokeHeight = 88;

/// An empty stroke that signals when all strokes queued before it are done
class DoneStroke : public QObject, public KisSimpleStrokeStrategy
{
    Q_OBJECT
public:
    DoneStroke()
        : KisSimpleStrokeStrategy(QLatin1String("KisPresetStrokeThumbnailsDone"))
    {
        setClearsRedoOnStart(false);
        enableJob(JOB_INIT, true, KisStrokeJobData::BARRIER);
    }

    void initStrokeCallback() override
    {
        Q_EMIT done();
    }

Q_SIGNALS:
    void done();
};

/// \p painted fading out along its length, as paint a blender drags runs out
QImage fadedOut(QImage painted)
{
    QPainter painter(&painted);
    painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    QLinearGradient fade(strokeWidth * 0.15, 0, strokeWidth * 0.9, 0);
    fade.setColorAt(0.0, QColor(0, 0, 0, 255));
    fade.setColorAt(0.25, QColor(0, 0, 0, 170));
    fade.setColorAt(0.6, QColor(0, 0, 0, 55));
    fade.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(painted.rect(), fade);
    painter.end();
    return painted;
}

/// False for engines that paint nothing on an empty layer or need a real canvas
bool paintsAlone(const QString &engine)
{
    static const QSet<QString> none = {QStringLiteral("duplicate"), QStringLiteral("deformbrush"),
                                       QStringLiteral("filter"), QStringLiteral("experimentbrush")};
    return !none.contains(engine);
}

/// The wave every stroke follows, as painted in next()
QPainterPath wave()
{
    const qreal w = strokeWidth, h = strokeHeight;
    QPainterPath path(QPointF(w * 0.07, h * 0.62));
    path.cubicTo(QPointF(w * 0.22, h * 0.08), QPointF(w * 0.34, h * 0.2), QPointF(w * 0.5, h * 0.5));
    path.cubicTo(QPointF(w * 0.66, h * 0.8), QPointF(w * 0.78, h * 0.92), QPointF(w * 0.93, h * 0.38));
    return path;
}

/**
 * What an engine that paints nothing alone does, drawn in \p ink as the strokes are: a lasso's fill for a shape
 * filler, a soft stroke for a filter, a stroke of copied picture (checks) for a clone, a dragged stroke running
 * out for a deformer. \p size is the brush's, as the strips paint it.
 */
QImage drawnStroke(const QString &engine, qreal size, const QColor &ink)
{
    QImage image(strokeWidth, strokeHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(Qt::NoPen);
    const qreal w = strokeWidth, h = strokeHeight;
    if (engine == QLatin1String("experimentbrush")) {
        // what a lasso along the wave and back leaves: one solid shape, its ends pointed where the hand turned
        QPainterPath shape(QPointF(w * 0.07, h * 0.7));
        shape.cubicTo(QPointF(w * 0.2, h * 0.1), QPointF(w * 0.36, h * 0.12), QPointF(w * 0.52, h * 0.4));
        shape.cubicTo(QPointF(w * 0.64, h * 0.62), QPointF(w * 0.8, h * 0.66), QPointF(w * 0.93, h * 0.3));
        shape.cubicTo(QPointF(w * 0.8, h * 0.95), QPointF(w * 0.3, h * 0.95), QPointF(w * 0.07, h * 0.7));
        painter.setBrush(ink);
        painter.drawPath(shape);
        return image;
    }
    const bool soft = engine == QLatin1String("filter");
    const bool copied = engine == QLatin1String("duplicate");
    const qreal radius = 0.5 * ((soft || copied) ? qMax<qreal>(size, strokeHeight * 0.3) : size);
    const QPainterPath path = wave();
    const int dabs = 160;
    for (int i = 0; i <= dabs; i++) {
        const qreal t = qreal(i) / dabs;
        // light pressure at both ends, full in the middle
        const qreal r = qMax<qreal>(0.75, radius * (0.15 + 0.85 * qSin(M_PI * t)));
        const QPointF at = path.pointAtPercent(t);
        if (soft) {
            QRadialGradient blur(at, r * 1.6);
            QColor middle = ink;
            middle.setAlphaF(0.1);
            QColor edge = ink;
            edge.setAlpha(0);
            blur.setColorAt(0, middle);
            blur.setColorAt(1, edge);
            painter.setBrush(blur);
            painter.drawEllipse(at, r * 1.6, r * 1.6);
        } else {
            painter.setBrush(ink);
            painter.drawEllipse(at, r, r);
        }
    }
    if (copied) {
        // the picture copied along the stroke: checks
        QImage checks(20, 20, QImage::Format_ARGB32_Premultiplied);
        checks.fill(QColor(0, 0, 0, 255));
        QPainter cells(&checks);
        cells.fillRect(0, 0, 10, 10, QColor(0, 0, 0, 60));
        cells.fillRect(10, 10, 10, 10, QColor(0, 0, 0, 60));
        cells.end();
        painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        painter.fillRect(image.rect(), QBrush(checks));
    }
    painter.end();
    return engine == QLatin1String("deformbrush") ? fadedOut(image) : image;
}
} // namespace

struct KisPresetStrokeThumbnails::Private {
    struct Request {
        int id;
        QString md5;
        QColor ink;
        QString key;
        /// Painted again laying down the ink, having painted nothing on an empty layer
        bool asPaint {false};
    };

    QHash<QString, QImage> made;
    /// made's keys, the one used longest ago first: the oldest go once there are too many, never the rows in view
    QList<QString> used;
    /// Presets that painted nothing or failed; their icon is shown instead
    QSet<QString> nothing;
    QList<Request> queue;
    QSet<QString> queued;

    KisImageSP image;
    KisPaintLayerSP layer;
    Request current;
    /// Identifies the current request, since one that timed out may still finish later
    quint64 serial {0};
    bool busy {false};
    QTimer watchdog;
    /// Images given up on by the watchdog, by request serial, until their strokes end
    QHash<quint64, KisImageSP> retired;
    /// Kept: a new one filters every brush again for each stroke asked for
    std::unique_ptr<KisResourceModel> presets;
};

KisPresetStrokeThumbnails *KisPresetStrokeThumbnails::instance()
{
    static KisPresetStrokeThumbnails *thumbnails = new KisPresetStrokeThumbnails();
    return thumbnails;
}

KisPresetStrokeThumbnails::KisPresetStrokeThumbnails()
    : d(new Private)
{
    // Give up on engines that never finish, and paint the next one on a fresh image
    d->watchdog.setSingleShot(true);
    d->watchdog.setInterval(6000);
    connect(&d->watchdog, &QTimer::timeout, this, [this]() {
        if (!d->busy) return;
        d->nothing.insert(d->current.key);

        // The stroke still holds the image: keep it here until its strokes are over, so that it is
        // never destroyed by its own updater thread
        const quint64 serial = d->serial;
        d->retired.insert(serial, d->image);
        d->image->requestStrokeCancellation();
        DoneStroke *done = new DoneStroke();
        connect(done, &DoneStroke::done, this, [this, serial]() {
            d->retired.remove(serial);
        }, Qt::QueuedConnection);
        d->image->endStroke(d->image->startStroke(done));

        d->image = nullptr;
        d->layer = nullptr;
        d->busy = false;
        next();
    });
}

KisPresetStrokeThumbnails::~KisPresetStrokeThumbnails()
{
}

bool KisPresetStrokeThumbnails::isEnabled()
{
    return KisConfig(true).readEntry(settingKey, false);
}

void KisPresetStrokeThumbnails::setEnabled(bool on)
{
    KisConfig(false).writeEntry(settingKey, on);
}

QImage KisPresetStrokeThumbnails::stroke(int id, const QString &md5, const QColor &ink)
{
    const QString key = QStringLiteral("%1 %2 %3").arg(id).arg(md5).arg(ink.rgba(), 0, 16);
    auto it = d->made.constFind(key);
    if (it != d->made.constEnd()) {
        if (d->used.isEmpty() || d->used.last() != key) {
            d->used.removeOne(key);
            d->used.append(key);
        }
        return *it;
    }
    if (d->nothing.contains(key) || d->queued.contains(key)) {
        return QImage();
    }
    d->queue.append({id, md5, ink, key});
    d->queued.insert(key);
    if (!d->busy) {
        QTimer::singleShot(0, this, [this]() { next(); });
    }
    return QImage();
}

QRect KisPresetStrokeThumbnails::paintedPart(const QImage &stroke)
{
    static QHash<qint64, QRect> found;
    auto it = found.constFind(stroke.cacheKey());
    if (it != found.constEnd()) return *it;
    int left = stroke.width(), right = -1, top = stroke.height(), bottom = -1;
    for (int y = 0; y < stroke.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(stroke.constScanLine(y));
        for (int x = 0; x < stroke.width(); x++) {
            if (qAlpha(line[x]) < 16) continue;
            left = qMin(left, x);
            right = qMax(right, x);
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
    }
    const QRect part = right < 0 ? stroke.rect() : QRect(QPoint(left, top), QPoint(right, bottom)).adjusted(-2, -2, 2, 2).intersected(stroke.rect());
    if (found.size() > 400) found.clear();
    found.insert(stroke.cacheKey(), part);
    return part;
}

void KisPresetStrokeThumbnails::next()
{
    while (!d->busy && !d->queue.isEmpty()) {
        const Private::Request request = d->queue.takeFirst();
        d->queued.remove(request.key);
        if (d->made.contains(request.key) || d->nothing.contains(request.key)) continue;

        if (!d->presets) d->presets.reset(new KisResourceModel(ResourceType::PaintOpPresets));
        KisPaintOpPresetSP preset = d->presets->resourceForId(request.id).dynamicCast<KisPaintOpPreset>();
        KisMainWindow *window = KisPart::instance()->currentMainwindow();
        KoCanvasResourceProvider *resources = window && window->viewManager() && window->viewManager()->canvasResourceProvider()
                                                  ? window->viewManager()->canvasResourceProvider()->resourceManager()
                                                  : nullptr;
        if (!resources) {
            // No window yet; the chooser will ask again when it repaints
            d->queued.clear();
            d->queue.clear();
            return;
        }
        if (!preset || !preset->valid() || !preset->settings()) {
            d->nothing.insert(request.key);
            continue;
        }
        if (!paintsAlone(preset->paintOp().id())) {
            QColor ink = request.ink;
            ink.setAlpha(255);
            d->made.insert(request.key, drawnStroke(preset->paintOp().id(),
                                                    qBound(1.5, preset->settings()->paintOpSize(), strokeHeight * 0.3), ink));
            d->used.append(request.key);
            Q_EMIT changed();
            continue;
        }

        if (!d->image) {
            const KoColorSpace *rgb = KoColorSpaceRegistry::instance()->rgb8();
            d->image = new KisImage(nullptr, strokeWidth, strokeHeight, rgb, "brush stroke thumbnails");
            d->layer = new KisPaintLayer(d->image, "stroke", OPACITY_OPAQUE_U8, rgb);
        }
        d->layer->paintDevice()->clear();

        // Shrink brushes that are too big for the strip
        KisPaintOpPresetSP sample = preset->clone().dynamicCast<KisPaintOpPreset>();
        KisPaintOpSettingsSP settings = sample->settings();
        settings->setPaintOpSize(qBound(1.5, settings->paintOpSize(), strokeHeight * 0.3));
        if (request.asPaint) {
            // a blender's own tip in the ink: its color rate full, nothing to smudge
            settings->setProperty("PressureColorRate", true);
            settings->setProperty("ColorRateValue", 1.0);
            settings->setProperty("SmudgeRateValue", 0.0);
        }
        sample->setSettings(settings);

        KisResourcesSnapshotSP snapshot = new KisResourcesSnapshot(d->image, d->layer, resources, nullptr, {}, sample);
        snapshot->setOpacity(settings->paintOpOpacity());
        snapshot->setMirroring(false, false);
        snapshot->setFGColorOverride(KoColor(request.ink, d->image->colorSpace()));
        // laid down as paint: a brush in use that erases or keeps alpha would leave every stroke empty
        snapshot->setCompositeOpId(COMPOSITE_OVER);
        snapshot->setGlobalAlphaLock(false);

        KisStrokeId strokeId = d->image->startStroke(
            new FreehandStrokeStrategy(snapshot, new KisFreehandStrokeInfo(), kundo2_noi18n("brush stroke thumbnail")));

        // A wave with light pressure at both ends and full pressure in the middle
        const qreal w = strokeWidth, h = strokeHeight;
        auto point = [](const QPointF &pos, qreal pressure, int time) {
            KisPaintInformation info(pos, pressure);
            info.setCurrentTime(time);
            return info;
        };
        const KisPaintInformation start = point(QPointF(w * 0.07, h * 0.62), 0.05, 0);
        const KisPaintInformation middle = point(QPointF(w * 0.5, h * 0.5), 1.0, 300);
        const KisPaintInformation end = point(QPointF(w * 0.93, h * 0.38), 0.05, 600);
        d->image->addJob(strokeId, new FreehandStrokeStrategy::Data(0, start, QPointF(w * 0.22, h * 0.08), QPointF(w * 0.34, h * 0.2), middle));
        d->image->addJob(strokeId, new FreehandStrokeStrategy::Data(0, middle, QPointF(w * 0.66, h * 0.8), QPointF(w * 0.78, h * 0.92), end));
        d->image->addJob(strokeId, new KisAsynchronousStrokeUpdateHelper::UpdateData(true));
        d->image->endStroke(strokeId);

        d->current = request;
        d->busy = true;
        d->watchdog.start();

        const quint64 serial = ++d->serial;
        DoneStroke *done = new DoneStroke();
        connect(done, &DoneStroke::done, this, [this, serial]() {
            if (serial == d->serial) finished();
        }, Qt::QueuedConnection);
        d->image->endStroke(d->image->startStroke(done));
    }
}

void KisPresetStrokeThumbnails::finished()
{
    if (!d->busy || !d->image) return;
    d->watchdog.stop();
    d->busy = false;

    QImage painted = d->layer->paintDevice()->convertToQImage(nullptr, d->image->bounds()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    // the most opaque the stroke got: nothing at all, e.g. smudging on an empty layer, or faint (watercolors, low
    // flow), which is brightened so its shape reads in the list, as the brush would build up over a few strokes
    int most = 0;
    for (int y = 0; y < painted.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(painted.constScanLine(y));
        for (int x = 0; x < painted.width(); x++) most = qMax(most, qAlpha(line[x]));
    }
    const bool any = most > 3;
    if (any && most < 160) {
        const qreal gain = 200.0 / most;
        for (int y = 0; y < painted.height(); y++) {
            QRgb *line = reinterpret_cast<QRgb *>(painted.scanLine(y));
            for (int x = 0; x < painted.width(); x++) {
                const QRgb c = line[x];
                line[x] = qRgba(qMin(255, qRound(qRed(c) * gain)), qMin(255, qRound(qGreen(c) * gain)), qMin(255, qRound(qBlue(c) * gain)),
                                qMin(255, qRound(qAlpha(c) * gain)));
            }
        }
    }
    if (any) {
        while (d->made.size() >= 400 && !d->used.isEmpty()) d->made.remove(d->used.takeFirst());
        d->made.insert(d->current.key, d->current.asPaint ? fadedOut(painted) : painted);
        d->used.removeOne(d->current.key);
        d->used.append(d->current.key);
    } else if (!d->current.asPaint) {
        // a blender: its stroke as if it carried paint
        Private::Request again = d->current;
        again.asPaint = true;
        d->queue.prepend(again);
        d->queued.insert(again.key);
    } else {
        d->nothing.insert(d->current.key);
    }
    Q_EMIT changed();
    next();
}

#include "KisPresetStrokeThumbnails.moc"
