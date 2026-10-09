/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushPicker.h"

#include <QElapsedTimer>
#include <QFuture>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QThread>
#include <QTimer>
#include <QtConcurrent>
#include <QtMath>

#include <atomic>

#include <KisGlobalResourcesInterface.h>
#include <KisResourceModel.h>
#include <KisResourceTypes.h>
#include <KisTagFilterResourceProxyModel.h>
#include <KisTagModel.h>
#include <KoCanvasResourceProvider.h>
#include <KoColorSpaceRegistry.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_image.h"
#include "kis_paint_device.h"
#include "kis_paint_layer.h"
#include "kis_paintop_preset.h"
#include "kis_paintop_settings.h"
#include "kis_painter.h"
#include "brushengine/kis_paint_information.h"
#include "kis_distance_information.h"
#include "KisRunnableStrokeJobData.h"
#include "brushengine/kis_paintop.h"

/**
 * The tiles' strokes: painted in a worker thread, one brush after another.
 * The GUI thread loads each brush and hands the worker a copy that needs
 * nothing from the GUI thread; the worker hands back the stroke, and the
 * copy, to be let go of in the GUI thread.
 */
struct KisStudioBrushStrokes {
    struct Brush {
        QString name;
        QString md5;
        QString title;
        QString engine;
        QStringList tags;
        QImage icon;
        QString kind;
    };

    struct Catalog {
        QStringList ids;                ///< by title
        QHash<QString, Brush> brushes;
    };

    struct Job {
        QString id;
        KisPaintOpPresetSP preset;      ///< a copy with its resources, at a size that fits
        KisImageSP image;
        KisPaintLayerSP layer;
        KoColor paint;
        QString compositeOp;
        qreal opacity {1.0};
        QString kind;
        QSize size;
    };

    std::shared_ptr<Catalog> catalog;   ///< all the brushes there are, as the picker opens
    QColor paper;                       ///< behind the strokes, set before the worker starts
    QMutex mutex;
    QList<Job> queue;                   ///< for the worker, under the mutex
    QHash<QString, QImage> made;        ///< by the worker, under the mutex
    QList<Job> spent;                   ///< painted, to be let go of in the GUI thread, under the mutex
    std::atomic<bool> cancelled {false};
    std::atomic<bool> allQueued {false};
    std::atomic<bool> finished {false};
    QHash<QString, QImage> shown;       ///< the ones collected, used in the GUI thread only
};

