/*
 *  kis_tool_freehand.cc - part of Krita
 *
 *  SPDX-FileCopyrightText: 2003-2007 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2004 Bart Coppens <kde@bartcoppens.be>
 *  SPDX-FileCopyrightText: 2007, 2008, 2010 Cyrille Berger <cberger@cberger.net>
 *  SPDX-FileCopyrightText: 2009 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  SPDX-FileCopyrightText: 2026 Ayanami Kaine <personal@ayanamikaine.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_tool_freehand.h"

#include "KisLastStrokeReplay.h"
#include "KisDocument.h"
#include "KisView.h"
#include <optional>
#include <QPainter>
#include <QRect>
#include <QThreadPool>
#include <QApplication>
#include <QScreen>
#include <KoColor.h>
#include <kis_icon_utils.h>
#include <KoCanvasResourceProvider.h>
#include <QElapsedTimer>

#include <Eigen/Core>

#include <kis_icon.h>
#include <KoPointerEvent.h>
#include <KoViewConverter.h>
#include <KoCanvasController.h>

//pop up palette
#include <kis_canvas_resource_provider.h>

// Krita/image
#include <kis_layer.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <brushengine/kis_paintop.h>
#include <kis_selection.h>
#include <kis_selection_mask.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/KisOptimizedBrushOutline.h>


// Krita/ui
#include "kis_abstract_perspective_grid.h"
#include "kis_config.h"
#include "kis_config_notifier.h"
#include "kis_image_config.h"
#include "canvas/kis_canvas2.h"
#include "kis_cursor.h"
#include <KisViewManager.h>
#include <kis_painting_assistants_decoration.h>
#include "kis_painting_information_builder.h"
#include "kis_tool_freehand_helper.h"
#include "strokes/freehand_stroke.h"
#include "kis_tool_utils.h"
#include "KisStrokeShape.h"
#include "KisSettingsRegistry.h"
#include "canvas/kis_coordinates_converter.h"

using namespace std::placeholders; // For _1 placeholder

namespace {
bool snapsTemporaryStraightRuler(const KisTemporaryPaintConstraint &constraint,
                                 Qt::KeyboardModifiers modifiers)
{
    return constraint.interactionMode() == KisTemporaryPaintConstraint::InteractionMode::SnapToAngle ||
        modifiers.testFlag(Qt::ShiftModifier);
}

bool hasLockedValidTemporaryStraightRuler(const std::optional<KisTemporaryPaintConstraint> &constraint)
{
    return constraint && constraint->isLocked() && constraint->hasValidLine();
}

/// Tells on the canvas (at most every few seconds) why a stroke starts where it paints nothing:
/// outside the selection, or on a transparent pixel with alpha locked. The stroke still runs.
void explainNothingPainted(KisCanvas2 *canvas, KisNodeSP node, const QPoint &pixel)
{
    static QElapsedTimer said;
    if (!canvas || !node || !canvas->image() || (said.isValid() && said.elapsed() < 4000)) return;
    KisPaintLayer *layer = dynamic_cast<KisPaintLayer *>(node.data());
    // the selection the stroke is limited to, found as the stroke finds it
    KisSelectionSP selection = canvas->image()->globalSelection();
    if (KisLayer *asLayer = dynamic_cast<KisLayer *>(node.data())) {
        selection = asLayer->selection();
    } else if (auto *mask = dynamic_cast<KisSelectionMask *>(node.data()); mask && mask->selection() == selection) {
        selection = nullptr;
    }
    // a vector layer's stroke is a shape, which no selection clips
    if (!node->inherits("KisPaintLayer") && !node->inherits("KisMask")) selection = nullptr;
    QString why;
    KoColor at;
    if (selection && selection->projection()->pixel(pixel.x(), pixel.y(), &at) && at.opacityU8() == OPACITY_TRANSPARENT_U8) {
        why = i18nc("@info on the canvas: a stroke started where the selection leaves nothing to paint", "Outside the selection");
    } else if (layer && layer->paintDevice()) {
        KoCanvasResourceProvider *resources = canvas->resourceManager();
        const bool erasing = resources->resource(KoCanvasResource::EraserMode).toBool();
        const bool alphaLocked = layer->alphaLocked() || (!erasing && resources->resource(KoCanvasResource::GlobalAlphaLock).toBool());
        if (alphaLocked && !erasing && layer->paintDevice()->pixel(pixel.x(), pixel.y(), &at) && at.opacityU8() == OPACITY_TRANSPARENT_U8) {
            why = i18nc("@info on the canvas: alpha locked, and the stroke started on a transparent pixel", "Alpha locked: nothing here");
        }
    }
    if (why.isEmpty()) return;
    said.restart();
    canvas->viewManager()->showFloatingMessage(why, KisIconUtils::loadIcon(QStringLiteral("object-locked")));
}
}


KisToolFreehand::KisToolFreehand(KoCanvasBase * canvas, const QCursor & cursor,
                                 const KUndo2MagicString &transactionText, bool useSavedSmoothing)
    : KisToolPaint(canvas, cursor),
      m_brushResizeCompressor(200, std::bind(&KisToolFreehand::slotDoResizeBrush, this, _1))
{

    setSupportOutline(true);
    updateMaskSyntheticEventsFromTouch();
    connect(KisConfigNotifier::instance(), SIGNAL(touchPaintingChanged()),
            SLOT(updateMaskSyntheticEventsFromTouch()));

    m_infoBuilder = new KisToolFreehandPaintingInformationBuilder(this);
    m_helper = new KisToolFreehandHelper(m_infoBuilder, canvas->resourceManager(), transactionText,
                                         new KisSmoothingOptions(useSavedSmoothing));

    connect(m_helper, SIGNAL(requestExplicitUpdateOutline()), SLOT(explicitUpdateOutline()));
    connect(m_helper, &KisToolFreehandHelper::sigStrokeEnded, this, &KisToolFreehand::reportStrokeEnded);

    m_shapeHold.setSingleShot(true);
    m_shapeHold.setInterval(500);
    connect(&m_shapeHold, &QTimer::timeout, this, &KisToolFreehand::snapToShape);

    connect(qobject_cast<KisCanvas2*>(canvas)->viewManager(), SIGNAL(brushOutlineToggled()), SLOT(explicitUpdateOutline()));

    KisCanvasResourceProvider *provider = qobject_cast<KisCanvas2*>(canvas)->viewManager()->canvasResourceProvider();

    connect(provider, SIGNAL(sigEffectiveCompositeOpChanged()), SLOT(explicitUpdateOutline()));
    connect(provider, SIGNAL(sigEffectiveCompositeOpChanged()), SLOT(resetCursorStyle()));
    connect(provider, SIGNAL(sigPaintOpPresetChanged(KisPaintOpPresetSP)), SLOT(explicitUpdateOutline()));
    connect(provider, SIGNAL(sigPaintOpPresetChanged(KisPaintOpPresetSP)), SLOT(resetCursorStyle()));

    KisPaintingAssistantsDecorationSP decoration = qobject_cast<KisCanvas2*>(canvas)->paintingAssistantsDecoration();
    connect(decoration.data(), SIGNAL(temporaryConstraintAvailabilityChanged()), SLOT(explicitUpdateOutline()));
    connect(decoration.data(), SIGNAL(temporaryConstraintAvailabilityChanged()), SLOT(resetCursorStyle()));
}

KisToolFreehand::~KisToolFreehand()
{
    delete m_helper;
    delete m_infoBuilder;
}

void KisToolFreehand::mouseMoveEvent(KoPointerEvent *event)
{
    m_helper->cursorMoved(convertToPixelCoord(event));

    if (temporaryStraightRulerActive()) {
        if (mode() == HOVER_MODE) {
            updateTemporaryStraightRulerPreview(event->point, event->modifiers());
            requestUpdateOutline(temporaryStraightRulerOutlinePosition(event->point), event);
        }
    } else {
        KisToolPaint::mouseMoveEvent(event);
    }
}

KisSmoothingOptionsSP KisToolFreehand::smoothingOptions() const
{
    return m_helper->smoothingOptions();
}

void KisToolFreehand::resetCursorStyle()
{
    if (temporaryStraightRulerActive()) {
        useCursor(KisCursor::loadWithSize("cursor-cross-full.svg", 31, 31));
        return;
    }

    KisConfig cfg(true);

    bool useSeparateEraserCursor = cfg.separateEraserCursor() && isEraser();

    switch (useSeparateEraserCursor ? cfg.eraserCursorStyle() : cfg.newCursorStyle()) {
    case CURSOR_STYLE_NO_CURSOR:
        useCursor(KisCursor::blankCursor());
        break;
    case CURSOR_STYLE_POINTER:
        useCursor(KisCursor::arrowCursor());
        break;
    case CURSOR_STYLE_SMALL_ROUND:
        useCursor(KisCursor::roundCursor());
        break;
    case CURSOR_STYLE_CROSSHAIR:
        useCursor(KisCursor::crossCursor());
        break;
    case CURSOR_STYLE_TRIANGLE_RIGHTHANDED:
        useCursor(KisCursor::triangleRightHandedCursor());
        break;
    case CURSOR_STYLE_TRIANGLE_LEFTHANDED:
        useCursor(KisCursor::triangleLeftHandedCursor());
        break;
    case CURSOR_STYLE_BLACK_PIXEL:
        useCursor(KisCursor::pixelBlackCursor());
        break;
    case CURSOR_STYLE_WHITE_PIXEL:
        useCursor(KisCursor::pixelWhiteCursor());
        break;
    case CURSOR_STYLE_ERASER:
        useCursor(KisCursor::eraserCursor());
        break;
    case CURSOR_STYLE_TOOLICON:
    default:
        KisToolPaint::resetCursorStyle();
        break;
    }
}

KisPaintingInformationBuilder* KisToolFreehand::paintingInformationBuilder() const
{
    return m_infoBuilder;
}

void KisToolFreehand::resetHelper(KisToolFreehandHelper *helper)
{
    delete m_helper;
    m_helper = helper;
    connect(m_helper, &KisToolFreehandHelper::sigStrokeEnded, this, &KisToolFreehand::reportStrokeEnded);
}

bool KisToolFreehand::supportsPaintingAssistants() const
{
    return true;
}

int KisToolFreehand::flags() const
{
    return KisTool::FLAG_USES_CUSTOM_COMPOSITEOP|KisTool::FLAG_USES_CUSTOM_PRESET
           |KisTool::FLAG_USES_CUSTOM_SIZE;
}

void KisToolFreehand::activate(const QSet<KoShape*> &shapes)
{
    KisToolPaint::activate(shapes);
}

void KisToolFreehand::deactivate()
{
    if (mode() == PAINT_MODE) {
        endStroke();
        setMode(KisTool::HOVER_MODE);
    }
    if (KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2*>(canvas())) {
        if (canvas2->paintingAssistantsDecoration()) {
            canvas2->paintingAssistantsDecoration()->setTemporaryConstraint(std::nullopt);
        }
    }
    KisToolPaint::deactivate();
}

bool KisToolFreehand::temporaryStraightRulerActive() const
{
    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2*>(canvas());
    return canvas2 &&
        canvas2->paintingAssistantsDecoration() &&
        canvas2->paintingAssistantsDecoration()->temporaryConstraint().has_value();
}

QPointF KisToolFreehand::temporaryStraightRulerOutlinePosition(const QPointF &documentPoint)
{
    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2*>(canvas());
    if (!canvas2 ||
        !canvas2->paintingAssistantsDecoration() ||
        !hasLockedValidTemporaryStraightRuler(canvas2->paintingAssistantsDecoration()->temporaryConstraint())) {

        return documentPoint;
    }

    return adjustPosition(documentPoint, documentPoint);
}

void KisToolFreehand::updateTemporaryStraightRulerPreview(const QPointF &documentPoint, Qt::KeyboardModifiers modifiers)
{
    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2*>(canvas());
    if (!canvas2 || !canvas2->paintingAssistantsDecoration()) {
        return;
    }

    std::optional<KisTemporaryPaintConstraint> constraint =
        canvas2->paintingAssistantsDecoration()->temporaryConstraint();

    if (!constraint || constraint->isLocked()) {
        return;
    }

    if (!constraint->hasLine()) {
        constraint->beginLine(documentPoint);
    } else {
        constraint->updateLine(documentPoint, snapsTemporaryStraightRuler(*constraint, modifiers));
    }

    canvas2->paintingAssistantsDecoration()->setTemporaryConstraint(constraint);
}

void KisToolFreehand::lockTemporaryStraightRuler(const QPointF &documentPoint, Qt::KeyboardModifiers modifiers)
{
    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2*>(canvas());
    if (!canvas2 || !canvas2->paintingAssistantsDecoration()) {
        return;
    }

    std::optional<KisTemporaryPaintConstraint> constraint =
        canvas2->paintingAssistantsDecoration()->temporaryConstraint();

    if (!constraint) {
        return;
    }

    if (!constraint->hasLine()) {
        return;
    }

    if (!constraint->isLocked()) {
        constraint->updateLine(documentPoint, snapsTemporaryStraightRuler(*constraint, modifiers));
    }

    if (constraint->hasValidLine()) {
        constraint->setLocked(true);
    } else {
        const KisTemporaryPaintConstraint::InteractionMode interactionMode = constraint->interactionMode();
        constraint = KisTemporaryPaintConstraint();
        constraint->setInteractionMode(interactionMode);
    }

    canvas2->paintingAssistantsDecoration()->setTemporaryConstraint(constraint);
}

void KisToolFreehand::reportStrokeEnded()
{
    // the last stroke, to be painted again as the brush is edited
    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2 *>(canvas());
    KisDocument *document = canvas2 && canvas2->imageView() ? canvas2->imageView()->document() : nullptr;
    if (!document || !currentPaintOpPreset()) return;
    KisLastStrokeReplay::instance()->strokeEnded(m_helper, document->undoStack(), currentPaintOpPreset()->name(),
                                                 currentPaintOpPreset()->settings()->toXML());
}

void KisToolFreehand::initStroke(KoPointerEvent *event)
{
    m_helper->initPaint(event,
                        convertToPixelCoord(event),
                        image(),
                        currentNode(),
                        image().data());
}

void KisToolFreehand::doStroke(KoPointerEvent *event)
{
    m_helper->paintEvent(event);
}

void KisToolFreehand::endStroke()
{
    m_shapeHold.stop();
    if (m_helper->isRunning()) {
        m_helper->endPaint();
    }
    bool paintOpIgnoredEvent = currentPaintOpPreset()->settings()->mouseReleaseEvent();
    Q_UNUSED(paintOpIgnoredEvent);
}

bool KisToolFreehand::primaryActionSupportsHiResEvents() const
{
    return true;
}

void KisToolFreehand::beginPrimaryAction(KoPointerEvent *event)
{
    // FIXME: workaround for the Duplicate Op
    trySampleByPaintOp(event, SampleFgImage);

    lockTemporaryStraightRuler(event->point, event->modifiers());

    requestUpdateOutline(temporaryStraightRulerOutlinePosition(event->point), event);

    NodePaintAbility paintability = nodePaintAbility();
    // XXX: move this to KisTool and make it work properly for clone layers: for clone layers, the shape paint tools don't work either
    if (!nodeEditable() || paintability != PAINT) {
        if (paintability == KisToolPaint::VECTOR || paintability == KisToolPaint::CLONE){
            KisCanvas2 * kiscanvas = static_cast<KisCanvas2*>(canvas());
            QString message = i18n("The brush tool cannot paint on this layer.  Please select a paint layer or mask.");
            kiscanvas->viewManager()->showFloatingMessage(message, koIcon("object-locked"));
        }
        else if (paintability == MYPAINTBRUSH_UNPAINTABLE) {
            KisCanvas2 * kiscanvas = static_cast<KisCanvas2*>(canvas());
            QString message = i18n("The MyPaint Brush Engine is not available for this colorspace");
            kiscanvas->viewManager()->showFloatingMessage(message, koIcon("object-locked"));
        }
        event->ignore();

        return;
    }

    KIS_SAFE_ASSERT_RECOVER_RETURN(!m_helper->isRunning());

    setMode(KisTool::PAINT_MODE);

    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2 *>(canvas());
    if (canvas2) {
        canvas2->viewManager()->disableControls();
        explainNothingPainted(canvas2, currentNode(), convertToPixelCoord(event).toPoint());
    }

    initStroke(event);

    m_shapeSnapped = false;
    m_shapePoints.clear();
    m_shapePressures.clear();
    // not while an assistant or the straight ruler guides the stroke: the shape would leave the guide
    if (m_snapsToShapes && KisSettingsRegistry::instance()->value(QStringLiteral("StrokeShapeSnap")).toBool() && !m_assistant
        && !temporaryStraightRulerActive()) {
        m_shapePoints << convertToPixelCoord(event);
        m_shapePressures << m_infoBuilder->strokePressure(event->pressure());
        m_shapeHoldPos = event->pos();
        m_shapeHold.start();
    }
}

void KisToolFreehand::continuePrimaryAction(KoPointerEvent *event)
{
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);

    requestUpdateOutline(temporaryStraightRulerOutlinePosition(event->point), event);

    // once it is a shape, the rest of the stroke only waits for the pen to lift
    if (m_shapeSnapped) return;

    /**
     * Actual painting
     */
    doStroke(event);

    if (!m_shapePoints.isEmpty()) {
        m_shapePoints << convertToPixelCoord(event);
        m_shapePressures << m_infoBuilder->strokePressure(event->pressure());
        // a hand holding still still trembles a little
        if (QLineF(event->pos(), m_shapeHoldPos).length() > 3) {
            m_shapeHoldPos = event->pos();
            m_shapeHold.start();
        }
    }
}

