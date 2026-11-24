/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioItemDrop.h"

#include <QMimeData>
#include <QPointer>
#include <QTimer>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisStudioFind.h"
#include "KisStudioItemCard.h"
#include "KisView.h"
#include "KisViewManager.h"
#include "input/kis_select_layer_action.h"
#include "kis_canvas2.h"
#include "kis_node.h"
#include "kis_node_manager.h"

namespace
{
const QString effectPrefix = QStringLiteral("effect:");
const QString brushPrefix = QStringLiteral("brush:");

QString keyOf(const QMimeData *data)
{
    return data ? QString::fromUtf8(data->data(KisStudioShortcuts::itemMimeType())) : QString();
}

void runLater(const QString &key)
{
    QPointer<KisMainWindow> window = KisPart::instance()->currentMainwindow();
    if (!window) return;
    // after the drag has finished, so what opens takes the focus
    QTimer::singleShot(0, window, [window, key]() {
        if (KisStudioFind *find = window ? window->studioFind() : nullptr) find->run(key);
    });
}
}

bool KisStudioItemDrop::carries(const QMimeData *data)
{
    const QString key = keyOf(data);
    return key.startsWith(effectPrefix) || key.startsWith(brushPrefix);
}

QString KisStudioItemDrop::effectOf(const QMimeData *data)
{
    const QString key = keyOf(data);
    return key.startsWith(effectPrefix) ? key.mid(effectPrefix.size()) : QString();
}

bool KisStudioItemDrop::takesEffect(KisNodeSP node)
{
    return node && node->hasEditablePaintDevice();
}

void KisStudioItemDrop::dropOnCanvas(KisView *view, const QMimeData *data, const QPoint &canvasPos)
{
    const QString effect = effectOf(data);
    if (effect.isEmpty()) {
        runLater(keyOf(data));
        return;
    }
    KisNodeSP target;
    Q_FOREACH (KisNodeSP node, KisSelectLayerAction::layersAt(view->canvasBase(), canvasPos)) {
        if (takesEffect(node)) {
            target = node;
            break;
        }
    }
    // on bare canvas the layer in use, which says why when it can't take one
    if (target) applyEffect(target, effect);
    else runLater(effectPrefix + effect);
}

void KisStudioItemDrop::applyEffect(KisNodeSP node, const QString &id)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    KisViewManager *viewManager = window ? window->viewManager() : nullptr;
    if (!viewManager || !node) return;
    if (viewManager->activeNode() != node) viewManager->nodeManager()->slotNonUiActivatedNode(node);
    runLater(effectPrefix + id);
}
