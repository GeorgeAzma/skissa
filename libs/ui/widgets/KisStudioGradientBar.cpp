/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioGradientBar.h"

#include <QDragEnterEvent>
#include <QImage>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

#include <KoColor.h>
#include <KoSegmentGradient.h>
#include <KoStopGradient.h>

#include "KisColorDrop.h"
#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "KisModernStyle.h"
#include "KisStudioGradientDrag.h"

namespace
{
constexpr int barHeight = 22;
constexpr int handle = 16;
constexpr int histogramHeight = 18;
} // namespace

KisStudioGradientBar::KisStudioGradientBar(QWidget *parent)
    : QWidget(parent)
{
    setMinimumHeight(barHeight + handle + KisModernStyle::space1);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMouseTracking(true);
    setAcceptDrops(true);
}

void KisStudioGradientBar::setStops(const Stops &stops)
{
    m_stops = stops;
    m_selected = qBound(0, m_selected, int(m_stops.size()) - 1);
    update();
}

void KisStudioGradientBar::setSelected(int index)
{
    m_selected = qBound(0, index, int(m_stops.size()) - 1);
    update();
}

void KisStudioGradientBar::setHistogram(const QVector<float> &values)
{
    m_histogram = values;
    setMinimumHeight(barHeight + handle + KisModernStyle::space1 + (m_histogram.isEmpty() ? 0 : histogramHeight));
    updateGeometry();
    update();
}

void KisStudioGradientBar::setMarker(qreal at)
{
    if (at == m_marker) return;
    m_marker = at;
    update();
}

QColor KisStudioGradientBar::blended(const Stops &stops, qreal at)
{
    if (stops.isEmpty()) return QColor();
    if (at <= stops.first().first) return stops.first().second;
    for (int i = 1; i < stops.size(); i++) {
        if (at <= stops[i].first) {
            const qreal t = (at - stops[i - 1].first) / std::max(1e-6, stops[i].first - stops[i - 1].first);
            return KisModernStyle::mix(stops[i - 1].second, stops[i].second, t);
        }
    }
    return stops.last().second;
}

QColor KisStudioGradientBar::shownAt(qreal at) const
{
    return colorAt ? colorAt(m_stops, at) : blended(m_stops, at);
}

QSize KisStudioGradientBar::sizeHint() const
{
    return QSize(200, minimumHeight());
}

QRectF KisStudioGradientBar::barRect() const
{
    return QRectF(handle / 2.0, m_histogram.isEmpty() ? 0 : histogramHeight, width() - handle, barHeight);
}

QPointF KisStudioGradientBar::handleAt(int index) const
{
    const QRectF bar = barRect();
    return QPointF(bar.left() + m_stops[index].first * bar.width(), bar.bottom() + handle / 2.0 + KisModernStyle::hairline);
}

qreal KisStudioGradientBar::positionOf(int x) const
{
    const QRectF bar = barRect();
    return qBound(0.0, (x - bar.left()) / bar.width(), 1.0);
}

int KisStudioGradientBar::stopAt(const QPoint &pos) const
{
    int found = -1;
    qreal nearest = handle;
    for (int i = 0; i < m_stops.size(); i++) {
        const qreal distance = std::abs(pos.x() - handleAt(i).x());
        if (distance < nearest && pos.y() >= barRect().bottom() - KisModernStyle::space1) {
            nearest = distance;
            found = i;
        }
    }
    return found;
}

void KisStudioGradientBar::sortKeeping(int index)
{
    const QPair<qreal, QColor> moved = m_stops[index];
    std::stable_sort(m_stops.begin(), m_stops.end(), [](const QPair<qreal, QColor> &a, const QPair<qreal, QColor> &b) { return a.first < b.first; });
    for (int i = 0; i < m_stops.size(); i++) {
        if (m_stops[i] == moved) m_selected = i;
    }
}

void KisStudioGradientBar::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bar = barRect();
    const QColor ink = palette().color(QPalette::WindowText);
    if (!m_histogram.isEmpty()) {
        // how much of the picture is at each place, as a soft hill over the bar
        QPainterPath hill;
        hill.moveTo(bar.left(), bar.top());
        for (int i = 0; i < m_histogram.size(); i++) {
            const qreal x = bar.left() + (i + 0.5) / m_histogram.size() * bar.width();
            hill.lineTo(x, bar.top() - KisModernStyle::hairline - m_histogram[i] * (histogramHeight - KisModernStyle::space1));
        }
        hill.lineTo(bar.right(), bar.top());
        hill.closeSubpath();
        QColor fill = ink;
        fill.setAlphaF(0.16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(hill);
    }
    if (!m_stops.isEmpty()) {
        const qreal dpr = devicePixelRatioF();
        QImage strip(std::max(1, qRound(bar.width() * dpr)), 1, QImage::Format_ARGB32_Premultiplied);
        for (int x = 0; x < strip.width(); x++) strip.setPixelColor(x, 0, shownAt((x + 0.5) / strip.width()));
        painter.save();
        painter.setClipPath(KisModernStyle::squircle(bar, KisModernStyle::cornerRadius));
        painter.drawImage(bar, strip);
        painter.restore();
    }
    if (m_marker >= 0) {
        const qreal x = bar.left() + m_marker * bar.width();
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3));
        painter.drawLine(QPointF(x, bar.top() + 2), QPointF(x, bar.bottom() - 2));
        painter.setPen(QPen(Qt::white, 1.5));
        painter.drawLine(QPointF(x, bar.top() + 2), QPointF(x, bar.bottom() - 2));
    }
    for (int i = 0; i < m_stops.size(); i++) {
        const QPointF at = handleAt(i);
        const qreal radius = handle / 2.0;
        painter.setOpacity(m_removing && i == m_selected ? 0.3 : 1.0);
        // white with a dark ring, so a stop shows on any color
        painter.setPen(QPen(QColor(0, 0, 0, 160), 1.5));
        painter.setBrush(Qt::white);
        painter.drawEllipse(at, radius, radius);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_stops[i].second);
        painter.drawEllipse(at, radius - 3, radius - 3);
        if (i == m_selected || i == m_hovered) {
            QColor ring = ink;
            if (i != m_selected) ring.setAlphaF(0.45);
            painter.setPen(QPen(ring, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(at, radius + 2.5, radius + 2.5);
        }
    }
    painter.setOpacity(1.0);
}

