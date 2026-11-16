/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisToolCurveStroke.h"

#include <QPainter>
#include <QtMath>

#include <KoCanvasBase.h>
#include <KoPointerEvent.h>
#include <kis_algebra_2d.h>
#include <kis_canvas2.h>
#include <kis_cursor.h>
#include <kis_painting_information_builder.h>
#include <kis_paint_information.h>
#include <kis_smoothing_options.h>
#include <kis_tool_freehand_helper.h>
#include <KisViewManager.h>

namespace
{

/// How far from a handle, in screen pixels, a press still takes it
constexpr qreal handleReach = 14;
/// The share of the stroke at each end over which it tapers
constexpr qreal taper = 0.3;

const KisCoordinatesConverter *converterOf(KoCanvasBase *canvas)
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas);
    KIS_ASSERT(kisCanvas);
    return kisCanvas->coordinatesConverter();
}

/// The pressure of the taper a share \p t along the stroke: rising in from nothing, falling out to nothing
qreal taperAt(qreal t)
{
    const qreal x = qBound<qreal>(0, qMin(t, 1 - t) / taper, 1);
    return qMax<qreal>(0.02, x * x * (3 - 2 * x));
}

} // namespace

/// The brush engine's stroke, painted again whole whenever the curve changes, until it is kept
class CurveStrokeHelper : public KisToolFreehandHelper
{
public:
    CurveStrokeHelper(KisPaintingInformationBuilder *infoBuilder, KoCanvasResourceProvider *resourceManager)
        : KisToolFreehandHelper(infoBuilder, resourceManager, kundo2_i18n("Draw Curve"), new KisSmoothingOptions(false))
    {
    }

    void paintThrough(const QVector<KisPaintInformation> &points, KisImageWSP image, KisNodeSP node, KisStrokesFacade *strokesFacade)
    {
        cancelPaint();
        if (points.size() < 2) return;
        const qreal startAngle = KisAlgebra2D::directionBetweenPoints(points[0].pos(), points[1].pos(), 0.0);
        initPaintImpl(startAngle, points.first(), resourceManager(), image, node, strokesFacade);
        for (int i = 1; i < points.size(); i++) paintLine(points[i - 1], points[i]);
    }

    void keep()
    {
        if (isRunning()) endPaint();
    }

    void drop()
    {
        if (isRunning()) cancelPaint();
    }
};

KisToolCurveStroke::KisToolCurveStroke(KoCanvasBase *canvas)
    : KisToolShape(canvas, KisCursor::loadWithSize("tool_line_cursor.svg", 32, 32, 6, 6))
    , m_infoBuilder(new KisConverterPaintingInformationBuilder(converterOf(canvas)))
    , m_helper(new CurveStrokeHelper(m_infoBuilder.data(), canvas->resourceManager()))
{
    m_repaint.setSingleShot(true);
    m_repaint.setInterval(16);
    setObjectName(QStringLiteral("tool_curve_stroke"));
    setSupportOutline(true);
    setIsOpacityPresetMode(true);
    connect(&m_repaint, &QTimer::timeout, this, &KisToolCurveStroke::repaintStroke);
}

KisToolCurveStroke::~KisToolCurveStroke()
{
}

void KisToolCurveStroke::activate(const QSet<KoShape *> &shapes)
{
    KisToolShape::activate(shapes);
}

void KisToolCurveStroke::deactivate()
{
    keep();
    KisToolShape::deactivate();
}

void KisToolCurveStroke::requestStrokeEnd()
{
    // a repaint starting its stroke asks the strokes before to end: not this one's own
    if (m_repainting) return;
    keep();
}

void KisToolCurveStroke::requestStrokeCancellation()
{
    drop();
}

KisToolCurveStroke::Part KisToolCurveStroke::partAt(const QPointF &pixel) const
{
    if (!m_open) return None;
    const QPointF view = pixelToView(pixel);
    Part nearest = None;
    qreal best = handleReach;
    // the handles before the ends: on a short stroke they lie close together
    for (int i : {First, Second, Start, End}) {
        const qreal distance = QLineF(view, pixelToView(m_points[i])).length();
        if (distance < best) {
            best = distance;
            nearest = Part(i);
        }
    }
    return nearest;
}

QVector<QPointF> KisToolCurveStroke::curve(int count) const
{
    // the cubic through all four points, at 0, 1/3, 2/3 and 1
    const QPointF p0 = m_points[Start], p3 = m_points[End];
    const QPointF a = 27 * m_points[First] - 8 * p0 - p3;
    const QPointF b = 27 * m_points[Second] - p0 - 8 * p3;
    const QPointF c1 = (2 * a - b) / 18;
    const QPointF c2 = (2 * b - a) / 18;
    QVector<QPointF> points;
    points.reserve(count + 1);
    for (int i = 0; i <= count; i++) {
        const qreal t = qreal(i) / count, s = 1 - t;
        points << s * s * s * p0 + 3 * s * s * t * c1 + 3 * s * t * t * c2 + t * t * t * p3;
    }
    return points;
}

