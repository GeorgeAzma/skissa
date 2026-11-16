/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISTOOLCURVESTROKE_H
#define KISTOOLCURVESTROKE_H

#include <QScopedPointer>
#include <QTimer>

#include <KisToolPaintFactoryBase.h>
#include <kis_icon.h>
#include <kis_tool_shape.h>

class KisPaintingInformationBuilder;
class CurveStrokeHelper;

/**
 * A stroke shaped after it is drawn: a drag paints a line with the brush at once, tapered at both ends; the line
 * stays open with two handles on it, and dragging them bends it (a cubic through both), repainted as it moves, as
 * do its ends. A press elsewhere keeps it and starts the next; Enter keeps it, Escape drops it.
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

    void activate(const QSet<KoShape *> &shapes) override;
    void deactivate() override;
    void requestStrokeEnd() override;
    void requestStrokeCancellation() override;

    void paint(QPainter &gc, const KoViewConverter &converter) override;

private Q_SLOTS:
    void repaintStroke();

private:
    /// What a press takes: nothing, an end, a handle, or a new stroke being drawn
    enum Part { None = -1, Start = 0, First = 1, Second = 2, End = 3, Drawing = 4 };

    Part partAt(const QPointF &pixel) const;
    /// \p count + 1 points along the curve, evenly by its parameter
    QVector<QPointF> curve(int count) const;
    void keep();
    void drop();
    void updateDecorations();

    QScopedPointer<KisPaintingInformationBuilder> m_infoBuilder;
    QScopedPointer<CurveStrokeHelper> m_helper;
    /// The stroke is painted again at most once a frame while it is reshaped
    QTimer m_repaint;
    /// On the curve: its start, the points a third and two thirds along, its end
    QPointF m_points[4];
    /// A stroke on the canvas, still being shaped
    bool m_open {false};
    Part m_dragged {None};
    QPointF m_pressedAt;
    QPointF m_pressedPoint;
    /// The firmest the pen pressed while the line was drawn: the stroke's middle
    qreal m_pressure {1.0};
    QRectF m_decorated;
    bool m_repainting {false};
};

class KisToolCurveStrokeFactory : public KisToolPaintFactoryBase
{
public:
    KisToolCurveStrokeFactory()
        : KisToolPaintFactoryBase(QStringLiteral("KritaShape/KisToolCurveStroke"))
    {
        setToolTip(i18nc("@info:tooltip", "Curve"));
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