namespace {

using Group = KisStudioPicker::Group;
using Job = KisStudioBrushStrokes::Job;
using Brush = KisStudioBrushStrokes::Brush;
using Catalog = KisStudioBrushStrokes::Catalog;

const QString pinnedKey = QStringLiteral("StudioBrushes");
const QString recentKey = QStringLiteral("StudioRecentBrushes");
constexpr int recentKept = 8;
/// The size of a tile's picture, see KisStudioPicker
const QSize strokeSize(112, 64);

/// The groups of "All brushes": by what a brush is for, in this order
QVector<QPair<QString, QString>> kindTitles()
{
    return {
        {QStringLiteral("sketch"), i18nc("brush group", "Sketch and dry media")},
        {QStringLiteral("ink"), i18nc("brush group", "Ink and lines")},
        {QStringLiteral("paint"), i18nc("brush group", "Paint")},
        {QStringLiteral("soft"), i18nc("brush group", "Airbrush and glow")},
        {QStringLiteral("blend"), i18nc("brush group", "Blend and smudge")},
        {QStringLiteral("texture"), i18nc("brush group", "Texture and scatter")},
        {QStringLiteral("pixel"), i18nc("brush group", "Pixel")},
        {QStringLiteral("erase"), i18nc("brush group", "Erasers")},
        {QStringLiteral("effect"), i18nc("brush group", "Effects, clone and deform")},
        {QStringLiteral("other"), i18nc("brush group", "Other")},
    };
}

/// What a brush is for: from the words in its name, then from its engine
QString kindOf(const QString &name, const QString &engine)
{
    const QString n = name.toLower();
    auto has = [&n](std::initializer_list<const char *> words) {
        for (const char *word : words) {
            if (n.contains(QLatin1String(word))) return true;
        }
        return false;
    };
    if (has({"eras"})) return QStringLiteral("erase");
    if (engine == QLatin1String("filter") || engine == QLatin1String("deformbrush") || engine == QLatin1String("duplicate")
        || has({"filter", "deform", "liquify", "clone", "dodge", "burn", "sharpen", "smart patch"})) {
        return QStringLiteral("effect");
    }
    if (has({"blend", "smudge", "smear", "blur", "mix"})) return QStringLiteral("blend");
    if (has({"airbrush", "glow", "fog", "mist", "cloud", "haze"})) return QStringLiteral("soft");
    if (has({"pencil", "graphite", "charcoal", "chalk", "crayon", "pastel", "sketch", "conte", "lead"})) return QStringLiteral("sketch");
    // wet media first: "WaterC Basic Lines" is paint, not ink
    if (has({"water", "wet", "oil", "acryl", "gouache", "bristle", "impasto"})) return QStringLiteral("paint");
    if (has({"ink", "liner", "pen", "marker", "calligraph", "sumi", "line"})) return QStringLiteral("ink");
    if (has({"pixel"})) return QStringLiteral("pixel");
    if (has({"texture", "grain", "splat", "spread", "rock", "hair", "fur", "grass", "foliage", "leaf", "leaves", "pattern",
             "stamp", "noise", "hatch", "shape", "scatter", "star", "spray", "speckle", "sparkle", "bokeh"})) {
        return QStringLiteral("texture");
    }
    if (engine == QLatin1String("spraybrush") || engine == QLatin1String("particlebrush") || engine == QLatin1String("hatchingbrush")
        || engine == QLatin1String("gridbrush") || engine == QLatin1String("sketchbrush") || engine == QLatin1String("curvebrush")) {
        return QStringLiteral("texture");
    }
    if (has({"paint", "dry", "basic", "brush", "round", "flat", "glaze", "rough", "knife", "fan"})
        || engine == QLatin1String("colorsmudge") || engine == QLatin1String("hairybrush") || engine == QLatin1String("mypaintbrush")) {
        return QStringLiteral("paint");
    }
    // the quick brush: a hard round tip, mostly for lines
    if (engine == QLatin1String("roundmarker")) return QStringLiteral("ink");
    return QStringLiteral("other");
}

std::shared_ptr<Catalog> loadCatalog()
{
    auto catalog = std::make_shared<Catalog>();
    KisResourceModel model(ResourceType::PaintOpPresets);
    // the bundled presets sort themselves with "b) ", which is noise here
    static const QRegularExpression sortPrefix(QStringLiteral("^[a-z]\\)\\s+"));
    for (int row = 0; row < model.rowCount(); row++) {
        const QModelIndex index = model.index(row, 0);
        Brush brush;
        brush.name = index.data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
        if (brush.name.isEmpty() || catalog->brushes.contains(brush.name)) {
            continue;
        }
        brush.md5 = index.data(Qt::UserRole + KisAbstractResourceModel::MD5).toString();
        brush.title = QString(brush.name).remove(sortPrefix).replace(QLatin1Char('_'), QLatin1Char(' '));
        brush.tags = index.data(Qt::UserRole + KisAbstractResourceModel::Tags).toStringList();
        brush.engine = index.data(Qt::UserRole + KisAbstractResourceModel::MetaData).toMap().value(QStringLiteral("paintopid")).toString();
        brush.icon = index.data(Qt::UserRole + KisAbstractResourceModel::Thumbnail).value<QImage>();
        brush.kind = kindOf(brush.name, brush.engine);
        catalog->ids << brush.name;
        catalog->brushes.insert(brush.name, brush);
    }
    std::sort(catalog->ids.begin(), catalog->ids.end(), [catalog](const QString &a, const QString &b) {
        return catalog->brushes.value(a).title.localeAwareCompare(catalog->brushes.value(b).title) < 0;
    });
    return catalog;
}

/// Yours until you change them: the pop-up palette's favorites, or a few for every kind of work
QStringList initialBrushes()
{
    QStringList names;
    const QString url = KisConfig(true).readEntry<QString>(QStringLiteral("favoritePresetsTag"), QStringLiteral("★ My Favorites"));
    KisTagModel tags(ResourceType::PaintOpPresets);
    KisTagSP tag = tags.tagForUrl(url);
    if (tag) {
        KisTagFilterResourceProxyModel tagged(ResourceType::PaintOpPresets);
        tagged.setTagFilter(tag);
        tagged.sort(KisAbstractResourceModel::Name);
        for (int row = 0; row < tagged.rowCount() && names.size() < 16; row++) {
            names << tagged.index(row, 0).data(Qt::UserRole + KisAbstractResourceModel::Name).toString();
        }
    }
    if (names.isEmpty()) {
        names = QStringList{
            QStringLiteral("c) Pencil-2"), QStringLiteral("d) Ink-2 Fineliner"), QStringLiteral("b) Basic-5 Size Opacity"),
            QStringLiteral("f) Bristles-2 Flat Rough"), QStringLiteral("b) Airbrush Soft"), QStringLiteral("k) Blender Basic"),
            QStringLiteral("m) Texture Big"), QStringLiteral("a) Eraser Soft"),
        };
    }
    return names;
}

/// Light paper, or dark paper for a light color
QColor paperFor(const QColor &paint)
{
    return paint.lightnessF() > 0.8 ? QColor(52, 52, 56) : QColor(244, 241, 234);
}

std::shared_ptr<KisStudioBrushStrokes> makeStrokes()
{
    auto strokes = std::make_shared<KisStudioBrushStrokes>();
    strokes->catalog = loadCatalog();
    return strokes;
}

/// @param strokes the tiles' strokes, or none when the source is not for showing
KisStudioPicker::Source brushSource(std::shared_ptr<Catalog> catalog, std::shared_ptr<KisStudioBrushStrokes> strokes)
{
    KisStudioPicker::Source source;
    source.pinnedKey = pinnedKey;
    source.initialPinned = initialBrushes;
    source.allIds = [catalog]() { return catalog->ids; };
    source.otherGroups = [catalog](const QStringList &pinned) {
        QVector<Group> groups;
        Q_FOREACH (const auto &title, kindTitles()) {
            Group group{title.second, {}};
            Q_FOREACH (const QString &id, catalog->ids) {
                if (!pinned.contains(id) && catalog->brushes.value(id).kind == title.first) group.ids << id;
            }
            if (!group.ids.isEmpty()) groups << group;
        }
        return groups;
    };
    source.name = [catalog](const QString &id) { return catalog->brushes.value(id).title; };
    source.aliases = [catalog](const QString &id) {
        const Brush brush = catalog->brushes.value(id);
        QStringList aliases = brush.tags;
        Q_FOREACH (const auto &title, kindTitles()) {
            if (title.first == brush.kind) aliases << title.second;
        }
        aliases << brush.engine;
        return aliases;
    };
    if (strokes) {
        source.paintPreview = [catalog, strokes](QPainter &painter, const QRect &rect, const QString &id) {
            painter.fillRect(rect, strokes->paper);
            const QImage stroke = strokes->shown.value(id);
            if (!stroke.isNull()) {
                painter.drawImage(rect, stroke);
                return;
            }
            // not painted (yet): the brush's own picture, faint
            const QImage icon = catalog->brushes.value(id).icon;
            if (!icon.isNull()) {
                const int side = rect.height() - 8;
                painter.save();
                painter.setOpacity(painter.opacity() * 0.45);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
                painter.drawImage(QRect(rect.center().x() - side / 2, rect.top() + 4, side, side), icon);
                painter.restore();
            }
        };
    }
    source.recent = []() { return KisConfig(true).readEntry<QStringList>(recentKey, QStringList()); };
    source.tiles = true;
    source.yoursTitle = i18nc("brush group", "Your brushes");
    source.searchPlaceholder = i18nc("@info:placeholder", "Search all %1 brushes, their tags and kinds", catalog->ids.size());
    // the number goes in later: %1 passes through
    source.allLabel = i18nc("@action:button %1 is a number", "All %1 brushes", QStringLiteral("%1"));
    return source;
}

/// The brush's stroke: a wave that starts and ends light, heaviest in the middle
QImage paintStroke(const Job &job)
{
    KisPaintDeviceSP device = job.layer->paintDevice();
    const int w = job.size.width();
    const int h = job.size.height();
    const QRect bounds(0, 0, w, h);

    if (job.kind == QLatin1String("blend") || job.kind == QLatin1String("effect")) {
        // something to blend or change: bands of the color over the paper
        KoColor band = job.paint;
        for (int i = 0; i < 4; i += 2) {
            device->fill(QRect(w * i / 4, 0, w / 4 + 1, h), band);
        }
        KoColor other(device->colorSpace());
        other.fromQColor(job.paint.toQColor().lighter(160));
        device->fill(QRect(w / 4, 0, w / 4 + 1, h), other);
    } else if (job.kind == QLatin1String("erase")) {
        // something to erase: a wash of the color
        KoColor wash = job.paint;
        wash.setOpacity(0.55);
        device->fill(bounds, wash);
    }

    KisPainter painter(device);
    painter.setPaintColor(job.paint);
    if (!job.compositeOp.isEmpty()) painter.setCompositeOpId(job.compositeOp);
    painter.setOpacityF(job.opacity);
    painter.setPaintOpPreset(job.preset, job.layer, job.image);
    if (!painter.paintOp()) {
        return QImage();
    }

    const qreal radius = job.preset->settings()->paintOpSize() / 2;
    const qreal margin = qMax<qreal>(radius, 3) + 3;
    const qreal amplitude = qMax<qreal>(h / 2.0 - margin, 0) * 0.8;
    constexpr int steps = 28;
    KisDistanceInformation distance;
    KisPaintInformation previous;
    for (int i = 0; i <= steps; i++) {
        const qreal t = qreal(i) / steps;
        const QPointF pos(margin + t * (w - 2 * margin), h / 2.0 - amplitude * qSin(2 * M_PI * t));
        const qreal pressure = qMax(0.05, qPow(qSin(M_PI * t), 0.6));
        KisPaintInformation info(pos, pressure);
        info.setCurrentTime(i * 12);
        if (i > 0) {
            painter.paintLine(previous, info, &distance);
        }
        previous = info;
    }
    // most engines only queue their dabs while painting, for the stroke to render them in its
    // own time; there is no stroke here: render them all now (the painter runs the jobs it makes)
    for (int round = 0; round < 256; round++) {
        QVector<KisRunnableStrokeJobData *> jobs;
        const bool more = painter.paintOp()->doAsynchronousUpdate(jobs).second;
        Q_FOREACH (KisRunnableStrokeJobData *data, jobs) {
            data->run();
            delete data;
        }
        if (!more) break;
    }
    return device->convertToQImage(nullptr, 0, 0, w, h);
}

/// Strokes already painted, by brush, for the color and size they were painted at
QHash<QString, QImage> &strokeCache()
{
    static QHash<QString, QImage> cache;
    return cache;
}

/// The last stroke painted for each brush, whatever the color: for showing a brush elsewhere (the canvas ring)
QHash<QString, QImage> &latestStrokes()
{
    static QHash<QString, QImage> latest;
    return latest;
}

QString &strokeCacheStamp()
{
    static QString stamp;
    return stamp;
}

} // namespace

