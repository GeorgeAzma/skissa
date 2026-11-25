/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioGradientStrip.h"

#include <QDragEnterEvent>
#include <QLinearGradient>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <KisModernStyle.h>
#include <KoColorSpaceRegistry.h>

#include "KisColorDrop.h"

namespace
{
constexpr qreal stopRadius = 5.0;
constexpr qreal stripHeight = 16.0;
/// How far from the strip a dragged stop goes away
constexpr qreal removeDistance = 24.0;

QColor mixed(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}
} // namespace

KisStudioGradientStrip::KisStudioGradientStrip(QWidget *parent)
    : QWidget(parent)
{
    setAcceptDrops(true);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    m_stops = {KoGradientStop(0.0, KoColor(Qt::black, KoColorSpaceRegistry::instance()->rgb8())),
               KoGradientStop(1.0, KoColor(Qt::white, KoColorSpaceRegistry::instance()->rgb8()))};
}

QSize KisStudioGradientStrip::sizeHint() const
{
    return QSize(200, int(stripHeight + 2 * stopRadius + 4));
}

QSize KisStudioGradientStrip::minimumSizeHint() const
{
    return QSize(60, sizeHint().height());
}

void KisStudioGradientStrip::setGradient(KoAbstractGradientSP gradient)
{
    if (!gradient || m_dragged >= 0) return;
    KoStopGradientSP stops = gradient.dynamicCast<KoStopGradient>();
    if (!stops) {
        QScopedPointer<QGradient> q(gradient->toQGradient());
        stops = q ? KoStopGradient::fromQGradient(q.data()) : KoStopGradientSP();
    }
    if (!stops || stops->stops().size() < 2) return;
    m_stops = stops->stops();
    // shown, and edited, as the colors they are now
    for (KoGradientStop &stop : m_stops) stop.type = COLORSTOP;
    sortStops();
    update();
}

KoStopGradientSP KisStudioGradientStrip::gradient() const
{
    KoStopGradientSP made(new KoStopGradient(QStringLiteral("skissa_strip.svg")));
    made->setName(QStringLiteral("Strip"));
    QList<KoGradientStop> stops = m_stops;
    if (m_removing && m_dragged >= 0 && stops.size() > 2) stops.removeAt(m_dragged);
    made->setStops(stops);
    made->setValid(true);
    made->updatePreview();
    return made;
}

QRectF KisStudioGradientStrip::stripRect() const
{
    return QRectF(stopRadius + 1, 2, width() - 2 * (stopRadius + 1), stripHeight);
}

qreal KisStudioGradientStrip::xOf(qreal position) const
{
    const QRectF strip = stripRect();
    return strip.left() + strip.width() * position;
}

qreal KisStudioGradientStrip::positionAt(qreal x) const
{
    const QRectF strip = stripRect();
    return qBound<qreal>(0.0, (x - strip.left()) / qMax<qreal>(1.0, strip.width()), 1.0);
}

