/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioEffectsPicker.h"

#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QTimer>
#include <QtConcurrent>

#include <atomic>

#include <KisGlobalResourcesInterface.h>
#include <KoColorSpaceRegistry.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisStudioFind.h"
#include "KisViewManager.h"
#include "filter/kis_filter.h"
#include "filter/kis_filter_category_ids.h"
#include "filter/kis_filter_configuration.h"
#include "filter/kis_filter_registry.h"
#include "kis_config.h"
#include "kis_filter_manager.h"
#include "kis_image.h"
#include "kis_node.h"
#include "kis_paint_device.h"

/**
 * The tiles' pictures: made in a worker thread, one effect after another,
 * from a small copy of the image; the picker collects the finished ones.
 */
struct KisStudioEffectThumbnails {
    QImage base;                        ///< the picture without effect, set before the worker starts
    QMutex mutex;
    QHash<QString, QImage> made;        ///< by the worker, under the mutex
    std::atomic<bool> cancelled {false};
    std::atomic<bool> finished {false};
    QHash<QString, QImage> shown;       ///< the ones collected, used in the GUI thread only
};

namespace {

using Group = KisStudioPicker::Group;

const QString recentKey = QStringLiteral("StudioRecentEffects");
constexpr int recentKept = 8;
/// The size of a tile's picture, see KisStudioPicker
const QSize thumbnailSize(112, 64);

/// Your effects until you change them: the ones most paintings get at some point
QStringList defaultEffects()
{
    return {
        QStringLiteral("gaussian blur"), QStringLiteral("motion blur"), QStringLiteral("unsharp"),
        QStringLiteral("levels"), QStringLiteral("perchannel"), QStringLiteral("brightnesscontrast"),
        QStringLiteral("hsvadjustment"), QStringLiteral("colorbalance"), QStringLiteral("desaturate"),
        QStringLiteral("gradientmap"), QStringLiteral("noise"), QStringLiteral("posterize"),
    };
}

/// The groups of "All effects": by what an effect does, in this order
QVector<QPair<QString, QString>> groupTitles()
{
    return {
        {QStringLiteral("soften"), i18nc("effect group", "Soften")},
        {QStringLiteral("sharpen"), i18nc("effect group", "Sharpen and clean up")},
        {QStringLiteral("tone"), i18nc("effect group", "Light and tone")},
        {QStringLiteral("color"), i18nc("effect group", "Color")},
        {QStringLiteral("stylize"), i18nc("effect group", "Stylize")},
        {QStringLiteral("distort"), i18nc("effect group", "Distort")},
        {QStringLiteral("edges"), i18nc("effect group", "Edges and relief")},
        {QStringLiteral("transparency"), i18nc("effect group", "Transparency")},
        {QStringLiteral("other"), i18nc("effect group", "Other")},
    };
}

/// Which group an effect is in: by what it does, or by its menu category for those not listed
QString groupOf(const QString &id)
{
    static const QHash<QString, QString> byId = {
        {QStringLiteral("blur"), QStringLiteral("soften")},
        {QStringLiteral("gaussian blur"), QStringLiteral("soften")},
        {QStringLiteral("lens blur"), QStringLiteral("soften")},
        {QStringLiteral("motion blur"), QStringLiteral("soften")},
        {QStringLiteral("unsharp"), QStringLiteral("sharpen")},
        {QStringLiteral("sharpen"), QStringLiteral("sharpen")},
        {QStringLiteral("gaussianhighpass"), QStringLiteral("sharpen")},
        {QStringLiteral("mean removal"), QStringLiteral("sharpen")},
        {QStringLiteral("gaussiannoisereducer"), QStringLiteral("sharpen")},
        {QStringLiteral("waveletnoisereducer"), QStringLiteral("sharpen")},
        {QStringLiteral("levels"), QStringLiteral("tone")},
        {QStringLiteral("brightnesscontrast"), QStringLiteral("tone")},
        {QStringLiteral("perchannel"), QStringLiteral("tone")},
        {QStringLiteral("autocontrast"), QStringLiteral("tone")},
        {QStringLiteral("normalize"), QStringLiteral("tone")},
        {QStringLiteral("dodge"), QStringLiteral("tone")},
        {QStringLiteral("burn"), QStringLiteral("tone")},
        {QStringLiteral("threshold"), QStringLiteral("tone")},
        {QStringLiteral("hsvadjustment"), QStringLiteral("color")},
        {QStringLiteral("colorbalance"), QStringLiteral("color")},
        {QStringLiteral("crosschannel"), QStringLiteral("color")},
        {QStringLiteral("desaturate"), QStringLiteral("color")},
        {QStringLiteral("invert"), QStringLiteral("color")},
        {QStringLiteral("gradientmap"), QStringLiteral("color")},
        {QStringLiteral("asc-cdl"), QStringLiteral("color")},
        {QStringLiteral("colortransfer"), QStringLiteral("color")},
        {QStringLiteral("fastcoloroverlay"), QStringLiteral("color")},
        {QStringLiteral("maximize"), QStringLiteral("color")},
        {QStringLiteral("minimize"), QStringLiteral("color")},
        {QStringLiteral("palettize"), QStringLiteral("color")},
        {QStringLiteral("indexcolors"), QStringLiteral("color")},
        {QStringLiteral("posterize"), QStringLiteral("stylize")},
        {QStringLiteral("pixelize"), QStringLiteral("stylize")},
        {QStringLiteral("oilpaint"), QStringLiteral("stylize")},
        {QStringLiteral("halftone"), QStringLiteral("stylize")},
        {QStringLiteral("smalltiles"), QStringLiteral("stylize")},
        {QStringLiteral("randompick"), QStringLiteral("stylize")},
        {QStringLiteral("noise"), QStringLiteral("stylize")},
        {QStringLiteral("roundcorners"), QStringLiteral("stylize")},
        {QStringLiteral("wave"), QStringLiteral("distort")},
        {QStringLiteral("raindrops"), QStringLiteral("distort")},
        {QStringLiteral("edge detection"), QStringLiteral("edges")},
        {QStringLiteral("height to normal"), QStringLiteral("edges")},
        {QStringLiteral("phongbumpmap"), QStringLiteral("edges")},
        {QStringLiteral("colortoalpha"), QStringLiteral("transparency")},
        {QStringLiteral("resettransparent"), QStringLiteral("transparency")},
    };
    const auto it = byId.constFind(id);
    if (it != byId.constEnd()) {
        return *it;
    }
    KisFilterSP filter = KisFilterRegistry::instance()->value(id);
    const QString category = filter ? filter->menuCategory().id() : QString();
    if (category == FiltersCategoryBlurId.id()) return QStringLiteral("soften");
    if (category == FiltersCategoryEnhanceId.id()) return QStringLiteral("sharpen");
    if (category == FiltersCategoryAdjustId.id()) return QStringLiteral("tone");
    if (category == FiltersCategoryColorId.id()) return QStringLiteral("color");
    if (category == FiltersCategoryArtisticId.id()) return QStringLiteral("stylize");
    if (category == FiltersCategoryEdgeDetectionId.id() || category == FiltersCategoryEmbossId.id()) return QStringLiteral("edges");
    if (category == FiltersCategoryMapId.id()) return QStringLiteral("distort");
    return QStringLiteral("other");
}

/// The image, cropped to the tiles' shape; the sample when there is nothing to see
QImage basePicture(KisViewManager *view, const QSize &size)
{
    QImage picture;
    KisImageSP image = view ? view->image() : nullptr;
    if (image && image->projection()) {
        const QImage whole = image->projection()->createThumbnail(size.width() * 2, size.height() * 2, Qt::KeepAspectRatio);
        if (!whole.isNull()) {
            const QImage covering = whole.scaled(size, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            picture = covering.copy((covering.width() - size.width()) / 2, (covering.height() - size.height()) / 2,
                                    size.width(), size.height());
        }
    }
    if (picture.isNull() || KisStudioPicker::isFlat(picture)) {
        picture = KisStudioPicker::samplePicture(size);
    }
    return picture.convertToFormat(QImage::Format_ARGB32);
}

KisStudioPicker::Source effectsSource(KisMainWindow *mainWindow, std::shared_ptr<KisStudioEffectThumbnails> thumbnails)
{
    // what Find knows of every effect that can be run: its name, its other names
    struct Info {
        QString title;
        QStringList aliases;
    };
    auto infos = std::make_shared<QHash<QString, Info>>();
    QStringList ids;
    KisStudioFind *find = mainWindow->studioFind();
    Q_FOREACH (const QString &id, KisFilterRegistry::instance()->keys()) {
        KisStudioFind::Item item;
        if (find && find->item(QStringLiteral("effect:") + id, &item)) {
            infos->insert(id, {item.title, item.aliases});
            ids << id;
        }
    }
    std::sort(ids.begin(), ids.end(), [infos](const QString &a, const QString &b) {
        return infos->value(a).title.localeAwareCompare(infos->value(b).title) < 0;
    });

    KisStudioPicker::Source source;
    source.pinnedKey = QStringLiteral("StudioEffects");
    source.initialPinned = defaultEffects;
    source.allIds = [ids]() { return ids; };
    source.otherGroups = [ids](const QStringList &pinned) {
        QVector<Group> groups;
        Q_FOREACH (const auto &title, groupTitles()) {
            Group group{title.second, {}};
            Q_FOREACH (const QString &id, ids) {
                if (!pinned.contains(id) && groupOf(id) == title.first) group.ids << id;
            }
            if (!group.ids.isEmpty()) groups << group;
        }
        return groups;
    };
    source.name = [infos](const QString &id) { return infos->value(id).title; };
    source.aliases = [infos](const QString &id) { return infos->value(id).aliases; };
    source.paintPreview = [thumbnails](QPainter &painter, const QRect &rect, const QString &id) {
        const QImage picture = thumbnails->shown.value(id);
        if (!picture.isNull()) {
            painter.drawImage(rect, picture);
        } else {
            // not made yet: the picture without the effect, faint
            painter.fillRect(rect, QColor(128, 128, 128, 40));
            painter.setOpacity(painter.opacity() * 0.3);
            painter.drawImage(rect, thumbnails->base);
        }
    };
    source.recent = []() { return KisConfig(true).readEntry<QStringList>(recentKey, QStringList()); };
    source.tiles = true;
    // only once the pointer rests on one, not for each it passes (a coarse preview shows within a frame or two)
    source.previewDelay = 150;
    source.yoursTitle = i18nc("effect group", "Your effects");
    source.searchPlaceholder = i18nc("@info:placeholder", "Search %1 effects", ids.size());
    // the number goes in later: %1 passes through
    source.allLabel = i18nc("@action:button %1 is a number", "All %1 effects", QStringLiteral("%1"));
    return source;
}

struct Job {
    QString id;
    KisFilterSP filter;
    KisFilterConfigurationSP config;
};

} // namespace

struct KisStudioEffectsPicker::Private
{
    KisMainWindow *mainWindow {nullptr};
    std::shared_ptr<KisStudioEffectThumbnails> thumbnails;
    QTimer collect;
    /// Whether the filter stroke running is this picker's preview, not an effect being adjusted
    bool previewing {false};

