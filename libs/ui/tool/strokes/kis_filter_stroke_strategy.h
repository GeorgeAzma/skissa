/*
 *  SPDX-FileCopyrightText: 2013 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef __KIS_FILTER_STROKE_STRATEGY_H
#define __KIS_FILTER_STROKE_STRATEGY_H

#include "kis_stroke_strategy_undo_command_based.h"
#include "kis_types.h"
#include "kis_lod_transform.h"
#include "kis_resources_snapshot.h"

#include <QAtomicInt>
#include <QMutex>
#include <QWaitCondition>

class KRITAUI_EXPORT KisFilterStrokeStrategy : public KisStrokeStrategyUndoCommandBased
{
public:
    class FilterJobData : public KisStrokeJobData {
    public:
        FilterJobData(int frameTime = -1)
            : KisStrokeJobData(CONCURRENT),
              frameTime(frameTime)
        {}

        KisStrokeJobData* createLodClone(int levelOfDetail) override {
            return new FilterJobData(*this, levelOfDetail);
        }

        int frameTime;

    private:
        FilterJobData(const FilterJobData &rhs, int levelOfDetail)
            : KisStrokeJobData(rhs)
            , frameTime(rhs.frameTime)
         {
             KisLodTransform t(levelOfDetail);
         }
    };

    class IdleBarrierData : public KisStrokeJobData {
    public:
        IdleBarrierData()
            : KisStrokeJobData(SEQUENTIAL),
              m_idleBarrierCookie(new std::tuple<>())
        {
        }

        KisStrokeJobData* createLodClone(int levelOfDetail) override {
            return new IdleBarrierData(*this, levelOfDetail);
        }

        using IdleBarrierCookie = QWeakPointer<std::tuple<>>;

        IdleBarrierCookie idleBarrierCookie() const {
            return m_idleBarrierCookie;
        }


    private:
        IdleBarrierData(IdleBarrierData &rhs, int /*levelOfDetail*/)
            : KisStrokeJobData(rhs)
         {
            // the cookie is used for preview only, therefore in
            // instant preview mode we pass it to the lodn stroke
            rhs.m_idleBarrierCookie.swap(m_idleBarrierCookie);
         }

        QSharedPointer<std::tuple<>> m_idleBarrierCookie;
    };

    struct ExternalCancelUpdatesStorage {
        QRect updateRect;
        QRect cancelledLod0UpdateRect;
        QAtomicInt shouldIssueCancellationUpdates;
    };

    using ExternalCancelUpdatesStorageSP = QSharedPointer<ExternalCancelUpdatesStorage>;

    /**
     * A progressive preview, as whoever shows it sees it: whether its coarse
     * pass is shown yet (the next settings can be sent then), and its full
     * pass let start at once (otherwise it waits for the settings to settle)
     */
    struct PreviewHandle {
        QAtomicInt coarseShown;

        /// the full pass started now, not waiting any more (the dialog's OK)
        void release()
        {
            QMutexLocker l(&mutex);
            released = true;
            wake.wakeAll();
        }

        QMutex mutex;
        QWaitCondition wake;
        bool released = false;
        bool cancelled = false;
    };
    using PreviewHandleSP = QSharedPointer<PreviewHandle>;

public:
    /**
     * A preview: a coarse pass first (the filter on the image made a few
     * times smaller, scaled back up), shown at once, then the pass at full
     * resolution in its place, once the settings stay the same a moment.
     * For filters that work on fewer pixels the same (supportsLevelOfDetail)
     * and need pixels around each (a blur, not a curve: those are as fast
     * at full resolution), over a large region.
     */
    PreviewHandleSP setProgressivePreview(bool progressive);

    KisFilterStrokeStrategy(KisFilterSP filter,
                            KisFilterConfigurationSP filterConfig,
                            KisResourcesSnapshotSP resources);

    KisFilterStrokeStrategy(KisFilterSP filter,
                            KisFilterConfigurationSP filterConfig,
                            KisResourcesSnapshotSP resources,
                            ExternalCancelUpdatesStorageSP externalCancelUpdatesStorage);
    KisFilterStrokeStrategy(const KisFilterStrokeStrategy &rhs, int levelOfDetail);

    ~KisFilterStrokeStrategy() override;


    void initStrokeCallback() override;
    void doStrokeCallback(KisStrokeJobData *data) override;
    void cancelStrokeCallback() override;
    void finishStrokeCallback() override;
    /// a preview's settle wait and full pass stopped, for the next settings
    void tryCancelCurrentStrokeJobAsync() override;

    KisStrokeStrategy* createLodClone(int levelOfDetail) override;

private:
    struct Private;
    Private* const m_d;
};

#endif /* __KIS_FILTER_STROKE_STRATEGY_H */
