/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioSelectionAdjust.h"

#include <klocalizedstring.h>

#include <KisMainWindow.h>
#include <KisPart.h>
#include <KisViewManager.h>
#include <kis_image.h>
#include <kis_painter.h>
#include <kis_pixel_selection.h>
#include <kis_processing_applicator.h>
#include <kis_selection.h>
#include <kis_selection_filters.h>
#include <kis_stroke_job_strategy.h>
#include <kis_transaction.h>
#include <kis_convolution_kernel.h>
#include <kis_convolution_painter.h>
#include <KoColorSpace.h>

#include <Eigen/Core>
#include <atomic>
#include <cmath>
#include <memory>

/**
 * One transaction, reverted and made again for each value, so however long the drag, its undo keeps one
 * copy of the selection
 */
struct KisStudioSelectionAdjust::Live {
    std::unique_ptr<KisTransaction> transaction;
};

namespace {

using Live = KisStudioSelectionAdjust::Live;

/// A soft edge \p reach px wide each way: a gaussian that has faded out by then (Krita's Feather is cut off at one
/// sigma, which leaves a faint box around a soft selection)
class SoftEdge : public KisSelectionFilter
{
public:
    explicit SoftEdge(int reach) : m_reach(reach) {}

    QRect changeRect(const QRect &rect, KisDefaultBoundsBaseSP) override { return rect.adjusted(-m_reach, -m_reach, m_reach, m_reach); }

    void process(KisPixelSelectionSP pixels, const QRect &rect) override
    {
        const int size = 2 * m_reach + 1;
        const qreal sigma = qMax(0.5, m_reach / 3.0);
        Eigen::Matrix<qreal, Eigen::Dynamic, Eigen::Dynamic> gaussian(1, size);
        for (int x = 0; x < size; x++) {
            const qreal d = x - m_reach;
            gaussian(0, x) = std::exp(-d * d / (2 * sigma * sigma));
        }
        KisConvolutionKernelSP across = KisConvolutionKernel::fromMatrix(gaussian, 0, gaussian.sum());
        KisConvolutionKernelSP down = KisConvolutionKernel::fromMatrix(gaussian.transpose(), 0, gaussian.sum());
        KisPaintDeviceSP between = new KisPaintDevice(pixels->colorSpace());
        between->prepareClone(pixels);
        KisConvolutionPainter first(between);
        first.setChannelFlags(between->colorSpace()->channelFlags(false, true));
        first.applyMatrix(across, pixels, rect.topLeft(), rect.topLeft(), rect.size(), BORDER_REPEAT);
        first.end();
        KisConvolutionPainter second(pixels);
        second.setChannelFlags(pixels->colorSpace()->channelFlags(false, true));
        second.applyMatrix(down, between, rect.topLeft(), rect.topLeft(), rect.size(), BORDER_REPEAT);
        second.end();
    }

private:
    int m_reach;
};

/// One value of the drag: the selection as it was, grown or shrunk, then softened
struct Preview : public KUndo2Command {
    Preview(KisSelectionSP selection, std::shared_ptr<Live> live, int grow, int soft, std::shared_ptr<std::atomic<bool>> busy)
        : m_selection(selection)
        , m_live(live)
        , m_grow(grow)
        , m_soft(soft)
        , m_busy(busy)
    {
    }

    void redo() override
    {
        // made once, while dragging; the step is Keep's
        if (m_done) return;
        m_done = true;
        KisPixelSelectionSP pixels = m_selection->pixelSelection();
        const QRect before = pixels->selectedExactRect();
        if (m_live->transaction) m_live->transaction->revert();
        m_live->transaction.reset(new KisTransaction(pixels));
        QRect rect = pixels->selectedExactRect();
        auto run = [&](KisSelectionFilter &&filter) {
            rect = filter.changeRect(rect, pixels->defaultBounds());
            filter.process(pixels, rect);
        };
        if (m_grow > 0) run(KisGrowSelectionFilter(m_grow, m_grow));
        else if (m_grow < 0) run(KisShrinkSelectionFilter(-m_grow, -m_grow, false));
        if (m_soft > 0) run(SoftEdge(m_soft));
        pixels->setDirty(before | rect);
        *m_busy = false;
    }

