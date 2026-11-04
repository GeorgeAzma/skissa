/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCurve.h"

#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>

#include <KisModernStyle.h>

namespace
{
constexpr int stripSize = 6;
}

QString KisStudioCurve::toString() const
{
    QString text;
    Q_FOREACH (const QPointF &point, points) {
        text += QString::number(point.x(), 'g', 5) + QLatin1Char(',') + QString::number(point.y(), 'g', 5) + QLatin1Char(';');
    }
    return text;
}

KisStudioCurve KisStudioCurve::fromString(const QString &text, const KisStudioCurve &fallback)
{
    KisStudioCurve curve;
    Q_FOREACH (const QString &pair, text.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList xy = pair.split(QLatin1Char(','));
        bool okX = false, okY = false;
        const qreal x = xy.value(0).toDouble(&okX);
        const qreal y = xy.value(1).toDouble(&okY);
        if (xy.size() == 2 && okX && okY) curve.points << QPointF(qBound(0.0, x, 1.0), qBound(0.0, y, 1.0));
    }
    if (curve.points.size() < 2) return fallback;
    std::stable_sort(curve.points.begin(), curve.points.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    return curve;
}

bool KisStudioCurve::operator==(const KisStudioCurve &other) const
{
    if (points.size() != other.points.size()) return false;
    for (int i = 0; i < points.size(); i++) {
        if (std::abs(points[i].x() - other.points[i].x()) > 1e-4 || std::abs(points[i].y() - other.points[i].y()) > 1e-4) return false;
    }
    return true;
}

QVector<float> KisStudioCurve::sampled(int count, bool wraps, bool rises) const
{
    QVector<QPointF> points = this->points;
    QVector<float> values(count);
    if (points.isEmpty()) {
        for (int k = 0; k < count; k++) values[k] = float(k) / qMax(1, count - 1);
        return values;
    }
    if (wraps) {
        // a turn before and after, so that the curve meets itself at the ends
        QVector<QPointF> around;
        for (int turn = -1; turn <= 1; turn++) {
            Q_FOREACH (const QPointF &point, points) around << QPointF(point.x() + turn, point.y() + (rises ? turn : 0));
        }
        points = around;
    }
    const int n = points.size();
    if (n == 1) {
        std::fill(values.begin(), values.end(), float(points.first().y()));
        return values;
    }
    // a monotone cubic (Fritsch and Carlson): through every point without overshooting between them
    QVector<double> slope(n - 1), tangent(n);
    for (int i = 0; i < n - 1; i++) {
        const double dx = std::max(points[i + 1].x() - points[i].x(), 1e-6);
        slope[i] = (points[i + 1].y() - points[i].y()) / dx;
    }
    tangent[0] = slope[0];
    tangent[n - 1] = slope[n - 2];
    for (int i = 1; i < n - 1; i++) tangent[i] = slope[i - 1] * slope[i] <= 0 ? 0 : (slope[i - 1] + slope[i]) / 2;
    for (int i = 0; i < n - 1; i++) {
        if (slope[i] == 0) {
            tangent[i] = tangent[i + 1] = 0;
            continue;
        }
        const double a = tangent[i] / slope[i];
        const double b = tangent[i + 1] / slope[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            tangent[i] = t * a * slope[i];
            tangent[i + 1] = t * b * slope[i];
        }
    }
    int segment = 0;
    for (int k = 0; k < count; k++) {
        const double x = double(k) / qMax(1, count - 1);
        double y;
        if (x <= points.first().x()) {
            y = points.first().y();
        } else if (x >= points.last().x()) {
            y = points.last().y();
        } else {
            while (segment < n - 2 && x > points[segment + 1].x()) segment++;
            const QPointF &p0 = points[segment];
            const QPointF &p1 = points[segment + 1];
            const double h = std::max(p1.x() - p0.x(), 1e-6);
            const double t = (x - p0.x()) / h;
            const double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * p0.y() + (t3 - 2 * t2 + t) * h * tangent[segment] + (-2 * t3 + 3 * t2) * p1.y()
                + (t3 - t2) * h * tangent[segment + 1];
        }
        values[k] = float(rises ? y : std::clamp(y, 0.0, 1.0));
    }
    return values;
}

KisStudioCurveView::KisStudioCurveView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void KisStudioCurveView::setCurve(const KisStudioCurve &curve, const KisStudioCurve &fallback, bool wraps, bool rises)
{
    const bool same = !m_values.isEmpty() && wraps == m_wraps && rises == m_rises;
    if (same && curve == m_curve && fallback == m_fallback) return;
    const bool glide = m_glideNext && same && curve != m_curve;
    m_glideNext = false;
    QVector<float> from;
    if (glide) {
        for (int i = 0; i < m_values.size(); i++) from << shownValue(i);
    }
    m_curve = curve;
    m_fallback = fallback;
    m_wraps = wraps;
    m_rises = rises;
    m_values = m_curve.sampled(256, wraps, rises);
    if (!glide) {
        stopGlide();
        update();
        return;
    }
    m_fromValues = from;
    if (!m_glider) {
        m_glider = new QVariantAnimation(this);
        m_glider->setStartValue(0.0);
        m_glider->setEndValue(1.0);
        m_glider->setDuration(KisModernStyle::changeDuration);
        m_glider->setEasingCurve(KisModernStyle::motionCurve());
        connect(m_glider, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_glide = value.toReal();
            update();
        });
    }
    m_glider->stop();
    m_glide = 0;
    m_glider->start();
}

