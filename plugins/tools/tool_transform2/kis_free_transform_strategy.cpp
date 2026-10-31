/*
 *  SPDX-FileCopyrightText: 2014 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_free_transform_strategy.h"

#include <QPointF>
#include <QPainter>
#include <QPainterPath>
#include <QMatrix4x4>

#include <KoResourcePaths.h>

#include "KoColorDisplayRendererInterface.h"
#include "kis_coordinates_converter.h"
#include "tool_transform_args.h"
#include "transform_transaction_properties.h"
#include "krita_utils.h"
#include "kis_cursor.h"
#include "kis_transform_utils.h"
#include "kis_free_transform_strategy_gsl_helpers.h"
#include "kis_algebra_2d.h"
#include "KisTransformPerspective.h"


namespace {
enum StrokeFunction {
    ROTATE = 0,
    MOVE,
    RIGHTSCALE,
    TOPRIGHTSCALE,
    TOPSCALE,
    TOPLEFTSCALE,
    LEFTSCALE,
    BOTTOMLEFTSCALE,
    BOTTOMSCALE,
    BOTTOMRIGHTSCALE,
    BOTTOMSHEAR,
    RIGHTSHEAR,
    TOPSHEAR,
    LEFTSHEAR,
    MOVECENTER,
    PERSPECTIVE,
    CAMERAHEIGHT,
    ROTATEBOUNDS,
    PERSPECTIVECORNER,
    PERSPECTIVEEDGE,
    VANISHINGPOINT,
    FLATTENCORNER
};

bool isPerspective(StrokeFunction function)
{
    return function == PERSPECTIVE || function == CAMERAHEIGHT || function == PERSPECTIVECORNER
        || function == PERSPECTIVEEDGE || function == VANISHINGPOINT || function == FLATTENCORNER;
}
}

struct KisFreeTransformStrategy::Private
{
    Private(KisFreeTransformStrategy *_q,
            const KisCoordinatesConverter *_converter,
            KoSnapGuide *_snapGuide,
            ToolTransformArgs &_currentArgs,
            TransformTransactionProperties &_transaction)
        : q(_q),
          converter(_converter),
          snapGuide(_snapGuide),
          currentArgs(_currentArgs),
          transaction(_transaction),
          imageTooBig(false),
          isTransforming(false)
    {
        scaleCursors[0] = KisCursor::sizeHorCursor();
        scaleCursors[1] = KisCursor::sizeFDiagCursor();
        scaleCursors[2] = KisCursor::sizeVerCursor();
        scaleCursors[3] = KisCursor::sizeBDiagCursor();
        scaleCursors[4] = KisCursor::sizeHorCursor();
        scaleCursors[5] = KisCursor::sizeFDiagCursor();
        scaleCursors[6] = KisCursor::sizeVerCursor();
        scaleCursors[7] = KisCursor::sizeBDiagCursor();

        shearCursorPixmap = QIcon(":/shear_cursor.svg").pixmap(22, 22);
        rotateHandlesCursor = QCursor(QIcon(":/rotate_cursor_handles.svg").pixmap(32, 32));
    }

    KisFreeTransformStrategy *q;

    /// standard members ///

    const KisCoordinatesConverter *converter;
    KoSnapGuide *snapGuide;

    //////
    ToolTransformArgs &currentArgs;
    //////
    TransformTransactionProperties &transaction;


    QTransform thumbToImageTransform;
    QImage originalImage;

    QTransform paintingTransform;
    QPointF paintingOffset;

    QTransform handlesTransform;
    QTransform imageToFlake;

    /// custom members ///

    StrokeFunction function {MOVE};

    struct HandlePoints {
        QPointF topLeft;
        QPointF topMiddle;
        QPointF topRight;

        QPointF middleLeft;
        QPointF rotationCenter;
        QPointF middleRight;

        QPointF bottomLeft;
        QPointF bottomMiddle;
        QPointF bottomRight;
    };
    HandlePoints transformedHandles;

    QRectF bounds;
    QTransform boundsTransform; // Transforms bounds into original image space (rotates by boundsRotation)

    QTransform transform;

    QCursor scaleCursors[8]; // cursors for the 8 directions
    QPixmap shearCursorPixmap;
    QCursor rotateHandlesCursor;

    bool imageTooBig {false};

    ToolTransformArgs clickArgs;
    QPointF clickPos;
    QTransform clickTransform;

    bool isTransforming {false};

    // perspective: the corner, edge (top, right, bottom, left) or vanishing point (across, down) dragged
    int handle {0};
    QPolygonF clickQuad;
    QPolygonF clickSource;
    KisTransformPerspective::Catcher catcher;
    bool showVanishing {false};

    // corners being placed on something seen at an angle, over the image as it was
    struct Flatten {
        bool active {false};
        ToolTransformArgs base;
        QPolygonF box;
        QPolygonF source;
        QPolygonF quad;
        QVector<bool> placed;
    } flatten;

    QPolygonF boxQuad() const;
    QPolygonF sourceQuad() const;
    bool flattenIsCurrent();
    bool applyFlatten();
    void setShowVanishing(bool value);

    QCursor getScaleCursor(const QPointF &handlePt);
    QCursor getShearCursor(const QPointF &start, const QPointF &end);
    void recalculateTransformations();
    void recalculateTransformedHandles();
    void recalculateBounds();
};

KisFreeTransformStrategy::KisFreeTransformStrategy(const KisCoordinatesConverter *converter,
                                                   KoSnapGuide *snapGuide,
                                                   ToolTransformArgs &currentArgs,
                                                   TransformTransactionProperties &transaction)
    : KisSimplifiedActionPolicyStrategy(converter, snapGuide),
      m_d(new Private(this, converter, snapGuide, currentArgs, transaction))
{
}

KisFreeTransformStrategy::~KisFreeTransformStrategy()
{
}

namespace {
QPointF middleLeft(const QRectF &rect)
{
    return (rect.topLeft() + rect.bottomLeft()) * 0.5;
}
QPointF middleRight(const QRectF &rect)
{
    return (rect.topRight() + rect.bottomRight()) * 0.5;
}
QPointF bottomMiddle(const QRectF &rect)
{
    return (rect.bottomLeft() + rect.bottomRight()) * 0.5;
}
QPointF topMiddle(const QRectF &rect)
{
    return (rect.topLeft() + rect.topRight()) * 0.5;
}
}

void KisFreeTransformStrategy::Private::recalculateBounds()
{
    const QPolygonF &convexHull = transaction.convexHull();
    if (!convexHull.isEmpty()) {
        bounds = boundsTransform.inverted().map(convexHull).boundingRect();
    } else {
        bounds = boundsTransform.inverted().mapRect(transaction.originalRect());
    }
}


void KisFreeTransformStrategy::Private::recalculateTransformedHandles()
{
    QTransform boundsFullTransform = boundsTransform * transform;
    transformedHandles.topLeft = boundsFullTransform.map(bounds.topLeft());
    transformedHandles.topMiddle = boundsFullTransform.map(topMiddle(bounds));
    transformedHandles.topRight = boundsFullTransform.map(bounds.topRight());

    transformedHandles.middleLeft = boundsFullTransform.map(middleLeft(bounds));
    transformedHandles.middleRight = boundsFullTransform.map(middleRight(bounds));

    transformedHandles.bottomLeft = boundsFullTransform.map(bounds.bottomLeft());
    transformedHandles.bottomMiddle = boundsFullTransform.map(bottomMiddle(bounds));
    transformedHandles.bottomRight = boundsFullTransform.map(bounds.bottomRight());

    transformedHandles.rotationCenter = transform.map(currentArgs.originalCenter() + currentArgs.rotationCenterOffset());
}

QPolygonF KisFreeTransformStrategy::Private::boxQuad() const
{
    return QPolygonF({transformedHandles.topLeft, transformedHandles.topRight,
                      transformedHandles.bottomRight, transformedHandles.bottomLeft});
}

QPolygonF KisFreeTransformStrategy::Private::sourceQuad() const
{
    return boundsTransform.map(QPolygonF({bounds.topLeft(), bounds.topRight(), bounds.bottomRight(), bounds.bottomLeft()}));
}

bool KisFreeTransformStrategy::Private::flattenIsCurrent()
{
    // anything else changing the transform (undo, reset, another layer) ends it
    if (flatten.active && !(currentArgs == flatten.base)) {
        flatten.active = false;
    }
    return flatten.active;
}

bool KisFreeTransformStrategy::Private::applyFlatten()
{
    QTransform straight;
    if (!KisTransformPerspective::flattening(flatten.quad, flatten.box.boundingRect().center(),
                                             QLineF(flatten.box[0], flatten.box[2]).length(), &straight)) {
        return false;
    }

    // the whole box must stay on the quad's side of the horizon
    auto w = [&straight](const QPointF &p) { return p.x() * straight.m13() + p.y() * straight.m23() + straight.m33(); };
    const qreal side = w(flatten.quad.boundingRect().center());
    for (const QPointF &p : flatten.box) {
        if (w(p) * side <= 0) return false;
    }

    ToolTransformArgs args = flatten.base;
    if (!KisTransformPerspective::fit(args, flatten.source, straight.map(flatten.box))) return false;
    currentArgs = args;
    flatten.active = false;
    return true;
}

void KisFreeTransformStrategy::Private::setShowVanishing(bool value)
{
    if (showVanishing == value) return;
    showVanishing = value;
    Q_EMIT q->requestCanvasUpdate();
}

void KisFreeTransformStrategy::setTransformFunction(const QPointF &mousePos, bool perspectiveModifierActive, bool shiftModifierActive, bool altModifierActive)
{
    Q_UNUSED(shiftModifierActive);

    qreal handleRadius = KisTransformUtils::effectiveHandleGrabRadius(m_d->converter);
    const bool perspectiveAllowed = !m_d->transaction.shouldAvoidPerspectiveTransform();
    const QPolygonF box = m_d->boxQuad();

    // corners being placed for a flattening are dragged without keys
    if (m_d->flattenIsCurrent()) {
        for (int i = 0; i < 4; i++) {
            if (kisDistance(mousePos, m_d->flatten.quad[i]) < handleRadius) {
                m_d->function = FLATTENCORNER;
                m_d->handle = i;
                m_d->setShowVanishing(false);
                return;
            }
        }
    }

    // Ctrl+Alt on a corner places it on something seen at an angle
    if (altModifierActive && perspectiveAllowed) {
        for (int i = 0; i < 4; i++) {
            if (kisDistance(mousePos, box[i]) < handleRadius) {
                m_d->function = FLATTENCORNER;
                m_d->handle = i;
                m_d->setShowVanishing(false);
                return;
            }
        }
    }

    if (perspectiveModifierActive && perspectiveAllowed) {
        KisTransformUtils::HandleChooser<StrokeFunction> chooser(mousePos, PERSPECTIVE);
        int handle = 0;
        for (int i = 0; i < 2; i++) {
            QPointF vp;
            if (KisTransformPerspective::vanishingPoint(box, i == 0, &vp) && chooser.addFunction(vp, handleRadius, VANISHINGPOINT)) {
                handle = i;
            }
        }
        for (int i = 0; i < 4; i++) {
            if (chooser.addFunction(0.5 * (box[i] + box[(i + 1) % 4]), handleRadius, PERSPECTIVEEDGE)) handle = i;
        }
        for (int i = 0; i < 4; i++) {
            if (chooser.addFunction(box[i], handleRadius, PERSPECTIVECORNER)) handle = i;
        }
        m_d->function = chooser.function();
        m_d->handle = handle;
        m_d->setShowVanishing(true);
        return;
    }
    m_d->setShowVanishing(false);

    QTransform boundsFullTransform = m_d->boundsTransform * m_d->transform;
    QPolygonF transformedPolygon = boundsFullTransform.map(QPolygonF(m_d->bounds));
    qreal rotationHandleRadius = KisTransformUtils::effectiveHandleGrabRadius(m_d->converter);


    StrokeFunction defaultFunction;
    if (transformedPolygon.containsPoint(mousePos, Qt::OddEvenFill))
        defaultFunction = MOVE;
    else if (m_d->transaction.boundsRotationAllowed() && altModifierActive)
        defaultFunction = ROTATEBOUNDS;
    else
        defaultFunction = ROTATE;
    KisTransformUtils::HandleChooser<StrokeFunction>
        handleChooser(mousePos, defaultFunction);

    handleChooser.addFunction(m_d->transformedHandles.topMiddle,
                              handleRadius, TOPSCALE);
    handleChooser.addFunction(m_d->transformedHandles.topRight,
                              handleRadius, TOPRIGHTSCALE);
    handleChooser.addFunction(m_d->transformedHandles.middleRight,
                              handleRadius, RIGHTSCALE);

    handleChooser.addFunction(m_d->transformedHandles.bottomRight,
                              handleRadius, BOTTOMRIGHTSCALE);
    handleChooser.addFunction(m_d->transformedHandles.bottomMiddle,
                              handleRadius, BOTTOMSCALE);
    handleChooser.addFunction(m_d->transformedHandles.bottomLeft,
                              handleRadius, BOTTOMLEFTSCALE);
    handleChooser.addFunction(m_d->transformedHandles.middleLeft,
                              handleRadius, LEFTSCALE);
    handleChooser.addFunction(m_d->transformedHandles.topLeft,
                              handleRadius, TOPLEFTSCALE);
    handleChooser.addFunction(m_d->transformedHandles.rotationCenter,
                              rotationHandleRadius, MOVECENTER);

    m_d->function = handleChooser.function();

    if (m_d->function == ROTATE || m_d->function == MOVE) {
        QRectF bounds = m_d->bounds;
        QPointF t = boundsFullTransform.inverted().map(mousePos);

        if (t.x() >= bounds.left() && t.x() <= bounds.right()) {
            if (fabs(t.y() - bounds.top()) <= handleRadius)
                m_d->function = TOPSHEAR;
            if (fabs(t.y() - bounds.bottom()) <= handleRadius)
                m_d->function = BOTTOMSHEAR;
        }
        if (t.y() >= bounds.top() && t.y() <= bounds.bottom()) {
            if (fabs(t.x() - bounds.left()) <= handleRadius)
                m_d->function = LEFTSHEAR;
            if (fabs(t.x() - bounds.right()) <= handleRadius)
                m_d->function = RIGHTSHEAR;
        }
    }
}

bool KisFreeTransformStrategy::shiftModifierIsUsed() const
{
    return true;
}

QCursor KisFreeTransformStrategy::Private::getScaleCursor(const QPointF &handlePt)
{
    QPointF handlePtInWidget = converter->imageToWidget(handlePt);
    QPointF centerPtInWidget = converter->imageToWidget(currentArgs.transformedCenter());

    QPointF direction = handlePtInWidget - centerPtInWidget;
    qreal angle = atan2(direction.y(), direction.x());
    angle = normalizeAngle(angle);

    int octant = qRound(angle * 4. / M_PI) % 8;
    return scaleCursors[octant];
}

QCursor KisFreeTransformStrategy::Private::getShearCursor(const QPointF &start, const QPointF &end)
{
    QPointF startPtInWidget = converter->imageToWidget(start);
    QPointF endPtInWidget = converter->imageToWidget(end);
    QPointF direction = endPtInWidget - startPtInWidget;

    qreal angle = atan2(-direction.y(), direction.x());
    return QCursor(shearCursorPixmap.transformed(QTransform().rotateRadians(-angle)));
}

QCursor KisFreeTransformStrategy::getCurrentCursor() const
{
    QCursor cursor;

    switch (m_d->function) {
    case MOVE:
        cursor = KisCursor::moveCursor();
        break;
    case ROTATEBOUNDS:
        cursor = m_d->rotateHandlesCursor;
        break;
    case ROTATE:
        cursor = KisCursor::rotateCursor();
        break;
    case PERSPECTIVE:
        //TODO: find another cursor for perspective
        cursor = KisCursor::rotateCursor();
        break;
    case CAMERAHEIGHT:
        cursor = KisCursor::sizeVerCursor();
        break;
    case PERSPECTIVECORNER:
    case PERSPECTIVEEDGE:
    case VANISHINGPOINT:
        cursor = KisCursor::pointingHandCursor();
        break;
    case FLATTENCORNER:
        cursor = KisCursor::crossCursor();
        break;
    case RIGHTSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.middleRight);
        break;
    case TOPSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.topMiddle);
        break;
    case LEFTSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.middleLeft);
        break;
    case BOTTOMSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.bottomMiddle);
        break;
    case TOPRIGHTSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.topRight);
        break;
    case BOTTOMLEFTSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.bottomLeft);
        break;
    case TOPLEFTSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.topLeft);
        break;
    case BOTTOMRIGHTSCALE:
        cursor = m_d->getScaleCursor(m_d->transformedHandles.bottomRight);
        break;
    case MOVECENTER:
        cursor = KisCursor::handCursor();
        break;
    case BOTTOMSHEAR:
        cursor = m_d->getShearCursor(m_d->transformedHandles.bottomLeft, m_d->transformedHandles.bottomRight);
        break;
    case RIGHTSHEAR:
        cursor = m_d->getShearCursor(m_d->transformedHandles.bottomRight, m_d->transformedHandles.topRight);
        break;
    case TOPSHEAR:
        cursor = m_d->getShearCursor(m_d->transformedHandles.topRight, m_d->transformedHandles.topLeft);
        break;
    case LEFTSHEAR:
        cursor = m_d->getShearCursor(m_d->transformedHandles.topLeft, m_d->transformedHandles.bottomLeft);
        break;
    }

    return cursor;
}

void KisFreeTransformStrategy::paint(QPainter &gc, const KoColorDisplayRendererInterface *displayRendererInterface)
{
    gc.save();

    gc.setOpacity(m_d->transaction.basePreviewOpacity());
    gc.setTransform(m_d->paintingTransform, true);
    gc.drawImage(m_d->paintingOffset, originalImage());

    gc.restore();

    QPen pen[2];
    pen[0].setWidth(decorationThickness());
    pen[0].setCosmetic(true);
    pen[1].setWidth(decorationThickness() * 2);
    pen[1].setCosmetic(true);
    pen[1].setColor(displayRendererInterface->convertColorToDisplayColorSpace(KoColor(Qt::lightGray, KoColorSpaceRegistry::instance()->rgb8())));

    auto paintVanishingLines = [&](const QPolygonF &quad) {
        gc.save();
        gc.setOpacity(0.45);
        for (int i = 1; i >= 0; --i) {
            KisTransformPerspective::paintVanishingLines(gc, quad, m_d->imageToFlake, pen[i]);
        }
        gc.restore();
    };

    if (m_d->flattenIsCurrent()) {
        const QPolygonF &quad = m_d->flatten.quad;
        QPainterPath path;
        path.addPolygon(m_d->imageToFlake.map(quad));
        path.closeSubpath();

        // a few lines across, as the flat thing's own grid
        QTransform grid;
        if (QTransform::squareToQuad(quad, grid)) {
            for (int i = 1; i < 4; i++) {
                path.moveTo(m_d->imageToFlake.map(grid.map(QPointF(i / 4.0, 0))));
                path.lineTo(m_d->imageToFlake.map(grid.map(QPointF(i / 4.0, 1))));
                path.moveTo(m_d->imageToFlake.map(grid.map(QPointF(0, i / 4.0))));
                path.lineTo(m_d->imageToFlake.map(grid.map(QPointF(1, i / 4.0))));
            }
        }

        const qreal r = KisTransformUtils::handleVisualRadius / 2.0;
        QPainterPath placed;
        for (int i = 0; i < 4; i++) {
            const QPointF at = m_d->imageToFlake.map(quad[i]);
            path.addRect(QRectF(at - QPointF(r, r), at + QPointF(r, r)));
            if (m_d->flatten.placed[i]) {
                placed.addRect(QRectF(at - QPointF(r - 2, r - 2), at + QPointF(r - 2, r - 2)));
            }
        }

        paintVanishingLines(quad);
        gc.save();
        for (int i = 1; i >= 0; --i) {
            gc.setPen(pen[i]);
            gc.drawPath(path);
        }
        gc.setPen(Qt::NoPen);
        gc.setBrush(pen[0].color());
        gc.drawPath(placed);
        gc.restore();
        return;
    }

    if ((m_d->isTransforming && isPerspective(m_d->function)) || m_d->showVanishing) {
        paintVanishingLines(m_d->boxQuad());
    }

    // Draw Handles

    QRectF handleRect =
        KisTransformUtils::handleRect(KisTransformUtils::handleVisualRadius,
                                      m_d->handlesTransform,
                                      m_d->bounds, 0, 0);

    qreal rX = 1;
    qreal rY = 1;
    QRectF rotationCenterRect =
        KisTransformUtils::handleRect(KisTransformUtils::rotationHandleVisualRadius,
                                      m_d->handlesTransform,
                                      m_d->bounds,
                                      &rX,
                                      &rY);

    QPainterPath handles;

    handles.moveTo(m_d->bounds.topLeft());
    handles.lineTo(m_d->bounds.topRight());
    handles.lineTo(m_d->bounds.bottomRight());
    handles.lineTo(m_d->bounds.bottomLeft());
    handles.lineTo(m_d->bounds.topLeft());

    handles.addRect(handleRect.translated(m_d->bounds.topLeft()));
    handles.addRect(handleRect.translated(m_d->bounds.topRight()));
    handles.addRect(handleRect.translated(m_d->bounds.bottomLeft()));
    handles.addRect(handleRect.translated(m_d->bounds.bottomRight()));
    handles.addRect(handleRect.translated(middleLeft(m_d->bounds)));
    handles.addRect(handleRect.translated(middleRight(m_d->bounds)));
    handles.addRect(handleRect.translated(topMiddle(m_d->bounds)));
    handles.addRect(handleRect.translated(bottomMiddle(m_d->bounds)));

    QPointF rotationCenter = m_d->boundsTransform.inverted().map(m_d->currentArgs.originalCenter() + m_d->currentArgs.rotationCenterOffset());
    QPointF dx(rX + 3, 0);
    QPointF dy(0, rY + 3);
    handles.addEllipse(rotationCenterRect.translated(rotationCenter));
    handles.moveTo(rotationCenter - dx);
    handles.lineTo(rotationCenter + dx);
    handles.moveTo(rotationCenter - dy);
    handles.lineTo(rotationCenter + dy);

    gc.save();

    if (m_d->isTransforming) {
        gc.setOpacity(0.1);
    }

    //gc.setTransform(m_d->handlesTransform, true); <-- don't do like this!
    QPainterPath mappedHandles = m_d->handlesTransform.map(handles);

    for (int i = 1; i >= 0; --i) {
        gc.setPen(pen[i]);
        gc.drawPath(mappedHandles);
    }

    gc.restore();
}

void KisFreeTransformStrategy::externalConfigChanged()
{
    m_d->recalculateTransformations();
}

bool KisFreeTransformStrategy::beginPrimaryAction(const QPointF &pt)
{
    if (m_d->flattenIsCurrent() && m_d->function != FLATTENCORNER) {
        m_d->flatten.active = false;
        Q_EMIT requestCanvasUpdate();
    }

    m_d->clickArgs = m_d->currentArgs;
    m_d->clickPos = pt;

    KisTransformUtils::MatricesPack m(m_d->clickArgs);
    m_d->clickTransform = m.finalTransform();

    if (m_d->function == ROTATEBOUNDS) {
        Q_EMIT requestConvexHullCalculation();
    }

    m_d->clickQuad = m_d->boxQuad();
    m_d->clickSource = m_d->sourceQuad();

    switch (m_d->function) {
    case PERSPECTIVECORNER:
    case TOPLEFTSCALE:
    case TOPRIGHTSCALE:
    case BOTTOMLEFTSCALE:
    case BOTTOMRIGHTSCALE:
        m_d->catcher.collect(m_d->snapGuide, m_d->transaction, m_d->converter);
        break;
    case FLATTENCORNER:
        if (!m_d->flatten.active) {
            m_d->flatten.active = true;
            m_d->flatten.base = m_d->currentArgs;
            m_d->flatten.box = m_d->clickQuad;
            m_d->flatten.source = m_d->clickSource;
            m_d->flatten.quad = m_d->clickQuad;
            m_d->flatten.placed = QVector<bool>(4, false);
        }
        m_d->flatten.placed[m_d->handle] = true;
        Q_EMIT requestCanvasUpdate();
        break;
    default:
        m_d->catcher.clear();
        break;
    }

    return true;
}

void KisFreeTransformStrategy::continuePrimaryAction(const QPointF &pt, bool shiftModifierActive, bool altModifierActive)
{
    KIS_SAFE_ASSERT_RECOVER_NOOP(0 && "this override is not used in KisFreeTransformStrategy");
}

void KisFreeTransformStrategy::continuePrimaryAction(const QPointF &mousePos,
                                                     bool shiftModifierActive,
                                                     bool altModifierActive,
                                                     bool hasOriginalModifiersSet)
{
    // Note: "shiftModifierActive" just tells us if the shift key is being pressed
    // Note: "altModifierActive" just tells us if the alt key is being pressed
    // Note: "hasOriginalModifiersSet" tells if the set of modifiers that triggered
    //       the whole action is still pressed in the original form

    m_d->isTransforming = true;
    const QPointF anchorPoint = m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset();

    // the image stays as it was while a flattening's corners are placed
    if (m_d->function == FLATTENCORNER) {
        m_d->flatten.quad[m_d->handle] = mousePos;
        Q_EMIT requestCanvasUpdate();
        return;
    }

    switch (m_d->function) {
    case PERSPECTIVECORNER:
    case PERSPECTIVEEDGE:
    case VANISHINGPOINT: {
        QPolygonF quad = m_d->clickQuad;
        const int i = m_d->handle;
        if (m_d->function == PERSPECTIVECORNER) {
            const qreal radius = 1.5 * KisTransformUtils::effectiveHandleGrabRadius(m_d->converter);
            quad[i] = m_d->catcher.corner(mousePos, radius, {quad[(i + 3) % 4], quad[(i + 1) % 4]});
        } else if (m_d->function == PERSPECTIVEEDGE) {
            quad[i] += mousePos - m_d->clickPos;
            quad[(i + 1) % 4] += mousePos - m_d->clickPos;
        } else {
            quad = KisTransformPerspective::withVanishingPoint(quad, i == 0, mousePos);
        }
        ToolTransformArgs args = m_d->clickArgs;
        if (KisTransformPerspective::fit(args, m_d->clickSource, quad)) {
            m_d->currentArgs = args;
        }
        break;
    }
    case FLATTENCORNER:
        break;
    case MOVE: {
        QPointF diff = mousePos - m_d->clickPos;

        if (shiftModifierActive) {

            KisTransformUtils::MatricesPack m(m_d->clickArgs);
            QTransform t = m.S * m.projectedP;
            QPointF originalDiff = t.inverted().map(diff);

            if (qAbs(originalDiff.x()) >= qAbs(originalDiff.y())) {
                originalDiff.setY(0);
            } else {
                originalDiff.setX(0);
            }

            diff = t.map(originalDiff);

        }

        m_d->currentArgs.setTransformedCenter(m_d->clickArgs.transformedCenter() + diff);

        break;
    }
    case ROTATEBOUNDS:
    {
        const KisTransformUtils::MatricesPack clickM(m_d->clickArgs);
        const QTransform clickT = clickM.finalTransform();

        const QPointF rotationCenter = m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset();
        const QPointF clickMouseImagePos = clickT.inverted().map(m_d->clickPos) - rotationCenter;
        const QPointF mouseImagePosClickSpace = clickT.inverted().map(mousePos) - rotationCenter;

        const qreal a1 = atan2(clickMouseImagePos.y(), clickMouseImagePos.x());
        const qreal a2 = atan2(mouseImagePosClickSpace.y(), mouseImagePosClickSpace.x());

        const qreal theta = a2 - a1;
        m_d->currentArgs.setBoundsRotation(m_d->clickArgs.boundsRotation() + theta);
        
        // Find new scale/shear/rotation for the rotated bounds
        QTransform newBR; newBR.rotateRadians(m_d->currentArgs.boundsRotation());
        QTransform clickZ; clickZ.rotateRadians(m_d->clickArgs.aZ());
        // newM.BRI * newM.SC * newM.S * newZ = clickM.BRI * clickM.SC * clickM.S * clickZ
        // newM.SC * newM.S * newZ = newM.BR * clickM.BRI * clickM.SC * clickM.S * clickZ
        QTransform desired = newBR * clickM.BRI * clickM.SC * clickM.S * clickZ;
        KisAlgebra2D::DecomposedMatrix dm(desired);
        if (dm.isValid()) {
            m_d->currentArgs.setScaleX(dm.scaleX);
            m_d->currentArgs.setScaleY(dm.scaleY);
            m_d->currentArgs.setShearX(dm.shearXY);
            m_d->currentArgs.setShearY(0);
            m_d->currentArgs.setAZ(kisDegreesToRadians(dm.angle));
        }

        // Snap with shift key
        // if (shiftModifierActive) {
        //     const qreal angle = m_d->currentArgs.boundsRotation();
        //     const qreal snapAngle = M_PI_4 / 6.0; // 7.5 degrees
        //     qint32 angleIndex = static_cast<qint32>((angle / snapAngle) + 0.5);
        //     m_d->currentArgs.setBoundsRotation(angleIndex * snapAngle);
        // }
    }
    break;
    case ROTATE:
    {
        const KisTransformUtils::MatricesPack clickM(m_d->clickArgs);
        const QTransform clickT = clickM.finalTransform();

        const QPointF rotationCenter = m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset();
        const QPointF clickMouseImagePos = clickT.inverted().map(m_d->clickPos) - rotationCenter;
        const QPointF mouseImagePosClickSpace = clickT.inverted().map(mousePos) - rotationCenter;

        const qreal a1 = atan2(clickMouseImagePos.y(), clickMouseImagePos.x());
        const qreal a2 = atan2(mouseImagePosClickSpace.y(), mouseImagePosClickSpace.x());

        /**
         * We use determinant of `clickM.SC` instead of `clickT` to be able to catch
         * the case when the image is flipped by 0x or 0y perspective rotations.
         */
        const qreal theta = KisAlgebra2D::signZZ(clickM.SC.determinant()) * (a2 - a1);

        // Snap with shift key
        if (shiftModifierActive) {
            const qreal snapAngle = M_PI_4 / 6.0; // 7.5 degrees
            qint32 thetaIndex = static_cast<qint32>((theta / snapAngle) + 0.5);
            m_d->currentArgs.setAZ(thetaIndex * snapAngle);
        }
        else {
            const qreal clickAngle = m_d->clickArgs.aZ();
            const qreal targetAngle = m_d->clickArgs.aZ() + theta;
            qreal shortestDistance = shortestAngularDistance(clickAngle, targetAngle);
            const bool clockwise =  (theta <= M_PI && theta >= 0) || (theta < -M_PI);
            shortestDistance = clockwise ? shortestDistance : shortestDistance * -1;

            m_d->currentArgs.setAZ(m_d->clickArgs.aZ() + shortestDistance);
        }

        KisTransformUtils::MatricesPack m(m_d->currentArgs);
        QTransform t = m.finalTransform();
        QPointF newRotationCenter = t.map(m_d->currentArgs.originalCenter() + m_d->currentArgs.rotationCenterOffset());
        QPointF oldRotationCenter = clickT.map(m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset());

        m_d->currentArgs.setTransformedCenter(m_d->currentArgs.transformedCenter() + oldRotationCenter - newRotationCenter);
    }
    break;
    case PERSPECTIVE:
    {
        if (!hasOriginalModifiersSet) {
            m_d->function = CAMERAHEIGHT;
            m_d->clickArgs = m_d->currentArgs;
            m_d->clickPos = mousePos;
            break;
        }

        QPointF diff = mousePos - m_d->clickPos;
        double thetaX = - diff.y() * M_PI / m_d->transaction.originalHalfHeight() / 2 / fabs(m_d->currentArgs.scaleY());
        m_d->currentArgs.setAX(normalizeAngle(m_d->clickArgs.aX() + thetaX));

        qreal sign = qAbs(m_d->currentArgs.aX() - M_PI) < M_PI / 2 ? -1.0 : 1.0;
        double thetaY = sign * diff.x() * M_PI / m_d->transaction.originalHalfWidth() / 2 / fabs(m_d->currentArgs.scaleX());
        m_d->currentArgs.setAY(normalizeAngle(m_d->clickArgs.aY() + thetaY));

        KisTransformUtils::MatricesPack m(m_d->currentArgs);
        QTransform t = m.finalTransform();
        QPointF newRotationCenter = t.map(m_d->currentArgs.originalCenter() + m_d->currentArgs.rotationCenterOffset());

        KisTransformUtils::MatricesPack clickM(m_d->clickArgs);
        QTransform clickT = clickM.finalTransform();
        QPointF oldRotationCenter = clickT.map(m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset());

        m_d->currentArgs.setTransformedCenter(m_d->currentArgs.transformedCenter() + oldRotationCenter - newRotationCenter);
    }
    break;
    case CAMERAHEIGHT: {
        if (hasOriginalModifiersSet) {
            m_d->function = PERSPECTIVE;
            m_d->clickPos = mousePos;
            break;
        }

        qreal dy = mousePos.y() - m_d->clickPos.y();
        m_d->clickPos = mousePos;

        // normalize distance from extremes 0 (close) to 1 (far)
        qreal currentHeight = m_d->currentArgs.cameraPos().z();
        qreal currentHeightScaled = (currentHeight - 10150.0) / 9850.0;
        qreal heightDelta = -(currentHeightScaled * currentHeightScaled) + 1.001;

        const qreal sensitivityFactor = 0.02;
        qreal scaledDy = dy / sensitivityFactor;

        // Multiply normalized distance by mouse movement to calculate new camera height to add.
        heightDelta = heightDelta * scaledDy;
        qreal newHeight = currentHeight + heightDelta;

        newHeight = qMax(300.0, qMin(20000.0, newHeight));
        m_d->currentArgs.setCameraPos(QVector3D(0, 0, newHeight));
    }
    break;
    case TOPSCALE:
    case BOTTOMSCALE: {
        QPointF staticPoint;
        QPointF movingPoint;

        if (m_d->function == TOPSCALE) {
            staticPoint = m_d->boundsTransform.map(bottomMiddle(m_d->bounds));
            movingPoint = m_d->boundsTransform.map(topMiddle(m_d->bounds));
        } else {
            staticPoint = m_d->boundsTransform.map(topMiddle(m_d->bounds));
            movingPoint = m_d->boundsTransform.map(bottomMiddle(m_d->bounds));
        }

        QPointF staticPointInView = m_d->clickTransform.map(staticPoint);
        const QPointF movingPointInView = m_d->clickTransform.map(movingPoint);

        const QPointF projNormVector =
            KisAlgebra2D::normalize(movingPointInView - staticPointInView);

        const qreal projLength =
            KisAlgebra2D::dotProduct(mousePos - staticPointInView, projNormVector);

        const QPointF targetMovingPointInView = staticPointInView + projNormVector * projLength;

        // override scale static point if it is locked
        if ((m_d->clickArgs.transformAroundRotationCenter() ^ altModifierActive) &&
            !qFuzzyCompare(anchorPoint.y(), movingPoint.y())) {

            staticPoint = anchorPoint;
            staticPointInView = m_d->clickTransform.map(staticPoint);
        }

        GSL::ScaleResult1D result =
            GSL::calculateScaleY(m_d->currentArgs,
                                 staticPoint,
                                 staticPointInView,
                                 movingPoint,
                                 targetMovingPointInView);

        if (!result.isValid) {
            break;
        }

        if (shiftModifierActive ||  m_d->currentArgs.keepAspectRatio()) {
            qreal aspectRatio = m_d->clickArgs.scaleX() / m_d->clickArgs.scaleY();
            m_d->currentArgs.setScaleX(aspectRatio * result.scale);
        }

        m_d->currentArgs.setScaleY(result.scale);
        m_d->currentArgs.setTransformedCenter(result.transformedCenter);
        break;
    }

    case LEFTSCALE:
    case RIGHTSCALE: {
        QPointF staticPoint;
        QPointF movingPoint;

        if (m_d->function == LEFTSCALE) {
            staticPoint = m_d->boundsTransform.map(middleRight(m_d->bounds));
            movingPoint = m_d->boundsTransform.map(middleLeft(m_d->bounds));
        } else {
            staticPoint = m_d->boundsTransform.map(middleLeft(m_d->bounds));
            movingPoint = m_d->boundsTransform.map(middleRight(m_d->bounds));
        }

        QPointF staticPointInView = m_d->clickTransform.map(staticPoint);
        const QPointF movingPointInView = m_d->clickTransform.map(movingPoint);

        const QPointF projNormVector =
            KisAlgebra2D::normalize(movingPointInView - staticPointInView);

        const qreal projLength =
            KisAlgebra2D::dotProduct(mousePos - staticPointInView, projNormVector);

        const QPointF targetMovingPointInView = staticPointInView + projNormVector * projLength;

        // override scale static point if it is locked
        if ((m_d->currentArgs.transformAroundRotationCenter() ^ altModifierActive) &&
            !qFuzzyCompare(anchorPoint.x(), movingPoint.x())) {

            staticPoint = anchorPoint;
            staticPointInView = m_d->clickTransform.map(staticPoint);
        }

        GSL::ScaleResult1D result =
            GSL::calculateScaleX(m_d->currentArgs,
                                 staticPoint,
                                 staticPointInView,
                                 movingPoint,
                                 targetMovingPointInView);

        if (!result.isValid) {
            break;
        }

        if (shiftModifierActive  ||  m_d->currentArgs.keepAspectRatio()) {
            qreal aspectRatio = m_d->clickArgs.scaleY() / m_d->clickArgs.scaleX();
            m_d->currentArgs.setScaleY(aspectRatio * result.scale);
        }

        m_d->currentArgs.setScaleX(result.scale);
        m_d->currentArgs.setTransformedCenter(result.transformedCenter);
        break;
    }
    case TOPRIGHTSCALE:
    case BOTTOMRIGHTSCALE:
    case TOPLEFTSCALE:
    case BOTTOMLEFTSCALE: {
        QPointF staticPoint;
        QPointF movingPoint;

        if (m_d->function == TOPRIGHTSCALE) {
            staticPoint = m_d->boundsTransform.map(m_d->bounds.bottomLeft());
            movingPoint = m_d->boundsTransform.map(m_d->bounds.topRight());
        } else if (m_d->function == BOTTOMRIGHTSCALE) {
            staticPoint = m_d->boundsTransform.map(m_d->bounds.topLeft());
            movingPoint = m_d->boundsTransform.map(m_d->bounds.bottomRight());
        } else if (m_d->function == TOPLEFTSCALE) {
            staticPoint = m_d->boundsTransform.map(m_d->bounds.bottomRight());
            movingPoint = m_d->boundsTransform.map(m_d->bounds.topLeft());
        } else {
            staticPoint = m_d->boundsTransform.map(m_d->bounds.topRight());
            movingPoint = m_d->boundsTransform.map(m_d->bounds.bottomLeft());
        }

        // override scale static point if it is locked
        if ((m_d->currentArgs.transformAroundRotationCenter() ^ altModifierActive) &&
            !(qFuzzyCompare(anchorPoint.x(), movingPoint.x()) ||
              qFuzzyCompare(anchorPoint.y(), movingPoint.y()))) {

            staticPoint = anchorPoint;
        }

        const QPointF caughtPos = m_d->catcher.corner(mousePos, 1.5 * KisTransformUtils::effectiveHandleGrabRadius(m_d->converter), {});
        QPointF staticPointInView = m_d->clickTransform.map(staticPoint);
        QPointF movingPointInView = caughtPos;

        if (shiftModifierActive  ||  m_d->currentArgs.keepAspectRatio()) {
            QPointF refDiff = m_d->clickTransform.map(movingPoint) - staticPointInView;
            QPointF realDiff = caughtPos - staticPointInView;
            realDiff = kisProjectOnVector(refDiff, realDiff);

            movingPointInView = staticPointInView + realDiff;
        }

        const bool isAffine =
            qFuzzyIsNull(m_d->currentArgs.aX()) &&
            qFuzzyIsNull(m_d->currentArgs.aY());

        GSL::ScaleResult2D result =
                !isAffine ?
                    GSL::calculateScale2D(m_d->currentArgs,
                                          staticPoint,
                                          staticPointInView,
                                          movingPoint,
                                          movingPointInView) :
                    GSL::calculateScale2DAffine(m_d->currentArgs,
                                                staticPoint,
                                                staticPointInView,
                                                movingPoint,
                                                movingPointInView);

        if (result.isValid) {
            m_d->currentArgs.setScaleX(result.scaleX);
            m_d->currentArgs.setScaleY(result.scaleY);
            m_d->currentArgs.setTransformedCenter(result.transformedCenter);
        }

        break;
    }
    case MOVECENTER: {

        QPointF pt;
        if (altModifierActive) {
            pt = (m_d->boundsTransform * m_d->transform).inverted().map(mousePos);
            pt = KisTransformUtils::clipInRect(pt, m_d->bounds);
            pt = m_d->boundsTransform.map(pt);
        } else {
            pt = m_d->transform.inverted().map(mousePos);
        }

        QPointF newRotationCenterOffset = pt - m_d->currentArgs.originalCenter();

        if (shiftModifierActive) {
            if (qAbs(newRotationCenterOffset.x()) > qAbs(newRotationCenterOffset.y())) {
                newRotationCenterOffset.ry() = 0;
            } else {
                newRotationCenterOffset.rx() = 0;
            }
        }

        m_d->currentArgs.setRotationCenterOffset(newRotationCenterOffset);
        Q_EMIT requestResetRotationCenterButtons();
    }
        break;
    case TOPSHEAR:
    case BOTTOMSHEAR: {
        KisTransformUtils::MatricesPack m(m_d->clickArgs);

        QPointF oldStaticPoint = m.finalTransform().map(m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset());

        QTransform backwardT = (m.S * m.projectedP).inverted();
        QPointF diff = backwardT.map(mousePos - m_d->clickPos);

        qreal sign = m_d->function == BOTTOMSHEAR ? 1.0 : -1.0;

        // get the dx pixels corresponding to the current shearX factor
        qreal dx = sign * m_d->clickArgs.shearX() * m_d->clickArgs.scaleY() * (m_d->bounds.height() / 2.0); // get the dx pixels corresponding to the current shearX factor
        dx += diff.x();

        // calculate the new shearX factor
        m_d->currentArgs.setShearX(sign * dx / m_d->currentArgs.scaleY() / (m_d->bounds.height() / 2.0)); // calculate the new shearX factor

        KisTransformUtils::MatricesPack currentM(m_d->currentArgs);
        QTransform t = currentM.finalTransform();
        QPointF newStaticPoint = t.map(m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset());
        m_d->currentArgs.setTransformedCenter(m_d->currentArgs.transformedCenter() + oldStaticPoint - newStaticPoint);
        break;
    }

    case LEFTSHEAR:
    case RIGHTSHEAR: {
        KisTransformUtils::MatricesPack m(m_d->clickArgs);

        QPointF oldStaticPoint = m.finalTransform().map(m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset());

        QTransform backwardT = (m.S * m.projectedP).inverted();
        QPointF diff = backwardT.map(mousePos - m_d->clickPos);

        qreal sign = m_d->function == RIGHTSHEAR ? 1.0 : -1.0;

        // get the dx pixels corresponding to the current shearX factor
        qreal dy = sign *  m_d->clickArgs.shearY() * m_d->clickArgs.scaleX() * (m_d->bounds.width() / 2.0);
        dy += diff.y();

        // calculate the new shearY factor
        m_d->currentArgs.setShearY(sign * dy / m_d->clickArgs.scaleX() / (m_d->bounds.width() / 2.0));

        KisTransformUtils::MatricesPack currentM(m_d->currentArgs);
        QTransform t = currentM.finalTransform();
        QPointF newStaticPoint = t.map(m_d->clickArgs.originalCenter() + m_d->clickArgs.rotationCenterOffset());
        m_d->currentArgs.setTransformedCenter(m_d->currentArgs.transformedCenter() + oldStaticPoint - newStaticPoint);
        break;
    }
    }

    m_d->recalculateTransformations();
}

