/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisLiquifyInPlace.h"

#include <QScopedPointer>

#include <kis_external_layer_iface.h>
#include <kis_liquify_transform_worker.h>
#include <kis_node.h>
#include <kis_paint_device.h>
#include <kis_painter.h>

#include "tool_transform_args.h"

struct KisLiquifyInPlace::State {
    // the device before the transform, the liquified layer, and the grid that made it
    KisPaintDeviceSP base;
    KisPaintDeviceSP result;
    QScopedPointer<KisLiquifyTransformWorker> worker;
    bool applied {false};
    // area written in place since the device was last written whole: each write keeps its tiles for undo
    qint64 written {0};
};

KisLiquifyInPlace::Key KisLiquifyInPlace::keyOf(KisPaintDevice *device, int levelOfDetail)
{
    return Key(device, qMax(0, levelOfDetail));
}

bool KisLiquifyInPlace::canUpdate(const KisNodeList &nodes, const ToolTransformArgs &args, int levelOfDetail) const
{
    if (args.mode() != ToolTransformArgs::LIQUIFY || !args.liquifyWorker() || nodes.isEmpty()) return false;

    QMutexLocker locker(&m_mutex);
    for (const KisNodeSP &node : nodes) {
        if (dynamic_cast<const KisExternalLayer *>(node.data())) return false;
        KisPaintDeviceSP device = node->paintDevice();
        if (!device) return false;

        const QSharedPointer<State> state = m_states.value(keyOf(device.data(), levelOfDetail));
        if (!state || !state->applied || !state->base || !state->result) return false;

        const QRect whole = state->result->extent();
        if (state->written > 2 * qint64(whole.width()) * whole.height()) return false;
    }
    return true;
}

QRect KisLiquifyInPlace::transform(const ToolTransformArgs &args, KisPaintDeviceSP src, KisPaintDeviceSP device,
                                   int levelOfDetail, bool inPlace)
{
    QSharedPointer<State> state;
    {
        QMutexLocker locker(&m_mutex);
        QSharedPointer<State> &kept = m_states[keyOf(device.data(), levelOfDetail)];
        if (!kept) kept.reset(new State);
        state = kept;
    }

    KisLiquifyTransformWorker &worker = *args.liquifyWorker();

    QRect changed;
    if (!state->result || !state->worker || !worker.runChanges(*state->worker, src, state->result, &changed)) {
        const QRect before = state->result ? state->result->extent() : QRect();
        KisPaintDeviceSP result = new KisPaintDevice(src->colorSpace());
        result->prepareClone(src);
        worker.run(src, result);
        state->result = result;
        changed = before | result->extent();
    }
    state->worker.reset(new KisLiquifyTransformWorker(worker));

    if (!inPlace) {
        if (!state->base) {
            state->base = new KisPaintDevice(device->colorSpace());
            state->base->prepareClone(device);
            const QRect extent = device->extent();
            KisPainter::copyAreaOptimized(extent.topLeft(), device, state->base, extent);
        }
        const QRect whole = state->result->extent();
        KisPainter painter(device);
        painter.bitBlt(whole.topLeft(), state->result, whole);
        state->applied = true;
        state->written = 0;
        return whole;
    }

    if (!changed.isEmpty()) {
        KisPainter::copyAreaOptimized(changed.topLeft(), state->base, device, changed);
        KisPainter painter(device);
        painter.bitBlt(changed.topLeft(), state->result, changed);
        state->written += qint64(changed.width()) * changed.height();
    }
    return changed;
}

void KisLiquifyInPlace::undone(int levelOfDetail)
{
    QMutexLocker locker(&m_mutex);
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        if ((it.key().second > 0) == (levelOfDetail > 0)) {
            it.value()->applied = false;
        }
    }
}

void KisLiquifyInPlace::undoneAll()
{
    QMutexLocker locker(&m_mutex);
    for (auto it = m_states.begin(); it != m_states.end(); ++it) {
        it.value()->applied = false;
    }
}