void KisStudioCurveView::glideNextChange()
{
    m_glideNext = true;
}

float KisStudioCurveView::shownValue(int i) const
{
    if (m_glide >= 1 || m_fromValues.size() != m_values.size()) return m_values[i];
    return float(m_fromValues[i] + (m_values[i] - m_fromValues[i]) * m_glide);
}

void KisStudioCurveView::stopGlide()
{
    if (m_glider) m_glider->stop();
    m_glide = 1;
}

void KisStudioCurveView::setHistogram(const QVector<float> &histogram)
{
    m_histogram = histogram;
    update();
}

void KisStudioCurveView::setMarker(qreal x)
{
    if (x == m_marker) return;
    m_marker = x;
    update();
}

qreal KisStudioCurveView::valueAt(qreal x) const
{
    return m_values.isEmpty() ? x : m_values[qBound(0, qRound(x * (m_values.size() - 1)), m_values.size() - 1)];
}

int KisStudioCurveView::pointAt(qreal x, qreal hold)
{
    int nearest = -1;
    for (int i = 0; i < m_curve.points.size(); i++) {
        const qreal distance = std::abs(m_curve.points[i].x() - x);
        if (distance < 0.025 && (nearest < 0 || distance < std::abs(m_curve.points[nearest].x() - x))) nearest = i;
    }
    if (nearest >= 0) return nearest;
    if (hold < 0) hold = m_wraps ? 1.0 / 12 : 0;
    if (hold > 0) {
        // the curve held where it is a little either way, unless a point is near already
        const qreal reach = hold;
        for (qreal side : {-reach, reach}) {
            const qreal at = x + side;
            bool held = false;
            Q_FOREACH (const QPointF &point, m_curve.points) {
                if (std::abs(point.x() - at) < reach * 0.8 || (side < 0 ? point.x() > at && point.x() < x : point.x() < at && point.x() > x)) {
                    held = true;
                }
            }
            if (!held && at > 0.01 && at < 0.99) insert(QPointF(at, qBound(0.0, valueAt(at), 1.0)));
        }
    }
    return insert(QPointF(x, qBound(0.0, valueAt(x), 1.0)));
}

void KisStudioCurveView::setPointY(int index, qreal y)
{
    if (index < 0 || index >= m_curve.points.size()) return;
    m_curve.points[index].setY(qBound(0.0, y, 1.0));
    tieEnds(index);
    commit();
}

void KisStudioCurveView::setShownPoint(int index)
{
    m_shown = index;
    update();
}

QSize KisStudioCurveView::sizeHint() const
{
    return QSize(360, 170);
}

int KisStudioCurveView::heightForWidth(int width) const
{
    return qBound(130, int(width * 0.4), 170);
}