void KisToolFreehand::snapToShape()
{
    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2*>(canvas());
    if (mode() != KisTool::PAINT_MODE || !m_helper->isRunning() || !canvas2 || m_shapePoints.size() < 2) return;

    const QTransform toWidget = canvas2->coordinatesConverter()->imageToWidgetTransform();
    const qreal pixel = 1.0 / qMax(1e-6, std::sqrt(std::abs(toWidget.determinant())));
    const KisStrokeShape shape = KisStrokeShape::recognize(m_shapePoints, pixel);
    if (shape.kind == KisStrokeShape::None) return;

    // the shape is drawn as hard as the stroke was at the same share of its way, evened out a little
    QVector<qreal> travelled {0};
    for (int i = 1; i < m_shapePoints.size(); i++) {
        travelled << travelled.last() + QLineF(m_shapePoints[i - 1], m_shapePoints[i]).length();
    }
    const qreal total = qMax<qreal>(1e-6, travelled.last());
    QVector<KisPaintInformation> points;
    for (int i = 0; i < shape.outline.size(); i++) {
        qreal sum = 0;
        int count = 0;
        int nearest = 0;
        for (int k = 0; k < travelled.size(); k++) {
            const qreal at = travelled[k] / total;
            if (std::abs(at - shape.along[i]) < 0.05) {
                sum += m_shapePressures[k];
                count++;
            }
            if (std::abs(at - shape.along[i]) < std::abs(travelled[nearest] / total - shape.along[i])) nearest = k;
        }
        points << KisPaintInformation(shape.outline[i], count ? sum / count : m_shapePressures[nearest]);
    }
    m_helper->repaintAs(points);
    m_shapeSnapped = true;
}

