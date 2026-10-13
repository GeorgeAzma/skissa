/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisStrokeShape.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>

#include <QtMath>

namespace
{

qreal distance(const QPointF &a, const QPointF &b)
{
    return std::hypot(a.x() - b.x(), a.y() - b.y());
}

qreal segmentDistance(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF d = b - a;
    const qreal l2 = d.x() * d.x() + d.y() * d.y();
    if (l2 <= 0) return distance(p, a);
    const qreal t = qBound<qreal>(0, QPointF::dotProduct(p - a, d) / l2, 1);
    return distance(p, a + t * d);
}

qreal pathLength(const QVector<QPointF> &points)
{
    qreal length = 0;
    for (int i = 1; i < points.size(); i++) length += distance(points[i - 1], points[i]);
    return length;
}

/// The corners of \p points within \p tolerance (Douglas–Peucker), first and last included
QVector<QPointF> corners(const QVector<QPointF> &points, int first, int last, qreal tolerance)
{
    if (last - first < 2) return {points[first], points[last]};
    int far = first;
    qreal farthest = -1;
    for (int i = first + 1; i < last; i++) {
        const qreal d = segmentDistance(points[i], points[first], points[last]);
        if (d > farthest) {
            farthest = d;
            far = i;
        }
    }
    if (farthest <= tolerance) return {points[first], points[last]};
    QVector<QPointF> left = corners(points, first, far, tolerance);
    left.removeLast();
    return left + corners(points, far, last, tolerance);
}

qreal cornerDegrees(const QPointF &a, const QPointF &b, const QPointF &c)
{
    const QPointF u = a - b, v = c - b;
    const qreal norms = std::hypot(u.x(), u.y()) * std::hypot(v.x(), v.y());
    if (norms <= 0) return 180;
    return qRadiansToDegrees(std::acos(qBound<qreal>(-1, QPointF::dotProduct(u, v) / norms, 1)));
}

/// How far any point lies from the nearest side of \p shape
qreal worstFit(const QVector<QPointF> &points, const QVector<QPointF> &shape, bool closed)
{
    qreal worst = 0;
    const int sides = closed ? shape.size() : shape.size() - 1;
    for (const QPointF &p : points) {
        qreal nearest = std::numeric_limits<qreal>::max();
        for (int i = 0; i < sides; i++) nearest = qMin(nearest, segmentDistance(p, shape[i], shape[(i + 1) % shape.size()]));
        worst = qMax(worst, nearest);
    }
    return worst;
}

/// The total turn of \p points around \p center, signed (radians)
qreal turnAround(const QVector<QPointF> &points, const QPointF &center)
{
    qreal turn = 0;
    qreal previous = std::atan2(points[0].y() - center.y(), points[0].x() - center.x());
    for (int i = 1; i < points.size(); i++) {
        const qreal angle = std::atan2(points[i].y() - center.y(), points[i].x() - center.x());
        qreal step = angle - previous;
        while (step > M_PI) step -= 2 * M_PI;
        while (step < -M_PI) step += 2 * M_PI;
        turn += step;
        previous = angle;
    }
    return turn;
}

/// Points along the polyline through \p vertices, at most \p step apart
QVector<QPointF> sampled(const QVector<QPointF> &vertices, qreal step)
{
    QVector<QPointF> out;
    for (int i = 1; i < vertices.size(); i++) {
        const int n = qMax(1, int(std::ceil(distance(vertices[i - 1], vertices[i]) / step)));
        for (int k = 0; k < n; k++) out << vertices[i - 1] + (vertices[i] - vertices[i - 1]) * (qreal(k) / n);
    }
    out << vertices.last();
    return out;
}

/// Four corners that are nearly square made square, in the same order
QVector<QPointF> squared(const QVector<QPointF> &quad)
{
    QPointF center;
    for (const QPointF &c : quad) center += c / 4.0;
    // the sides' common direction, from their angles taken four times over (so 0° and 90° agree)
    std::complex<qreal> sum;
    for (int i = 0; i < 4; i++) {
        const QPointF d = quad[(i + 1) % 4] - quad[i];
        sum += std::polar<qreal>(1, 4 * std::atan2(d.y(), d.x()));
    }
    const qreal angle = std::arg(sum) / 4;
    const QPointF u(std::cos(angle), std::sin(angle)), v(-u.y(), u.x());
    qreal width = 0, height = 0;
    for (const QPointF &c : quad) {
        width += std::abs(QPointF::dotProduct(c - center, u)) / 4;
        height += std::abs(QPointF::dotProduct(c - center, v)) / 4;
    }
    QVector<QPointF> out;
    for (const QPointF &c : quad) {
        const qreal su = QPointF::dotProduct(c - center, u) < 0 ? -1 : 1;
        const qreal sv = QPointF::dotProduct(c - center, v) < 0 ? -1 : 1;
        out << center + su * width * u + sv * height * v;
    }
    return out;
}

} // namespace

