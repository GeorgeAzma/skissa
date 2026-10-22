/*
 *  SPDX-FileCopyrightText: 2011 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISSTUDIOMULTIBRUSH_H
#define KISSTUDIOMULTIBRUSH_H

#include "kis_tool_brush.h"
#include <kis_icon.h>

class KisToolMultihandHelper;

/**
 * The brush, painting Symmetry's radial and tiled copies (see
 * KisStudioSymmetry, whose settings it follows). Radial copies turn around a
 * center that is dragged on the canvas, as is the knob that turns them; a
 * double click on the center puts it back in the middle of the image.
 */
class KisToolMultihand : public KisToolBrush
{
    Q_OBJECT
public:
    KisToolMultihand(KoCanvasBase *canvas);
    ~KisToolMultihand() override;

    void beginPrimaryAction(KoPointerEvent *event) override;
    void continuePrimaryAction(KoPointerEvent *event) override;
    void endPrimaryAction(KoPointerEvent *event) override;
    void beginPrimaryDoubleClickAction(KoPointerEvent *event) override;
    void mouseMoveEvent(KoPointerEvent *event) override;

protected:
    void paint(QPainter &gc, const KoViewConverter &converter) override;

private:
    enum class Handle { None, Center, Knob };

    /// The handle at \p point (document coordinates), if any
    Handle handleAt(const QPointF &point) const;
    /// Where the knob that turns the copies is, in image pixels
    QPointF knob() const;
    void initTransformations();
    /// Tile: the part of the image the copies are spread over (the selection, or all of it)
    QRect tileBounds() const;
    void updateCanvas();

private Q_SLOTS:
    void readSettings();
    void resetCenter();

private:
    KisToolMultihandHelper *m_helper;

    bool m_tile {false};
    int m_copies {6};
    bool m_mirrored {false};
    QSize m_spacing;
    QPointF m_center;
    /// Tile: where the stroke started, its place in its cell the same in every cell
    QPointF m_tileStart;
    /// Of the first axis, from straight up, clockwise
    qreal m_angle {0};
    Handle m_dragging {Handle::None};
    Handle m_hovered {Handle::None};
};


class KisToolMultiBrushFactory : public KisToolBrushFactory
{

public:
    KisToolMultiBrushFactory()
        : KisToolBrushFactory("KritaShape/KisToolMultiBrush") {

        setToolTip(i18n("Multibrush Tool"));

        // Temporarily
        setSection(ToolBoxSection::Shape);
        setIconName(koIconNameCStr("krita_tool_multihand"));
        setShortcut(QKeySequence(Qt::Key_Q));
        setPriority(11);
        setActivationShapeId(KRITA_TOOL_ACTIVATION_ID);
    }

    ~KisToolMultiBrushFactory() override {}

    KoToolBase * createTool(KoCanvasBase *canvas) override {
        return new KisToolMultihand(canvas);
    }

};

#endif /* KISSTUDIOMULTIBRUSH_H */
