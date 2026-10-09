/*
 *  SPDX-FileCopyrightText: 2013 Lukáš Tvrdý <lukast.dev@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_offset_processing_visitor.h"

#include <klocalizedstring.h>

#include "commands_new/kis_node_move_command2.h"

#include "kis_group_layer.h"
#include "kis_paint_layer.h"
#include "kis_adjustment_layer.h"
#include "generator/kis_generator_layer.h"

#include "kis_transparency_mask.h"
#include "kis_filter_mask.h"
#include "kis_selection_mask.h"

#include "kis_external_layer_iface.h"

#include "kis_paint_device.h"
#include "kis_transaction.h"
#include "kis_undo_adapter.h"
#include <kis_transform_worker.h>
#include "lazybrush/kis_colorize_mask.h"
#include <KisStrokeLog.h>

#include <QtMath>


KisOffsetProcessingVisitor::KisOffsetProcessingVisitor(const QPoint &offsetPoint, const QRect &rect)
    :   m_offset(offsetPoint),
        m_wrapRect(rect)

{
}

void KisOffsetProcessingVisitor::offsetPaintDevice(KisPaintDeviceSP device, KisUndoAdapter *undoAdapter)
{
    KisTransaction transaction(device);
    KisTransformWorker::offset(device, m_offset, m_wrapRect);
    transaction.commit(undoAdapter);
}

void KisOffsetProcessingVisitor::visitNodeWithPaintDevice(KisNode *node, KisUndoAdapter *undoAdapter)
{
    offsetPaintDevice(node->paintDevice(), undoAdapter);

    // its strokes go where its paint went, around the edges too
    const QPoint offset = m_offset;
    const QRect wrap = m_wrapRect;
    if (wrap.isEmpty()) {
        return;
    }
    auto tile = [offset, wrap](const QPointF &point) {
        return QPoint(qFloor((point.x() - wrap.x() + offset.x()) / wrap.width()),
                      qFloor((point.y() - wrap.y() + offset.y()) / wrap.height()));
    };
    if (KUndo2Command *strokes = KisStrokeLog::mapCommand(
            node,
            [offset, wrap, tile](const QPointF &point) {
                const QPoint whole = tile(point);
                return point + offset - QPointF(whole.x() * wrap.width(), whole.y() * wrap.height());
            },
            KisStrokeLog::Selected(),
            // a stroke over the edge is cut in two, one part at each side
            [tile](const QPointF &a, const QPointF &b) { return tile(a) != tile(b); })) {
        undoAdapter->addCommand(strokes);
    }
}

void KisOffsetProcessingVisitor::visitExternalLayer(KisExternalLayer *layer, KisUndoAdapter *undoAdapter)
{
    Q_UNUSED(layer);
    Q_UNUSED(undoAdapter);
}

void KisOffsetProcessingVisitor::visitColorizeMask(KisColorizeMask *node, KisUndoAdapter *undoAdapter)
{
    QVector<KisPaintDeviceSP> devices = node->allPaintDevices();

    Q_FOREACH (KisPaintDeviceSP device, devices) {
        offsetPaintDevice(device, undoAdapter);
    }
}