void KisToolFreehand::endPrimaryAction(KoPointerEvent *event)
{
    Q_UNUSED(event);
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);

    endStroke();

    if (m_assistant && static_cast<KisCanvas2*>(canvas())->paintingAssistantsDecoration()) {
        static_cast<KisCanvas2*>(canvas())->paintingAssistantsDecoration()->endStroke();
    }

    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2 *>(canvas());
    if (canvas2) {
        canvas2->viewManager()->enableControls();
    }

    setMode(KisTool::HOVER_MODE);
}

bool KisToolFreehand::trySampleByPaintOp(KoPointerEvent *event, AlternateAction action)
{
    if (action != SampleFgNode && action != SampleFgImage) return false;

    /**
     * FIXME: we need some better way to implement modifiers
     * for a paintop level. This method is used in DuplicateOp only!
     */
    QPointF pos = adjustPosition(event->point, event->point);
    qreal perspective = calculatePerspective(pos);
    if (!currentPaintOpPreset()) {
        return false;
    }
    KisPaintInformation info(convertToPixelCoord(event->point),
                             m_infoBuilder->pressureToCurve(event->pressure()),
                             event->xTilt(), event->yTilt(),
                             event->rotation(),
                             event->tangentialPressure(),
                             perspective, 0, 0);
    info.setRandomSource(new KisRandomSource());
    info.setPerStrokeRandomSource(new KisPerStrokeRandomSource());

    bool paintOpIgnoredEvent = currentPaintOpPreset()->settings()->mousePressEvent(info,
                                                                                   event->modifiers(),
                                                                                   currentNode());
    // DuplicateOP during the sampling of new source point (origin)
    // is the only paintop that returns "false" here
    return !paintOpIgnoredEvent;
}