struct KisStudioBrushPicker::Private
{
    KisMainWindow *mainWindow {nullptr};
    std::shared_ptr<Catalog> catalog;
    std::shared_ptr<KisStudioBrushStrokes> strokes;
    KisImageSP image;
    KoColor paint;
    QSize size;
    QStringList toQueue;
    int next {0};
    QTimer feed;
    QTimer collect;
    QFuture<void> worker;

    /// The next brushes for the worker, for a few ms at a time: loading them takes a moment
    void feedSome();
};

KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow)
    : KisStudioBrushPicker(mainWindow, makeStrokes())
{
}

KisStudioBrushPicker::KisStudioBrushPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioBrushStrokes> strokes)
    : KisStudioPicker(brushSource(strokes->catalog, strokes),
                      mainWindow->viewManager() && mainWindow->viewManager()->canvasResourceProvider()
                              && mainWindow->viewManager()->canvasResourceProvider()->currentPreset()
                          ? mainWindow->viewManager()->canvasResourceProvider()->currentPreset()->name()
                          : QString(),
                      QSet<QString>(), mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->strokes = strokes;
    d->catalog = strokes->catalog;

    KisViewManager *view = mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    const KoColorSpace *rgb = KoColorSpaceRegistry::instance()->rgb8();
    d->paint = resources ? resources->fgColor().convertedTo(rgb) : KoColor(Qt::black, rgb);
    d->paint.setOpacity(1.0);
    strokes->paper = paperFor(d->paint.toQColor());
    d->size = strokeSize * mainWindow->devicePixelRatioF();

    connect(this, &KisStudioPicker::sigPicked, this, [mainWindow](const QString &id) {
        recordUse(id);
        use(mainWindow->viewManager(), id);
    });

    if (!resources || !resources->resourceManager()) {
        strokes->finished = true;
        return;
    }

    // painted before for this color: shown at once
    const QString stamp = d->paint.toQColor().name() + QLatin1Char('@') + QString::number(d->size.width());
    if (strokeCacheStamp() != stamp) {
        strokeCache().clear();
        strokeCacheStamp() = stamp;
    }

    // the recent ones and yours first, then the rest: the ones seen first are painted first
    const Source source = brushSource(d->catalog, nullptr);
    QStringList order = source.recent() + pinnedIds(source) + d->catalog->ids;
    order.removeDuplicates();
    Q_FOREACH (const QString &id, order) {
        const Brush brush = d->catalog->brushes.value(id);
        const QImage cached = strokeCache().value(brush.md5 + id);
        if (!cached.isNull()) {
            strokes->shown.insert(id, cached);
        } else if (!brush.name.isEmpty()) {
            d->toQueue << id;
        }
    }

    d->image = new KisImage(nullptr, d->size.width(), d->size.height(), rgb, QStringLiteral("brush strokes"));

    d->worker = QtConcurrent::run([strokes]() {
        while (!strokes->cancelled) {
            Job job;
            bool have = false;
            {
                QMutexLocker locker(&strokes->mutex);
                if (!strokes->queue.isEmpty()) {
                    job = strokes->queue.takeFirst();
                    have = true;
                } else if (strokes->allQueued) {
                    break;
                }
            }
            if (!have) {
                QThread::msleep(4);
                continue;
            }
            const QImage stroke = paintStroke(job);
            QMutexLocker locker(&strokes->mutex);
            if (!stroke.isNull()) strokes->made.insert(job.id, stroke);
            strokes->spent << job;
        }
        strokes->finished = true;
    });

    d->feed.setInterval(0);
    connect(&d->feed, &QTimer::timeout, this, [this]() { d->feedSome(); });
    d->feed.start();

    // the finished ones, a few times a second, until all are
    d->collect.setInterval(80);
    connect(&d->collect, &QTimer::timeout, this, [this]() {
        bool any = false;
        QList<Job> spent;
        {
            QMutexLocker locker(&d->strokes->mutex);
            for (auto it = d->strokes->made.constBegin(); it != d->strokes->made.constEnd(); ++it) {
                d->strokes->shown.insert(it.key(), it.value());
                strokeCache().insert(d->catalog->brushes.value(it.key()).md5 + it.key(), it.value());
                latestStrokes().insert(it.key(), it.value());
                any = true;
            }
            d->strokes->made.clear();
            spent.swap(d->strokes->spent);
        }
        // the copies go here, in the GUI thread
        spent.clear();
        if (any) updatePreviews();
        if (d->strokes->finished && !any) d->collect.stop();
    });
    d->collect.start();
}

