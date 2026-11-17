/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisToolCurveStroke.h"

#include <QPainter>
#include <QtMath>

#include <KoCanvasBase.h>
#include <KoCanvasResourceProvider.h>
#include <KoPointerEvent.h>
#include <brushengine/kis_paintop_preset.h>
#include <brushengine/kis_paintop_settings.h>
#include <kis_algebra_2d.h>
#include <kis_canvas2.h>
#include <kis_cursor.h>
#include <kis_image.h>
#include <kis_paint_information.h>
#include <kis_distance_information.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_painting_information_builder.h>
#include <kis_smoothing_options.h>
#include <kis_tool_freehand_helper.h>

namespace
{

/// How far from the stroke, in screen pixels beyond its own width, a press still takes it
constexpr qreal reach = 10;
/// The share of the stroke at each end over which it tapers
constexpr qreal taper = 0.3;
/// The share of the stroke at each end that moves the end rather than bending it
constexpr qreal endGrip = 0.12;

const KisCoordinatesConverter *converterOf(KoCanvasBase *canvas)
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas);
    KIS_ASSERT(kisCanvas);
    return kisCanvas->coordinatesConverter();
}

/// The taper a share \p t along the stroke: rising in from nothing, falling out to nothing
qreal taperAt(qreal t)
{
    const qreal x = qBound<qreal>(0, qMin(t, 1 - t) / taper, 1);
    return qMax<qreal>(0.02, x * x * (3 - 2 * x));
}

} // namespace

/// The brush engine's stroke, painted again whole whenever the curve is let go, until it is kept
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
    setObjectName(QStringLiteral("tool_curve_stroke"));
    setSupportOutline(true);
    setIsOpacityPresetMode(true);
    m_previewLater.setSingleShot(true);
    m_previewLater.setInterval(0);
    connect(&m_previewLater, &QTimer::timeout, this, [this]() {
        paintPreview();
        updateDecorations();
    });
    // the shape goes once the image has no work left from the paint: two quiet looks in a row
    m_untilPainted.setInterval(30);
    connect(&m_untilPainted, &QTimer::timeout, this, [this]() {
        static int quiet = 0;
        quiet = image() && !image()->hasUpdatesRunning() ? quiet + 1 : 0;
        if (quiet >= 3) {
            quiet = 0;
            m_untilPainted.stop();
            m_showsShape = false;
            updateDecorations();
        }
    });
}