void KisToolFreehand::activateAlternateAction(AlternateAction action)
{
    if (action != ChangeSize && action != ChangeSizeSnap) {
        KisToolPaint::activateAlternateAction(action);
        return;
    }

    useCursor(KisCursor::blankCursor());
    setOutlineVisible(true);
}

void KisToolFreehand::deactivateAlternateAction(AlternateAction action)
{
    if (action != ChangeSize && action != ChangeSizeSnap) {
        KisToolPaint::deactivateAlternateAction(action);
        return;
    }

    resetCursorStyle();
    setOutlineVisible(false);
}

void KisToolFreehand::beginAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (trySampleByPaintOp(event, action)) {
        m_paintopBasedSamplingInAction = true;
        return;
    }

    if (action != ChangeSize && action != ChangeSizeSnap) {
        KisToolPaint::beginAlternateAction(event, action);
        return;
    }

    setMode(GESTURE_MODE);
    m_initialGestureDocPoint = event->point;
    m_initialGestureGlobalPoint = event->globalPos();

    m_lastDocumentPoint = event->point;
    m_lastPaintOpSize = currentPaintOpPreset()->settings()->paintOpSize();

    m_beginAlternateActionEvent = event->deepCopyEvent();
    requestUpdateOutline(m_initialGestureDocPoint, &m_beginAlternateActionEvent->event);
}