void KisStudioCurveView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = palette().color(QPalette::WindowText);
    auto inked = [&ink](qreal alpha) {
        QColor color = ink;
        color.setAlphaF(alpha);
        return color;
    };
    const QRectF box = boxRect();

    painter.setPen(Qt::NoPen);
    painter.setBrush(inked(0.05));
    painter.drawPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius));

    painter.save();
    painter.setClipPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius));
    if (!m_histogram.isEmpty()) {
        QPainterPath shape;
        shape.moveTo(box.bottomLeft());
        for (int i = 0; i < m_histogram.size(); i++) {
            const qreal x = box.left() + box.width() * (i + 0.5) / m_histogram.size();
            shape.lineTo(x, box.bottom() - m_histogram[i] * box.height() * 0.85);
        }
        shape.lineTo(box.bottomRight());
        shape.closeSubpath();
        painter.setBrush(inked(0.09));
        painter.drawPath(shape);
    }
    if (backdrop) {
        painter.save();
        backdrop(painter, box);
        painter.restore();
    }
    painter.setPen(QPen(inked(0.07), 1));
    for (int i = 1; i < 4; i++) {
        const qreal x = box.left() + box.width() * i / 4;
        const qreal y = box.top() + box.height() * i / 4;
        painter.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
        painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
    }
    const QVector<float> fallback = m_fallback.sampled(64, m_wraps, m_rises);
    QPolygonF resting;
    for (int i = 0; i < fallback.size(); i++) resting << toScreen(QPointF(qreal(i) / (fallback.size() - 1), fallback[i]));
    painter.setPen(QPen(inked(0.2), 1));
    painter.drawPolyline(resting);

    QPainterPath line;
    for (int i = 0; i < m_values.size(); i++) {
        const QPointF at = toScreen(QPointF(qreal(i) / (m_values.size() - 1), shownValue(i)));
        if (i == 0) line.moveTo(at);
        else line.lineTo(at);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(inked(0.9), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(line);

    if (m_marker >= 0) {
        const QPointF on = toScreen(QPointF(m_marker, valueAt(m_marker)));
        painter.setPen(QPen(inked(0.4), 1));
        painter.drawLine(QPointF(on.x(), box.top()), QPointF(on.x(), box.bottom()));
        painter.setPen(Qt::NoPen);
        painter.setBrush(inked(0.9));
        painter.drawEllipse(on, 3, 3);
    }
    painter.restore();

    const int active = m_dragged >= 0 ? m_dragged : m_shown;
    for (int i = 0; i < m_curve.points.size(); i++) {
        QPointF point = m_curve.points[i];
        if (m_glide < 1 && m_fromValues.size() == m_values.size()) {
            // from where the curve it comes from was at its place
            const qreal from = m_fromValues[qBound(0, qRound(point.x() * (m_fromValues.size() - 1)), m_fromValues.size() - 1)];
            point.setY(from + (point.y() - from) * m_glide);
        }
        const QPointF at = toScreen(point);
        const qreal radius = i == active || i == m_hovered ? 5.5 : 4.5;
        painter.setOpacity(i == m_dragged && m_leaving ? 0.35 : 1.0);
        // white with a dark ring, seen on any background
        painter.setPen(QPen(QColor(0, 0, 0, 170), 1.5));
        painter.setBrush(Qt::white);
        painter.drawEllipse(at, radius, radius);
    }
    painter.setOpacity(1);

    if (active >= 0 && active < m_curve.points.size() && describe) {
        const QPointF point = m_curve.points[active];
        painter.setPen(KisModernStyle::secondaryInk(ink));
        painter.drawText(box.adjusted(KisModernStyle::space2, KisModernStyle::space1, -KisModernStyle::space2, 0),
                         Qt::AlignLeft | Qt::AlignTop, describe(point.x(), point.y()));
    }

    // what each axis is, as a strip of its colors
    auto strip = [&painter](const QRectF &rect, bool across, const std::function<QColor(qreal)> &color) {
        if (!color) return;
        QLinearGradient gradient(rect.topLeft(), across ? rect.topRight() : rect.bottomLeft());
        for (int i = 0; i <= 24; i++) {
            const qreal t = i / 24.0;
            gradient.setColorAt(t, color(across ? t : 1 - t));
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(gradient);
        painter.drawPath(KisModernStyle::squircle(rect, rect.height() < rect.width() ? rect.height() / 2 : rect.width() / 2));
    };
    strip(QRectF(box.left(), box.bottom() + KisModernStyle::space1, box.width(), stripSize), true, xColor);
    strip(QRectF(0, box.top(), stripSize, box.height()), false, yColor);
}

void KisStudioCurveView::mousePressEvent(QMouseEvent *event)
{
    stopGlide();
    const int near = pointNear(event->pos());
    if (event->button() == Qt::RightButton) {
        // a point goes; elsewhere, the curve resets
        if (near >= 0 && removable(near)) {
            m_curve.points.removeAt(near);
        } else {
            m_curve = m_fallback;
        }
        m_hovered = -1;
        commit();
        return;
    }
    if (event->button() != Qt::LeftButton) return;
    m_dragged = near >= 0 ? near : insert(QPointF(qBound(0.0, fromScreen(event->pos()).x(), 1.0),
                                                   qBound(0.0, valueAt(fromScreen(event->pos()).x()), 1.0)));
    m_grab = event->pos();
    m_start = m_curve.points[m_dragged];
    m_fine = event->modifiers() & Qt::ShiftModifier;
    m_leaving = false;
    commit();
}

void KisStudioCurveView::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragged < 0 || !(event->buttons() & Qt::LeftButton)) {
        if (m_dragged >= 0) finishDrag();
        const int near = pointNear(event->pos());
        if (near != m_hovered) {
            m_hovered = near;
            update();
        }
        return;
    }
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    if (fine != m_fine) {
        // on from here at the new speed
        m_fine = fine;
        m_grab = event->pos();
        m_start = m_curve.points[m_dragged];
    }
    const QRectF box = boxRect();
    const QPointF moved = QPointF(event->pos() - m_grab) * (fine ? 0.25 : 1.0);
    QPointF point(m_start.x() + moved.x() / box.width(), m_start.y() - moved.y() / box.height());
    const int last = m_curve.points.size() - 1;
    const bool end = m_dragged == 0 || m_dragged == last;
    if (m_wraps && end) {
        point.setX(m_dragged == 0 ? 0.0 : 1.0);
    } else {
        const qreal low = m_dragged > 0 ? m_curve.points[m_dragged - 1].x() + 0.01 : 0.0;
        const qreal high = m_dragged < last ? m_curve.points[m_dragged + 1].x() - 0.01 : 1.0;
        point.setX(qBound(low, point.x(), qMax(low, high)));
    }
    point.setY(qBound(0.0, point.y(), 1.0));
    m_curve.points[m_dragged] = point;
    tieEnds(m_dragged);
    // pulled far off the box, the point will go
    const qreal off = qMax(box.top() - event->pos().y(), event->pos().y() - box.bottom());
    m_leaving = removable(m_dragged) && off > 32;
    commit();
}

