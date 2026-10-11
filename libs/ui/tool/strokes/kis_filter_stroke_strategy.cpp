/*
 *  SPDX-FileCopyrightText: 2013 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisUpdatePacer.h"
#include <QElapsedTimer>
#include <cmath>
#include <kis_painter.h>
#include <kis_lod_transform_base.h>
#include <kis_filter_strategy.h>
#include <kis_transform_worker.h>
#include "kis_filter_stroke_strategy.h"

#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <krita_utils.h>
#include <kis_layer_utils.h>
#include <kis_raster_keyframe_channel.h>
#include <kis_transaction.h>
#include <kis_paint_device_frames_interface.h>
#include <KisRunnableStrokeJobUtils.h>
#include <KisRunnableStrokeJobsInterface.h>
#include <KoCompositeOpRegistry.h>
#include "kis_image_config.h"
#include "kis_image_animation_interface.h"
#include "kis_painter.h"
#include "KisAnimAutoKey.h"
#include <commands_new/KisDisableDirtyRequestsCommand.h>
#include <KoMixColorsOp.h>
#include <KoChannelInfo.h>


struct KisFilterStrokeStrategy::Private {
    Private()
        : updatesFacade(0)
        , levelOfDetail(0)
    {
    }

    Private(const Private &rhs)
        : filter(rhs.filter)
        , filterConfig(rhs.filterConfig)
        , node(rhs.node)
        , targetDevice(rhs.targetDevice)
        , activeSelection(rhs.activeSelection)
        , image(rhs.image)
        , updatesFacade(rhs.updatesFacade)
        , levelOfDetail(0)
        , cancelledUpdates(rhs.cancelledUpdates)

    {
        KIS_ASSERT_RECOVER_RETURN(!rhs.levelOfDetail);
    }

    KisFilterSP filter;
    KisFilterConfigurationSP filterConfig;
    KisNodeSP node;
    KisPaintDeviceSP targetDevice;
    KisSelectionSP activeSelection;
    KisImageSP image;
    KisUpdatesFacade *updatesFacade;
    int levelOfDetail;

    ExternalCancelUpdatesStorageSP cancelledUpdates;
    QRect nextExternalUpdateRect;
    bool hasBeenLodCloned = false;

    /// A preview: a coarse pass first (see setProgressivePreview)
    bool progressive = false;
    KisFilterStrokeStrategy::PreviewHandleSP preview;
    /// the full pass's progress, interrupted on cancelling
    QMutex progressMutex;
    QWeakPointer<KisProcessingVisitor::ProgressHelper> progress;
    /// the coarse pass being written to the target (its scaling up), reverted if cancelled then
    QScopedPointer<KisTransaction> coarseWriting;
    /// The coarse pass written to the target, not in the history: undone before the full pass is written, or on cancelling
    QSharedPointer<KUndo2Command> coarse;
};

namespace {
/// The bounds of a device as it is a level of detail smaller: what a filter reads to scale its sizes to it (KisLodTransformScalar)
class LodBounds : public KisDefaultBoundsBase
{
public:
    LodBounds(KisDefaultBoundsBaseSP base, int lod)
        : m_base(base)
        , m_lod(lod)
    {
    }
    QRect bounds() const override { return KisLodTransformBase::scaledRect(m_base->bounds(), m_lod); }
    bool wrapAroundMode() const override { return m_base->wrapAroundMode(); }
    WrapAroundAxis wrapAroundModeAxis() const override { return m_base->wrapAroundModeAxis(); }
    int currentLevelOfDetail() const override { return m_lod; }
    int currentTime() const override { return m_base->currentTime(); }
    bool externalFrameActive() const override { return m_base->externalFrameActive(); }
    void *sourceCookie() const override { return m_base->sourceCookie(); }

private:
    KisDefaultBoundsBaseSP m_base;
    int m_lod;
};

/**
 * The level a coarse pass is made at, or 0 for none worth it: the region
 * about half a megapixel then, and the pixels around each a filter reads
 * few enough (a large blur made as small as it needs, its cost a small one's)
 */