void KisToolFreehand::continueAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (trySampleByPaintOp(event, action) || m_paintopBasedSamplingInAction) return;

    if (action != ChangeSize && action != ChangeSizeSnap) {
        KisToolPaint::continueAlternateAction(event, action);
        return;
    }

    QPointF lastWidgetPosition = convertDocumentToWidget(m_lastDocumentPoint);
    QPointF actualWidgetPosition = convertDocumentToWidget(event->point);

    QPointF offset = actualWidgetPosition - lastWidgetPosition;

    KisCanvas2 *canvas2 = dynamic_cast<KisCanvas2 *>(canvas());
    KIS_SAFE_ASSERT_RECOVER_RETURN(canvas2);
    QRect screenRect = QGuiApplication::primaryScreen()->availableVirtualGeometry();

    qreal scaleX = 0;
    qreal scaleY = 0;
    canvas2->coordinatesConverter()->imageScale(&scaleX, &scaleY);

    const qreal maxBrushSize = KisImageConfig(true).maxBrushSize();
    const qreal effectiveMaxDragSize = 0.5 * screenRect.width();
    const qreal effectiveMaxBrushSize = qMin(maxBrushSize, effectiveMaxDragSize / scaleX);

    const qreal scaleCoeff = effectiveMaxBrushSize / effectiveMaxDragSize;
    const qreal sizeDiff = scaleCoeff * offset.x() ;

    if (qAbs(sizeDiff) > 0.01) {
        KisPaintOpSettingsSP settings = currentPaintOpPreset()->settings();

        qreal newSize = m_lastPaintOpSize + sizeDiff;

        if (action == ChangeSizeSnap) {
            newSize = qMax(qRound(newSize), 1);
        }

        newSize = qBound(0.01, newSize, maxBrushSize);

        settings->setPaintOpSize(newSize);

        requestUpdateOutline(
            m_initialGestureDocPoint,
            m_beginAlternateActionEvent.has_value() ? &m_beginAlternateActionEvent->event : nullptr);
        //m_brushResizeCompressor.start(newSize);

        m_lastDocumentPoint = event->point;
        m_lastPaintOpSize = newSize;
    }
}

