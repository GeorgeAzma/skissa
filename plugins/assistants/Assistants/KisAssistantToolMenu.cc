/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <kis_assistant_tool.h>

#include <klocalizedstring.h>

#include <KoViewConverter.h>
#include <canvas/kis_canvas2.h>
#include <kis_coordinates_converter.h>
#include <kis_painting_assistants_decoration.h>
#include <kis_undo_adapter.h>
#include <KisViewManager.h>

#include "EditAssistantsCommand.h"

// A guide's own commands on its right click, over one of its handles or its box: what the box's buttons did
QMenu *KisAssistantTool::popupActionsMenu()
{
    if (!m_canvas || !m_canvas->paintingAssistantsDecoration()) return nullptr;
    KisPaintingAssistantsDecorationSP decoration = m_canvas->paintingAssistantsDecoration();
    const AssistantEditorData &editor = decoration->globalEditorWidgetData;
    const KisCoordinatesConverter *converter = m_canvas->coordinatesConverter();
    const QTransform toWidget = converter->documentToWidgetTransform();
    const QPointF at = toWidget.map(m_hoverPoint);

    KisPaintingAssistantSP hit;
    double nearest = m_handleMaxDist;
    Q_FOREACH (KisPaintingAssistantSP assistant, decoration->assistants()) {
        QList<KisPaintingAssistantHandleSP> handles = assistant->handles();
        handles.append(assistant->sideHandles());
        Q_FOREACH (const KisPaintingAssistantHandleSP handle, handles) {
            const double distance = KisPaintingAssistant::norm2(at - toWidget.map(QPointF(*handle)));
            if (distance < nearest) {
                nearest = distance;
                hit = assistant;
            }
        }
        if (!assistant->isAssistantComplete() || !editor.widgetActivated) continue;
        const QPointF box = toWidget.map(assistant->viewportConstrainedEditorPosition(converter, editor.boundingSize))
            + QPointF(editor.widgetOffset, editor.widgetOffset);
        if (QRectF(box, editor.boundingSize).contains(at)) hit = assistant;
    }
    if (!hit) return nullptr;
    assistantSelected(hit);

    m_popupMenu.reset(new QMenu());
    QMenu *menu = m_popupMenu.data();
    QAction *snap = menu->addAction(i18nc("a guide lines are pulled onto", "Snap"));
    snap->setCheckable(true);
    snap->setChecked(hit->isSnappingActive());
    connect(snap, &QAction::toggled, this, [this, hit](bool on) {
        hit->setSnappingActive(on);
        hit->uncache();
        m_canvas->updateCanvas();
    });
    QAction *lock = menu->addAction(i18nc("a guide kept from being moved", "Lock"));
    lock->setCheckable(true);
    lock->setChecked(hit->isLocked());
    connect(lock, &QAction::toggled, this, [this, hit](bool on) {
        hit->setLocked(on);
        m_canvas->updateCanvas();
    });
    menu->addSeparator();
    connect(menu->addAction(i18nc("make a copy of the guide", "Duplicate")), &QAction::triggered, this, [this, hit]() {
        KisPaintingAssistantsDecorationSP decoration = m_canvas->paintingAssistantsDecoration();
        const QList<KisPaintingAssistantSP> before = KisPaintingAssistant::cloneAssistantList(decoration->assistants());
        QMap<KisPaintingAssistantHandleSP, KisPaintingAssistantHandleSP> handleMap;
        KisPaintingAssistantSP copy = hit->clone(handleMap);
        copy->copySharedData(hit);
        // beside the original, a little down and to the right on the screen
        const QPointF step = m_canvas->viewConverter()->viewToDocument(QPointF(24, 24))
            - m_canvas->viewConverter()->viewToDocument(QPointF(0, 0));
        QList<KisPaintingAssistantHandleSP> handles = copy->handles();
        handles.append(copy->sideHandles());
        Q_FOREACH (KisPaintingAssistantHandleSP handle, handles) *handle = QPointF(*handle) + step;
        copy->uncache();
        decoration->addAssistant(copy);
        const QList<KisPaintingAssistantSP> after = decoration->assistants();
        m_canvas->viewManager()->undoAdapter()->addCommand(new EditAssistantsCommand(
            m_canvas, before, KisPaintingAssistant::cloneAssistantList(after), EditAssistantsCommand::ADD, after.indexOf(copy)));
        m_handles = decoration->handles();
        assistantSelected(copy);
        m_canvas->updateCanvas();
    });
    QAction *remove = menu->addAction(i18nc("remove the guide", "Delete"));
    remove->setEnabled(!hit->isLocked());
    connect(remove, &QAction::triggered, this, [this, hit]() {
        m_origAssistantList = KisPaintingAssistant::cloneAssistantList(m_canvas->paintingAssistantsDecoration()->assistants());
        removeAssistant(hit);
        m_canvas->updateCanvas();
    });
    return menu;
}
