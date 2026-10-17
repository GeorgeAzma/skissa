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
#include <commands_new/kis_transaction_based_command.h>
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
#include <cmath>

namespace {

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

/// The selection made again from how it was: grown or shrunk, then softened
struct Readjust : public KisTransactionBasedCommand {
    Readjust(KisSelectionSP selection, KisPixelSelectionSP original, int grow, int soft)
        : m_selection(selection)
        , m_original(original)
        , m_grow(grow)
        , m_soft(soft)
    {
    }

    KUndo2Command *paint() override
    {
        KisPixelSelectionSP pixels = m_selection->pixelSelection();
        KisTransaction transaction(pixels);
        const QRect before = pixels->selectedExactRect();
        const QRect from = m_original->selectedExactRect();
        pixels->clear();
        KisPainter::copyAreaOptimized(from.topLeft(), m_original, pixels, from);
        QRect rect = from;
        auto run = [&](KisSelectionFilter &&filter) {
            rect = filter.changeRect(rect, pixels->defaultBounds());
            filter.process(pixels, rect);
        };
        if (m_grow > 0) run(KisGrowSelectionFilter(m_grow, m_grow));
        else if (m_grow < 0) run(KisShrinkSelectionFilter(-m_grow, -m_grow, false));
        if (m_soft > 0) run(SoftEdge(m_soft));
        KUndo2Command *command = transaction.endAndTake();
        pixels->setDirty(before | rect);
        return command;
    }

    KisSelectionSP m_selection;
    KisPixelSelectionSP m_original;
    int m_grow;
    int m_soft;
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
    m_original = new KisPixelSelection(*selection->pixelSelection());
    m_grow = 0;
    m_soft = 0;
    m_applicator = new KisProcessingApplicator(view->image(), KisNodeSP(), KisProcessingApplicator::NONE, KisImageSignalVector(),
                                               kundo2_i18n("Grow or Soften Selection"));
}

void KisStudioSelectionAdjust::set(int grow, int soft)
{
    if (!m_applicator) return;
    m_grow = grow;
    m_soft = soft;
    m_pending = true;
    if (!m_timer.isActive()) m_timer.start();
}

void KisStudioSelectionAdjust::apply()
{
    if (!m_applicator || !m_pending) return;
    m_pending = false;
    m_applicator->applyCommand(new Readjust(m_selection, m_original, m_grow, m_soft), KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::NORMAL);
}

void KisStudioSelectionAdjust::end()
{
    m_timer.stop();
    if (!m_applicator) return;
    // dragged back to nothing: as it was, and no step
    if (m_grow == 0 && m_soft == 0) {
        m_applicator->cancel();
    } else {
        apply();
        m_applicator->end();
    }
    delete m_applicator;
    m_applicator = nullptr;
    m_selection.clear();
    m_original.clear();
}