void KisToolFreehand::endAlternateAction(KoPointerEvent *event, AlternateAction action)
{
    if (trySampleByPaintOp(event, action) || m_paintopBasedSamplingInAction) {
        m_paintopBasedSamplingInAction = false;
        return;
    }

    if (action != ChangeSize && action != ChangeSizeSnap) {
        KisToolPaint::endAlternateAction(event, action);
        return;
    }

    KisToolUtils::setCursorPos(m_initialGestureGlobalPoint);
    requestUpdateOutline(m_initialGestureDocPoint, 0);

    setMode(HOVER_MODE);

    m_beginAlternateActionEvent.reset();
}

bool KisToolFreehand::wantsAutoScroll() const
{
    return false;
}

void KisToolFreehand::setAssistant(bool assistant)
{
    m_assistant = assistant;
}

void KisToolFreehand::setOnlyOneAssistantSnap(bool assistant)
{
    m_only_one_assistant = assistant;
}

void KisToolFreehand::setSnapEraser(bool assistant)
{
    m_eraser_snapping = assistant;
}

void KisToolFreehand::slotDoResizeBrush(qreal newSize)
{
    KisPaintOpSettingsSP settings = currentPaintOpPreset()->settings();

    settings->setPaintOpSize(newSize);
    requestUpdateOutline(m_initialGestureDocPoint, 0);

}