int KisStudioGradientStrip::stopAt(const QPointF &pos) const
{
    int best = -1;
    qreal nearest = stopRadius + 3;
    for (int i = 0; i < m_stops.size(); i++) {
        const qreal distance = qAbs(xOf(m_stops[i].position) - pos.x());
        if (distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return best;
}

KoColor KisStudioGradientStrip::colorAt(qreal position) const
{
    const KoColorSpace *space = m_stops.first().color.colorSpace();
    if (position <= m_stops.first().position) return m_stops.first().color;
    for (int i = 1; i < m_stops.size(); i++) {
        if (position > m_stops[i].position) continue;
        const qreal span = m_stops[i].position - m_stops[i - 1].position;
        const qreal t = span > 0 ? (position - m_stops[i - 1].position) / span : 0;
        return KoColor(mixed(m_stops[i - 1].color.toQColor(), m_stops[i].color.toQColor(), t), space);
    }
    return m_stops.last().color;
}

void KisStudioGradientStrip::sortStops()
{
    const KoGradientStop dragged = m_dragged >= 0 ? m_stops[m_dragged] : KoGradientStop();
    std::stable_sort(m_stops.begin(), m_stops.end(), [](const KoGradientStop &a, const KoGradientStop &b) { return a.position < b.position; });
    if (m_dragged >= 0) {
        for (int i = 0; i < m_stops.size(); i++) {
            if (m_stops[i].position == dragged.position && m_stops[i].color == dragged.color) m_dragged = i;
        }
    }
}

void KisStudioGradientStrip::emitChanged()
{
    update();
    if (changed) changed(gradient());
}

void KisStudioGradientStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF strip = stripRect();
    const QPainterPath shape = KisModernStyle::squircle(strip, KisModernStyle::cornerRadius);

    // a checkerboard under it, so see-through stops read as such
    painter.save();
    painter.setClipPath(shape);
    painter.fillRect(strip, QColor(200, 200, 200));
    const int cell = 4;
    for (int y = 0; y * cell < strip.height(); y++) {
        for (int x = 0; x * cell < strip.width(); x++) {
            if ((x + y) % 2) painter.fillRect(QRectF(strip.left() + x * cell, strip.top() + y * cell, cell, cell), QColor(255, 255, 255));
        }
    }
    QLinearGradient fill(strip.topLeft(), strip.topRight());
    for (int i = 0; i < m_stops.size(); i++) {
        if (m_removing && i == m_dragged) continue;
        fill.setColorAt(m_stops[i].position, m_stops[i].color.toQColor());
    }
    painter.fillRect(strip, fill);
    painter.restore();
    QColor edge = palette().color(QPalette::WindowText);
    edge.setAlphaF(0.15);
    painter.setPen(QPen(edge, 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(shape);

    // the stops along its lower edge, each its own color in a ring
    for (int i = 0; i < m_stops.size(); i++) {
        const QPointF at(xOf(m_stops[i].position), strip.bottom());
        painter.setOpacity(m_removing && i == m_dragged ? 0.3 : 1.0);
        painter.setPen(QPen(QColor(0, 0, 0, 120), 1));
        painter.setBrush(Qt::white);
        painter.drawEllipse(at, stopRadius + 1, stopRadius + 1);
        painter.setPen(Qt::NoPen);
        QColor color = m_stops[i].color.toQColor();
        color.setAlpha(255);
        painter.setBrush(color);
        painter.drawEllipse(at, stopRadius - 1, stopRadius - 1);
        if (i == m_hovered && m_dragged < 0) {
            QColor ring = palette().color(QPalette::WindowText);
            ring.setAlphaF(0.45);
            painter.setPen(QPen(ring, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(at, stopRadius + 3.5, stopRadius + 3.5);
        }
    }
}

void KisStudioGradientStrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    m_moved = false;
    m_removing = false;
    m_dragged = stopAt(event->pos());
    if (m_dragged < 0) {
        // a new stop where it was clicked, the color the strip has there
        const qreal position = positionAt(event->pos().x());
        m_stops.append(KoGradientStop(position, colorAt(position)));
        m_dragged = m_stops.size() - 1;
        sortStops();
        emitChanged();
    }
}

void KisStudioGradientStrip::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragged < 0) {
        const int hovered = stopAt(event->pos());
        if (hovered != m_hovered) {
            m_hovered = hovered;
            update();
        }
        return;
    }
    m_moved = true;
    const bool off = qAbs(event->pos().y() - stripRect().center().y()) > removeDistance && m_stops.size() > 2;
    m_removing = off;
    m_stops[m_dragged].position = positionAt(event->pos().x());
    sortStops();
    emitChanged();
}

void KisStudioGradientStrip::mouseReleaseEvent(QMouseEvent *)
{
    if (m_dragged < 0) return;
    if (m_removing) m_stops.removeAt(m_dragged);
    m_dragged = -1;
    m_removing = false;
    emitChanged();
}

void KisStudioGradientStrip::leaveEvent(QEvent *)
{
    m_hovered = -1;
    update();
}

void KisStudioGradientStrip::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int stop = stopAt(event->pos());
    if (stop < 0 || !currentColor) return;
    m_stops[stop].color = currentColor();
    emitChanged();
}

void KisStudioGradientStrip::dragEnterEvent(QDragEnterEvent *event)
{
    if (KisColorDrop::carriesColor(event->mimeData())) event->acceptProposedAction();
}

void KisStudioGradientStrip::dragMoveEvent(QDragMoveEvent *event)
{
    if (KisColorDrop::carriesColor(event->mimeData())) event->acceptProposedAction();
}

void KisStudioGradientStrip::dropEvent(QDropEvent *event)
{
    KoColor color;
    if (!KisColorDrop::colorOf(event->mimeData(), &color)) return;
    event->acceptProposedAction();
    const int stop = stopAt(event->pos());
    if (stop >= 0) {
        m_stops[stop].color = color;
    } else {
        m_stops.append(KoGradientStop(positionAt(event->pos().x()), color));
        sortStops();
    }
    emitChanged();
}
