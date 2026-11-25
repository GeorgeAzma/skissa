/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTransformPerspective.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <Eigen/Dense>
#include <Eigen/Geometry>

#include <KoSnapGuide.h>
#include <canvas/kis_canvas2.h>
#include <kis_algebra_2d.h>
#include <kis_coordinates_converter.h>
#include <kis_global.h>
#include <kis_image.h>
#include <kis_layer_utils.h>
#include <kis_node.h>
#include <kis_painting_assistant.h>
#include <kis_painting_assistants_decoration.h>

#include "tool_transform_args.h"
#include "transform_transaction_properties.h"

namespace KisTransformPerspective
{

namespace
{

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

qreal signedArea(const QPolygonF &quad)
{
    qreal sum = 0;
    for (int i = 0; i < quad.size(); i++) {
        sum += cross(quad[i], quad[(i + 1) % quad.size()]);
    }
    return 0.5 * sum;
}

/// Where two endless lines cross, unless they are close to parallel
bool crossing(const QLineF &a, const QLineF &b, QPointF *point)
{
    const QPointF da = a.p2() - a.p1();
    const QPointF db = b.p2() - b.p1();
    const qreal den = cross(da, db);
    if (std::abs(den) < 1e-9 * std::sqrt(KisAlgebra2D::dotProduct(da, da) * KisAlgebra2D::dotProduct(db, db))) {
        return false;
    }
    const qreal t = cross(b.p1() - a.p1(), db) / den;
    *point = a.p1() + t * da;
    return true;
}

qreal distanceToLine(const QPointF &pos, const QLineF &line)
{
    const QPointF d = line.p2() - line.p1();
    const qreal length = std::hypot(d.x(), d.y());
    return length > 0 ? std::abs(cross(pos - line.p1(), d)) / length : kisDistance(pos, line.p1());
}

QPointF projected(const QPointF &pos, const QLineF &line)
{
    const QPointF d = line.p2() - line.p1();
    const qreal lengthSq = KisAlgebra2D::dotProduct(d, d);
    if (lengthSq <= 0) return line.p1();
    return line.p1() + KisAlgebra2D::dotProduct(pos - line.p1(), d) / lengthSq * d;
}

/// The two edges that meet at a vanishing point: top and bottom, or the sides
void edgesOf(bool across, int edges[2][2])
{
    if (across) {
        edges[0][0] = 0; edges[0][1] = 1;
        edges[1][0] = 3; edges[1][1] = 2;
    } else {
        edges[0][0] = 0; edges[0][1] = 3;
        edges[1][0] = 1; edges[1][1] = 2;
    }
}

/// A rectangle's width over its height in the world, from its picture (Zhang and He's whiteboard method)
qreal worldAspect(const QPointF &tl, const QPointF &tr, const QPointF &bl, const QPointF &br,
                  const QPointF &principal, qreal fallbackFocal)
{
    auto at = [&principal](const QPointF &p) { return Eigen::Vector3d(p.x() - principal.x(), p.y() - principal.y(), 1.0); };
    const Eigen::Vector3d m1 = at(tl), m2 = at(tr), m3 = at(bl), m4 = at(br);

    const qreal k2 = m1.cross(m4).dot(m3) / m2.cross(m4).dot(m3);
    const qreal k3 = m1.cross(m4).dot(m2) / m3.cross(m4).dot(m2);
    const Eigen::Vector3d n2 = k2 * m2 - m1;
    const Eigen::Vector3d n3 = k3 * m3 - m1;

    // with a pair of edges (close to) parallel the focal length can't be told
    const qreal size = std::hypot(br.x() - tl.x(), br.y() - tl.y());
    auto parallel = [size](const Eigen::Vector3d &n) { return std::abs(n.z()) * size < 0.02 * n.head<2>().norm(); };
    qreal f2 = parallel(n2) || parallel(n3) ? -1 : -(n2.x() * n3.x() + n2.y() * n3.y()) / (n2.z() * n3.z());
    if (!std::isfinite(f2) || f2 <= 0) {
        f2 = fallbackFocal * fallbackFocal;
    }

    const qreal ratio2 = (n2.x() * n2.x() + n2.y() * n2.y() + f2 * n2.z() * n2.z())
                       / (n3.x() * n3.x() + n3.y() * n3.y() + f2 * n3.z() * n3.z());
    return std::isfinite(ratio2) && ratio2 > 0 ? std::sqrt(ratio2) : 1.0;
}

/// The four handles of a perspective guide as a convex quad, in order around it
bool convexQuad(QPolygonF points, QPolygonF *quad)
{
    if (points.size() != 4) return false;
    QPointF center;
    for (const QPointF &p : points) center += p / 4;
    std::sort(points.begin(), points.end(), [center](const QPointF &a, const QPointF &b) {
        return std::atan2(a.y() - center.y(), a.x() - center.x()) < std::atan2(b.y() - center.y(), b.x() - center.x());
    });
    if (!KisAlgebra2D::isPolygonTrulyConvex(points)) return false;
    *quad = points;
    return true;
}

}

bool fit(ToolTransformArgs &args, const QPolygonF &src, const QPolygonF &dst)
{
    if (src.size() != 4 || dst.size() != 4) return false;
    if (!KisAlgebra2D::isPolygonTrulyConvex(dst, true, 177.0)) return false;

    QTransform H;
    if (!QTransform::quadToQuad(src, dst, H)) return false;

    // what the box does around its center, which is then moved into place
    const QPointF center = args.originalCenter();
    const QPointF moved = H.map(center);
    const QTransform M = QTransform::fromTranslate(center.x(), center.y()) * H * QTransform::fromTranslate(-moved.x(), -moved.y());
    if (qFuzzyIsNull(M.m33())) return false;
    const qreal k = 1.0 / M.m33();

    // in column form p -> A p / (1 + g.p); free transform makes it as
    // proj(R L p) for a turn R of the plane seen by the camera, L the rest
    Eigen::Matrix2d A;
    A << M.m11() * k, M.m21() * k,
         M.m12() * k, M.m22() * k;
    const Eigen::Vector2d g(M.m13() * k, M.m23() * k);
    if (std::abs(A.determinant()) < 1e-12) return false;

    const qreal d = args.cameraPos().z();
    const Eigen::Vector2d u = -d * A.transpose().inverse() * g;
    const qreal tilt = u.norm();

    // turning the plane about the axis across u by atan(|u|) gives R2^-T r3 = u
    Eigen::Matrix3d R = Eigen::Matrix3d::Identity();
    if (tilt > 1e-12) {
        const Eigen::Vector2d n = u / tilt;
        R = Eigen::AngleAxisd(std::atan(tilt), Eigen::Vector3d(n.y(), -n.x(), 0)).toRotationMatrix();
    }
    const Eigen::Matrix2d L = R.topLeftCorner<2, 2>().inverse() * A;

    // R = Rx(aX) Ry(aY) Rz(gamma); the turn about z joins the rest
    const qreal aY = std::asin(qBound(-1.0, R(0, 2), 1.0));
    const qreal aX = std::atan2(-R(1, 2), R(2, 2));
    const qreal gamma = std::atan2(-R(0, 1), R(0, 0));
    Eigen::Matrix2d Rz;
    Rz << std::cos(gamma), -std::sin(gamma),
          std::sin(gamma), std::cos(gamma);
    const Eigen::Matrix2d N = Rz * L;

    QTransform BRI;
    BRI.rotate(180. * -args.boundsRotation() / M_PI);
    const QTransform rest = BRI.inverted() * QTransform(N(0, 0), N(1, 0), N(0, 1), N(1, 1), 0, 0);
    const KisAlgebra2D::DecomposedMatrix dm(rest);
    if (!dm.isValid()) return false;

    args.setScaleX(dm.scaleX);
    args.setScaleY(dm.scaleY);
    args.setShearX(dm.shearXY);
    args.setShearY(0);
    args.setAZ(normalizeAngle(kisDegreesToRadians(dm.angle)));
    args.setAX(normalizeAngle(aX));
    args.setAY(normalizeAngle(aY));
    args.setTransformedCenter(moved);
    return true;
}

bool vanishingPoint(const QPolygonF &quad, bool across, QPointF *point)
{
    if (quad.size() != 4) return false;
    int e[2][2];
    edgesOf(across, e);
    const QLineF a(quad[e[0][0]], quad[e[0][1]]);
    const QLineF b(quad[e[1][0]], quad[e[1][1]]);
    // edges this close to parallel meet beyond any use
    if (!crossing(a, b, point)) return false;
    const qreal size = std::max(a.length(), b.length());
    return kisDistance(*point, quad.boundingRect().center()) < 200 * size;
}

QPolygonF withVanishingPoint(const QPolygonF &quad, bool across, const QPointF &pos)
{
    QPointF vp;
    if (!vanishingPoint(quad, across, &vp)) return quad;

    int e[2][2];
    edgesOf(across, e);
    int far[2], near[2];
    for (int i = 0; i < 2; i++) {
        const bool firstFar = kisSquareDistance(quad[e[i][0]], vp) > kisSquareDistance(quad[e[i][1]], vp);
        far[i] = firstFar ? e[i][0] : e[i][1];
        near[i] = firstFar ? e[i][1] : e[i][0];
    }

    QPolygonF result = quad;
    const QLineF nearEdge(quad[near[0]], quad[near[1]]);
    for (int i = 0; i < 2; i++) {
        QPointF p;
        if (!crossing(QLineF(quad[far[i]], pos), nearEdge, &p)) return quad;
        result[near[i]] = p;
    }
    return result;
}

bool flattening(const QPolygonF &quad, const QPointF &principal, qreal fallbackFocal, QTransform *result)
{
    if (quad.size() != 4 || !KisAlgebra2D::isPolygonTrulyConvex(quad)) return false;

    QPointF center;
    for (const QPointF &p : quad) center += p / 4;

    // keep the picture's own handedness; turn by whole quarters to stay closest to upright
    const QVector<int> around = signedArea(quad) >= 0 ? QVector<int>{0, 1, 2, 3} : QVector<int>{0, 3, 2, 1};
    qreal bestCost = std::numeric_limits<qreal>::max();
    QPolygonF bestFrom, bestTo;
    for (int shift = 0; shift < 4; shift++) {
        QPolygonF from;
        for (int i = 0; i < 4; i++) from << quad[around[(i + shift) % 4]];

        const qreal aspect = worldAspect(from[0], from[1], from[3], from[2], principal, fallbackFocal);
        const qreal wide = std::max(kisDistance(from[0], from[1]), kisDistance(from[3], from[2]));
        const qreal tall = std::max(kisDistance(from[0], from[3]), kisDistance(from[1], from[2]));
        const qreal w = std::max(wide, tall * aspect);
        const qreal h = w / aspect;
        const QRectF rect(center.x() - w / 2, center.y() - h / 2, w, h);
        const QPolygonF to({rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()});

        qreal cost = 0;
        for (int i = 0; i < 4; i++) cost += kisSquareDistance(from[i] - center, to[i] - center);
        if (cost < bestCost) {
            bestCost = cost;
            bestFrom = from;
            bestTo = to;
        }
    }
    return QTransform::quadToQuad(bestFrom, bestTo, *result);
}

void paintVanishingLines(QPainter &gc, const QPolygonF &quad, const QTransform &imageToView, const QPen &pen)
{
    QPainterPath path;
    for (bool across : {true, false}) {
        QPointF vp;
        if (!vanishingPoint(quad, across, &vp)) continue;
        int e[2][2];
        edgesOf(across, e);
        const QPointF to = imageToView.map(vp);
        for (int i = 0; i < 2; i++) {
            const QPointF a = quad[e[i][0]];
            const QPointF b = quad[e[i][1]];
            path.moveTo(imageToView.map(kisSquareDistance(a, vp) < kisSquareDistance(b, vp) ? a : b));
            path.lineTo(to);
        }
        path.addEllipse(to, 3, 3);
    }
    gc.save();
    gc.setPen(pen);
    gc.setBrush(Qt::NoBrush);
    gc.drawPath(path);
    gc.restore();
}

void Catcher::clear()
{
    m_points.clear();
    m_lines.clear();
    m_vanishing.clear();
}

void Catcher::collect(KoSnapGuide *snapGuide, const TransformTransactionProperties &transaction, const KisCoordinatesConverter *converter)
{
    clear();

    KisCanvas2 *canvas = snapGuide ? dynamic_cast<KisCanvas2 *>(snapGuide->canvas()) : nullptr;
    if (!canvas || !canvas->image()) return;
    KisImageSP image = canvas->image();

    auto addRect = [this](const QRectF &rect, bool edges) {
        const QPolygonF corners({rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()});
        for (int i = 0; i < 4; i++) {
            m_points << corners[i];
            if (edges) m_lines << QLineF(corners[i], corners[(i + 1) % 4]);
        }
    };
    addRect(image->bounds(), true);

    const KisNodeList moving = transaction.rootNodes();
    KisLayerUtils::recursiveApplyNodes(image->root(), [&](KisNodeSP node) {
        if (!node->parent() || !node->inherits("KisLayer") || node->childCount() > 0 || !node->visible(true)) return;
        for (KisNodeSP up = node; up; up = up->parent()) {
            if (moving.contains(up)) return;
        }
        const QRect bounds = node->exactBounds();
        if (!bounds.isEmpty() && bounds != image->bounds()) addRect(bounds, false);
    });

    KisPaintingAssistantsDecorationSP guides = canvas->paintingAssistantsDecoration();
    if (!guides || !guides->visible()) return;
    Q_FOREACH (KisPaintingAssistantSP guide, guides->assistants()) {
        if (!guide->isAssistantComplete()) continue;
        QPolygonF handles;
        Q_FOREACH (KisPaintingAssistantHandleSP handle, guide->handles()) {
            handles << converter->documentToImage(QPointF(*handle));
        }
        const QString id = guide->id();
        QPolygonF quad;
        if (id == QLatin1String("perspective") && convexQuad(handles, &quad)) {
            for (int i = 0; i < 4; i++) {
                m_points << quad[i];
                m_lines << QLineF(quad[i], quad[(i + 1) % 4]);
            }
            for (bool across : {true, false}) {
                QPointF vp;
                if (vanishingPoint(quad, across, &vp)) m_vanishing << vp;
            }
        } else if (id == QLatin1String("vanishing point") && handles.size() >= 1) {
            m_vanishing << handles[0];
        } else if (id == QLatin1String("two point") && handles.size() >= 2) {
            m_vanishing << handles[0] << handles[1];
        } else if ((id == QLatin1String("ruler") || id == QLatin1String("infinite ruler")) && handles.size() >= 2) {
            m_lines << QLineF(handles[0], handles[1]);
        }
    }
}

QPointF Catcher::corner(const QPointF &pos, qreal radius, const QVector<QPointF> &neighbours) const
{
    qreal best = radius;
    QPointF caught = pos;
    for (const QPointF &p : m_points) {
        const qreal d = kisDistance(p, pos);
        if (d < best) {
            best = d;
            caught = p;
        }
    }
    if (best < radius) return caught;

    QVector<QLineF> lines;
    for (const QLineF &line : m_lines) {
        if (distanceToLine(pos, line) < radius) lines << line;
    }
    for (const QPointF &from : neighbours) {
        for (const QPointF &vp : m_vanishing) {
            if (kisDistance(from, vp) < 1.0) continue;
            const QLineF line(from, vp);
            if (distanceToLine(pos, line) < radius) lines << line;
        }
    }
    if (lines.isEmpty()) return pos;

    // where two of them cross, then the nearest line
    best = 1.5 * radius;
    for (int i = 0; i < lines.size(); i++) {
        for (int j = i + 1; j < lines.size(); j++) {
            QPointF p;
            if (std::abs(std::sin(kisDegreesToRadians(lines[i].angleTo(lines[j])))) > 0.09 && crossing(lines[i], lines[j], &p)) {
                const qreal d = kisDistance(p, pos);
                if (d < best) {
                    best = d;
                    caught = p;
                }
            }
        }
    }
    if (best < 1.5 * radius) return caught;

    best = radius;
    for (const QLineF &line : lines) {
        const qreal d = distanceToLine(pos, line);
        if (d < best) {
            best = d;
            caught = projected(pos, line);
        }
    }
    return caught;
}

}
