/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSelectionGestures.h"

#include <QColor>
#include <QRectF>
#include <QtMath>

#include <KoColorSpace.h>
#include <kis_config.h>
#include <kis_paint_device.h>
#include <kis_random_accessor_ng.h>

namespace
{
/// How far, on average, a rough shape may stray from the clean one, as a share of its size
constexpr qreal fitShare = 0.08;
/// The least lightness step, 0 to 1, that counts as an edge
constexpr qreal leastEdge = 0.06;
} // namespace

bool KisSelectionGestures::followsEdges()
{
    return KisConfig(true).readEntry<bool>("StudioSelectFollowEdges", false);
}

void KisSelectionGestures::setFollowsEdges(bool on)
{
    KisConfig(false).writeEntry("StudioSelectFollowEdges", on);
}

QVector<QPointF> KisSelectionGestures::cleanShape(const QVector<QPointF> &points)
{
    if (points.size() < 8) return {};
    QRectF box(points.first(), QSizeF());
    for (const QPointF &p : points) box |= QRectF(p, QSizeF(0.0001, 0.0001));
    const qreal shorter = qMin(box.width(), box.height());
    if (shorter < 4) return {};
    // only a loop: a line that ends near where it began
    if (QLineF(points.first(), points.last()).length() > 0.3 * qMax(box.width(), box.height())) return {};

    qreal offBox = 0;
    qreal offEllipse = 0;
    const QPointF center = box.center();
    const qreal a = box.width() / 2;
    const qreal b = box.height() / 2;
    for (const QPointF &p : points) {
        offBox += qMin(qMin(qAbs(p.x() - box.left()), qAbs(p.x() - box.right())), qMin(qAbs(p.y() - box.top()), qAbs(p.y() - box.bottom())));
        const qreal r = std::hypot((p.x() - center.x()) / a, (p.y() - center.y()) / b);
        offEllipse += qAbs(r - 1);
    }
    offBox /= points.size() * shorter;
    offEllipse /= points.size();
    // an ellipse's radius share is half its size's: the two errors compared as shares of the same size
    offEllipse /= 2;
    if (qMin(offBox, offEllipse) > fitShare) return {};

    QVector<QPointF> shape;
    if (offBox <= offEllipse) {
        shape = {box.topLeft(), box.topRight(), box.bottomRight(), box.bottomLeft()};
        return shape;
    }
    // nearly round is meant round
    qreal rx = a;
    qreal ry = b;
    if (qAbs(a - b) < 0.12 * qMax(a, b)) rx = ry = (a + b) / 2;
    const int steps = qBound(32, int(2 * M_PI * qMax(rx, ry) / 4), 256);
    for (int i = 0; i < steps; i++) {
        const qreal t = 2 * M_PI * i / steps;
        shape.append(center + QPointF(rx * std::cos(t), ry * std::sin(t)));
    }
    return shape;
}

QPointF KisSelectionGestures::onEdge(KisPaintDeviceSP picture, const QPointF &from, const QPointF &to, qreal band)
{
    const QPointF along = to - from;
    const qreal length = std::hypot(along.x(), along.y());
    if (!picture || length < 0.5 || band < 1) return to;
    const QPointF across(-along.y() / length, along.x() / length);
    KisRandomConstAccessorSP pixels = picture->createRandomConstAccessorNG();
    const KoColorSpace *space = picture->colorSpace();
    auto lightness = [&](const QPointF &p) {
        pixels->moveTo(qFloor(p.x()), qFloor(p.y()));
        QColor color;
        space->toQColor(pixels->oldRawData(), &color);
        // see-through counts as its own lightness, so a drawing on clear paper has edges too
        return (0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF()) * color.alphaF() + (1 - color.alphaF());
    };
    QPointF best = to;
    qreal bestScore = leastEdge;
    for (qreal s = -band; s <= band; s += 1.0) {
        const QPointF p = to + across * s;
        const qreal step = qAbs(lightness(p + across) - lightness(p - across)) / 2;
        // a nearer edge wins over a slightly stronger far one
        const qreal score = step * (1.0 - 0.3 * qAbs(s) / band);
        if (score > bestScore) {
            bestScore = score;
            best = p;
        }
    }
    return best;
}
