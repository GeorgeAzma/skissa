/*
 *  SPDX-FileCopyrightText: 2007 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_selection_tool_helper.h"

#include <QPointer>


#include <kundo2command.h>
#include <kactioncollection.h>

#include <KoShapeController.h>
#include <KoPathShape.h>

#include "kis_pixel_selection.h"
#include "kis_shape_selection.h"
#include "kis_image.h"
#include "canvas/kis_canvas2.h"
#include "KisViewManager.h"
#include "kis_selection_manager.h"
#include "kis_transaction.h"
#include "commands/kis_selection_commands.h"
#include "kis_shape_controller.h"

#include <kis_icon.h>
#include "kis_processing_applicator.h"
#include "commands_new/kis_transaction_based_command.h"
#include "kis_gui_context_command.h"
#include "kis_command_utils.h"
#include "commands/kis_deselect_global_selection_command.h"

#include "kis_algebra_2d.h"
#include "kis_config.h"
#include "kis_action_manager.h"
#include "kis_action.h"
#include <QMenu>


KisSelectionToolHelper::KisSelectionToolHelper(KisCanvas2* canvas, const KUndo2MagicString& name)
        : m_canvas(canvas)
        , m_name(name)
{
    m_image = m_canvas->viewManager()->image();
}

KisSelectionToolHelper::~KisSelectionToolHelper()
{
}

struct LazyInitGlobalSelection : public KisTransactionBasedCommand {
    LazyInitGlobalSelection(KisView *view) : m_view(view) {}
    KisView *m_view;

    KUndo2Command* paint() override {
        return !m_view->selection() ?
            new KisSetEmptyGlobalSelectionCommand(m_view->image()) : 0;
    }
};


void KisSelectionToolHelper::selectPixelSelection(KisPixelSelectionSP selection, SelectionAction action)
{
    KisView* view = m_canvas->imageView();
    KisProcessingApplicator applicator(view->image(),
                                       0 /* we need no automatic updates */,
                                       KisProcessingApplicator::SUPPORTS_WRAPAROUND_MODE,
                                       KisImageSignalVector(),
                                       m_name);

    selectPixelSelection(applicator, selection, action);

    applicator.end();

}

void KisSelectionToolHelper::selectPixelSelection(KisProcessingApplicator& applicator, KisPixelSelectionSP selection, SelectionAction action)
{

    KisView* view = m_canvas->imageView();

    QPointer<KisCanvas2> canvas = m_canvas;

    applicator.applyCommand(new LazyInitGlobalSelection(view), KisStrokeJobData::SEQUENTIAL);

    struct ApplyToPixelSelection : public KisTransactionBasedCommand {
        ApplyToPixelSelection(KisView *view,
                              KisPixelSelectionSP selection,
                              SelectionAction action,
                              QPointer<KisCanvas2> canvas) : m_view(view),
                                                            m_selection(selection),
                                                            m_action(action),
                                                            m_canvas(canvas) {}
        KisView *m_view;
        KisPixelSelectionSP m_selection;
        SelectionAction m_action;
        QPointer<KisCanvas2> m_canvas;

        KUndo2Command* paint() override {

            KUndo2Command *savedCommand = 0;
            if (!m_selection->selectedExactRect().isEmpty()) {

                KisSelectionSP selection = m_view->selection();
                KIS_SAFE_ASSERT_RECOVER(selection) { return 0; }

                KisPixelSelectionSP pixelSelection = selection->pixelSelection();
                KIS_SAFE_ASSERT_RECOVER(pixelSelection) { return 0; }

                bool hasSelection = !pixelSelection->isEmpty();

                KisSelectionTransaction transaction(pixelSelection);

                if (!hasSelection && m_action == SELECTION_SYMMETRICDIFFERENCE) {
                    m_action = SELECTION_REPLACE;
                }

                if (!hasSelection && m_action == SELECTION_SUBTRACT) {
                    pixelSelection->invert();
                }

                pixelSelection->applySelection(m_selection, m_action);

                QRect dirtyRect = m_view->image()->bounds();
                if (hasSelection &&
                    m_action != SELECTION_REPLACE &&
                    m_action != SELECTION_INTERSECT &&
                    m_action != SELECTION_SYMMETRICDIFFERENCE) {

                    dirtyRect = m_selection->selectedRect();
                }
                m_view->selection()->updateProjection(dirtyRect);

                savedCommand = transaction.endAndTake();
                pixelSelection->setDirty(dirtyRect);

                // release resources: transaction will care about
                // undo/redo, we don't need the selection anymore
                m_selection.clear();
            }

            if (m_view->selection()->selectedExactRect().isEmpty()) {
                KUndo2Command *deselectCommand = new KisDeselectActiveSelectionCommand(m_view->selection(), m_view->image());
                if (savedCommand) {
                    KisCommandUtils::CompositeCommand *cmd = new KisCommandUtils::CompositeCommand();
                    cmd->addCommand(savedCommand);
                    cmd->addCommand(deselectCommand);
                    savedCommand = cmd;
                } else {
                    savedCommand = deselectCommand;
                }
            }

            return savedCommand;
        }
    };

    applicator.applyCommand(new ApplyToPixelSelection(view, selection, action, canvas), KisStrokeJobData::SEQUENTIAL);

}

