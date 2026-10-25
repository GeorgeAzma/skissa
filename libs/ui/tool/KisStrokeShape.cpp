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
#include <optional>

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

int sideCount(const QVector<QPointF> &shape, bool closed)
{
    return closed ? shape.size() : shape.size() - 1;
}

const QPointF &vertex(const QVector<QPointF> &shape, int i)
{
    return shape[(i % shape.size() + shape.size()) % shape.size()];
}

/// How far each point lies from the nearest side of \p shape, smallest first
QVector<qreal> deviations(const QVector<QPointF> &points, const QVector<QPointF> &shape, bool closed)
{
    QVector<qreal> out;
    for (const QPointF &p : points) {
        qreal nearest = std::numeric_limits<qreal>::max();
        for (int i = 0; i < sideCount(shape, closed); i++) nearest = qMin(nearest, segmentDistance(p, shape[i], vertex(shape, i + 1)));
        out << nearest;
    }
    std::sort(out.begin(), out.end());
    return out;
}

qreal rootMeanSquare(const QVector<qreal> &values)
{
    qreal sum = 0;
    for (qreal v : values) sum += v * v;
    return std::sqrt(sum / qMax(1, values.size()));
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

/// A closed polygon's exterior angles summed: a full turn when it goes around once
qreal turning(const QVector<QPointF> &polygon)
{
    qreal total = 0;
    for (int i = 0; i < polygon.size(); i++) {
        const QPointF a = vertex(polygon, i - 1), b = polygon[i], c = vertex(polygon, i + 1);
        qreal t = std::atan2(c.y() - b.y(), c.x() - b.x()) - std::atan2(b.y() - a.y(), b.x() - a.x());
        while (t > M_PI) t -= 2 * M_PI;
        while (t < -M_PI) t += 2 * M_PI;
        total += t;
    }
    return total;
}

bool crosses(const QPointF &a, const QPointF &b, const QPointF &c, const QPointF &d)
{
    auto side = [](const QPointF &p, const QPointF &q, const QPointF &r) {
        return (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x());
    };
    return side(a, b, c) * side(a, b, d) < 0 && side(c, d, a) * side(c, d, b) < 0;
}

/// No two sides of the closed \p polygon cross
bool isSimple(const QVector<QPointF> &polygon)
{
    const int n = polygon.size();
    for (int i = 0; i < n; i++) {
        for (int j = i + 2; j < n; j++) {
            if (i == 0 && j == n - 1) continue;
            if (crosses(polygon[i], vertex(polygon, i + 1), polygon[j], vertex(polygon, j + 1))) return false;
        }
    }
    return true;
}

struct Line {
    QPointF point;
    QPointF direction;
};

Line fitLine(const QVector<QPointF> &points)
{
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
    return {mean, QPointF(std::cos(angle), std::sin(angle))};
}

/// Where two lines meet, unless they run nearly the same way (no clear corner)
std::optional<QPointF> meet(const Line &a, const Line &b)
{
    const qreal cross = a.direction.x() * b.direction.y() - a.direction.y() * b.direction.x();
    if (std::abs(cross) < 0.17) return std::nullopt;
    const QPointF d = b.point - a.point;
    const qreal t = (d.x() * b.direction.y() - d.y() * b.direction.x()) / cross;
    return a.point + t * a.direction;
}

/// The stroke from where its end comes closest to its beginning: an overshoot past the start is cut off
QVector<QPointF> closedLoop(const QVector<QPointF> &points)
{
    const int n = points.size();
    const int reach = qMax(2, n * 3 / 10);
    qreal best = std::numeric_limits<qreal>::max();
    int from = 0, to = n - 1;
    for (int i = n - reach; i < n; i++) {
        for (int j = 0; j < reach; j++) {
            const qreal d = distance(points[i], points[j]);
            if (d < best) {
                best = d;
                from = j;
                to = i;
            }
        }
    }
    return points.mid(from, to - from + 1);
}

/// The few corners of a hand-drawn polygon or polyline, before straightening
QVector<QPointF> roughCorners(const QVector<QPointF> &points, qreal tolerance, bool closed)
{
    QVector<QPointF> polygon;
    if (closed) {
        QVector<QPointF> loop = points;
        loop << points.first();
        int far = 0;
        for (int i = 0; i < points.size(); i++) {
            if (distance(points[i], points.first()) > distance(points[far], points.first())) far = i;
        }
        polygon = corners(loop, 0, far, tolerance);
        polygon.removeLast();
        QVector<QPointF> rest = corners(loop, far, loop.size() - 1, tolerance);
        rest.removeLast();
        polygon += rest;
    } else {
        polygon = corners(points, 0, points.size() - 1, tolerance);
    }
    const int least = closed ? 3 : 2;
    // a corner where the sides run straight on is no corner
    for (bool merged = true; merged && polygon.size() > least;) {
        merged = false;
        for (int i = closed ? 0 : 1; i < (closed ? polygon.size() : polygon.size() - 1); i++) {
            if (segmentDistance(polygon[i], vertex(polygon, i - 1), vertex(polygon, i + 1)) < 0.8 * tolerance) {
                polygon.remove(i);
                merged = true;
                break;
            }
        }
    }
    // a rounded corner leaves a short side between two corners: they are one
    const qreal length = pathLength(points);
    for (bool merged = true; merged && polygon.size() > least;) {
        merged = false;
        for (int i = 0; i < sideCount(polygon, closed); i++) {
            const bool inner = closed || (i > 0 && i + 1 < polygon.size() - 1);
            if (inner && distance(polygon[i], vertex(polygon, i + 1)) < 0.06 * length) {
                polygon[i] = (polygon[i] + vertex(polygon, i + 1)) / 2;
                polygon.remove((i + 1) % polygon.size());
                merged = true;
                break;
            }
        }
    }
    return polygon;
}

/// Corners where lines fitted to the middle of each side meet; none if a side curves or the corners aren't clear
QVector<QPointF> straightened(const QVector<QPointF> &points, const QVector<QPointF> &rough, bool closed, qreal pixel)
{
    const int sides = sideCount(rough, closed);
    QVector<QVector<QPointF>> middles(sides);
    for (const QPointF &p : points) {
        int nearest = 0;
        qreal best = std::numeric_limits<qreal>::max();
        for (int i = 0; i < sides; i++) {
            const qreal d = segmentDistance(p, rough[i], vertex(rough, i + 1));
            if (d < best) {
                best = d;
                nearest = i;
            }
        }
        const QPointF a = rough[nearest], b = vertex(rough, nearest + 1);
        const qreal l2 = QPointF::dotProduct(b - a, b - a);
        if (l2 <= 0) continue;
        // away from the rounded corners
        const qreal t = QPointF::dotProduct(p - a, b - a) / l2;
        if (t > 0.2 && t < 0.8) middles[nearest] << p;
    }
    QVector<Line> lines;
    for (int i = 0; i < sides; i++) {
        if (middles[i].size() < 3) return {};
        const Line line = fitLine(middles[i]);
        const qreal side = distance(rough[i], vertex(rough, i + 1));
        for (const QPointF &p : middles[i]) {
            const QPointF d = p - line.point;
            if (std::abs(d.x() * line.direction.y() - d.y() * line.direction.x()) > qMax(0.045 * side, 2.5 * pixel)) return {};
        }
        lines << line;
    }
    const qreal reach = 0.25 * pathLength(points) / qMax(3, sides);
    QVector<QPointF> out;
    for (int i = 0; i < rough.size(); i++) {
        if (!closed && (i == 0 || i == rough.size() - 1)) {
            // an open end: where the stroke ends, moved onto its side's line
            const Line &line = i == 0 ? lines.first() : lines.last();
            const QPointF end = i == 0 ? points.first() : points.last();
            out << line.point + QPointF::dotProduct(end - line.point, line.direction) * line.direction;
            continue;
        }
        const std::optional<QPointF> corner = meet(lines[(i - 1 + sides) % sides], lines[i % sides]);
        if (!corner || distance(*corner, rough[i]) > reach) return {};
        out << *corner;
    }
    return out;
}

/// Whether the stroke turns at each corner rather than curving past it
bool turnsAtCorners(const QVector<QPointF> &points, const QVector<QPointF> &shape, bool closed)
{
    for (int i = closed ? 0 : 1; i < (closed ? shape.size() : shape.size() - 1); i++) {
        const qreal side = qMin(distance(shape[i], vertex(shape, i - 1)), distance(shape[i], vertex(shape, i + 1)));
        qreal nearest = std::numeric_limits<qreal>::max();
        for (const QPointF &p : points) nearest = qMin(nearest, distance(p, shape[i]));
        if (nearest > 0.15 * side) return false;
    }
    return true;
}

/// Whether the stroke follows \p shape closely: most points near it, none much further
bool hugs(const QVector<QPointF> &points, const QVector<QPointF> &shape, bool closed, qreal length, qreal pixel, qreal *rms = nullptr)
{
    const QVector<qreal> d = deviations(points, shape, closed);
    const qreal spread = rootMeanSquare(d);
    if (rms) *rms = spread;
    return spread < qMax(2 * pixel, 0.011 * length) && d[int(d.size() * 0.9)] < qMax(4 * pixel, 0.022 * length);
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
    // the hold at the end piles points up; keep ones that moved, at most about two thousand: finding the
    // loop compares them pairwise, and a long scribble held still would stall the stroke
    const qreal spacing = qMax(0.75 * pixel, pathLength(input) / 2000);
    QVector<QPointF> points;
    for (const QPointF &p : input) {
        if (points.isEmpty() || distance(p, points.last()) >= spacing) points << p;
    }
    if (!input.isEmpty() && points.size() > 1 && distance(input.last(), points.last()) >= 0.75 * pixel) points << input.last();
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
            // a polygon: a few straight sides that the stroke goes round once, turning at each corner
            const QVector<QPointF> loop = closedLoop(points);
            const QVector<QPointF> rough = roughCorners(loop, qMax(4 * pixel, 0.035 * length), true);
            QVector<QPointF> polygon = rough.size() >= 3 && rough.size() <= 6 ? straightened(loop, rough, true, pixel) : QVector<QPointF>();
            if (!polygon.isEmpty() && isSimple(polygon) && std::abs(std::abs(turning(polygon)) - 2 * M_PI) < 0.3
                && turnsAtCorners(loop, polygon, true) && hugs(loop, polygon, true, length, pixel)) {
                if (polygon.size() == 4) {
                    bool square = true;
                    for (int i = 0; i < 4; i++) square &= std::abs(cornerDegrees(vertex(polygon, i - 1), polygon[i], vertex(polygon, i + 1)) - 90) < 12;
                    if (square) polygon = squared(polygon);
                }
                // from the corner nearest where the stroke began, the way it went, back to it
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
        // a few straight segments meeting at clear corners, or an arc: whichever the stroke follows more closely
        qreal polylineSpread = std::numeric_limits<qreal>::max();
        const QVector<QPointF> rough = roughCorners(points, qMax(4 * pixel, 0.03 * length), false);
        const QVector<QPointF> polyline = rough.size() >= 3 && rough.size() <= 4 ? straightened(points, rough, false, pixel) : QVector<QPointF>();
        if (polyline.size() >= 3) {
            bool clear = turnsAtCorners(points, polyline, false);
            for (int i = 1; i < polyline.size(); i++) clear &= distance(polyline[i - 1], polyline[i]) > 0.15 * length;
            for (int i = 1; i + 1 < polyline.size(); i++) clear &= cornerDegrees(polyline[i - 1], polyline[i], polyline[i + 1]) < 150;
            qreal spread = 0;
            if (clear && hugs(points, polyline, false, length, pixel, &spread)) {
                // a polyline has more to fit with, so it must fit clearly better
                polylineSpread = spread * 1.15;
                shape.kind = Polyline;
                vertices = polyline;
            }
        }

        // the circle that fits the stroke best (Kåsa's fit)
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
            QVector<qreal> errors;
            for (const QPointF &p : points) errors << std::abs(distance(p, center) - radius);
            std::sort(errors.begin(), errors.end());
            if (errors[int(errors.size() * 0.95)] < qMax(3 * pixel, 0.05 * radius) && errors.last() < qMax(4 * pixel, 0.08 * radius)
                && length / radius < 5.5 && radius < 4 * length && rootMeanSquare(errors) < polylineSpread) {
                const qreal from = std::atan2(start.y() - center.y(), start.x() - center.x());
                const qreal turn = turnAround(points, center);
                const int n = qMax(8, int(std::abs(turn) * radius / step));
                vertices.clear();
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