void KisToolCurveStroke::repaintStroke()
{
    if (!m_open || nodePaintAbility() != PAINT) return;
    // a point every couple of pixels along it
    const qreal length = QLineF(m_points[Start], m_points[First]).length() + QLineF(m_points[First], m_points[Second]).length()
        + QLineF(m_points[Second], m_points[End]).length();
    const int count = qBound(8, int(length / 2), 4000);
    const QVector<QPointF> positions = curve(count);
    QVector<KisPaintInformation> points;
    points.reserve(positions.size());
    for (int i = 0; i < positions.size(); i++) {
        points << KisPaintInformation(positions[i], m_pressure * taperAt(qreal(i) / count));
    }
    m_repainting = true;
    m_helper->paintThrough(points, image(), currentNode(), image().data());
    m_repainting = false;
}

void KisToolCurveStroke::keep()
{
    m_repaint.stop();
    if (m_open && m_dragged == None) repaintStroke();
    m_helper->keep();
    m_open = false;
    m_dragged = None;
    updateDecorations();
}

void KisToolCurveStroke::drop()
{
    m_repaint.stop();
    m_helper->drop();
    m_open = false;
    m_dragged = None;
    updateDecorations();
}

void KisToolCurveStroke::beginPrimaryAction(KoPointerEvent *event)
{
    if (nodePaintAbility() != PAINT || !nodeEditable()) {
        event->ignore();
        return;
    }
    const QPointF pixel = convertToPixelCoord(event);
    const Part part = partAt(pixel);
    setMode(KisTool::PAINT_MODE);
    m_pressedAt = pixel;
    if (part != None) {
        // reshaping the open stroke
        m_dragged = part;
        m_pressedPoint = m_points[part];
        return;
    }
    // the stroke before is kept; a new one starts here
    if (m_open) keep();
    for (QPointF &point : m_points) point = pixel;
    m_pressure = event->isTabletEvent() ? qMax<qreal>(0.05, event->pressure()) : 1.0;
    m_open = true;
    m_dragged = Drawing;
}

void KisToolCurveStroke::continuePrimaryAction(KoPointerEvent *event)
{
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);
    const QPointF pixel = convertToPixelCoord(event);
    if (m_dragged == Drawing) {
        // a straight line while it is drawn, its handles a third and two thirds along
        m_points[End] = pixel;
        m_points[First] = m_points[Start] + (pixel - m_points[Start]) / 3;
        m_points[Second] = m_points[Start] + 2 * (pixel - m_points[Start]) / 3;
        if (event->isTabletEvent()) m_pressure = qMax(m_pressure, event->pressure());
    } else if (m_dragged != None) {
        const QPointF moved = pixel - m_pressedAt;
        if (m_dragged == Start || m_dragged == End) {
            // an end moved takes the handles along with it, less the further they are from it
            const QPointF before = m_points[m_dragged];
            const QPointF now = m_pressedPoint + moved;
            const QPointF step = now - before;
            m_points[m_dragged] = now;
            m_points[First] += step * (m_dragged == Start ? 2.0 / 3 : 1.0 / 3);
            m_points[Second] += step * (m_dragged == Start ? 1.0 / 3 : 2.0 / 3);
        } else {
            m_points[m_dragged] = m_pressedPoint + moved;
        }
    }
    if (m_dragged != None && !m_repaint.isActive()) m_repaint.start();
    updateDecorations();
    requestUpdateOutline(event->point, event);
}

void KisToolCurveStroke::endPrimaryAction(KoPointerEvent *event)
{
    Q_UNUSED(event);
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);
    setMode(KisTool::HOVER_MODE);
    if (m_dragged == Drawing && QLineF(pixelToView(m_points[Start]), pixelToView(m_points[End])).length() < 3) {
        // a click: nothing to draw
        drop();
        return;
    }
    m_dragged = None;
    m_repaint.stop();
    repaintStroke();
    updateDecorations();
}

void KisToolCurveStroke::mouseMoveEvent(KoPointerEvent *event)
{
    // over a handle, the pointer says it can be taken
    if (mode() == KisTool::HOVER_MODE) {
        if (partAt(convertToPixelCoord(event)) != None) useCursor(Qt::SizeAllCursor);
        else resetCursorStyle();
    }
    KisToolShape::mouseMoveEvent(event);
}

void KisToolCurveStroke::updateDecorations()
{
    QRectF now;
    if (m_open) {
        for (const QPointF &point : m_points) now |= QRectF(point, QSizeF(1, 1));
    }
    const QRectF dirty = m_decorated | now;
    m_decorated = now;
    if (dirty.isEmpty()) return;
    // the handles are a fixed size on screen
    const qreal margin = handleReach / qMax<qreal>(1e-3, pixelToView(QRectF(0, 0, 1, 1)).width());
    updateCanvasPixelRect(dirty.adjusted(-margin, -margin, margin, margin));
}

void KisToolCurveStroke::paint(QPainter &gc, const KoViewConverter &converter)
{
    if (m_open && m_dragged != Drawing) {
        gc.save();
        gc.setRenderHint(QPainter::Antialiasing, true);
        // white with a dark ring: seen on any picture
        auto handle = [&gc](const QPointF &at, qreal radius) {
            gc.setPen(QPen(QColor(0, 0, 0, 170), 1.5));
            gc.setBrush(Qt::white);
            gc.drawEllipse(at, radius, radius);
        };
        handle(pixelToView(m_points[Start]), 3.5);
        handle(pixelToView(m_points[End]), 3.5);
        handle(pixelToView(m_points[First]), 5.5);
        handle(pixelToView(m_points[Second]), 5.5);
        gc.restore();
    }
    KisToolShape::paint(gc, converter);
}
