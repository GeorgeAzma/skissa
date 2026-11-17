/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISTOOLCURVESTROKE_H
#define KISTOOLCURVESTROKE_H

#include <QPolygonF>
#include <QScopedPointer>
#include <QTimer>

#include <KisToolPaintFactoryBase.h>
#include <kis_icon.h>
#include <kis_tool_shape.h>

class KisPaintingInformationBuilder;
class CurveStrokeHelper;

/**
 * A stroke shaped after it is drawn: a drag draws a line, tapered at both ends, painted with the brush once let go;
 * the stroke stays open, and dragging any point of it bends it there (a quadratic through the point held), its ends
 * move it. While it is dragged the brush paints it off the image and that shows over the canvas (its shape in the
 * paint color for a brush that paints nothing alone); the brush paints it again once let go, the preview staying until
 * the paint is there, so nothing flashes. A press away from it keeps it and starts the next;
 * Enter keeps it, Escape drops it.
 */
class KisToolCurveStroke : public KisToolShape
{
    Q_OBJECT
public:
    explicit KisToolCurveStroke(KoCanvasBase *canvas);
    ~KisToolCurveStroke() override;

    void beginPrimaryAction(KoPointerEvent *event) override;
    void continuePrimaryAction(KoPointerEvent *event) override;
    void endPrimaryAction(KoPointerEvent *event) override;
    void mouseMoveEvent(KoPointerEvent *event) override;
    bool primaryActionSupportsHiResEvents() const override { return true; }

    void deactivate() override;
    void requestStrokeEnd() override;
    void requestStrokeCancellation() override;

    void paint(QPainter &gc, const KoViewConverter &converter) override;

private:
    /// What a press takes: nothing, the start, the end, the stroke between, or a new stroke being drawn
    enum Grip { None, Start, End, Bend, Drawing };

    Grip gripAt(const QPointF &pixel, qreal *along = nullptr) const;
    QPointF pointAt(qreal t) const;
    /// The pressure of the stroke a share \p t along it
    qreal pressureAt(qreal t) const;
    /// The stroke as the brush would paint it, as an outline in the view
    QPolygonF shapeInView() const;
    /// The points the brush paints the stroke through
    QVector<KisPaintInformation> strokePoints() const;
    /// The stroke painted by the brush off the image, shown while it is shaped; null for a brush that paints nothing alone
    void paintPreview();
    void paintWithBrush();
    void keep();
    void drop();
    void updateDecorations();

    QScopedPointer<KisPaintingInformationBuilder> m_infoBuilder;
    QScopedPointer<CurveStrokeHelper> m_helper;
    /// The shape is shown until the brush's paint has arrived
    QTimer m_untilPainted;
    bool m_showsShape {false};
    /// The stroke: its ends and the control point between
    QPointF m_start, m_end, m_control;
    /// A stroke on the canvas, still being shaped
    bool m_open {false};
    Grip m_grip {None};
    /// Where along the stroke it was taken, for a bend
    qreal m_along {0.5};
    QPointF m_pressedAt;
    QPointF m_start0, m_end0, m_control0;
    /// The firmest the pen pressed while the line was drawn: the stroke's middle
    qreal m_pressure {1.0};
    QRectF m_decorated;
    bool m_repainting {false};
    /// The brush's stroke as shown while shaping, and where it lies in the image
    QImage m_preview;
    QRect m_previewRect;
    /// One preview made per turn of the event loop, however many moves came
    QTimer m_previewLater;
};

class KisToolCurveStrokeFactory : public KisToolPaintFactoryBase
{
public:
    KisToolCurveStrokeFactory()
        : KisToolPaintFactoryBase(QStringLiteral("KritaShape/KisToolCurveStroke"))
    {
        setToolTip(i18nc("@info:tooltip a line that bends", "Line"));
        setSection(ToolBoxSection::Shape);
        setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
        setPriority(2);
        setIconName(QStringLiteral("studio_curve"));
    }

    KoToolBase *createTool(KoCanvasBase *canvas) override
    {
        return new KisToolCurveStroke(canvas);
    }
};

#endif // KISTOOLCURVESTROKE_H