KisToolCurveStroke::~KisToolCurveStroke()
{
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

QPointF KisToolCurveStroke::pointAt(qreal t) const
{
    const qreal s = 1 - t;
    return s * s * m_start + 2 * s * t * m_control + t * t * m_end;
}

qreal KisToolCurveStroke::pressureAt(qreal t) const
{
    return m_pressure * taperAt(t);
}

KisToolCurveStroke::Grip KisToolCurveStroke::gripAt(const QPointF &pixel, qreal *along) const
{
    if (!m_open) return None;
    const QPointF view = pixelToView(pixel);
    // the brush's own width counts as the stroke
    KisPaintOpPresetSP preset = canvas()->resourceManager()->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    const qreal radius = preset ? 0.5 * preset->settings()->paintOpSize() * pixelToView(QRectF(0, 0, 1, 1)).width() : 0;
    qreal best = reach + radius, bestT = -1;
    constexpr int samples = 96;
    for (int i = 0; i <= samples; i++) {
        const qreal t = qreal(i) / samples;
        const qreal distance = QLineF(view, pixelToView(pointAt(t))).length();
        if (distance < best) {
            best = distance;
            bestT = t;
        }
    }
    if (bestT < 0) return None;
    if (along) *along = bestT;
    return bestT < endGrip ? Start : bestT > 1 - endGrip ? End : Bend;
}

QPolygonF KisToolCurveStroke::shapeInView() const
{
    KisPaintOpPresetSP preset = canvas()->resourceManager()->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    const qreal size = preset ? preset->settings()->paintOpSize() : 10;
    constexpr int count = 64;
    QVector<QPointF> left, right;
    for (int i = 0; i <= count; i++) {
        const qreal t = qreal(i) / count;
        const QPointF at = pointAt(t);
        // the tangent of the quadratic there
        QPointF along = 2 * (1 - t) * (m_control - m_start) + 2 * t * (m_end - m_control);
        const qreal length = std::hypot(along.x(), along.y());
        along = length > 1e-9 ? along / length : QPointF(1, 0);
        const QPointF across(-along.y(), along.x());
        const qreal half = qMax<qreal>(0.5, 0.5 * size * pressureAt(t));
        left << pixelToView(at + across * half);
        right << pixelToView(at - across * half);
    }
    QPolygonF shape(left);
    for (int i = right.size() - 1; i >= 0; i--) shape << right[i];
    return shape;
}

QVector<KisPaintInformation> KisToolCurveStroke::strokePoints() const
{
    // a point every couple of pixels along it
    const qreal length = QLineF(m_start, m_control).length() + QLineF(m_control, m_end).length();
    const int count = qBound(8, int(length / 2), 4000);
    QVector<KisPaintInformation> points;
    points.reserve(count + 1);
    for (int i = 0; i <= count; i++) {
        const qreal t = qreal(i) / count;
        KisPaintInformation info(pointAt(t), pressureAt(t));
        info.setCurrentTime(i);
        points << info;
    }
    return points;
}

void KisToolCurveStroke::paintPreview()
{
    m_preview = QImage();
    KisPaintOpPresetSP preset = canvas()->resourceManager()->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    if (!m_open || !preset || !image() || !currentNode()) return;
    const QVector<KisPaintInformation> points = strokePoints();
    KisPaintDeviceSP device = new KisPaintDevice(image()->colorSpace());
    KisPainter painter(device);
    painter.setPaintColor(canvas()->resourceManager()->foregroundColor());
    painter.setPaintOpPreset(preset, currentNode(), image());
    KisDistanceInformation distance(points.first().pos(), KisAlgebra2D::directionBetweenPoints(points[0].pos(), points[1].pos(), 0.0));
    for (int i = 1; i < points.size(); i++) painter.paintLine(points[i - 1], points[i], &distance);
    painter.end();
    m_previewRect = device->exactBounds();
    if (m_previewRect.isEmpty()) return;
    m_preview = device->convertToQImage(nullptr, m_previewRect);
}

void KisToolCurveStroke::paintWithBrush()
{
    if (!m_open || nodePaintAbility() != PAINT) return;
    m_repainting = true;
    m_helper->paintThrough(strokePoints(), image(), currentNode(), image().data());
    m_repainting = false;
    // the shape stays over the paint until it has arrived
    m_showsShape = true;
    m_untilPainted.start();
}

void KisToolCurveStroke::keep()
{
    m_preview = QImage();
    m_helper->keep();
    m_open = false;
    m_grip = None;
    updateDecorations();
}

void KisToolCurveStroke::drop()
{
    m_untilPainted.stop();
    m_showsShape = false;
    m_preview = QImage();
    m_helper->drop();
    m_open = false;
    m_grip = None;
    updateDecorations();
}

void KisToolCurveStroke::beginPrimaryAction(KoPointerEvent *event)
{
    if (nodePaintAbility() != PAINT || !nodeEditable()) {
        event->ignore();
        return;
    }
    const QPointF pixel = convertToPixelCoord(event);
    setMode(KisTool::PAINT_MODE);
    m_pressedAt = pixel;
    m_start0 = m_start;
    m_end0 = m_end;
    m_control0 = m_control;
    const Grip grip = gripAt(pixel, &m_along);
    if (grip != None) {
        // reshaping the open stroke: its paint gives way to the preview until it is let go
        m_grip = grip;
        paintPreview();
        m_helper->drop();
        m_showsShape = true;
        m_untilPainted.stop();
        updateDecorations();
        return;
    }
    // the stroke before is kept; a new one starts here
    if (m_open) keep();
    m_start = m_end = m_control = pixel;
    m_pressure = event->isTabletEvent() ? qMax<qreal>(0.05, event->pressure()) : 1.0;
    m_open = true;
    m_grip = Drawing;
    m_showsShape = true;
}

void KisToolCurveStroke::continuePrimaryAction(KoPointerEvent *event)
{
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);
    const QPointF pixel = convertToPixelCoord(event);
    const QPointF moved = pixel - m_pressedAt;
    switch (m_grip) {
    case Drawing:
        // straight while it is drawn
        m_end = pixel;
        m_control = (m_start + m_end) / 2;
        if (event->isTabletEvent()) m_pressure = qMax(m_pressure, event->pressure());
        break;
    case Bend: {
        // the curve goes through the pointer at the place it was taken
        const qreal t = qBound<qreal>(0.1, m_along, 0.9), s = 1 - t;
        m_control = (pixel - s * s * m_start0 - t * t * m_end0) / (2 * s * t);
        break;
    }
    case Start:
    case End: {
        // an end moved takes the bend along by half
        QPointF &end = m_grip == Start ? m_start : m_end;
        end = (m_grip == Start ? m_start0 : m_end0) + moved;
        m_control = m_control0 + moved / 2;
        break;
    }
    case None:
        break;
    }
    if (!m_previewLater.isActive()) m_previewLater.start();
    updateDecorations();
    requestUpdateOutline(event->point, event);
}