int coarseLevel(KisFilterSP filter, const KisFilterConfigurationSP &config, const QRect &rect)
{
    const qreal pixels = qreal(rect.width()) * rect.height();
    if (pixels < 2.0e6) return 0;
    // per pixel only (a curve, levels): as fast at full resolution as making it smaller and back
    const QRect needed = filter->neededRect(rect, config, 0);
    if (needed.width() <= rect.width() + 4 && needed.height() <= rect.height() + 4) return 0;
    const int margin = qMax(needed.width() - rect.width(), needed.height() - rect.height()) / 2;
    int lod = qBound(1, qCeil(std::log(pixels / 0.5e6) / std::log(4.0)), 6);
    while (lod < 6 && (margin >> lod) > 24) lod++;
    while (lod > 0 && !filter->supportsLevelOfDetail(config, lod)) lod--;
    return lod;
}

/**
 * \p patch of a device \p scale times larger than \p in (its pixels \p from),
 * bilinear: each pixel the mix of the four small ones around it, a row of
 * the small ones mixed first, then along it. For channels all of one plain
 * type (8, 16 bit, float), mixed as numbers.
 */
template <typename T>
void upscaleBilinear(const quint8 *in, const QRect &from, quint8 *out, const QRect &patch, int channels, int scale)
{
    auto source = [scale](int x) { return (x + 0.5f) / scale - 0.5f; };
    QVector<int> x0(patch.width()), x1(patch.width());
    QVector<float> wx(patch.width());
    for (int x = 0; x < patch.width(); x++) {
        const float fx = source(patch.left() + x);
        const int column = qBound(0, qFloor(fx) - from.left(), from.width() - 1);
        x0[x] = column * channels;
        x1[x] = qMin(column + 1, from.width() - 1) * channels;
        wx[x] = fx - qFloor(fx);
    }
    const int rowLength = from.width() * channels;
    QVector<float> mixed(rowLength);
    const T *src = reinterpret_cast<const T *>(in);
    T *dst = reinterpret_cast<T *>(out);
    const bool integer = std::numeric_limits<T>::is_integer;
    for (int y = 0; y < patch.height(); y++) {
        const float fy = source(patch.top() + y);
        const int y0 = qBound(0, qFloor(fy) - from.top(), from.height() - 1);
        const int y1 = qMin(y0 + 1, from.height() - 1);
        const float wy = fy - qFloor(fy);
        const T *a = src + y0 * rowLength;
        const T *b = src + y1 * rowLength;
        for (int i = 0; i < rowLength; i++) {
            mixed[i] = float(a[i]) + (float(b[i]) - float(a[i])) * wy;
        }
        for (int x = 0; x < patch.width(); x++) {
            const float *p = mixed.constData() + x0[x];
            const float *q = mixed.constData() + x1[x];
            const float w = wx[x];
            for (int c = 0; c < channels; c++) {
                const float v = p[c] + (q[c] - p[c]) * w;
                *dst++ = integer ? T(v + 0.5f) : T(v);
            }
        }
    }
}
} // namespace

KisFilterStrokeStrategy::PreviewHandleSP KisFilterStrokeStrategy::setProgressivePreview(bool progressive)
{
    m_d->progressive = progressive;
    m_d->preview = progressive ? PreviewHandleSP(new PreviewHandle) : PreviewHandleSP();
    return m_d->preview;
}

void KisFilterStrokeStrategy::tryCancelCurrentStrokeJobAsync()
{
    if (m_d->preview) {
        QMutexLocker l(&m_d->preview->mutex);
        m_d->preview->cancelled = true;
        m_d->preview->wake.wakeAll();
    }
    QMutexLocker l(&m_d->progressMutex);
    if (QSharedPointer<KisProcessingVisitor::ProgressHelper> progress = m_d->progress.toStrongRef()) {
        progress->cancel();
    }
}

struct SubTaskSharedData {

    SubTaskSharedData(KisImageSP image, KisNodeSP node, int levelOfDetail,
                      KisSelectionSP selection, KisFilterSP filter, KisFilterConfigurationSP config,
                      KisFilterStrokeStrategy::FilterJobData* filterFrameData )
        : m_image(image)
        , m_node(node)
        , m_levelOfDetail(levelOfDetail)
        , m_targetDevice(node->paintDevice())
        , m_selection(selection)
        , m_filter(filter)
        , m_filterConfig(config)
        , m_storage(new KisLayerUtils::SwitchFrameCommand::SharedStorage()){

        m_frameTime = filterFrameData->frameTime;
        m_shouldSwitchTime = filterFrameData->frameTime != -1;

        m_shouldRedraw = !m_shouldSwitchTime || filterFrameData->frameTime == KisLayerUtils::fetchLayerActiveRasterFrameTime(m_node);
    }