void KisSelectionToolHelper::addSelectionShape(KoShape* shape, SelectionAction action)
{
    QList<KoShape*> shapes;
    shapes.append(shape);
    addSelectionShapes(shapes, action);
}
#include "krita_utils.h"
void KisSelectionToolHelper::addSelectionShapes(QList< KoShape* > shapes, SelectionAction action)
{
    KisView *view = m_canvas->imageView();

    if (view->image()->wrapAroundModePermitted()) {
        view->showFloatingMessage(
            i18n("Shape selection does not fully "
                 "support wraparound mode. Please "
                 "use pixel selection instead"),
                 KisIconUtils::loadIcon("selection-info"));
    }

    KisProcessingApplicator applicator(view->image(),
                                       0 /* we need no automatic updates */,
                                       KisProcessingApplicator::NONE,
                                       KisImageSignalVector(),
                                       m_name);

    applicator.applyCommand(new LazyInitGlobalSelection(view));

    struct ClearPixelSelection : public KisTransactionBasedCommand {
        ClearPixelSelection(KisView *view) : m_view(view) {}
        KisView *m_view;

        KUndo2Command* paint() override {

            KisPixelSelectionSP pixelSelection = m_view->selection()->pixelSelection();
            KIS_ASSERT_RECOVER(pixelSelection) { return 0; }

            KisSelectionTransaction transaction(pixelSelection);
            pixelSelection->clear();
            return transaction.endAndTake();
        }
    };

    if (action == SELECTION_REPLACE || action == SELECTION_DEFAULT) {
        applicator.applyCommand(new ClearPixelSelection(view));
    }

    struct AddSelectionShape : public KisTransactionBasedCommand {
        AddSelectionShape(KisView *view, QList<KoShape*> shapes, SelectionAction action)
            : m_view(view),
              m_shapes(shapes),
              m_action(action) {}

        KisView *m_view;
        QList<KoShape*> m_shapes;
        SelectionAction m_action;

        KUndo2Command* paint() override {
            KUndo2Command *resultCommand = 0;

            KisSelectionSP selection = m_view->selection();
            if (selection) {
                KisShapeSelection * shapeSelection = static_cast<KisShapeSelection*>(selection->shapeSelection());

                if (shapeSelection ||
                        m_action == SELECTION_SUBTRACT) {

                    QPainterPath path1;
                    QList<KoShape*> existingShapes;

                    if (shapeSelection) {
                        existingShapes = shapeSelection->shapes();

                        path1.setFillRule(Qt::WindingFill);
                        Q_FOREACH(KoShape *shape, existingShapes) {
                            path1 += shape->absoluteTransformation().map(shape->outline());
                        }
                    } else if (m_action == SELECTION_SUBTRACT) {
                        KisImageSP image = m_view->image();
                        path1.addRect(m_view->viewConverter()->imageRectInDocumentPixels());
                    }

                    QPainterPath path2;
                    path2.setFillRule(Qt::WindingFill);
                    Q_FOREACH(KoShape *shape, m_shapes) {
                        path2 += shape->absoluteTransformation().map(shape->outline());
                    }

                    const QTransform booleanWorkaroundTransform =
                        KritaUtils::pathShapeBooleanSpaceWorkaround(m_view->image());

                    path1 = booleanWorkaroundTransform.map(path1);
                    path2 = booleanWorkaroundTransform.map(path2);

                    QPainterPath path = path2;

                    switch (m_action) {
                    case SELECTION_DEFAULT:
                    case SELECTION_REPLACE:
                        path = path2;
                        break;

                    case SELECTION_INTERSECT:
                        path = path1 & path2;
                        path = KritaUtils::tryCloseTornSubpathsAfterIntersection(path);
                        break;
                    case SELECTION_ADD:
                        path = path1 | path2;
                        break;

                    case SELECTION_SUBTRACT:
                        path = path1 - path2;
                        break;
                    case SELECTION_SYMMETRICDIFFERENCE:
                        path = (path1 | path2) - (path1 & path2);
                        break;
                    }

                    path = booleanWorkaroundTransform.inverted().map(path);

                    KoShape *newShape = KoPathShape::createShapeFromPainterPath(path);
                    newShape->setUserData(new KisShapeSelectionMarker);

                    KUndo2Command *parentCommand = new KUndo2Command();

                    if (!existingShapes.isEmpty()) {
                        m_view->canvasBase()->shapeController()->removeShapes(existingShapes, parentCommand);
                    }
                    m_view->canvasBase()->shapeController()->addShape(newShape, 0, parentCommand);

                    if (path.isEmpty()) {
                        KisCommandUtils::CompositeCommand *cmd = new KisCommandUtils::CompositeCommand();
                        cmd->addCommand(parentCommand);
                        cmd->addCommand(new KisDeselectActiveSelectionCommand(m_view->selection(), m_view->image()));
                        parentCommand = cmd;
                    }

                    resultCommand = parentCommand;
                } else if (m_action == SELECTION_INTERSECT) {
                    // just do nothing if there is nothing to intersect with
                    return nullptr;
                }
            }

            if (!resultCommand) {
                /**
                 * Mark the shapes that they belong to a shape selection
                 */
                Q_FOREACH(KoShape *shape, m_shapes) {
                    if(!shape->userData()) {
                        shape->setUserData(new KisShapeSelectionMarker);
                    }
                }

                resultCommand = m_view->canvasBase()->shapeController()->addShapesDirect(m_shapes, 0);
            }
            return resultCommand;
        }
    };

    applicator.applyCommand(
        new KisGuiContextCommand(new AddSelectionShape(view, shapes, action), view));
    applicator.end();
}

