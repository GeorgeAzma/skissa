/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SvgScaleRotateTextStrategy.h"

#include <QtMath>

#include <KoCanvasBase.h>
#include <KoShapeContainer.h>
#include <KoSvgTextShape.h>
#include <KoToolBase.h>
#include <commands/KoShapeTransformCommand.h>
#include <kis_algebra_2d.h>
#include <kundo2command.h>

SvgScaleRotateTextStrategy::SvgScaleRotateTextStrategy(KoToolBase *tool, KoSvgTextShape *shape, const QPointF &clicked, int corner, bool rotate)
    : KoInteractionStrategy(tool)
    , m_shape(shape)
    , m_dragStart(clicked)
    , m_rotate(rotate)
    , m_initial(shape->transformation())
    , m_final(m_initial)
{
    const QPolygonF corners = cornersOf(shape);
    m_pivot = rotate ? shape->absoluteTransformation().map(shape->outlineRect().center()) : corners.value((corner + 2) % 4);
}

QPolygonF SvgScaleRotateTextStrategy::cornersOf(const KoSvgTextShape *shape)
{
    const QRectF box = shape->outlineRect();
    return shape->absoluteTransformation().map(QPolygonF({box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()}));
}

void SvgScaleRotateTextStrategy::apply(const QTransform &change)
{
    // the change is in the document; the shape's own transformation is inside its parent's
    const QTransform parent = m_shape->parent() ? m_shape->parent()->absoluteTransformation() : QTransform();
    m_final = m_initial * parent * change * parent.inverted();
    m_shape->update();
    m_shape->setTransformation(m_final);
    m_shape->update();
}

void SvgScaleRotateTextStrategy::handleMouseMove(const QPointF &mouseLocation, Qt::KeyboardModifiers modifiers)
{
    const QTransform toPivot = QTransform::fromTranslate(-m_pivot.x(), -m_pivot.y());
    const QTransform back = QTransform::fromTranslate(m_pivot.x(), m_pivot.y());
    if (m_rotate) {
        const QPointF from = m_dragStart - m_pivot, to = mouseLocation - m_pivot;
        qreal degrees = qRadiansToDegrees(std::atan2(to.y(), to.x()) - std::atan2(from.y(), from.x()));
        if (modifiers & Qt::ShiftModifier) degrees = std::round(degrees / 15) * 15;
        QTransform turn;
        turn.rotate(degrees);
        apply(toPivot * turn * back);
    } else {
        // as far from the opposite corner as the pointer is, along where it was taken
        const qreal before = QLineF(m_pivot, m_dragStart).length();
        const qreal now = QLineF(m_pivot, mouseLocation).length();
        const qreal scale = before > 1e-6 ? qMax<qreal>(0.02, now / before) : 1.0;
        apply(toPivot * QTransform::fromScale(scale, scale) * back);
    }
    tool()->repaintDecorations();
}

KUndo2Command *SvgScaleRotateTextStrategy::createCommand()
{
    if (m_final == m_initial) return nullptr;
    auto *command = new KoShapeTransformCommand({m_shape}, {m_initial}, {m_final});
    command->setText(m_rotate ? kundo2_i18nc("undo: text turned", "Rotate text") : kundo2_i18nc("undo: text scaled", "Scale text"));
    return command;
}

void SvgScaleRotateTextStrategy::cancelInteraction()
{
    m_shape->update();
    m_shape->setTransformation(m_initial);
    m_shape->update();
    m_final = m_initial;
    tool()->repaintDecorations();
}

void SvgScaleRotateTextStrategy::finishInteraction(Qt::KeyboardModifiers /*modifiers*/)
{
}