    ~SubTaskSharedData(){}


    KisImageSP image() { return m_image; }

    KisNodeSP node() { return m_node; }

    int levelOfDetail() { return m_levelOfDetail; }

    KisPaintDeviceSP targetDevice() { return m_targetDevice; }

    KisSelectionSP selection() { return m_selection; }

    KisFilterSP filter() { return m_filter; }

    KisFilterConfigurationSP filterConfig() { return m_filterConfig; }

    int frameTime() {
        return m_frameTime;
    }

    bool shouldSwitchTime() { return m_shouldSwitchTime; }

    bool shouldRedraw() { return m_shouldRedraw; }

    KisLayerUtils::SwitchFrameCommand::SharedStorageSP storage() { return m_storage; }

public:
    KisPaintDeviceSP filterDevice;
    QRect filterDeviceBounds;
    QSharedPointer<KisTransaction> filterDeviceTransaction;
    QRect processRect;

private:
    KisImageSP m_image;
    KisNodeSP m_node;
    int m_levelOfDetail;
    KisPaintDeviceSP m_targetDevice;
    KisSelectionSP m_selection;
    KisFilterSP m_filter;
    KisFilterConfigurationSP m_filterConfig;
    bool m_shouldSwitchTime;
    bool m_shouldRedraw;
    int m_frameTime;
    KisLayerUtils::SwitchFrameCommand::SharedStorageSP m_storage;

};

using namespace KritaUtils;

KisFilterStrokeStrategy::KisFilterStrokeStrategy(KisFilterSP filter, KisFilterConfigurationSP filterConfig, KisResourcesSnapshotSP resources)
    : KisFilterStrokeStrategy(filter, filterConfig, resources, toQShared(new ExternalCancelUpdatesStorage()))
{
    // by default, cancellation updates are disabled, so we should enable them
    m_d->cancelledUpdates->shouldIssueCancellationUpdates.ref();
}

KisFilterStrokeStrategy::KisFilterStrokeStrategy(KisFilterSP filter,
                                                 KisFilterConfigurationSP filterConfig,
                                                 KisResourcesSnapshotSP resources,
                                                 ExternalCancelUpdatesStorageSP externalCancelUpdatesStorage)
    : KisStrokeStrategyUndoCommandBased(kundo2_i18nc("Filter as an effect", "Filter \"%1\"", filter->name()),
                                        false,
                                        resources->image().data())
    , m_d(new Private())
{
    m_d->filter = filter;
    m_d->filterConfig = filterConfig;
    m_d->node = resources->currentNode();
    m_d->targetDevice = resources->currentNode()->paintDevice();
    m_d->activeSelection = resources->activeSelection();
    m_d->image = resources->image();
    m_d->updatesFacade = resources->image().data();
    m_d->levelOfDetail = 0;
    m_d->cancelledUpdates = externalCancelUpdatesStorage;

    setNeedsExplicitCancel(true);
    setSupportsWrapAroundMode(true);
    enableJob(KisSimpleStrokeStrategy::JOB_INIT);
    enableJob(KisSimpleStrokeStrategy::JOB_DOSTROKE);
    enableJob(KisSimpleStrokeStrategy::JOB_CANCEL);
    enableJob(KisSimpleStrokeStrategy::JOB_FINISH);
}

KisFilterStrokeStrategy::KisFilterStrokeStrategy(const KisFilterStrokeStrategy &rhs, int levelOfDetail)
    : KisStrokeStrategyUndoCommandBased(rhs)
    , m_d(new Private(*rhs.m_d))
{
    m_d->levelOfDetail = levelOfDetail;
}

KisFilterStrokeStrategy::~KisFilterStrokeStrategy()
{
    delete m_d;
}

void KisFilterStrokeStrategy::initStrokeCallback()
{
    KisStrokeStrategyUndoCommandBased::initStrokeCallback();

    qSwap(m_d->nextExternalUpdateRect, m_d->cancelledUpdates->updateRect);
    KisLodTransform t(m_d->levelOfDetail);
    m_d->nextExternalUpdateRect = t.map(m_d->nextExternalUpdateRect);

    KUndo2Command *autoKeyframeCommand =
        KisAutoKey::tryAutoCreateDuplicatedFrame(m_d->node->paintDevice(),
                                                 KisAutoKey::SupportsLod);
    if (autoKeyframeCommand) {
        runAndSaveCommand(toQShared(autoKeyframeCommand), KisStrokeJobData::BARRIER, KisStrokeJobData::NORMAL);
    }
}


