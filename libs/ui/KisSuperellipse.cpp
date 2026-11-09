/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisSuperellipse.h"

#include <algorithm>
#include <cmath>

#include <QtMath>

#include <klocalizedstring.h>

namespace KisSuperellipse
{

QPainterPath pathOf(const QRectF &box, const Look &look)
{
    QPainterPath path;
    const QRectF r = box.normalized();
    if (r.isEmpty()) return path;
    const qreal a = r.width() / 2, b = r.height() / 2;
    const QPointF middle = r.center();
    const qreal reach = std::clamp(look.roundness, 0.0, 1.0) * std::max(a, b);
    const qreal rx = std::min(reach, a), ry = std::min(reach, b);
    if (rx < 1e-6 || ry < 1e-6) {
        path.addRect(r);
        return path;
    }
    const qreal power = 2 / std::clamp(look.curve, leastCurve, mostCurve);
    // enough points that a corner's curve is smooth at any size
    const int steps = std::clamp(int(std::max(rx, ry) / 2), 24, 256);
    // the corners clockwise on screen from the bottom right, each walked from the side before it to the side after
    const struct {
        int sx, sy;
        bool fromSide;
    } corners[] = {{1, 1, true}, {-1, 1, false}, {-1, -1, true}, {1, -1, false}};
    bool first = true;
    for (const auto &corner : corners) {
        const QPointF centre(middle.x() + corner.sx * (a - rx), middle.y() + corner.sy * (b - ry));
        for (int i = 0; i <= steps; i++) {
            // from the side the corner leaves (its x side when fromSide) to the other
            const qreal t = qreal(corner.fromSide ? i : steps - i) / steps * M_PI / 2;
            const qreal c = std::pow(std::max(0.0, std::cos(t)), power), s = std::pow(std::max(0.0, std::sin(t)), power);
            const QPointF p(centre.x() + corner.sx * rx * c, centre.y() + corner.sy * ry * s);
            if (first) {
                path.moveTo(p);
                first = false;
            } else {
                path.lineTo(p);
            }
        }
    }
    path.closeSubpath();
    return path;
}

QVector<Preset> presets()
{
    return {
        {QStringLiteral("rectangle"), i18nc("a shape", "Rectangle"), {0, 2}},
        {QStringLiteral("rounded"), i18nc("a shape: a rectangle with round corners", "Rounded"), {0.25, 2}},
        {QStringLiteral("squircle"), i18nc("a shape between a square and a circle", "Squircle"), {1, 4.5}},
        {QStringLiteral("ellipse"), i18nc("a shape", "Ellipse"), {1, 2}},
        {QStringLiteral("diamond"), i18nc("a shape", "Diamond"), {1, 1}},
        {QStringLiteral("star"), i18nc("a shape: four points curving inward", "Star"), {1, 0.5}},
    };
}

} // namespace KisSuperellipse