    void undo() override
    {
        // a cancelled drag: the selection as it was
        if (!m_live->transaction) return;
        KisPixelSelectionSP pixels = m_selection->pixelSelection();
        const QRect before = pixels->selectedExactRect();
        m_live->transaction->revert();
        m_live->transaction.reset();
        pixels->setDirty(before | pixels->selectedExactRect());
    }

    KisSelectionSP m_selection;
    std::shared_ptr<Live> m_live;
    int m_grow;
    int m_soft;
    std::shared_ptr<std::atomic<bool>> m_busy;
    bool m_done {false};
};

/// The value let go at, as the drag's one undo step
struct Keep : public KUndo2Command {
    explicit Keep(std::shared_ptr<Live> live)
        : m_live(live)
    {
    }

    void redo() override
    {
        if (!m_taken) {
            m_taken = true;
            if (m_live->transaction) m_command.reset(m_live->transaction->endAndTake());
            m_live->transaction.reset();
        }
        if (m_command) m_command->redo();
    }

    void undo() override
    {
        if (m_command) m_command->undo();
    }

    std::shared_ptr<Live> m_live;
    std::unique_ptr<KUndo2Command> m_command;
    bool m_taken {false};
};

} // namespace

KisStudioSelectionAdjust::KisStudioSelectionAdjust(QObject *parent)
    : QObject(parent)
{
    m_timer.setSingleShot(true);
    m_timer.setInterval(60);
    connect(&m_timer, &QTimer::timeout, this, [this]() { apply(); });
}

KisStudioSelectionAdjust::~KisStudioSelectionAdjust()
{
    end();
}

void KisStudioSelectionAdjust::begin()
{
    end();
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisViewManager *view = window ? window->viewManager() : nullptr;
    KisSelectionSP selection = view ? view->selection() : KisSelectionSP();
    if (!view || !view->image() || !selection) return;
    m_image = view->image();
    m_selection = selection;
    m_live = std::make_shared<KisStudioSelectionAdjust::Live>();
    m_grow = 0;
    m_soft = 0;
    m_madeGrow = 0;
    m_madeSoft = 0;
    m_busy = std::make_shared<std::atomic<bool>>(false);
    m_applicator = new KisProcessingApplicator(view->image(), KisNodeSP(), KisProcessingApplicator::NONE, KisImageSignalVector(),
                                               kundo2_i18n("Grow or Soften Selection"));
}

void KisStudioSelectionAdjust::set(int grow, int soft)
{
    if (!m_applicator) return;
    m_grow = grow;
    m_soft = soft;
    m_pending = grow != m_madeGrow || soft != m_madeSoft;
    if (m_pending && !m_timer.isActive()) m_timer.start();
}

void KisStudioSelectionAdjust::apply()
{
    if (!m_applicator || !m_pending) return;
    // one at a time while dragging: a wide soft edge takes longer than the drag's steps, which queued up behind
    // it (each also keeping a copy of the selection for the drag's undo)
    if (*m_busy && !m_ending) {
        m_timer.start();
        return;
    }
    m_pending = false;
    m_madeGrow = m_grow;
    m_madeSoft = m_soft;
    *m_busy = true;
    m_applicator->applyCommand(new Preview(m_selection, m_live, m_grow, m_soft, m_busy), KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::NORMAL);
}

void KisStudioSelectionAdjust::end()
{
    m_timer.stop();
    if (!m_applicator) return;
    // dragged back to nothing: as it was, and no step
    if (m_grow == 0 && m_soft == 0) {
        m_applicator->cancel();
    } else {
        // the value let go at, even while the one before it is still being made
        m_ending = true;
        apply();
        m_ending = false;
        m_applicator->applyCommand(new Keep(m_live), KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::NORMAL);
        m_applicator->end();
    }
    delete m_applicator;
    m_applicator = nullptr;
    m_selection.clear();
    m_live.reset();
}
