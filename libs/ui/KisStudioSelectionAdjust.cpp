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
#include <KoColorSpace.h>

#include <QtConcurrent>

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <numeric>
#include <vector>

/**
 * One transaction, reverted and made again for each value, so however long the drag, its undo keeps one
 * copy of the selection
 */
struct KisStudioSelectionAdjust::Live {
    std::unique_ptr<KisTransaction> transaction;
};

namespace {

using Live = KisStudioSelectionAdjust::Live;

/// The radii of \p count box blurs that one after another make a gaussian of \p sigma
QVector<int> boxRadii(qreal sigma, int count)
{
    const qreal ideal = std::sqrt(12 * sigma * sigma / count + 1);
    int lower = int(std::floor(ideal));
    if (lower % 2 == 0) lower--;
    const int upper = lower + 2;
    const qreal lowerCount = std::round((12 * sigma * sigma - count * lower * lower - 4 * count * lower - 3 * count) / (-4.0 * lower - 4));
    QVector<int> radii;
    for (int i = 0; i < count; i++) radii << ((i < lowerCount ? lower : upper) - 1) / 2;
    // the smallest edge still softens by a pixel
    radii.last() = qMax(1, radii.last());
    return radii;
}

/// A box blur of radius \p r along a row of \p n bytes, by a running sum: the same work at any radius
void boxRow(quint8 *row, int n, int r)
{
    std::vector<quint8> in(row, row + n);
    const int width = 2 * r + 1;
    int sum = 0;
    for (int k = -r; k <= r; k++) sum += in[qBound(0, k, n - 1)];
    for (int i = 0; i < n; i++) {
        row[i] = quint8((sum + width / 2) / width);
        sum += in[qMin(i + r + 1, n - 1)] - in[qMax(i - r, 0)];
    }
}

/// The same down \p count columns from \p x0 of an image \p w wide and \p h high, a block at a time so it reads
/// memory in rows
void boxColumns(quint8 *data, int w, int h, int x0, int count, int r)
{
    const int width = 2 * r + 1;
    std::vector<quint8> in(size_t(h) * count);
    for (int y = 0; y < h; y++) memcpy(&in[size_t(y) * count], data + size_t(y) * w + x0, count);
    std::vector<int> sum(count, 0);
    for (int k = -r; k <= r; k++) {
        const quint8 *row = &in[size_t(qBound(0, k, h - 1)) * count];
        for (int c = 0; c < count; c++) sum[c] += row[c];
    }
    for (int y = 0; y < h; y++) {
        quint8 *out = data + size_t(y) * w + x0;
        for (int c = 0; c < count; c++) out[c] = quint8((sum[c] + width / 2) / width);
        const quint8 *add = &in[size_t(qMin(y + r + 1, h - 1)) * count];
        const quint8 *sub = &in[size_t(qMax(y - r, 0)) * count];
        for (int c = 0; c < count; c++) sum[c] += add[c] - sub[c];
    }
}

/**
 * A soft edge \p reach px wide each way: a gaussian that has faded out by then (Krita's Feather is cut off at one
 * sigma, which leaves a faint box around a soft selection). Three box blurs by running sums make the gaussian, on
 * all threads: the same work for any reach, where a convolution of a kernel this wide went through a whole-area FFT
 * and took seconds a step on a large canvas.
 */
class SoftEdge : public KisSelectionFilter
{
public:
    explicit SoftEdge(int reach) : m_reach(reach) {}

    QRect changeRect(const QRect &rect, KisDefaultBoundsBaseSP) override { return rect.adjusted(-m_reach, -m_reach, m_reach, m_reach); }

    void process(KisPixelSelectionSP pixels, const QRect &rect) override
    {
        const int w = rect.width(), h = rect.height();
        if (w <= 0 || h <= 0) return;
        std::vector<quint8> mask(size_t(w) * h);
        pixels->readBytes(mask.data(), rect);
        quint8 *data = mask.data();
        QVector<int> rows(h), blocks((w + 63) / 64);
        std::iota(rows.begin(), rows.end(), 0);
        std::iota(blocks.begin(), blocks.end(), 0);
        Q_FOREACH (const int r, boxRadii(qMax(0.5, m_reach / 3.0), 3)) {
            if (r <= 0) continue;
            QtConcurrent::blockingMap(rows, [data, w, r](int y) { boxRow(data + size_t(y) * w, w, r); });
            QtConcurrent::blockingMap(blocks, [data, w, h, r](int b) { boxColumns(data, w, h, b * 64, qMin(64, w - b * 64), r); });
        }
        pixels->writeBytes(mask.data(), rect);
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
    // closing the image, saving or undoing mid-drag waits for the strokes there are: this one ends (or is
    // dropped) then, or the wait would never end
    m_interrupted = false;
    m_endRequest = connect(m_image.data(), &KisImage::sigStrokeEndRequested, this, [this]() {
        m_interrupted = true;
        end();
    });
    m_cancelRequest = connect(m_image.data(), &KisImage::sigStrokeCancellationRequested, this, [this]() {
        m_interrupted = true;
        m_grow = 0;
        m_soft = 0;
        end();
    });
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
    disconnect(m_endRequest);
    disconnect(m_cancelRequest);
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