KisStrokeShape KisStrokeShape::recognize(const QVector<QPointF> &input, qreal pixel)
{
    KisStrokeShape shape;
    // the hold at the end piles points up; keep ones that moved
    QVector<QPointF> points;
    for (const QPointF &p : input) {
        if (points.isEmpty() || distance(p, points.last()) >= 0.75 * pixel) points << p;
    }
    if (points.size() < 5) return shape;
    const qreal length = pathLength(points);
    if (length < 30 * pixel) return shape;
    const QPointF start = points.first(), end = points.last();
    const qreal chord = distance(start, end);
    const qreal step = 3 * pixel;
    QVector<QPointF> vertices;

    // a line: near the chord all along, going one way along it
    qreal deviation = 0;
    for (const QPointF &p : points) deviation = qMax(deviation, segmentDistance(p, start, end));
    if (chord > 20 * pixel && deviation < qMax(4 * pixel, 0.05 * chord)) {
        const QPointF direction = (end - start) / chord;
        qreal furthest = 0, back = 0;
        for (const QPointF &p : points) {
            const qreal at = QPointF::dotProduct(p - start, direction);
            furthest = qMax(furthest, at);
            back = qMax(back, furthest - at);
        }
        if (back < qMax(6 * pixel, 0.08 * chord)) {
            shape.kind = Line;
            vertices = {start, end};
        }
    }

    const bool closed = chord < qMax(0.14 * length, 12 * pixel);
    if (!shape.kind && closed) {
        // an ellipse: the stroke's main axes, and its reach along each
        QPointF mean;
        for (const QPointF &p : points) mean += p / points.size();
        qreal sxx = 0, syy = 0, sxy = 0;
        for (const QPointF &p : points) {
            const QPointF d = p - mean;
            sxx += d.x() * d.x();
            syy += d.y() * d.y();
            sxy += d.x() * d.y();
        }
        const qreal angle = 0.5 * std::atan2(2 * sxy, sxx - syy);
        const QPointF u(std::cos(angle), std::sin(angle)), v(-u.y(), u.x());
        qreal minU = 1e18, maxU = -1e18, minV = 1e18, maxV = -1e18;
        for (const QPointF &p : points) {
            const qreal pu = QPointF::dotProduct(p - mean, u), pv = QPointF::dotProduct(p - mean, v);
            minU = qMin(minU, pu);
            maxU = qMax(maxU, pu);
            minV = qMin(minV, pv);
            maxV = qMax(maxV, pv);
        }
        qreal a = (maxU - minU) / 2, b = (maxV - minV) / 2;
        const QPointF center = mean + (maxU + minU) / 2 * u + (maxV + minV) / 2 * v;
        if (a > 3 * pixel && b > 3 * pixel) {
            qreal average = 0, worst = 0;
            QVector<qreal> angles;
            for (const QPointF &p : points) {
                const qreal pu = QPointF::dotProduct(p - center, u) / a, pv = QPointF::dotProduct(p - center, v) / b;
                const qreal error = std::abs(std::hypot(pu, pv) - 1);
                average += error / points.size();
                worst = qMax(worst, error);
                angles << std::atan2(pv, pu);
            }
            // it must go all the way round, not leave a side out
            std::sort(angles.begin(), angles.end());
            qreal gap = angles.first() + 2 * M_PI - angles.last();
            for (int i = 1; i < angles.size(); i++) gap = qMax(gap, angles[i] - angles[i - 1]);
            if (average < 0.06 && worst < 0.16 && gap < 0.9) {
                // nearly round is round
                if (std::abs(a - b) < 0.08 * qMax(a, b)) a = b = (a + b) / 2;
                const qreal turn = turnAround(points, center) < 0 ? -1 : 1;
                const qreal from = std::atan2(QPointF::dotProduct(start - center, v) / b, QPointF::dotProduct(start - center, u) / a);
                const int n = qMax(24, int(M_PI * (a + b) / step));
                for (int i = 0; i <= n; i++) {
                    const qreal t = from + turn * 2 * M_PI * i / n;
                    vertices << center + a * std::cos(t) * u + b * std::sin(t) * v;
                }
                shape.kind = Ellipse;
            }
        }

        if (!shape.kind) {
            // a polygon: a few straight sides that the stroke hugs
            const qreal tolerance = qMax(4 * pixel, 0.035 * length);
            QVector<QPointF> loop = points;
            loop << start;
            int far = 0;
            for (int i = 0; i < points.size(); i++) {
                if (distance(points[i], start) > distance(points[far], start)) far = i;
            }
            QVector<QPointF> polygon = corners(loop, 0, far, tolerance);
            polygon.removeLast();
            QVector<QPointF> rest = corners(loop, far, loop.size() - 1, tolerance);
            rest.removeLast();
            polygon += rest;
            // a corner where the sides run straight on is no corner
            for (bool merged = true; merged && polygon.size() > 3;) {
                merged = false;
                for (int i = 0; i < polygon.size(); i++) {
                    const QPointF &before = polygon[(i + polygon.size() - 1) % polygon.size()];
                    const QPointF &after = polygon[(i + 1) % polygon.size()];
                    if (segmentDistance(polygon[i], before, after) < 0.8 * tolerance) {
                        polygon.remove(i);
                        merged = true;
                        break;
                    }
                }
            }
            if (polygon.size() >= 3 && polygon.size() <= 6 && worstFit(points, polygon, true) < qMax(4 * pixel, 0.016 * length)) {
                if (polygon.size() == 4) {
                    bool square = true;
                    for (int i = 0; i < 4; i++) square &= std::abs(cornerDegrees(polygon[(i + 3) % 4], polygon[i], polygon[(i + 1) % 4]) - 90) < 12;
                    if (square) polygon = squared(polygon);
                }
                // from the corner nearest where the stroke began, back to it
                int first = 0;
                for (int i = 0; i < polygon.size(); i++) {
                    if (distance(polygon[i], start) < distance(polygon[first], start)) first = i;
                }
                std::rotate(polygon.begin(), polygon.begin() + first, polygon.end());
                polygon << polygon.first();
                vertices = polygon;
                shape.kind = Polygon;
            }
        }
    }

    if (!shape.kind && !closed) {
        // a few straight segments meeting at clear corners
        const QVector<QPointF> polyline = corners(points, 0, points.size() - 1, qMax(4 * pixel, 0.03 * length));
        if (polyline.size() >= 3 && polyline.size() <= 4 && worstFit(points, polyline, false) < qMax(4 * pixel, 0.016 * length)) {
            bool clear = true;
            for (int i = 1; i < polyline.size(); i++) clear &= distance(polyline[i - 1], polyline[i]) > 0.15 * length;
            for (int i = 1; i + 1 < polyline.size(); i++) clear &= cornerDegrees(polyline[i - 1], polyline[i], polyline[i + 1]) < 140;
            if (clear) {
                shape.kind = Polyline;
                vertices = polyline;
            }
        }
    }

    if (!shape.kind && !closed) {
        // an arc: the circle that fits the stroke best (Kåsa's fit)
        QPointF mean;
        for (const QPointF &p : points) mean += p / points.size();
        qreal suu = 0, svv = 0, suv = 0, suuu = 0, svvv = 0, suvv = 0, svuu = 0;
        for (const QPointF &p : points) {
            const qreal u = p.x() - mean.x(), v = p.y() - mean.y();
            suu += u * u;
            svv += v * v;
            suv += u * v;
            suuu += u * u * u;
            svvv += v * v * v;
            suvv += u * v * v;
            svuu += v * u * u;
        }
        const qreal det = suu * svv - suv * suv;
        if (std::abs(det) > 1e-9) {
            const qreal r1 = 0.5 * (suuu + suvv), r2 = 0.5 * (svvv + svuu);
            const QPointF center = mean + QPointF((r1 * svv - r2 * suv) / det, (suu * r2 - suv * r1) / det);
            qreal radius = 0;
            for (const QPointF &p : points) radius += distance(p, center) / points.size();
            qreal worst = 0;
            for (const QPointF &p : points) worst = qMax(worst, std::abs(distance(p, center) - radius));
            if (worst < qMax(3 * pixel, 0.05 * radius) && length / radius < 5.5 && radius < 4 * length) {
                const qreal from = std::atan2(start.y() - center.y(), start.x() - center.x());
                const qreal turn = turnAround(points, center);
                const int n = qMax(8, int(std::abs(turn) * radius / step));
                for (int i = 0; i <= n; i++) {
                    const qreal t = from + turn * i / n;
                    vertices << center + radius * QPointF(std::cos(t), std::sin(t));
                }
                shape.kind = Arc;
            }
        }
    }

    if (!shape.kind) return shape;
    shape.outline = sampled(vertices, step);
    const qreal total = qMax<qreal>(1e-6, pathLength(shape.outline));
    qreal done = 0;
    shape.along << 0;
    for (int i = 1; i < shape.outline.size(); i++) {
        done += distance(shape.outline[i - 1], shape.outline[i]);
        shape.along << done / total;
    }
    return shape;
}
