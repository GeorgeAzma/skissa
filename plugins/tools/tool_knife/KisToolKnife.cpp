/*
 *  SPDX-FileCopyrightText: 2025 Agata Cacko
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisToolKnife.h"

#include "QApplication"
#include "QPainterPath"

#include <klocalizedstring.h>
#include <KoColor.h>
#include <KisViewManager.h>
#include "kis_canvas2.h"
#include "kis_cursor.h"
#include "kis_painter.h"
#include "kis_paintop_preset.h"
#include "kis_shape_layer.h"

#include "kundo2magicstring.h"
#include "kundo2stack.h"
#include "commands_new/kis_transaction_based_command.h"
#include "kis_transaction.h"
#include "KoPathShape.h"

#include "kis_processing_applicator.h"
#include "kis_datamanager.h"
#include "kis_canvas_resource_provider.h"

#include "KoColorSpaceRegistry.h"
#include <KisCursorOverrideLock.h>

#include "KisToolKnifeOptionsWidget.h"
#include "libs/image/kis_paint_device_debug_utils.h"

#include <KoSelectedShapesProxy.h>

#include "CutThroughShapeStrategy.h"
#include "RemoveGutterStrategy.h"

#include "kis_paint_layer.h"
#include "kis_algebra_2d.h"
#include "kis_resources_snapshot.h"
#include <KoSelection.h>
#include <KoShapeManager.h>
#include <KoShapeController.h>
#include <KoShapeStroke.h>
#include <kis_image.h>
#include <kis_node_manager.h>


struct KisToolKnife::Private {
    KisToolKnifeOptionsWidget *optionsWidget = nullptr;
    QPointF startPoint = QPointF(0, 0);
    QPointF endPoint = QPointF(0, 0);
    QRectF previousLineDirtyRect = QRectF();
};


KisToolKnife::KisToolKnife(KoCanvasBase * canvas)
    : KoInteractionTool(canvas),
      m_d(new Private)
{
    setObjectName("tool_knife");
    useCursor(Qt::ArrowCursor);
    repaintDecorations();
}

KisToolKnife::~KisToolKnife()
{
    m_d->optionsWidget = nullptr;
}

void paintSelectedEdge(QPainter &painter, const KoViewConverter &converter, const QLineF &lineSegment)
{
    QLineF lineInView = converter.documentToView().map(lineSegment);
    QList<QLineF> parallels = KisAlgebra2D::getParallelLines(lineInView, 5);

    painter.save();
    qreal width = 2;
    QColor color = Qt::blue;
    color.setAlphaF(0.8);
    QColor white = Qt::white;
    white.setAlphaF(0.75);

    QPen pen = QPen(color, width);
    QPen alternative = QPen(white, width);

    alternative.setStyle(Qt::CustomDashLine);
    qreal dashLength = 6;
    alternative.setDashPattern({dashLength - 1, dashLength + 1});
    alternative.setCapStyle(Qt::RoundCap);

    pen.setCosmetic(true);
    painter.setPen(pen);

    //painter.drawLines(parallels.toVector());
    painter.drawLine(lineInView);

    alternative.setCosmetic(true);
    painter.setPen(alternative);
    //painter.drawLines(parallels.toVector());
    painter.drawLine(lineInView);


    painter.restore();

}

QPolygonF createDiamond(int size, QPointF location = QPointF())
{
    QPolygonF polygon;
    polygon << QPointF(-size, 0);
    polygon << QPointF(0, size);
    polygon << QPointF(size, 0);
    polygon << QPointF(0, -size);
    polygon.translate(location);
    return polygon;
}

void paintSelectedPoint(QPainter &painter, const KoViewConverter &converter, const QPointF &point)
{
    QPointF p = point;
    p = converter.documentToView().map(p);
    QPolygonF diamond = createDiamond(6, p);
    painter.save();
    QColor color = Qt::blue;
    color.setAlphaF(0.9);
    QColor white = Qt::white;
    white.setAlphaF(0.75);

    QPen pen = QPen(color, 2);
    pen.setCosmetic(true);
    QBrush brush = QBrush(white);
    painter.setPen(pen);
    painter.setBrush(brush);

    painter.drawPolygon(diamond);
    painter.restore();
}

void KisToolKnife::paint(QPainter &painter, const KoViewConverter &converter)
{
    Q_UNUSED(converter);

    painter.save();
    painter.restore();

    painter.save();
    painter.setBrush(Qt::darkGray);
    //painter.drawEllipse(converter.documentToView(m_d->mousePoint), 4, 4);

    painter.restore();

    KoInteractionTool::paint(painter, converter);


#ifdef KNIFE_DEBUG
    bool paintSelection = true;
    if (paintSelection) {

        KIS_SAFE_ASSERT_RECOVER_RETURN(canvas());
        KIS_SAFE_ASSERT_RECOVER_RETURN(canvas()->selectedShapesProxy());

        KIS_SAFE_ASSERT_RECOVER_RETURN(canvas()->selectedShapesProxy()->selection());

        KoSelection *selection = canvas()->selectedShapesProxy()->selection();

        QList<KoShape*> shapes = selection->selectedEditableShapes();

        Q_FOREACH(KoShape* shape, shapes) {
            KisAlgebra2D::VectorPath vector = KisAlgebra2D::VectorPath(shape->outline());
            painter.save();
            painter.setTransform(painter.transform());
            for (int i = 0; i < vector.segmentsCount(); i++) {
                paintSelectedEdge(painter, converter, shape->absoluteTransformation().map(vector.segmentAtAsLine(i)));
            }
            for (int i =  0; i < vector.pointsCount(); i++) {
                paintSelectedPoint(painter, converter, shape->absoluteTransformation().map(vector.pointAt(i).endPoint));
            }
            painter.restore();

        }
    }
#endif


}

void KisToolKnife::activate(const QSet<KoShape *> &shapes)
{
    KoInteractionTool::activate(shapes);
    useCursor(KisCursor::arrowCursor());

}

void KisToolKnife::deactivate()
{
    KoInteractionTool::deactivate();
}

void KisToolKnife::mousePressEvent(KoPointerEvent *event)
{
    // panels are cut from a vector layer's shapes: on any other layer the first press makes the page, a layer of
    // panels with one panel inside the image's edge, and the next drags cut it
    if (!isValidForCurrentLayer()) {
        makePage();
        event->accept();
        return;
    }

    KoInteractionTool::mousePressEvent(event);
}

void KisToolKnife::makePage()
{
    KisCanvas2 *kiscanvas = static_cast<KisCanvas2 *>(canvas());
    KisViewManager *view = kiscanvas ? kiscanvas->viewManager() : nullptr;
    KisImageSP image = kiscanvas ? KisImageSP(kiscanvas->image()) : KisImageSP();
    if (!view || !image || !view->nodeManager()) return;
    KisNodeSP node = view->nodeManager()->createNode(QStringLiteral("KisShapeLayer"));
    auto *layer = qobject_cast<KisShapeLayer *>(node.data());
    if (!layer) return;
    layer->KisBaseNode::setName(i18nc("the vector layer comic panels are cut from", "Panels"));
    // in points, as shapes are; a margin of 4% of the shorter side
    const QRectF page(0, 0, image->width() / image->xRes(), image->height() / image->yRes());
    const qreal margin = 0.04 * qMin(page.width(), page.height());
    QPainterPath outline;
    outline.addRect(page.adjusted(margin, margin, -margin, -margin));
    KoPathShape *panel = KoPathShape::createShapeFromPainterPath(outline);
    panel->setStroke(KoShapeStrokeSP(new KoShapeStroke(qMax<qreal>(1.0, 0.004 * qMin(page.width(), page.height())), Qt::black)));
    panel->setBackground(QSharedPointer<KoShapeBackground>());
    canvas()->addCommand(canvas()->shapeController()->addShape(panel, layer));
}

void KisToolKnife::mouseMoveEvent(KoPointerEvent *event)
{
    KoInteractionTool::mouseMoveEvent(event);

    if (event->buttons().testFlag(Qt::MouseButton::LeftButton)) {

        m_d->endPoint = event->point;
        QRectF dirtyRect;
        KisAlgebra2D::accumulateBounds(m_d->startPoint, &dirtyRect);
        KisAlgebra2D::accumulateBounds(m_d->endPoint, &dirtyRect);

        QRectF accumulatedWithPrevious = m_d->previousLineDirtyRect;
        accumulatedWithPrevious |= dirtyRect;

        canvas()->updateCanvas(accumulatedWithPrevious);
        m_d->previousLineDirtyRect = dirtyRect;

    }

    repaintDecorations();
}

void KisToolKnife::mouseReleaseEvent(KoPointerEvent *event)
{
    KoInteractionTool::mouseReleaseEvent(event);

    m_d->endPoint = event->point;

    QRectF dirtyRect;
    KisAlgebra2D::accumulateBounds(m_d->startPoint, &dirtyRect);
    KisAlgebra2D::accumulateBounds(m_d->endPoint, &dirtyRect);

    QRectF accumulatedWithPrevious = m_d->previousLineDirtyRect | dirtyRect;

    canvas()->updateCanvas(accumulatedWithPrevious);
    m_d->previousLineDirtyRect = dirtyRect;
}

KoInteractionStrategy *KisToolKnife::createStrategy(KoPointerEvent *event)
{
    QList<KoShape*> shapes = canvas()->shapeManager()->shapes();

    // Alt joins the panels either side of a gutter, whatever the mode
    if (m_d->optionsWidget->getToolMode() == KisToolKnifeOptionsWidget::ToolMode::AddGutter && !(event->modifiers() & Qt::AltModifier)) {
        return new CutThroughShapeStrategy(this, canvas()->selectedShapesProxy()->selection(), shapes, event->point, m_d->optionsWidget->getCurrentWidthsConfig());
    } else {
        return new RemoveGutterStrategy(this, canvas()->selectedShapesProxy()->selection(), shapes, event->point);
    }

    //return NULL;
}

bool KisToolKnife::isValidForCurrentLayer() const
{
    KisCanvas2 *kisCanvas = static_cast<KisCanvas2 *>(canvas());
    KisNodeSP node = kisCanvas->viewManager()->canvasResourceProvider()->currentNode();
    const KisShapeLayer *shapeLayer = qobject_cast<const KisShapeLayer*>(node.data());
    return (shapeLayer != nullptr);
}

QWidget * KisToolKnife::createOptionWidget()
{
    KisCanvas2 * kiscanvas = dynamic_cast<KisCanvas2*>(canvas());
    KIS_ASSERT(kiscanvas);
    qreal resolution = 1.0;
    if (kiscanvas->image()) {
        // we're going to assume isotropic image
        resolution = kiscanvas->image()->xRes();
    }

    m_d->optionsWidget = new KisToolKnifeOptionsWidget(kiscanvas->viewManager()->canvasResourceProvider(), 0, toolId(), resolution);
    m_d->optionsWidget->setObjectName(toolId() + "option widget");


    return m_d->optionsWidget;
}