void KisFilterStrokeStrategy::doStrokeCallback(KisStrokeJobData *data)
{
    FilterJobData *filterFrameData = dynamic_cast<FilterJobData*>(data);
    KisRunnableStrokeJobData *jobData = dynamic_cast<KisRunnableStrokeJobData*>(data);

    // Populate list of jobs for filter application...
    if (filterFrameData) {

        QVector<KisRunnableStrokeJobData*> jobs;

        QSharedPointer<SubTaskSharedData> shared( new SubTaskSharedData(m_d->image, m_d->node, m_d->levelOfDetail,
                                                                        m_d->activeSelection, m_d->filter, m_d->filterConfig, filterFrameData) );
        QSharedPointer<KisProcessingVisitor::ProgressHelper> progress( new KisProcessingVisitor::ProgressHelper(m_d->node) );
        if (m_d->preview) {
            QMutexLocker l(&m_d->progressMutex);
            m_d->progress = progress;
        }
        addJobSequential(jobs, [this, shared, progress](){
            // Switch time if necessary..
            if (shared->shouldSwitchTime()) {
                runAndSaveCommand( toQShared( new KisLayerUtils::SwitchFrameCommand(shared->image(), shared->frameTime(), false, shared->storage()) )
                                   , KisStrokeJobData::BARRIER, KisStrokeJobData::NORMAL);
            }

            // Copy snapshot of targetDevice for filter processing..
            shared->filterDevice = new KisPaintDevice(*shared->targetDevice());

            // Update all necessary rect data based on contents of frame..
            shared->filterDeviceBounds = shared->filterDevice->exactBounds();

            if (shared->filter()->needsTransparentPixels(shared->filterConfig().data(), shared->targetDevice()->colorSpace())) {
                shared->filterDeviceBounds |= shared->targetDevice()->defaultBounds()->bounds();
            }

            shared->processRect = shared->filter()->changedRect(shared->filterDeviceBounds, shared->filterConfig(), shared->levelOfDetail());
            shared->processRect &= shared->targetDevice()->defaultBounds()->bounds();

            //If we're dealing with some kind of transparency mask, we will create a compositionSourceDevice instead.
            //  Carry over from commit ca810f85 ...
            if (shared->selection() ||
                    (shared->targetDevice()->colorSpace() != shared->targetDevice()->compositionSourceColorSpace() &&
                    *shared->targetDevice()->colorSpace() != *shared->targetDevice()->compositionSourceColorSpace())) {

                shared->filterDevice = shared->targetDevice()->createCompositionSourceDevice(shared->targetDevice());

                if (shared->selection()) {
                    shared->filterDeviceBounds &= shared->selection()->selectedRect();
                }
            }

            // the full pass: its patches processed, after which the next job writes them
            auto startFull = [this, shared, progress]() {
                // Filter device needs a transaction to prevent grid-patch artifacts from multithreaded read/write.
                shared->filterDeviceTransaction.reset(new KisTransaction(shared->filterDevice));


                // Actually process the device

                QVector<KisRunnableStrokeJobData*> processJobs;

                if (shared->filter()->supportsThreading()) {
                    // Split stroke into patches...
                    QSize size = KritaUtils::optimalPatchSize();
                    QVector<QRect> patches = KritaUtils::splitRectIntoPatches(shared->processRect, size);

                    Q_FOREACH (const QRect &patch, patches) {
                        if (!patch.isEmpty()) {
                            addJobConcurrent(processJobs, [patch, shared, progress](){
                                shared->filter()->processImpl(shared->filterDevice, patch,
                                                              shared->filterConfig().data(),
                                                              progress->updater());
                            });
                        }
                    }
                } else {
                    if (!shared->processRect.isEmpty()) {
                        addJobSequential(processJobs, [shared, progress](){
                            shared->filter()->processImpl(shared->filterDevice, shared->processRect,
                                                          shared->filterConfig().data(),
                                                          progress->updater());
                        });
                    }
                }
                runnableJobsInterface()->addRunnableJobs(processJobs);
            };

            /**
             * A preview, at full resolution, of a large region: first the
             * filter on the source made a few times smaller (box averaged,
             * as Krita's instant preview makes it, its sizes scaled to it),
             * scaled back up and shown; the full pass takes its place when it
             * is done (see m_d->coarse). Its filter and its scaling up run in
             * patches, on all the threads, as the full pass does.
             */
            const int lod = m_d->progressive && m_d->levelOfDetail == 0 && !shared->shouldSwitchTime()
                ? coarseLevel(shared->filter(), shared->filterConfig(), shared->processRect) : 0;
            if (lod <= 0 || shared->processRect.isEmpty()) {
                startFull();
                return;
            }

            struct Coarse {
                KisPaintDeviceSP small;
                KisPaintDeviceSP up;
                QScopedPointer<KisTransaction> smallTransaction;
                QElapsedTimer time;
                qint64 filterStart = 0;
                qint64 filterTime = 0;
            };
            QSharedPointer<Coarse> coarse(new Coarse);
            coarse->time.start();
            const KoColorSpace *space = shared->filterDevice->colorSpace();
            coarse->small = new KisPaintDevice(space);
            coarse->small->setDefaultBounds(new LodBounds(shared->filterDevice->defaultBounds(), lod));
            coarse->small->setDefaultPixel(shared->filterDevice->defaultPixel());
            const QRect aligned = KisLodTransformBase::alignedRect(shared->processRect, lod);
            const QRect needed = KisLodTransformBase::alignedRect(shared->filter()->neededRect(aligned, shared->filterConfig().data(), 0), lod);
            const QRect smallRect = KisLodTransformBase::scaledRect(aligned, lod);
            // scaled up into the target itself, or with a selection, a device of its own copied through it
            if (shared->selection()) {
                coarse->up = new KisPaintDevice(space);
                coarse->up->setDefaultPixel(shared->filterDevice->defaultPixel());
            }

            QVector<KisRunnableStrokeJobData*> coarseJobs;
            /**
             * Made small in strips, on all the threads (each a whole number
             * of the small image's tiles high); smaller than a quarter, a
             * quarter first (as many strips), then the rest from it.
             */
            {
                const int first = qMin(lod, 2);
                KisPaintDeviceSP quarter = lod > first ? new KisPaintDevice(space) : coarse->small;
                const int strip = 64 << first;
                // what is outside the source is its default pixel, in the small one as well
                const QRect source = needed & KisLodTransformBase::alignedRect(shared->filterDevice->extent(), lod);
                // on the small one's tile rows, no two strips writing one tile
                for (int y = source.top() - (source.top() % strip + strip) % strip; y <= source.bottom(); y += strip) {
                    const QRect part = QRect(source.left(), y, source.width(), strip) & source;
                    addJobConcurrent(coarseJobs, [shared, quarter, part, first]() {
                        shared->filterDevice->generateLodCloneDevice(quarter, part, first);
                    });
                }
                if (quarter != coarse->small) {
                    addJobSequential(coarseJobs, [coarse, quarter, source, first, lod]() mutable {
                        quarter->generateLodCloneDevice(coarse->small, KisLodTransformBase::scaledRect(source, first), lod - first);
                    });
                }
                addJobSequential(coarseJobs, [coarse]() {
                    coarse->filterStart = coarse->time.elapsed();
                    // as the full pass: read what it is, not what a patch beside it already wrote
                    coarse->smallTransaction.reset(new KisTransaction(coarse->small));
                });
            }
            const QSize patchSize = KritaUtils::optimalPatchSize();
            // the small image in patches its size: as many as the full one has
            const QSize smallPatch(qMax(64, patchSize.width() >> lod), qMax(64, patchSize.height() >> lod));
            if (shared->filter()->supportsThreading()) {
                Q_FOREACH (const QRect &patch, KritaUtils::splitRectIntoPatches(smallRect, smallPatch)) {
                    if (patch.isEmpty()) continue;
                    addJobConcurrent(coarseJobs, [shared, coarse, patch]() {
                        shared->filter()->processImpl(coarse->small, patch, shared->filterConfig().data(), nullptr);
                    });
                }
            } else {
                addJobSequential(coarseJobs, [shared, coarse, smallRect]() {
                    shared->filter()->processImpl(coarse->small, smallRect, shared->filterConfig().data(), nullptr);
                });
            }
            addJobSequential(coarseJobs, [this, shared, coarse]() {
                coarse->filterTime = coarse->time.elapsed() - coarse->filterStart;
                delete coarse->smallTransaction->endAndTake();
                coarse->smallTransaction.reset();
                m_d->coarseWriting.reset(new KisTransaction(shared->targetDevice()));
            });
            // back up to full resolution, bilinear: each pixel the mix of the four small ones around it
            Q_FOREACH (const QRect &patch, KritaUtils::splitRectIntoPatches(shared->processRect, patchSize)) {
                if (patch.isEmpty()) continue;
                addJobConcurrent(coarseJobs, [shared, coarse, patch, lod]() {
                    const int scale = 1 << lod;
                    const KoColorSpace *cs = coarse->small->colorSpace();
                    const int pixelSize = int(cs->pixelSize());
                    auto source = [scale](int x) { return (x + 0.5) / scale - 0.5; };
                    const QRect from(QPoint(qFloor(source(patch.left())), qFloor(source(patch.top()))),
                                     QPoint(qFloor(source(patch.right())) + 1, qFloor(source(patch.bottom())) + 1));
                    QVector<quint8> in(from.width() * from.height() * pixelSize);
                    coarse->small->readBytes(in.data(), from);
                    QVector<quint8> out(patch.width() * patch.height() * pixelSize);
                    KoMixColorsOp *mix = cs->mixColorsOp();
                    const quint8 *colors[4];
                    qint16 weights[4];
                    // channels all of one plain type (8, 16 bit, float): each mixed as a number, not through the color space
                    const QList<KoChannelInfo *> channels = cs->channels();
                    const KoChannelInfo::enumChannelValueType type = channels.first()->channelValueType();
                    bool plain = type == KoChannelInfo::UINT8 || type == KoChannelInfo::UINT16 || type == KoChannelInfo::FLOAT32;
                    Q_FOREACH (const KoChannelInfo *channel, channels) plain &= channel->channelValueType() == type;
                    const int count = channels.size();
                    KisPaintDeviceSP target = coarse->up ? coarse->up : shared->targetDevice();
                    if (plain && count * int(channels.first()->size()) == pixelSize) {
                        if (type == KoChannelInfo::UINT8) {
                            upscaleBilinear<quint8>(in.constData(), from, out.data(), patch, count, scale);
                        } else if (type == KoChannelInfo::UINT16) {
                            upscaleBilinear<quint16>(in.constData(), from, out.data(), patch, count, scale);
                        } else {
                            upscaleBilinear<float>(in.constData(), from, out.data(), patch, count, scale);
                        }
                        target->writeBytes(out.constData(), patch);
                        return;
                    }
                    auto blend = [&](quint8 *dst, const qreal w[4]) {
                        for (int c = 0; c < count; c++) {
                            if (type == KoChannelInfo::UINT8) {
                                qreal v = 0;
                                for (int k = 0; k < 4; k++) v += w[k] * colors[k][c];
                                dst[c] = quint8(qBound(0.0, v + 0.5, 255.0));
                            } else if (type == KoChannelInfo::UINT16) {
                                qreal v = 0;
                                for (int k = 0; k < 4; k++) v += w[k] * reinterpret_cast<const quint16 *>(colors[k])[c];
                                reinterpret_cast<quint16 *>(dst)[c] = quint16(qBound(0.0, v + 0.5, 65535.0));
                            } else {
                                float v = 0;
                                for (int k = 0; k < 4; k++) v += float(w[k]) * reinterpret_cast<const float *>(colors[k])[c];
                                reinterpret_cast<float *>(dst)[c] = v;
                            }
                        }
                    };
                    for (int y = 0; y < patch.height(); y++) {
                        const qreal fy = source(patch.top() + y);
                        const int y0 = qBound(0, qFloor(fy) - from.top(), from.height() - 1);
                        const int y1 = qMin(y0 + 1, from.height() - 1);
                        const qreal wy = fy - qFloor(fy);
                        for (int x = 0; x < patch.width(); x++) {
                            const qreal fx = source(patch.left() + x);
                            const int x0 = qBound(0, qFloor(fx) - from.left(), from.width() - 1);
                            const int x1 = qMin(x0 + 1, from.width() - 1);
                            const qreal wx = fx - qFloor(fx);
                            colors[0] = in.constData() + (y0 * from.width() + x0) * pixelSize;
                            colors[1] = in.constData() + (y0 * from.width() + x1) * pixelSize;
                            colors[2] = in.constData() + (y1 * from.width() + x0) * pixelSize;
                            colors[3] = in.constData() + (y1 * from.width() + x1) * pixelSize;
                            quint8 *dst = out.data() + (y * patch.width() + x) * pixelSize;
                            if (plain) {
                                const qreal w[4] = {(1 - wx) * (1 - wy), wx * (1 - wy), (1 - wx) * wy, wx * wy};
                                blend(dst, w);
                                continue;
                            }
                            weights[0] = qint16(qRound((1 - wx) * (1 - wy) * 255));
                            weights[1] = qint16(qRound(wx * (1 - wy) * 255));
                            weights[2] = qint16(qRound((1 - wx) * wy * 255));
                            weights[3] = qint16(255 - weights[0] - weights[1] - weights[2]);
                            mix->mixColors(colors, weights, 4, dst, 255);
                        }
                    }
                    target->writeBytes(out.constData(), patch);
                });
            }
            // shown, and the full pass started
            addJobSequential(coarseJobs, [this, shared, coarse, lod, startFull]() {
                if (coarse->up) {
                    KisPainter::copyAreaOptimized(shared->processRect.topLeft(), coarse->up, shared->targetDevice(), shared->processRect, shared->selection());
                }
                m_d->coarse = toQShared(m_d->coarseWriting->endAndTake());
                m_d->coarseWriting.reset();
                KisUpdatePacer::log(QStringLiteral("filter coarse pass (lod %1) shown %2 ms").arg(lod).arg(coarse->time.elapsed()));
                if (shared->shouldRedraw()) {
                    shared->node()->setDirty(shared->processRect);
                    m_d->nextExternalUpdateRect |= shared->processRect;
                }
                if (m_d->preview) m_d->preview->coarseShown.storeRelease(1);

                /**
                 * The full pass once the settings stay the same a moment
                 * (or OK is pressed): while they change, each coarse pass
                 * follows the last, none of them waiting for a full pass.
                 * The moment longer as the full pass is (about the coarse
                 * filter's time, as many times as it has fewer pixels): a
                 * long one, once started, is slow to stop for the next
                 * settings (a large blur, a part of it at a time).
                 */
                const qint64 estimate = coarse->filterTime << (2 * lod);
                const qint64 settleTime = qBound<qint64>(150, estimate / 4, 1000);
                QVector<KisRunnableStrokeJobData*> settle;
                addJobSequential(settle, [this, startFull, settleTime]() {
                    if (m_d->preview) {
                        QMutexLocker l(&m_d->preview->mutex);
                        QElapsedTimer waited;
                        waited.start();
                        while (!m_d->preview->released && !m_d->preview->cancelled && waited.elapsed() < settleTime) {
                            m_d->preview->wake.wait(&m_d->preview->mutex, settleTime - waited.elapsed());
                        }
                        if (m_d->preview->cancelled) return;
                    }
                    startFull();
                });
                runnableJobsInterface()->addRunnableJobs(settle);
            });
            runnableJobsInterface()->addRunnableJobs(coarseJobs);

        });

        addJobSequential(jobs, [this, shared](){
            // We will first apply the transaction to the temporary filterDevice
            runAndSaveCommand(toQShared(shared->filterDeviceTransaction->endAndTake()), KisStrokeJobData::BARRIER, KisStrokeJobData::NORMAL);
            shared->filterDeviceTransaction.reset();

            if (!shared->filterDeviceBounds.intersects(
                    shared->filter()->neededRect(shared->processRect, shared->filterConfig().data(), shared->levelOfDetail()))) {
                return;
            }

            KisUpdatePacer::log(QStringLiteral("filter full pass written"));
            // the coarse pass taken back first, so that undoing this restores the source, not it
            if (m_d->coarse) {
                m_d->coarse->undo();
                m_d->coarse.clear();
            }

            // Make a transaction, change the target device, and "end" transaction.
            // Should be useful for undoing later.
            QScopedPointer<KisTransaction> workingTransaction( new KisTransaction(shared->targetDevice()) );
            KisPainter::copyAreaOptimized(shared->processRect.topLeft(), shared->filterDevice, shared->targetDevice(), shared->processRect, shared->selection());
            runAndSaveCommand( toQShared(workingTransaction->endAndTake()), KisStrokeJobData::BARRIER, KisStrokeJobData::EXCLUSIVE );

            if (shared->shouldRedraw()) {
                QRect extraUpdateRect;
                qSwap(extraUpdateRect, m_d->nextExternalUpdateRect);

                shared->node()->setDirty(shared->processRect | extraUpdateRect);

               /**
                * Save the last update to be able to restore the
                * original state on the cancellation (even when
                * the cancellation step is explicitly prohibited)
                */
                m_d->nextExternalUpdateRect = shared->processRect;
            }
        });

        addJobSequential(jobs, [this, shared](){
            shared->image()->animationInterface()->invalidateFrame(shared->frameTime(), shared->node());
            if (shared->shouldSwitchTime()) {
                runAndSaveCommand( toQShared( new KisLayerUtils::SwitchFrameCommand(shared->image(), shared->frameTime(), true, shared->storage()) )
                                   , KisStrokeJobData::BARRIER, KisStrokeJobData::EXCLUSIVE);
            }
        });

        runnableJobsInterface()->addRunnableJobs(jobs);

    } else if (dynamic_cast<IdleBarrierData*>(data)) {
        /* noop, just delete that */
    } else if (jobData) {
        jobData->run();
    } else {
        KisStrokeStrategyUndoCommandBased::doStrokeCallback(data);
    }
}