bool KisSelectionToolHelper::canShortcutToDeselect(const QRect &rect, SelectionAction action)
{
    return rect.isEmpty() && (action == SELECTION_INTERSECT || action == SELECTION_REPLACE);
}

bool KisSelectionToolHelper::canShortcutToNoop(const QRect &rect, SelectionAction action)
{
    return rect.isEmpty() && action == SELECTION_ADD;
}

bool KisSelectionToolHelper::tryDeselectCurrentSelection(const QRectF selectionViewRect, SelectionAction action)
{
    bool result = false;

    if (KisAlgebra2D::maxDimension(selectionViewRect) < KisConfig(true).selectionViewSizeMinimum() &&
        (action == SELECTION_INTERSECT || action == SELECTION_SYMMETRICDIFFERENCE || action == SELECTION_REPLACE)) {

        // Queueing this action to ensure we avoid a race condition when unlocking the node system
        QTimer::singleShot(0, m_canvas->viewManager()->selectionManager(), SLOT(deselect()));
        result = true;
    }

    return result;
}


QMenu* KisSelectionToolHelper::getSelectionContextMenu(KisCanvas2* canvas)
{
    // one menu at a time: the one shown before goes (the caller only empties it)
    static QPointer<QMenu> menu;
    delete menu;
    menu = new QMenu();

    KisKActionCollection *actionCollection = canvas->viewManager()->actionCollection();

    /**
     * Short entries (the actions keep their full names in Find and the main
     * menu), each with its shortcut; what changes the selection's shape in
     * one list, how it is shown in another.
     */
    auto add = [](QMenu *into, QAction *action, const QString &label) {
        if (!action) return;
        QString text = label;
        if (!action->shortcut().isEmpty()) {
            text += QLatin1Char('\t') + action->shortcut().toString(QKeySequence::NativeText);
        }
        QAction *entry = into->addAction(text);
        entry->setEnabled(action->isEnabled());
        // its shortcut is the action's: shown as a key cap, and changed there
        entry->setProperty("kisShortcutTarget", QVariant::fromValue<QObject *>(action));
        entry->setCheckable(action->isCheckable());
        entry->setChecked(action->isChecked());
        QPointer<QAction> target(action);
        QObject::connect(entry, &QAction::triggered, entry, [target]() {
            if (target) target->trigger();
        });
    };
    auto action = [actionCollection](const char *id) { return actionCollection->action(QLatin1String(id)); };

    add(menu, action("select_all"), i18nc("@action:inmenu", "Select all"));
    add(menu, action("deselect"), i18nc("@action:inmenu", "Deselect"));
    add(menu, action("reselect"), i18nc("@action:inmenu", "Reselect"));
    add(menu, action("invert_selection"), i18nc("@action:inmenu", "Invert"));
    menu->addSeparator();
    add(menu, action("cut_selection_to_new_layer"), i18nc("@action:inmenu", "Cut to new layer"));
    add(menu, action("copy_selection_to_new_layer"), i18nc("@action:inmenu", "Copy to new layer"));

    KisSelectionSP selection = canvas->viewManager()->selection();
    if (selection && canvas->viewManager()->selectionEditable()) {
        menu->addSeparator();
        add(menu, action("KisToolTransform"), i18nc("@action:inmenu", "Transform"));
        add(menu, action("studio_make_brush"), i18nc("@action:inmenu a brush of what is selected", "Make brush"));
        QMenu *modify = menu->addMenu(i18nc("@title:menu change the selection's shape", "Modify"));
        add(modify, action("growselection"), i18nc("@action:inmenu", "Grow…"));
        add(modify, action("shrinkselection"), i18nc("@action:inmenu", "Shrink…"));
        add(modify, action("borderselection"), i18nc("@action:inmenu", "Border…"));
        add(modify, action("smoothselection"), i18nc("@action:inmenu", "Smooth"));
        add(modify, action("featherselection"), i18nc("@action:inmenu", "Feather…"));
        add(modify, action("selectionscale"), i18nc("@action:inmenu", "Scale…"));
        add(menu, action("stroke_selection"), i18nc("@action:inmenu", "Stroke…"));
        add(menu, action("edit_selection"), i18nc("@action:inmenu", "Edit"));
        QMenu *convert = menu->addMenu(i18nc("@title:menu", "Convert"));
        if (!selection->hasShapeSelection()) {
            add(convert, action("convert_to_vector_selection"), i18nc("@action:inmenu", "To vector"));
        } else {
            add(convert, action("convert_to_raster_selection"), i18nc("@action:inmenu", "To raster"));
        }
        add(convert, action("convert_selection_to_shape"), i18nc("@action:inmenu", "To shape"));
    }

    menu->addSeparator();
    add(menu, action("resizeimagetoselection"), i18nc("@action:inmenu", "Crop image to it"));
    QMenu *show = menu->addMenu(i18nc("@title:menu", "Show"));
    add(show, action("toggle_display_selection"), i18nc("@action:inmenu", "Selection"));
    add(show, action("show-global-selection-mask"), i18nc("@action:inmenu", "As a layer"));

    return menu;
}

SelectionMode KisSelectionToolHelper::tryOverrideSelectionMode(KisSelectionSP activeSelection, SelectionMode currentMode, SelectionAction currentAction) const
{
    if (currentAction != SELECTION_DEFAULT && currentAction != SELECTION_REPLACE) {
        if (activeSelection) {
            currentMode = activeSelection->hasShapeSelection() ? SHAPE_PROTECTION : PIXEL_SELECTION;
        }
    }

    return currentMode;
}
