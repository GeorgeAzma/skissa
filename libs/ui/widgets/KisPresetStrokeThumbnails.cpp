/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPresetStrokeThumbnails.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QTimer>

#include <KoColor.h>
#include <KoColorSpaceRegistry.h>
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

/// False for engines that paint nothing on an empty layer or need a real canvas
bool paintsAlone(const QString &engine)
{
    static const QSet<QString> none = {QStringLiteral("duplicate"), QStringLiteral("deformbrush"),
                                       QStringLiteral("filter"), QStringLiteral("experimentbrush")};
    return !none.contains(engine);
}
} // namespace

struct KisPresetStrokeThumbnails::Private {
    struct Request {
        int id;
        QString md5;
        QColor ink;
        QString key;
    };

    QHash<QString, QImage> made;
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

void KisPresetStrokeThumbnails::next()
{
    while (!d->busy && !d->queue.isEmpty()) {
        const Private::Request request = d->queue.takeFirst();
        d->queued.remove(request.key);
        if (d->made.contains(request.key) || d->nothing.contains(request.key)) continue;

        KisResourceModel model(ResourceType::PaintOpPresets);
        KisPaintOpPresetSP preset = model.resourceForId(request.id).dynamicCast<KisPaintOpPreset>();
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
        if (!preset || !preset->valid() || !preset->settings() || !paintsAlone(preset->paintOp().id())) {
            d->nothing.insert(request.key);
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
        sample->setSettings(settings);

        KisResourcesSnapshotSP snapshot = new KisResourcesSnapshot(d->image, d->layer, resources, nullptr, {}, sample);
        snapshot->setOpacity(settings->paintOpOpacity());
        snapshot->setMirroring(false, false);
        snapshot->setFGColorOverride(KoColor(request.ink, d->image->colorSpace()));

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

    const QImage painted = d->layer->paintDevice()->convertToQImage(nullptr, d->image->bounds());
    // Treat a (nearly) empty result as nothing painted, e.g. smudging on an empty layer
    bool any = false;
    for (int y = 0; y < painted.height() && !any; y += 2) {
        const QRgb *line = reinterpret_cast<const QRgb *>(painted.constScanLine(y));
        for (int x = 0; x < painted.width(); x += 2) {
            if (qAlpha(line[x]) > 24) {
                any = true;
                break;
            }
        }
    }
    if (any) {
        if (d->made.size() > 400) d->made.clear();
        d->made.insert(d->current.key, painted.convertToFormat(QImage::Format_ARGB32_Premultiplied));
    } else {
        d->nothing.insert(d->current.key);
    }
    Q_EMIT changed();
    next();
}

#include "KisPresetStrokeThumbnails.moc"