void KisStudioCurveView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dragged >= 0) finishDrag();
}

void KisStudioCurveView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int near = pointNear(event->pos());
    if (event->button() == Qt::LeftButton && near >= 0 && removable(near)) {
        m_curve.points.removeAt(near);
        m_dragged = -1;
        m_hovered = -1;
        commit();
    }
}

void KisStudioCurveView::leaveEvent(QEvent *)
{
    m_hovered = -1;
    update();
}

QRectF KisStudioCurveView::boxRect() const
{
    const qreal left = yColor ? stripSize + KisModernStyle::space1 : 0;
    const qreal under = xColor ? stripSize + KisModernStyle::space1 : 0;
    return QRectF(left, 0, width() - left, height() - under);
}

QPointF KisStudioCurveView::toScreen(const QPointF &value) const
{
    const QRectF box = boxRect();
    return QPointF(box.left() + value.x() * box.width(), box.bottom() - value.y() * box.height());
}

QPointF KisStudioCurveView::fromScreen(const QPointF &pos) const
{
    const QRectF box = boxRect();
    return QPointF((pos.x() - box.left()) / box.width(), (box.bottom() - pos.y()) / box.height());
}

int KisStudioCurveView::pointNear(const QPoint &pos) const
{
    int best = -1;
    qreal bestDistance = 10;
    for (int i = 0; i < m_curve.points.size(); i++) {
        const qreal distance = QLineF(toScreen(m_curve.points[i]), pos).length();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

bool KisStudioCurveView::removable(int index) const
{
    // a curve keeps two points; a curve that wraps keeps its ends, which are one point
    if (m_curve.points.size() <= 2) return false;
    return !(m_wraps && (index == 0 || index == m_curve.points.size() - 1));
}

int KisStudioCurveView::insert(const QPointF &point)
{
    int at = 0;
    while (at < m_curve.points.size() && m_curve.points[at].x() < point.x()) at++;
    m_curve.points.insert(at, point);
    return at;
}

void KisStudioCurveView::tieEnds(int index)
{
    // the two ends of a curve that wraps are the same place
    if (!m_wraps || m_curve.points.size() < 2) return;
    const int last = m_curve.points.size() - 1;
    if (index == 0 && m_curve.points[last].x() >= 0.999) m_curve.points[last].setY(m_curve.points[0].y());
    if (index == last && m_curve.points[0].x() <= 0.001) m_curve.points[0].setY(m_curve.points[last].y());
}

void KisStudioCurveView::finishDrag()
{
    if (m_leaving && m_dragged >= 0) m_curve.points.removeAt(m_dragged);
    m_dragged = -1;
    m_leaving = false;
    commit();
}

void KisStudioCurveView::commit()
{
    m_values = m_curve.sampled(256, m_wraps, m_rises);
    update();
    if (changed) changed(m_curve);
}