void KisStudioGradientBar::removeStop(int index)
{
    m_stops.removeAt(index);
    m_selected = qBound(0, m_selected - (index <= m_selected ? 1 : 0), int(m_stops.size()) - 1);
    update();
    Q_EMIT stopsChanged();
    Q_EMIT selectedChanged();
}

void KisStudioGradientBar::mousePressEvent(QMouseEvent *event)
{
    const int index = stopAt(event->pos());
    if (event->button() == Qt::RightButton) {
        if (index >= 0 && m_stops.size() > 2) removeStop(index);
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    if (index >= 0) {
        m_selected = index;
    } else {
        const qreal at = positionOf(event->pos().x());
        m_stops << qMakePair(at, shownAt(at));
        sortKeeping(m_stops.size() - 1);
        Q_EMIT stopsChanged();
    }
    m_dragging = true;
    m_moved = false;
    m_removing = false;
    m_pressX = event->pos().x();
    update();
    Q_EMIT selectedChanged();
}

void KisStudioGradientBar::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging || !(event->buttons() & Qt::LeftButton)) {
        m_dragging = false;
        const int hovered = stopAt(event->pos());
        setCursor(hovered >= 0 ? Qt::SizeHorCursor : Qt::ArrowCursor);
        if (hovered != m_hovered) {
            m_hovered = hovered;
            update();
        }
        return;
    }
    if (!m_moved) {
        if (qAbs(event->pos().x() - m_pressX) < KisModernStyle::dragDistance(event)) return;
        // from here, without a jump by the distance it took to start
        m_moved = true;
        m_grab = event->pos().x() - handleAt(m_selected).x();
    }
    // pulled well away from the bar, the stop shows it will go (a gradient keeps two)
    const bool off = m_stops.size() > 2 && !rect().adjusted(0, -handle, 0, handle).contains(event->pos());
    if (off != m_removing) {
        m_removing = off;
        update();
    }
    if (off) return;
    m_stops[m_selected].first = positionOf(qRound(event->pos().x() - m_grab));
    sortKeeping(m_selected);
    update();
    Q_EMIT stopsChanged();
}

void KisStudioGradientBar::mouseReleaseEvent(QMouseEvent *)
{
    m_dragging = false;
    if (m_removing) {
        m_removing = false;
        removeStop(m_selected);
    }
}

void KisStudioGradientBar::leaveEvent(QEvent *)
{
    m_hovered = -1;
    update();
}

void KisStudioGradientBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int index = stopAt(event->pos());
    if (index < 0) return;
    QColor color;
    if (currentColor) {
        color = currentColor();
    } else if (KisMainWindow *window = KisPart::instance()->currentMainwindow()) {
        if (window->viewManager() && window->viewManager()->canvasResourceProvider()) {
            color = window->viewManager()->canvasResourceProvider()->fgColor().toQColor();
        }
    }
    if (!color.isValid()) return;
    m_stops[index].second = color;
    m_selected = index;
    update();
    Q_EMIT stopsChanged();
    Q_EMIT selectedChanged();
}

void KisStudioGradientBar::dragEnterEvent(QDragEnterEvent *event)
{
    if (KisColorDrop::carriesColor(event->mimeData()) || KisStudioGradientDrag::carries(event->mimeData())) event->acceptProposedAction();
}

void KisStudioGradientBar::dragMoveEvent(QDragMoveEvent *event)
{
    if (KisColorDrop::carriesColor(event->mimeData()) || KisStudioGradientDrag::carries(event->mimeData())) event->acceptProposedAction();
}

void KisStudioGradientBar::dropEvent(QDropEvent *event)
{
    if (KoAbstractGradientSP gradient = KisStudioGradientDrag::gradientOf(event->mimeData())) {
        // its own stops where it has them, else enough of it to follow its curve
        Stops stops;
        if (auto stopGradient = gradient.dynamicCast<KoStopGradient>()) {
            for (const KoGradientStop &stop : stopGradient->stops()) stops << qMakePair(stop.position, stop.color.toQColor());
        } else {
            KoColor color;
            for (int i = 0; i <= 8; i++) {
                gradient->colorAt(color, i / 8.0);
                stops << qMakePair(i / 8.0, color.toQColor());
            }
        }
        if (stops.size() < 2) return;
        event->acceptProposedAction();
        m_stops = stops;
        m_selected = 0;
        update();
        Q_EMIT stopsChanged();
        Q_EMIT selectedChanged();
        return;
    }
    KoColor color;
    if (!KisColorDrop::colorOf(event->mimeData(), &color)) return;
    event->acceptProposedAction();
    const QPoint pos = event->pos();
    const int index = stopAt(pos);
    if (index >= 0) {
        m_stops[index].second = color.toQColor();
        m_selected = index;
    } else {
        const qreal at = positionOf(pos.x());
        m_stops << qMakePair(at, color.toQColor());
        sortKeeping(m_stops.size() - 1);
    }
    update();
    Q_EMIT stopsChanged();
    Q_EMIT selectedChanged();
}