void KisStudioBrushPicker::Private::feedSome()
{
    KisViewManager *view = mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    if (!resources || !resources->resourceManager() || next >= toQueue.size()) {
        feed.stop();
        strokes->allQueued = true;
        return;
    }
    KisPaintOpPresetResourceServer *server = KisResourceServerProvider::instance()->paintOpPresetServer();
    const KoColorSpace *rgb = KoColorSpaceRegistry::instance()->rgb8();
    QElapsedTimer spent;
    spent.start();
    QList<Job> jobs;
    while (next < toQueue.size() && spent.elapsed() < 6) {
        const Brush brush = catalog->brushes.value(toQueue[next++]);
        KisPaintOpPresetSP preset = server->resource(brush.md5, QString(), brush.name);
        // the clone brush paints what is elsewhere in the image: nothing to show
        if (!preset || !preset->valid() || !preset->settings() || brush.engine == QLatin1String("duplicate")) {
            continue;
        }
        KisPaintOpPresetSP copy = preset->cloneWithResourcesSnapshot(KisGlobalResourcesInterface::instance(),
                                                                     resources->resourceManager()->canvasResourcesInterface(), nullptr);
        if (!copy || !copy->settings()) {
            continue;
        }
        // big enough to see the tip, small enough to fit the tile
        const qreal dpr = qreal(size.height()) / strokeSize.height();
        const qreal largest = size.height() * 0.32;
        copy->settings()->setPaintOpSize(qBound(2.5 * dpr, copy->settings()->paintOpSize() * dpr, largest));
        Job job;
        job.id = brush.name;
        job.preset = copy;
        job.image = image;
        job.layer = new KisPaintLayer(image, QStringLiteral("stroke"), OPACITY_OPAQUE_U8, rgb);
        job.paint = paint;
        job.compositeOp = copy->settings()->paintOpCompositeOp();
        job.opacity = copy->settings()->paintOpOpacity();
        job.kind = brush.kind;
        job.size = size;
        jobs << job;
    }
    QMutexLocker locker(&strokes->mutex);
    strokes->queue << jobs;
    if (next >= toQueue.size()) {
        feed.stop();
        strokes->allQueued = true;
    }
}

