/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPresetStrokeThumbnails.h"

#include <QHash>
#include <QList>
#include <QSet>
#include <QTimer>
#include <QVector>

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

/// The patch of paint where the stroke starts reaches this far
constexpr int patchEdge = strokeWidth * 22 / 100;

/// Paint of \p ink for a brush that only moves paint to smear: a patch where the stroke starts (\p period 0),
/// or stripes \p period apart
QImage backdropOf(const QColor &ink, int period)
{
    QImage image(strokeWidth, strokeHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    for (int y = 0; y < strokeHeight; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < strokeWidth; x++) {
            if (period ? x >= 8 && (x - 8) % period < period / 2 : x < patchEdge) line[x] = ink.rgba();
        }
    }
    return image;
}

/// How far past the patch's edge \p after carries its paint
int reachOf(const QImage &after)
{
    int reach = 0;
    for (int x = patchEdge; x < strokeWidth; x++) {
        float column = 0;
        for (int y = 0; y < strokeHeight; y++) column += qAlpha(reinterpret_cast<const QRgb *>(after.constScanLine(y))[x]) / 255.0f;
        if (column > 0.5f) reach = x - patchEdge;
    }
    return reach;
}

/// \p values blurred by a box of \p radius, twice, which is close to a gaussian
void soften(QVector<float> &values, int radius)
{
    const int w = strokeWidth, h = strokeHeight;
    QVector<float> pass(values.size());
    for (int round = 0; round < 2; round++) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float sum = 0;
                for (int i = qMax(0, x - radius); i <= qMin(w - 1, x + radius); i++) sum += values[y * w + i];
                pass[y * w + x] = sum / (2 * radius + 1);
            }
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float sum = 0;
                for (int i = qMax(0, y - radius); i <= qMin(h - 1, y + radius); i++) sum += pass[i * w + x];
                values[y * w + x] = sum / (2 * radius + 1);
            }
        }
    }
}

/// What a brush did to \p before, painted as \p after: the paint it moved whole, the paint around it faint and
/// what it never reached gone; nothing when it moved nothing
QImage smearOf(const QImage &before, const QImage &after)
{
    const int w = strokeWidth, h = strokeHeight;
    QVector<float> changed(w * h);
    float total = 0;
    for (int y = 0; y < h; y++) {
        const QRgb *was = reinterpret_cast<const QRgb *>(before.constScanLine(y));
        const QRgb *is = reinterpret_cast<const QRgb *>(after.constScanLine(y));
        for (int x = 0; x < w; x++) {
            const int alpha = qAbs(qAlpha(is[x]) - qAlpha(was[x]));
            const int color = qMax(qAbs(qRed(is[x]) - qRed(was[x])), qMax(qAbs(qGreen(is[x]) - qGreen(was[x])), qAbs(qBlue(is[x]) - qBlue(was[x]))));
            const float change = qMax(alpha, color) / 255.0f;
            changed[y * w + x] = change;
            total += change;
        }
    }
    if (total < 40) return QImage();
    QVector<float> near = changed;
    soften(changed, 2);
    soften(near, 12);
    QImage smear = after.copy();
    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(smear.scanLine(y));
        for (int x = 0; x < w; x++) {
            const float weight = qMin(1.0f, 0.3f * qMin(1.0f, near[y * w + x] * 8) + 0.7f * qMin(1.0f, changed[y * w + x] * 3));
            const QRgb pixel = line[x];
            line[x] = qRgba(qRound(qRed(pixel) * weight), qRound(qGreen(pixel) * weight), qRound(qBlue(pixel) * weight), qRound(qAlpha(pixel) * weight));
        }
    }
    return smear;
}

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
        /// What it is painted over: nothing, then for a brush that painted nothing there a patch of paint to drag
        /// out, then, if it carried that paint only a little way, stripes spaced by how far it did
        enum Under { Nothing, Patch, Stripes } under {Nothing};
        int period {0};
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
        if (request.under != Private::Request::Nothing) {
            d->layer->paintDevice()->convertFromQImage(backdropOf(request.ink, request.period), nullptr);
        }

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

    const QImage painted = d->layer->paintDevice()->convertToQImage(nullptr, d->image->bounds()).convertToFormat(QImage::Format_ARGB32_Premultiplied);
    QImage made;
    Private::Request again = d->current;
    if (d->current.under == Private::Request::Patch) {
        // dragged far, it shows as it is; otherwise stripes as far apart as it carries paint
        const int reach = reachOf(painted);
        if (reach > (strokeWidth * 93 / 100 - patchEdge) * 0.4) made = smearOf(backdropOf(d->current.ink, 0), painted);
        if (made.isNull()) {
            again.under = Private::Request::Stripes;
            again.period = qBound(32, 16 + 2 * reach, 96);
        }
    } else if (d->current.under == Private::Request::Stripes) {
        made = smearOf(backdropOf(d->current.ink, d->current.period), painted);
    } else {
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
            made = painted;
        } else {
            // a blender: what it does to paint
            again.under = Private::Request::Patch;
        }
    }
    if (!made.isNull()) {
        if (d->made.size() > 400) d->made.clear();
        d->made.insert(d->current.key, made);
    } else if (again.under != d->current.under) {
        d->queue.prepend(again);
        d->queued.insert(again.key);
    } else {
        d->nothing.insert(d->current.key);
    }
    Q_EMIT changed();
    next();
}

#include "KisPresetStrokeThumbnails.moc"