void KisFilterStrokeStrategy::cancelStrokeCallback()
{
    using namespace KritaUtils;

    const bool shouldIssueCancellationUpdates = m_d->cancelledUpdates->shouldIssueCancellationUpdates;

    QVector<KisStrokeJobData *> jobs;

    jobs << new Data(toQShared(new KisDisableDirtyRequestsCommand(m_d->updatesFacade, KisDisableDirtyRequestsCommand::INITIALIZING)));
    // a coarse pass shown and its full pass not written yet: taken back as well
    addJobSequential(jobs, [this] () {
        if (m_d->coarseWriting) {
            m_d->coarseWriting->revert();
            m_d->coarseWriting.reset();
        }
        if (m_d->coarse) {
            m_d->coarse->undo();
            m_d->coarse.clear();
        }
    });
    KisStrokeStrategyUndoCommandBased::cancelStrokeCallbackImpl(jobs);
    jobs << new Data(toQShared(new KisDisableDirtyRequestsCommand(m_d->updatesFacade, KisDisableDirtyRequestsCommand::FINALIZING)));

    if (shouldIssueCancellationUpdates) {
        addJobSequential(jobs, [this] () {
            QRect updateRect =
                m_d->cancelledUpdates->updateRect |
                m_d->nextExternalUpdateRect;

            if (m_d->levelOfDetail <= 0) {
                updateRect |= m_d->cancelledUpdates->cancelledLod0UpdateRect;
            }

            if (!updateRect.isEmpty()) {
                m_d->node->setDirty(updateRect);
            }
        });
    } else if (!m_d->nextExternalUpdateRect.isEmpty()) {

        KisLodTransform t(m_d->levelOfDetail);
        m_d->cancelledUpdates->updateRect = t.mapInverted(m_d->nextExternalUpdateRect);

        if (!m_d->hasBeenLodCloned && m_d->levelOfDetail <= 0) {
            m_d->cancelledUpdates->cancelledLod0UpdateRect = m_d->nextExternalUpdateRect;
        }
    }

    addMutatedJobs(jobs);
}

void KisFilterStrokeStrategy::finishStrokeCallback()
{
    KisStrokeStrategyUndoCommandBased::finishStrokeCallback();
}

KisStrokeStrategy* KisFilterStrokeStrategy::createLodClone(int levelOfDetail)
{
    if (!m_d->filter->supportsLevelOfDetail(m_d->filterConfig.data(), levelOfDetail)) return 0;
    if (!m_d->node->supportsLodPainting()) return 0;

    KisFilterStrokeStrategy *clone = new KisFilterStrokeStrategy(*this, levelOfDetail);
    m_d->hasBeenLodCloned = true;
    return clone;
}