    void previewOnCanvas(const QString &id);
};

KisStudioEffectsPicker::KisStudioEffectsPicker(KisMainWindow *mainWindow)
    : KisStudioEffectsPicker(mainWindow, std::make_shared<KisStudioEffectThumbnails>())
{
}

KisStudioEffectsPicker::KisStudioEffectsPicker(KisMainWindow *mainWindow, std::shared_ptr<KisStudioEffectThumbnails> thumbnails)
    : KisStudioPicker(effectsSource(mainWindow, thumbnails), QString(), QSet<QString>(), mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->thumbnails = thumbnails;
    thumbnails->base = basePicture(mainWindow->viewManager(), thumbnailSize * mainWindow->devicePixelRatioF());

    // yours first, then the recent ones, then the rest: the ones seen first are made first
    const Source source = effectsSource(mainWindow, thumbnails);
    QStringList order = pinnedIds(source) + source.recent() + source.allIds();
    order.removeDuplicates();
    QVector<Job> jobs;
    Q_FOREACH (const QString &id, order) {
        KisFilterSP filter = KisFilterRegistry::instance()->value(id);
        KisFilterConfigurationSP config = filter ? filter->defaultConfiguration(KisGlobalResourcesInterface::instance()) : nullptr;
        if (config) {
            // the worker must not reach for resources itself
            jobs << Job{id, filter, config->cloneWithResourcesSnapshot()};
        }
    }
    (void)QtConcurrent::run([thumbnails, jobs]() {
        const QImage base = thumbnails->base;
        const QRect rect(QPoint(0, 0), base.size());
        Q_FOREACH (const Job &job, jobs) {
            if (thumbnails->cancelled) break;
            KisPaintDeviceSP device = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
            device->convertFromQImage(base, nullptr);
            job.filter->process(device, rect, job.config);
            const QImage picture = device->convertToQImage(nullptr, rect.x(), rect.y(), rect.width(), rect.height());
            QMutexLocker locker(&thumbnails->mutex);
            thumbnails->made.insert(job.id, picture);
        }
        thumbnails->finished = true;
    });

    // the finished ones, a few times a second, until all are
    d->collect.setInterval(80);
    connect(&d->collect, &QTimer::timeout, this, [this]() {
        bool any = false;
        {
            QMutexLocker locker(&d->thumbnails->mutex);
            for (auto it = d->thumbnails->made.constBegin(); it != d->thumbnails->made.constEnd(); ++it) {
                d->thumbnails->shown.insert(it.key(), it.value());
                any = true;
            }
            d->thumbnails->made.clear();
        }
        if (any) updatePreviews();
        if (d->thumbnails->finished && !any) d->collect.stop();
    });
    d->collect.start();

    connect(this, &KisStudioPicker::sigPreview, this, [this](const QString &id) { d->previewOnCanvas(id); });
    connect(this, &KisStudioPicker::sigPicked, this, [mainWindow](const QString &id) {
        recordUse(id);
        // as from anywhere else: adjusted in the effect panel, or applied if there is nothing to adjust
        QTimer::singleShot(0, mainWindow, [mainWindow, id]() {
            if (KisStudioFind *find = mainWindow->studioFind()) find->run(QStringLiteral("effect:") + id);
        });
    });
}

KisStudioEffectsPicker::~KisStudioEffectsPicker()
{
    // the worker stops after the effect it is making
    d->thumbnails->cancelled = true;
}

void KisStudioEffectsPicker::Private::previewOnCanvas(const QString &id)
{
    KisViewManager *view = mainWindow->viewManager();
    KisFilterManager *filterManager = view ? view->filterManager() : nullptr;
    if (!filterManager) {
        return;
    }
    if (id.isEmpty()) {
        if (previewing && filterManager->isStrokeRunning()) {
            filterManager->cancelRunningStroke();
        }
        previewing = false;
        return;
    }
    // an effect being adjusted owns the filter stroke: leave it alone
    if (filterManager->isStrokeRunning() && !previewing) {
        return;
    }
    KisNodeSP node = view->activeNode();
    KisFilterSP filter = KisFilterRegistry::instance()->value(id);
    if (!node || !node->paintDevice() || !node->isEditable() || !filter) {
        return;
    }
    KisFilterConfigurationSP config = filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
    if (!config) {
        return;
    }
    filterManager->apply(config);
    previewing = true;
}

void KisStudioEffectsPicker::recordUse(const QString &filterId)
{
    QStringList recent = KisConfig(true).readEntry<QStringList>(recentKey, QStringList());
    recent.removeAll(filterId);
    recent.prepend(filterId);
    KisConfig(false).writeEntry(recentKey, recent.mid(0, recentKept));
}