KisStudioBrushPicker::~KisStudioBrushPicker()
{
    // the worker stops after the brush it is painting; what it holds goes here, in the GUI thread
    d->strokes->cancelled = true;
    d->worker.waitForFinished();
    QMutexLocker locker(&d->strokes->mutex);
    d->strokes->queue.clear();
    d->strokes->spent.clear();
}

void KisStudioBrushPicker::recordUse(const QString &presetName)
{
    QStringList recent = KisConfig(true).readEntry<QStringList>(recentKey, QStringList());
    if (!recent.isEmpty() && recent.first() == presetName) {
        return;
    }
    recent.removeAll(presetName);
    recent.prepend(presetName);
    KisConfig(false).writeEntry(recentKey, recent.mid(0, recentKept));
}

QString KisStudioBrushPicker::neighbour(KisMainWindow *, const QString &current, int step)
{
    const QStringList yours = pinnedIds(brushSource(loadCatalog(), nullptr));
    if (yours.isEmpty()) {
        return QString();
    }
    const int index = yours.indexOf(current);
    if (index < 0) {
        // not one of yours: into your list from its start or end
        return step > 0 ? yours.first() : yours.last();
    }
    const int to = index + step;
    return to >= 0 && to < yours.size() ? yours[to] : QString();
}

void KisStudioBrushPicker::use(KisViewManager *view, const QString &presetName)
{
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    if (!resources) {
        return;
    }
    KisPaintOpPresetSP preset = KisResourceServerProvider::instance()->paintOpPresetServer()->resource(QString(), QString(), presetName);
    if (preset) {
        resources->setPaintOpPreset(preset);
    }
}

QImage KisStudioBrushPicker::strokePicture(const QString &presetName)
{
    return latestStrokes().value(presetName);
}

QStringList KisStudioBrushPicker::yourBrushes()
{
    return pinnedIds(brushSource(loadCatalog(), nullptr));
}
