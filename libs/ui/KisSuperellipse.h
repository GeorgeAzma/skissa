/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSUPERELLIPSE_H
#define KISSUPERELLIPSE_H

#include <QPainterPath>
#include <QtGlobal>
#include <QRectF>
#include <QString>
#include <QVector>

#include "kritaui_export.h"

/**
 * One shape for every box: a rectangle whose corners are superellipses (Lamé curves, |x/rx|^n + |y/ry|^n = 1).
 * Roundness is how far the corners reach, as a share of the box's longer half side, each kept within its own half
 * side: none is a rectangle, a little rounds the corners with circles' worth of curve, as much as the shorter half
 * side makes a pill, and all of it the whole box, an ellipse. Curve is the corners' exponent: 2 round, more flatter
 * and squarer (4 to 5 a squircle), 1 a straight cut (a diamond when the corners are the whole box), under 1 pinched
 * inward (a four-pointed star).
 */
namespace KisSuperellipse
{

constexpr qreal leastCurve = 0.25;
constexpr qreal mostCurve = 12;

struct Look {
    qreal roundness {0};
    qreal curve {2};
    bool operator==(const Look &other) const { return qAbs(roundness - other.roundness) < 1e-3 && qAbs(curve - other.curve) < 1e-3; }
};

/// \p box's shape, closed, clockwise on screen
KRITAUI_EXPORT QPainterPath pathOf(const QRectF &box, const Look &look);

struct Preset {
    QString id;
    QString name;
    Look look;
};

/// Rectangle, Rounded, Squircle, Ellipse, Diamond and Star
KRITAUI_EXPORT QVector<Preset> presets();

} // namespace KisSuperellipse

#endif
