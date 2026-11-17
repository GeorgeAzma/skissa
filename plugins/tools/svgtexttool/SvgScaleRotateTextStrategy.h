/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef SVG_SCALE_ROTATE_TEXT_STRATEGY_H
#define SVG_SCALE_ROTATE_TEXT_STRATEGY_H

#include <KoInteractionStrategy.h>

#include <QPointF>
#include <QPolygonF>
#include <QTransform>

class KoSvgTextShape;

/**
 * Text scaled by a corner of its box (about the opposite corner, keeping its shape) or turned by a drag just
 * outside one (about its middle; Shift by 15°), in the text tool itself.
 */
class SvgScaleRotateTextStrategy : public KoInteractionStrategy
{
public:
    /// \p corner is the corner of the box taken (0 to 3, as the box's corners go round); \p rotate turns instead
    SvgScaleRotateTextStrategy(KoToolBase *tool, KoSvgTextShape *shape, const QPointF &clicked, int corner, bool rotate);
    ~SvgScaleRotateTextStrategy() override = default;

    /// The text box's corners in the document, in order round it
    static QPolygonF cornersOf(const KoSvgTextShape *shape);

    void handleMouseMove(const QPointF &mouseLocation, Qt::KeyboardModifiers modifiers) override;
    KUndo2Command *createCommand() override;
    void cancelInteraction() override;
    void finishInteraction(Qt::KeyboardModifiers modifiers) override;

private:
    /// Makes the text's transformation one that maps its starting absolute one by \p change, in the document
    void apply(const QTransform &change);

    KoSvgTextShape *m_shape;
    QPointF m_dragStart;
    QPointF m_pivot;
    bool m_rotate;
    QTransform m_initial;
    QTransform m_final;
};

#endif
