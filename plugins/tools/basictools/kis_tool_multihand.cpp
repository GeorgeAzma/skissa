/*
 *  SPDX-FileCopyrightText: 2011 Lukáš Tvrdý <lukast.dev@gmail.com>
 *  SPDX-FileCopyrightText: 2011 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "kis_tool_multihand.h"

#include <cmath>

#include <QLineF>
#include <QPainter>
#include <QPainterPath>
#include <QTransform>

#include <KoPointerEvent.h>
#include <KoViewConverter.h>

#include "KisStudioSymmetry.h"
#include "KisViewManager.h"
#include "kis_canvas2.h"
#include "kis_cursor.h"
#include "kis_selection.h"
#include "kis_tool_multihand_helper.h"

namespace
{

/// Tile: more copies than this are not painted (each is a stroke of its own)
const int maximumTiles = 100;
/// How far the handles are taken, in screen pixels: the knob from the center, a press from either
const qreal knobDistance = 64;
const qreal grab = 10;

} // namespace

KisToolMultihand::KisToolMultihand(KoCanvasBase *canvas)
    : KisToolBrush(canvas)
{
    m_helper = new KisToolMultihandHelper(paintingInformationBuilder(), canvas->resourceManager(), kundo2_i18n("Multibrush Stroke"));
    resetHelper(m_helper);
    if (image()) {
        m_center = QPointF(0.5 * image()->width(), 0.5 * image()->height());
        connect(image(), SIGNAL(sigSizeChanged(QPointF, QPointF)), this, SLOT(resetCenter()));
    }
    connect(KisStudioSymmetry::instance(), &KisStudioSymmetry::changed, this, &KisToolMultihand::readSettings);
    readSettings();
}

KisToolMultihand::~KisToolMultihand()
{
}

void KisToolMultihand::readSettings()
{
    KisStudioSymmetry *symmetry = KisStudioSymmetry::instance();
    m_tile = symmetry->mode() == KisStudioSymmetry::Tile;
    m_copies = symmetry->copies();
    m_mirrored = symmetry->mirrored();
    m_spacing = symmetry->spacing();
    if (m_dragging != Handle::Knob) m_angle = symmetry->turn();
    updateCanvas();
}

void KisToolMultihand::resetCenter()
{
    if (image()) {
        m_center = QPointF(0.5 * image()->width(), 0.5 * image()->height());
    }
    updateCanvas();
}

KisToolMultihand::Handle KisToolMultihand::handleAt(const QPointF &point) const
{
    if (m_tile) {
        return Handle::None;
    }
    const QPointF at = canvas()->viewConverter()->documentToView(point);
    if (QLineF(at, pixelToView(m_center)).length() <= grab) {
        return Handle::Center;
    }
    if (QLineF(at, pixelToView(knob())).length() <= grab) {
        return Handle::Knob;
    }
    return Handle::None;
}

QPointF KisToolMultihand::knob() const
{
    // as far from the center on the screen at any zoom, the way the first axis goes
    const QPointF way(std::sin(m_angle), -std::cos(m_angle));
    const qreal onScreen = QLineF(pixelToView(m_center), pixelToView(m_center + way)).length();
    return m_center + way * (knobDistance / qMax(onScreen, 1e-6));
}

void KisToolMultihand::beginPrimaryAction(KoPointerEvent *event)
{
    m_dragging = handleAt(event->point);
    if (m_dragging != Handle::None) {
        setMode(KisTool::OTHER);
        return;
    }
    if (m_tile) {
        m_tileStart = convertToPixelCoord(event->point);
    }
    initTransformations();
    KisToolBrush::beginPrimaryAction(event);
}

void KisToolMultihand::continuePrimaryAction(KoPointerEvent *event)
{
    if (mode() != KisTool::OTHER) {
        KisToolBrush::continuePrimaryAction(event);
        return;
    }
    const QPointF at = convertToPixelCoord(event->point);
    if (m_dragging == Handle::Center) {
        m_center = at;
    } else if (m_dragging == Handle::Knob && at != m_center) {
        m_angle = std::atan2(at.x() - m_center.x(), m_center.y() - at.y());
    }
    updateCanvas();
}

void KisToolMultihand::endPrimaryAction(KoPointerEvent *event)
{
    if (mode() != KisTool::OTHER) {
        KisToolBrush::endPrimaryAction(event);
        return;
    }
    setMode(KisTool::HOVER_MODE);
    const bool turned = m_dragging == Handle::Knob;
    m_dragging = Handle::None;
    if (turned) KisStudioSymmetry::instance()->setTurn(m_angle);
    updateCanvas();
}

void KisToolMultihand::beginPrimaryDoubleClickAction(KoPointerEvent *event)
{
    if (handleAt(event->point) != Handle::Center) {
        KisToolBrush::beginPrimaryDoubleClickAction(event);
        return;
    }
    // back in the middle; the release that follows ends nothing
    resetCenter();
    m_dragging = Handle::None;
    setMode(KisTool::OTHER);
}

void KisToolMultihand::mouseMoveEvent(KoPointerEvent *event)
{
    if (mode() == KisTool::HOVER_MODE) {
        const Handle hovered = handleAt(event->point);
        if (hovered != m_hovered) {
            m_hovered = hovered;
            if (hovered == Handle::None) {
                resetCursorStyle();
            } else {
                useCursor(hovered == Handle::Center ? KisCursor::moveCursor() : KisCursor::rotateCursor());
            }
            updateCanvas();
        }
    }
    KisToolBrush::mouseMoveEvent(event);
}

void KisToolMultihand::paint(QPainter &gc, const KoViewConverter &converter)
{
    KisToolBrush::paint(gc, converter);
    // out of the way while painting
    if (mode() == KisTool::PAINT_MODE || !currentImage()) {
        return;
    }

    QPainterPath guides;
    if (m_tile) {
        // the cells' edges
        const QRect bounds = tileBounds();
        if (m_spacing.width() > 0) {
            for (int x = bounds.left() + m_spacing.width(); x <= bounds.right(); x += m_spacing.width()) {
                guides.moveTo(x, bounds.top());
                guides.lineTo(x, bounds.bottom() + 1);
            }
        }
        if (m_spacing.height() > 0) {
            for (int y = bounds.top() + m_spacing.height(); y <= bounds.bottom(); y += m_spacing.height()) {
                guides.moveTo(bounds.left(), y);
                guides.lineTo(bounds.right() + 1, y);
            }
        }
    } else {
        // the axes the copies are turned by (and mirrored across), from a little way out of the center
        const qreal far = currentImage()->width() + currentImage()->height();
        const qreal inset = (knobDistance / 4) * QLineF(m_center, knob()).length() / knobDistance;
        const int rays = m_mirrored ? 2 * m_copies : m_copies;
        for (int i = 0; i < rays; i++) {
            const qreal a = m_angle + 2 * M_PI * i / rays;
            const QPointF along(std::sin(a), -std::cos(a));
            guides.moveTo(m_center + along * inset);
            guides.lineTo(m_center + along * far);
        }
    }
    QPainterPath outline = pixelToView(guides);
    if (!m_tile) {
        // the handles, drawn as the brush outline is (seen over any color): the center, a ring with a dot, and the
        // knob on the first axis; larger under the pointer
        const QPointF center = pixelToView(m_center);
        const qreal centerRadius = m_hovered == Handle::Center || m_dragging == Handle::Center ? 8 : 6;
        outline.addEllipse(center, centerRadius, centerRadius);
        outline.addEllipse(center, 1.5, 1.5);
        const qreal knobRadius = m_hovered == Handle::Knob || m_dragging == Handle::Knob ? 6.5 : 4.5;
        outline.addEllipse(pixelToView(knob()), knobRadius, knobRadius);
    }
    paintToolOutline(&gc, outline);
}

QRect KisToolMultihand::tileBounds() const
{
    KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas());
    KIS_ASSERT_RECOVER_RETURN_VALUE(kisCanvas, QRect());
    return kisCanvas->viewManager()->selection() ? kisCanvas->viewManager()->selection()->selectedExactRect() : kisCanvas->currentImage()->bounds();
}

void KisToolMultihand::initTransformations()
{
    QVector<QTransform> transformations;
    const QPointF c = m_center;

    if (m_tile) {
        const QRect bounds = tileBounds();
        const int sx = m_spacing.width();
        const int sy = m_spacing.height();
        const int across = sx > 0 ? (bounds.width() - 1) / sx + 1 : 1;
        const int down = sy > 0 ? (bounds.height() - 1) / sy + 1 : 1;
        if (across * down > maximumTiles) {
            if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas())) {
                kisCanvas->viewManager()->showFloatingMessage(
                    i18n("Tiles are too close: at most %1 copies, the stroke is painted once.", maximumTiles), QIcon());
            }
            transformations << QTransform();
        } else {
            // the cell the stroke starts in: every other cell gets it in the same place
            const QPointF cell(sx > 0 ? bounds.left() + sx * std::floor((m_tileStart.x() - bounds.left()) / sx) : bounds.left(),
                               sy > 0 ? bounds.top() + sy * std::floor((m_tileStart.y() - bounds.top()) / sy) : bounds.top());
            for (int i = 0; i < across; i++) {
                for (int j = 0; j < down; j++) {
                    transformations << QTransform::fromTranslate(bounds.left() + i * sx - cell.x(), bounds.top() + j * sy - cell.y());
                }
            }
        }
    } else {
        // turned around the center; mirrored, also across each axis halfway between them (a kaleidoscope)
        for (int k = 0; k < m_copies; k++) {
            transformations << QTransform().translate(c.x(), c.y()).rotateRadians(2 * M_PI * k / m_copies).translate(-c.x(), -c.y());
        }
        if (m_mirrored) {
            for (int k = 0; k < m_copies; k++) {
                // the axis's angle from the x axis (axes are angles from straight up)
                const qreal axis = m_angle + M_PI * k / m_copies - M_PI_2;
                transformations << QTransform().translate(c.x(), c.y()).rotateRadians(axis).scale(1, -1).rotateRadians(-axis).translate(-c.x(), -c.y());
            }
        }
    }

    m_helper->setupTransformations(transformations);
}

void KisToolMultihand::updateCanvas()
{
    if (KisCanvas2 *kisCanvas = dynamic_cast<KisCanvas2 *>(canvas())) {
        kisCanvas->updateCanvas();
    }
}