void KisToolCurveStroke::endPrimaryAction(KoPointerEvent *event)
{
    Q_UNUSED(event);
    CHECK_MODE_SANITY_OR_RETURN(KisTool::PAINT_MODE);
    setMode(KisTool::HOVER_MODE);
    if (m_grip == Drawing && QLineF(pixelToView(m_start), pixelToView(m_end)).length() < 3) {
        // a click: nothing to draw
        drop();
        return;
    }
    m_grip = None;
    paintWithBrush();
}

void KisToolCurveStroke::mouseMoveEvent(KoPointerEvent *event)
{
    // over the open stroke, the pointer says it can be taken
    if (mode() == KisTool::HOVER_MODE) {
        if (gripAt(convertToPixelCoord(event)) != None) useCursor(Qt::SizeAllCursor);
        else resetCursorStyle();
    }
    KisToolShape::mouseMoveEvent(event);
}

void KisToolCurveStroke::updateDecorations()
{
    QRectF now;
    if (m_open) now = QRectF(m_start, QSizeF(1, 1)) | QRectF(m_end, QSizeF(1, 1)) | QRectF(pointAt(0.5), QSizeF(1, 1)) | QRectF(m_control, QSizeF(1, 1));
    const QRectF dirty = m_decorated | now;
    m_decorated = now;
    if (dirty.isEmpty()) return;
    KisPaintOpPresetSP preset = canvas()->resourceManager()->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
    const qreal margin = (preset ? preset->settings()->paintOpSize() : 10) + 4;
    updateCanvasPixelRect(dirty.adjusted(-margin, -margin, margin, margin));
}

void KisToolCurveStroke::paint(QPainter &gc, const KoViewConverter &converter)
{
    if (m_open && m_showsShape) {
        // the brush's stroke, while it hasn't painted it on the image yet
        gc.save();
        gc.setRenderHint(QPainter::Antialiasing, true);
        gc.setRenderHint(QPainter::SmoothPixmapTransform, true);
        KisPaintOpPresetSP preset = canvas()->resourceManager()->resource(KoCanvasResource::CurrentPaintOpPreset).value<KisPaintOpPresetSP>();
        const qreal opacity = preset ? qBound<qreal>(0.15, preset->settings()->paintOpOpacity(), 1.0) : 1.0;
        if (!m_preview.isNull()) {
            const QRectF r(m_previewRect);
            const QPolygonF from({r.topLeft(), r.topRight(), r.bottomRight(), r.bottomLeft()});
            const QPolygonF to({pixelToView(r.topLeft()), pixelToView(r.topRight()), pixelToView(r.bottomRight()), pixelToView(r.bottomLeft())});
            QTransform place;
            if (QTransform::quadToQuad(from, to, place)) {
                gc.setTransform(place, true);
                gc.setOpacity(opacity);
                gc.drawImage(r.topLeft(), m_preview);
            }
        } else {
            QColor color = canvas()->resourceManager()->foregroundColor().toQColor();
            color.setAlphaF(opacity);
            gc.setPen(Qt::NoPen);
            gc.setBrush(color);
            gc.drawPolygon(shapeInView());
        }
        gc.restore();
    }
    KisToolShape::paint(gc, converter);
}
