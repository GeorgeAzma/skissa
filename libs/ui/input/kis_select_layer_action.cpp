/* This file is part of the KDE project
 * SPDX-FileCopyrightText: 2012 Arjen Hiemstra <ahiemstra@heimr.nl>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_select_layer_action.h"

#include <kis_debug.h>
#include <QMouseEvent>
#include <QApplication>
#include <QMenu>

#include <klocalizedstring.h>

#include <kis_canvas2.h>
#include <kis_image.h>
#include <KisViewManager.h>
#include <kis_node_manager.h>
#include <kis_cursor.h>

#include "kis_input_manager.h"
#include "kis_tool_utils.h"
#include "tool/strokes/move_stroke_strategy.h"
#include "input/KisLayerHover.h"
#include <kis_group_layer.h>

#include <kis_assert.h>
#include <kis_layer.h>
#include <kis_paint_device.h>
#include <KoColorSpace.h>
#include <QPointer>
#include <QElapsedTimer>
#include <QLineF>
#include <QtMath>

#include <algorithm>

class KisSelectLayerAction::Private
{
public:
    KisSelectLayerAction *q {nullptr};
    QPointer<KisLayerHover> hover;
    int shortcut {makeShortcut(LayerSelectionMode_TopLayer, SelectionOverrideMode_Replace)};

    /// Clicking again where the last click picked a layer picks the next one with paint there
    QPointF pressedAt;
    QPointF lastPickAt;
    QElapsedTimer lastPick;

    static constexpr qreal againDistance = 8;
    static constexpr qint64 againTime = 4000;

    /// A press picks when let go (pendingPick); pressed on paint and dragged, the layer pressed on moves with the
    /// pointer instead
    KisNodeSP pendingPick;
    int pendingOverride {SelectionOverrideMode_Replace};
    KisNodeSP pressedNode;
    QPoint pressedImageAt;
    KisStrokeId moveStroke;
    /// The image the layer moves in, to end its stroke even when the canvas is gone by then
    KisImageWSP moveImage;

    /// Paint this close to the pointer counts as under it, so thin lines are easy to hit (screen pixels; at most
    /// maxReach image pixels, so a view zoomed far out reads little)
    static constexpr qreal reach = 3;
    static constexpr qreal maxReach = 32;
    /// Layers showing this nearly the same are a tie, which the upper one wins
    static constexpr float tie = 1.0f / 255;

    /// The pixels around the pointer, with how much of each the layers so far let through
    struct Spot {
        QRect rect;
        QVector<int> inside;
        QVector<float> through;
    };

    struct Shown {
        KisNodeSP node;
        float most;
    };

    static QVector<quint8> alphaOf(const KisPaintDeviceSP &device, const Spot &spot)
    {
        const KoColorSpace *space = device->colorSpace();
        const int count = spot.rect.width() * spot.rect.height();
        QVector<quint8> bytes(count * int(space->pixelSize()));
        device->readBytes(bytes.data(), spot.rect);
        QVector<quint8> alpha(count);
        space->copyOpacityU8(bytes.data(), alpha.data(), count);
        return alpha;
    }

    /// Every layer from \p node down with paint near the pointer, with the most of it that shows there: its alpha
    /// times what the layers above let through. Hidden and locked layers neither count nor cover.
    static void collect(KisNodeSP node, qreal opacity, Spot &spot, QVector<Shown> &shown)
    {
        for (; node; node = node->prevSibling()) {
            KisLayer *layer = qobject_cast<KisLayer *>(node.data());
            if (!layer || !layer->isEditable()) continue;
            const KisPaintDeviceSP device = layer->projection();
            if (!device) continue;
            const qreal scale = opacity * layer->opacity() / 255.0;
            const KisGroupLayer *group = dynamic_cast<KisGroupLayer *>(layer);
            if (group) {
                // a group with nothing near isn't entered
                bool painted = group->passThroughMode();
                if (!painted) {
                    const QVector<quint8> alpha = alphaOf(device, spot);
                    painted = std::any_of(spot.inside.cbegin(), spot.inside.cend(), [&](int i) { return alpha[i] != 0; });
                }
                if (painted) collect(node->lastChild(), scale, spot, shown);
                continue;
            }
            const QVector<quint8> alpha = alphaOf(device, spot);
            const float weight = float(scale / 255);
            bool painted = false;
            float most = 0;
            for (const int i : spot.inside) {
                if (!alpha[i]) continue;
                painted = true;
                const float a = alpha[i] * weight;
                most = qMax(most, a * spot.through[i]);
                spot.through[i] *= 1 - a;
            }
            if (painted) shown.append(Shown{node, most});
        }
    }

    /// The layers with paint near \p widgetPos, the one that shows most there first; layers showing alike in the
    /// order they stack
    KisNodeList stackAt(const QPointF &widgetPos) const
    {
        KisCanvas2 *canvas = q->inputManager()->canvas();
        if (!canvas || !canvas->image()) return KisNodeList();
        const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
        const QPointF at = converter->widgetToImage(widgetPos);
        const qreal radius =
            qMin(maxReach, QLineF(at, converter->widgetToImage(widgetPos + QPointF(reach, 0))).length());
        const QPoint under(qFloor(at.x()), qFloor(at.y()));

        Spot spot;
        spot.rect = QRect(QPoint(qFloor(at.x() - radius), qFloor(at.y() - radius)),
                          QPoint(qFloor(at.x() + radius), qFloor(at.y() + radius)));
        for (int y = spot.rect.top(); y <= spot.rect.bottom(); y++) {
            for (int x = spot.rect.left(); x <= spot.rect.right(); x++) {
                const QPointF d = QPointF(x + 0.5, y + 0.5) - at;
                if (QPoint(x, y) == under || d.x() * d.x() + d.y() * d.y() <= radius * radius) {
                    spot.inside.append((y - spot.rect.top()) * spot.rect.width() + x - spot.rect.left());
                }
            }
        }
        spot.through.fill(1, spot.rect.width() * spot.rect.height());

        QVector<Shown> shown;
        collect(canvas->image()->root()->lastChild(), 1, spot, shown);

        KisNodeList stack;
        while (!shown.isEmpty()) {
            int best = 0;
            for (int i = 1; i < shown.size(); i++) {
                if (shown[i].most > shown[best].most + tie) best = i;
            }
            stack.append(shown.takeAt(best).node);
        }
        return stack;
    }

    /// What a click at \p widgetPos picks from the \p stack there: the layer that shows most, or the next one when
    /// clicking again (or when that one is the current one already, so a click always changes something)
    KisNodeSP pickFrom(const KisNodeList &stack, const QPointF &widgetPos) const
    {
        if (stack.isEmpty()) return KisNodeSP();
        const KisNodeSP active = q->inputManager()->canvas()->viewManager()->nodeManager()->activeNode();
        const bool again = lastPick.isValid() && lastPick.elapsed() < againTime
            && QLineF(widgetPos, lastPickAt).length() < againDistance && stack.contains(active);
        return again || stack.first() == active ? stack[(stack.indexOf(active) + 1) % stack.size()] : stack.first();
    }

    KisNodeSP pickAt(const QPointF &widgetPos) const
    {
        return pickFrom(stackAt(widgetPos), widgetPos);
    }

    Private(KisSelectLayerAction *q)
        : q(q)
    {}

    static int makeShortcut(LayerSelectionMode layerSelectionMode, SelectionOverrideMode selectionOverrideMode)
    {
        // Store the layer selection mode on the second byte and the selection override mode on the first one
        return (layerSelectionMode << 8) | selectionOverrideMode;
    }

    static int layerSelectionMode(int shortcut)
    {
        // Get the layer selection mode from the second byte
        return (shortcut >> 8) & 0xFF;
    }

    static int selectionOverrideMode(int shortcut)
    {
        // Get the selection override mode from the first byte
        return shortcut & 0xFF;
    }

    void selectNodes(const KisNodeList &nodesToSelect, int selectionOverrideMode, bool includeGroups) const
    {
        KisNodeManager *nodeManager = q->inputManager()->canvas()->viewManager()->nodeManager();
        KisNodeSP activeNode = nodeManager->activeNode();
        KisNodeList finalSelectedNodes;

        // Make the final list of nodes to select, excluding the group layers,
        // if needed
        if (includeGroups) {
            finalSelectedNodes = nodesToSelect;
        } else {
            Q_FOREACH(KisNodeSP node, nodesToSelect) {
                if (!dynamic_cast<KisGroupLayer*>(node.data())) {
                    finalSelectedNodes.append(node);
                }
            }
        }

        // Expand the group layers that contain newly selected nodes
        Q_FOREACH(KisNodeSP node, finalSelectedNodes) {
            KisNodeSP tmpNode = node->parent();
            while (tmpNode) {
                if (dynamic_cast<KisGroupLayer*>(tmpNode.data())) {
                    tmpNode->setCollapsed(false);
                }
                tmpNode = tmpNode->parent();
            }
        }

        // Combine the list of nodes with the current selection
        if (selectionOverrideMode == SelectionOverrideMode_Add) {
            KisNodeList currentlySelectedNodes = nodeManager->selectedNodes();
            Q_FOREACH(KisNodeSP node, currentlySelectedNodes) {
                if (!finalSelectedNodes.contains(node)) {
                    finalSelectedNodes.append(node);
                }
            }
        }

        // Try to retain the previously selected node or select the top one otherwise
        if (!finalSelectedNodes.contains(activeNode)) {
            activeNode = finalSelectedNodes.last();
        }

        // Select
        const KisNodeSP previous = nodeManager->activeNode();
        nodeManager->slotImageRequestNodeReselection(activeNode, finalSelectedNodes);
        // picked on the canvas: chosen by the user, as a click in the layers panel is
        if (activeNode && activeNode != previous) {
            Q_EMIT nodeManager->sigUserActivatedNode(activeNode);
        }
    }

    void selectNode(KisNodeSP node, int selectionOverrideMode) const
    {
        KisNodeList nodesToSelect;
        nodesToSelect.append(node);
        selectNodes(nodesToSelect, selectionOverrideMode, true);
    }
};

KisSelectLayerAction::KisSelectLayerAction()
    : KisAbstractInputAction("Select Layer")
    , d(new Private(this))
{
    setName(i18n("Select Layer"));
    setDescription(i18n("Select layers under the cursor position"));

    QHash<QString, int> shortcuts;
    shortcuts.insert(i18n("Select Top Layer (Replace Selection)"),
                     d->makeShortcut(LayerSelectionMode_TopLayer, SelectionOverrideMode_Replace));
    shortcuts.insert(i18n("Select All Layers (Replace Selection)"),
                     d->makeShortcut(LayerSelectionMode_AllLayers, SelectionOverrideMode_Replace));
    shortcuts.insert(i18n("Select from Menu (Replace Selection)"),
                     d->makeShortcut(LayerSelectionMode_Ask, SelectionOverrideMode_Replace));
    shortcuts.insert(i18n("Select Top Layer (Add to Selection)"),
                     d->makeShortcut(LayerSelectionMode_TopLayer, SelectionOverrideMode_Add));
    shortcuts.insert(i18n("Select All Layers (Add to Selection)"),
                     d->makeShortcut(LayerSelectionMode_AllLayers, SelectionOverrideMode_Add));
    shortcuts.insert(i18n("Select from Menu (Add to Selection)"),
                     d->makeShortcut(LayerSelectionMode_Ask, SelectionOverrideMode_Add));
    setShortcutIndexes(shortcuts);
}

KisSelectLayerAction::~KisSelectLayerAction()
{
    delete d;
}

int KisSelectLayerAction::priority() const
{
    return 5;
}

void KisSelectLayerAction::activate(int shortcut)
{
    QApplication::setOverrideCursor(KisCursor::pickLayerCursor());
    delete d->hover;
    // which layer a click would pick, shown before the click (not for all of them, or a menu)
    KisCanvas2 *canvas = inputManager()->canvas();
    if (canvas && Private::layerSelectionMode(shortcut) == LayerSelectionMode_TopLayer) {
        d->hover = new KisLayerHover(canvas, [this](const QPointF &widgetPos) { return d->pickAt(widgetPos); });
    }
}

void KisSelectLayerAction::deactivate(int shortcut)
{
    Q_UNUSED(shortcut);
    QApplication::restoreOverrideCursor();
    delete d->hover;
}

void KisSelectLayerAction::begin(int shortcut, QEvent *event)
{
    KisAbstractInputAction::begin(shortcut, event);

    d->shortcut = shortcut;
    inputEvent(event);
}

void KisSelectLayerAction::end(QEvent *event)
{
    if (d->moveStroke) {
        if (KisImageSP image = d->moveImage) image->endStroke(d->moveStroke);
        d->moveStroke.clear();
        d->moveImage = KisImageWSP();
    } else if (d->pendingPick) {
        // let go without dragging: the click picks
        d->selectNodes({d->pendingPick}, d->pendingOverride, true);
        if (d->hover) d->hover->picksChanged(d->pressedAt);
    }
    d->pendingPick.clear();
    d->pressedNode.clear();
    KisAbstractInputAction::end(event);
}

void KisSelectLayerAction::inputEvent(QEvent *event)
{
    // Event not recognized (a quick second click comes as a double click: a press too)
    if (!event || (event->type() != QEvent::MouseMove && event->type() != QEvent::TabletMove &&
                   event->type() != QTouchEvent::TouchUpdate && event->type() != QEvent::MouseButtonPress &&
                   event->type() != QEvent::MouseButtonDblClick && event->type() != QEvent::TabletPress
                   && event->type() != QTouchEvent::TouchBegin)) {
        return;
    }

    const int layerSelectionMode = d->layerSelectionMode(d->shortcut);
    const int selectionOverrideMode = d->selectionOverrideMode(d->shortcut);

    // Shortcut not recognized
    KIS_SAFE_ASSERT_RECOVER_RETURN(
        (layerSelectionMode == LayerSelectionMode_TopLayer ||
         layerSelectionMode == LayerSelectionMode_AllLayers ||
         layerSelectionMode == LayerSelectionMode_Ask) &&
        (selectionOverrideMode == SelectionOverrideMode_Replace ||
         selectionOverrideMode == SelectionOverrideMode_Add)
    );

    QPoint pos =
        inputManager()->canvas()->
        coordinatesConverter()->widgetToImage(eventPosF(event)).toPoint();

    // First make a list with the nodes to be selected
    KisNodeList nodesToSelect;

    if (layerSelectionMode == LayerSelectionMode_TopLayer) {
        const QPointF widgetPos = eventPosF(event);
        const bool press = event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick
            || event->type() == QEvent::TabletPress || event->type() == QTouchEvent::TouchBegin;
        KisNodeSP foundNode;
        if (press) {
            // a click: the layer that shows most there, or the next one when clicking again where the last one
            // picked; picked when let go, as a drag moves the one pressed on instead
            const KisNodeList stack = d->stackAt(widgetPos);
            d->pendingPick = d->pickFrom(stack, widgetPos);
            d->pendingOverride = selectionOverrideMode;
            d->pressedAt = widgetPos;
            d->lastPickAt = widgetPos;
            d->lastPick.restart();
            d->pressedNode = stack.isEmpty() ? KisNodeSP() : stack.first();
            d->pressedImageAt = pos;
            return;
        } else if (selectionOverrideMode == SelectionOverrideMode_Replace && d->pressedNode
                   && (d->moveStroke || QLineF(widgetPos, d->pressedAt).length() >= Private::againDistance)) {
            // pressed on paint and dragged: the layer pressed on is picked and moves along
            KisImageSP image = inputManager()->canvas()->image();
            if (!d->moveStroke) {
                d->pendingPick.clear();
                d->lastPick.invalidate();
                d->selectNodes({d->pressedNode}, SelectionOverrideMode_Replace, true);
                d->moveStroke = image->startStroke(new MoveStrokeStrategy(KisNodeList{d->pressedNode}, image.data(), image.data()));
                d->moveImage = image;
            }
            image->addJob(d->moveStroke, new MoveStrokeStrategy::Data(pos - d->pressedImageAt));
            return;
        } else if (QLineF(widgetPos, d->pressedAt).length() >= Private::againDistance) {
            // dragged from nothing (or adding to the selection): the layer that shows most under the pointer, as it goes
            d->pendingPick.clear();
            const KisNodeList stack = d->stackAt(widgetPos);
            foundNode = stack.isEmpty() ? KisNodeSP() : stack.first();
            d->lastPick.invalidate();
        }
        if (!foundNode) {
            return;
        }
        nodesToSelect.append(foundNode);
    } else {
        // Retrieve group nodes only if the mode is LayerSelectionMode_Ask
        const KisNodeList foundNodes = KisToolUtils::findNodes(
                                            inputManager()->canvas()->image()->root()->firstChild(),
                                            pos, false, layerSelectionMode == LayerSelectionMode_Ask);

        if (foundNodes.isEmpty()) {
            return;
        }

        if (layerSelectionMode == LayerSelectionMode_AllLayers) {
            nodesToSelect = foundNodes;
        } else { //LayerSelectionMode_Ask
            QWidget *canvasWidget = inputManager()->canvas()->canvasWidget();
            QMenu *menu = new QMenu(canvasWidget);
            menu->setAttribute(Qt::WA_DeleteOnClose);
            int numberOfLayers = 0;

            // Traverse the list in reverse order so that the menu entries order
            // resembles that of the layer stack
            for (int i = foundNodes.size() - 1; i >= 0; --i) {
                KisNodeSP node = foundNodes[i];
                int indentation = -1;
                {
                    KisNodeSP tempNode = node;
                    while (tempNode->parent()) {
                        ++indentation;
                        tempNode = tempNode->parent();
                    }
                }
                QAction *action = menu->addAction(QString(4 * indentation, ' ') + node->name());
                QObject::connect(action, &QAction::triggered,
                    [this, node, selectionOverrideMode]()
                    {
                        d->selectNode(node, selectionOverrideMode);
                    }
                );
                if (!dynamic_cast<KisGroupLayer*>(node.data())) {
                    ++numberOfLayers;
                }
            }

            // Add separator
            menu->addSeparator();

            // Add "select all layers" menu item
            {
                QAction *action = menu->addAction(i18nc("Menu entry for the select layer under cursor canvas input action",
                                                        "Select all layers"));
                action->setVisible(numberOfLayers > 1);
                QObject::connect(action, &QAction::triggered,
                    [this, foundNodes, selectionOverrideMode]()
                    {
                        d->selectNodes(foundNodes, selectionOverrideMode, false);
                    }
                );
            }

            menu->popup(canvasWidget->mapToGlobal(eventPos(event)));
            return;
        }
    }

    // Now select the nodes
    d->selectNodes(nodesToSelect, selectionOverrideMode, true);
    if (d->hover) {
        d->hover->picksChanged(eventPosF(event));
    }
}