bool KisFreeTransformStrategy::endPrimaryAction()
{
    m_d->catcher.clear();

    // once all four corners are placed, the image is shown straight on
    if (m_d->function == FLATTENCORNER && m_d->flattenIsCurrent()) {
        m_d->isTransforming = false;
        if (!m_d->flatten.placed.contains(false) && m_d->applyFlatten()) {
            m_d->recalculateTransformations();
            return !m_d->imageTooBig;
        }
        Q_EMIT requestCanvasUpdate();
        return false;
    }

    bool shouldSave = !m_d->imageTooBig;
    m_d->isTransforming = false;

    if (m_d->imageTooBig) {
        m_d->currentArgs = m_d->clickArgs;
        m_d->recalculateTransformations();
    }

    return shouldSave;
}

void KisFreeTransformStrategy::Private::recalculateTransformations()
{
    KisTransformUtils::MatricesPack m(currentArgs);
    QTransform sanityCheckMatrix = m.TS * m.SC * m.S * m.projectedP;

    /**
     * The center of the original image should still
     * stay the origin of CS
     */
    KIS_ASSERT_RECOVER_NOOP(sanityCheckMatrix.map(currentArgs.originalCenter()).manhattanLength() < 1e-4);

    transform = m.finalTransform();
    boundsTransform = m.BRI.inverted();
    recalculateBounds();

    QTransform viewScaleTransform = converter->imageToDocumentTransform() * converter->documentToFlakeTransform();
    imageToFlake = viewScaleTransform;
    handlesTransform = boundsTransform * transform * viewScaleTransform;

    QTransform tl = QTransform::fromTranslate(transaction.originalTopLeft().x(), transaction.originalTopLeft().y());
    paintingTransform = tl.inverted() * q->thumbToImageTransform() * tl * transform * viewScaleTransform;
    paintingOffset = transaction.originalTopLeft();

    // check whether image is too big to be displayed or not
    imageTooBig = KisTransformUtils::checkImageTooBig(transaction.originalRect(), m, currentArgs.cameraPos().z());

    // recalculate cached handles position
    recalculateTransformedHandles();

    Q_EMIT q->requestShowImageTooBig(imageTooBig);
    Q_EMIT q->requestImageRecalculation();
}