QPointF KisToolFreehand::adjustPosition(const QPointF& point, const QPointF& strokeBegin)
{
    KisCanvas2* c = static_cast<KisCanvas2*>(canvas());
    if (!c || !c->paintingAssistantsDecoration()) {
        return point;
    }

    if (hasLockedValidTemporaryStraightRuler(c->paintingAssistantsDecoration()->temporaryConstraint())) {
        c->paintingAssistantsDecoration()->setEraserSnap(m_eraser_snapping);
        QPointF ap = c->paintingAssistantsDecoration()->adjustPosition(point, strokeBegin);
        c->paintingAssistantsDecoration()->setAdjustedBrushPosition(ap);
        return ap;
    }

    if (m_assistant) {
        c->paintingAssistantsDecoration()->setOnlyOneAssistantSnap(m_only_one_assistant);
        c->paintingAssistantsDecoration()->setEraserSnap(m_eraser_snapping);
        QPointF ap = c->paintingAssistantsDecoration()->adjustPosition(point, strokeBegin);
        QPointF fp = (1.0 - m_magnetism) * point + m_magnetism * ap;
        // Report the final position back to the assistant so the guides
        // can follow the brush
        c->paintingAssistantsDecoration()->setAdjustedBrushPosition(fp);
        return fp;
    }
    return point;
}

qreal KisToolFreehand::calculatePerspective(const QPointF &documentPoint)
{
    qreal perspective = 1.0;
    Q_FOREACH (const KisPaintingAssistantSP assistant, static_cast<KisCanvas2*>(canvas())->paintingAssistantsDecoration()->assistants()) {
        QPointer<KisAbstractPerspectiveGrid> grid = dynamic_cast<KisAbstractPerspectiveGrid*>(assistant.data());
        if (grid && grid->isActive() && grid->contains(documentPoint)) {
            perspective = grid->distance(documentPoint);
            break;
        }
    }
    return perspective;
}

void KisToolFreehand::updateMaskSyntheticEventsFromTouch()
{
    setMaskSyntheticEvents(KisConfig(true).disableTouchOnCanvas());
}

void KisToolFreehand::explicitUpdateOutline()
{
    requestUpdateOutline(m_outlineDocPoint, 0);
}

KisOptimizedBrushOutline KisToolFreehand::getOutlinePath(const QPointF &documentPos,
                                             const KoPointerEvent *event,
                                             KisPaintOpSettings::OutlineMode outlineMode)
{
    if (temporaryStraightRulerActive()) {
        return KisOptimizedBrushOutline();
    }

    if (currentPaintOpPreset())
        return m_helper->paintOpOutline(convertToPixelCoord(documentPos),
                                        event,
                                        currentPaintOpPreset()->settings(),
                                        outlineMode);
    else
        return KisOptimizedBrushOutline();
}
