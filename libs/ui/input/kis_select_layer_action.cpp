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
#include <kis_group_layer.h>

#include <kis_assert.h>
#include <kis_coordinates_converter.h>
#include <kis_paint_device.h>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTabletEvent>
#include <KisModernStyle.h>

namespace {

/**
 * While the key is held: the layer a click would select, tinted on the
 * canvas where it has pixels, with its name by the pointer. Shown over the
 * canvas, never in the way of it.
 */
class LayerHover : public QWidget
{
public:
    explicit LayerHover(KisCanvas2 *canvas)
        : QWidget(canvas->canvasWidget())
        , m_canvas(canvas)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(parentWidget()->rect());
        parentWidget()->installEventFilter(this);
        show();
        raise();
        const QPoint cursor = parentWidget()->mapFromGlobal(QCursor::pos());
        if (rect().contains(cursor)) pointAt(cursor);
    }

    void pointAt(const QPointF &widgetPos)
    {
        m_cursor = widgetPos;
        KisImageSP image = m_canvas ? KisImageSP(m_canvas->image()) : KisImageSP();
        if (!image) {
            return;
        }
        const QPoint imagePos = m_canvas->coordinatesConverter()->widgetToImage(widgetPos).toPoint();
        KisNodeSP node = KisToolUtils::findNode(image->root(), imagePos, false, false);
        if (node != m_node) {
            m_node = node;
            m_tint = QImage();
            m_bounds = QRect();
            if (node && node->projection()) {
                m_bounds = node->exactBounds();
                if (!m_bounds.isEmpty()) {
                    // where it has pixels, in the selection color, at a size good enough over the canvas
                    const QSize size = m_bounds.size().scaled(QSize(480, 480), Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
                    QImage alpha = node->projection()->createThumbnailUncached(size.width(), size.height(), m_bounds)
                                       .convertToFormat(QImage::Format_ARGB32);
                    const QColor ink = palette().color(QPalette::Highlight);
                    for (int y = 0; y < alpha.height(); y++) {
                        QRgb *line = reinterpret_cast<QRgb *>(alpha.scanLine(y));
                        for (int x = 0; x < alpha.width(); x++) {
                            line[x] = qRgba(ink.red(), ink.green(), ink.blue(), qAlpha(line[x]) * 110 / 255);
                        }
                    }
                    m_tint = alpha;
                }
            }
        }
        update();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()) {
            switch (event->type()) {
            case QEvent::MouseMove:
                pointAt(static_cast<QMouseEvent *>(event)->localPos());
                break;
            case QEvent::TabletMove:
                pointAt(static_cast<QTabletEvent *>(event)->posF());
                break;
            case QEvent::Resize:
                setGeometry(parentWidget()->rect());
                break;
            default:
                break;
            }
        }
        return false;
    }

    void paintEvent(QPaintEvent *) override
    {
        if (!m_node || !m_canvas) {
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        const QColor ink = palette().color(QPalette::Highlight);
        if (!m_tint.isNull()) {
            // in image coordinates: rotated and mirrored with the canvas
            painter.save();
            painter.setTransform(m_canvas->coordinatesConverter()->imageToWidgetTransform());
            painter.drawImage(QRectF(m_bounds), m_tint);
            QPen pen(ink, 1.5);
            pen.setCosmetic(true);
            pen.setStyle(Qt::DashLine);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(QRectF(m_bounds));
            painter.restore();
        }
        // the name, by the pointer
        QFont font = this->font();
        font.setBold(true);
        painter.setFont(font);
        const QString name = m_node->name();
        const QFontMetrics metrics(font);
        QRectF pill(m_cursor + QPointF(18, 14), QSizeF(metrics.horizontalAdvance(name) + 20, metrics.height() + 10));
        if (pill.right() > width() - 4) pill.moveRight(m_cursor.x() - 10);
        if (pill.bottom() > height() - 4) pill.moveBottom(m_cursor.y() - 10);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawPath(KisModernStyle::squircle(pill, pill.height() / 2));
        painter.setPen(palette().color(QPalette::HighlightedText));
        painter.drawText(pill, Qt::AlignCenter, name);
    }

private:
    QPointer<KisCanvas2> m_canvas;
    KisNodeSP m_node;
    QRect m_bounds;
    QImage m_tint;
    QPointF m_cursor;
};

} // namespace

class KisSelectLayerAction::Private
{
public:
    KisSelectLayerAction *q {nullptr};
    QPointer<QWidget> hover;
    int shortcut {makeShortcut(LayerSelectionMode_TopLayer, SelectionOverrideMode_Replace)};

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
    Q_UNUSED(shortcut);
    QApplication::setOverrideCursor(KisCursor::pickLayerCursor());
    // which layer a click would select, before the click
    if (KisCanvas2 *canvas = inputManager()->canvas()) {
        delete d->hover;
        d->hover = new LayerHover(canvas);
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

void KisSelectLayerAction::inputEvent(QEvent *event)
{
    // Event not recognized
    if (!event || (event->type() != QEvent::MouseMove && event->type() != QEvent::TabletMove &&
                   event->type() != QTouchEvent::TouchUpdate && event->type() != QEvent::MouseButtonPress &&
                   event->type() != QEvent::TabletPress && event->type() != QTouchEvent::TouchBegin)) {
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
        KisNodeSP foundNode = KisToolUtils::findNode(inputManager()->canvas()->image()->root(), pos, false);
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
}
