/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISSUPERELLIPSE_H
#define KISSUPERELLIPSE_H

#include <QColor>
#include <QIcon>
#include <QObject>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include "kritaui_export.h"

/**
 * One shape for every box: a regular polygon of a few sides, turned, stretched to the box, its corners Lamé curves
 * (superellipses) between the two sides that meet there. Four sides at no turn is the box itself.
 *
 * Roundness is how far each corner reaches along its two sides, as a share of the longest half side, each kept
 * within its own half side: none is the polygon, a little rounds its corners, all of it leaves no straight side (an
 * ellipse from a box). Curve is the corners' exponent in the frame of their two sides: 2 round, more flatter and
 * squarer (4 to 5 a squircle), 1 a straight cut (a box becomes a diamond), under 1 pinched inward, a star whose
 * points are the sides' middles.
 */
namespace KisSuperellipse
{

constexpr qreal leastCurve = 0.3;
constexpr qreal mostCurve = 12;
constexpr int leastSides = 3;
constexpr int mostSides = 12;

struct Look {
    int sides {4};
    qreal roundness {0};
    qreal curve {2};
    /// Degrees, clockwise on screen, before the shape is stretched to its box
    qreal angle {0};
    bool operator==(const Look &other) const
    {
        return sides == other.sides && qAbs(roundness - other.roundness) < 1e-3 && qAbs(curve - other.curve) < 1e-3
            && qAbs(angle - other.angle) < 1e-3;
    }
    bool operator!=(const Look &other) const { return !(*this == other); }
};

/// \p box's shape, closed, filling the box
KRITAUI_EXPORT QPainterPath pathOf(const QRectF &box, const Look &look);

struct Preset {
    QString id;
    QString name;
    Look look;
};

/// Two rows of six: the box's shapes (Rectangle to Star), then polygons (Triangle to Five-point star)
KRITAUI_EXPORT QVector<Preset> presets();
constexpr int presetsPerRow = 6;

/// Its key as Find, the rail and the top bar know it: "shape:" and a preset's id, or its numbers
KRITAUI_EXPORT QString keyOf(const Look &look);
/// The look a key names; false if it names none
KRITAUI_EXPORT bool lookOf(const QString &key, Look *look);
/// Its preset's name, or "Shape" and its sides
KRITAUI_EXPORT QString nameOf(const Look &look);
/// The look drawn in \p ink, \p size px square, its line as thick at any size
KRITAUI_EXPORT QIcon iconOf(const Look &look, const QColor &ink, int size = 24);

/// The look the shape tool draws with
KRITAUI_EXPORT Look current();
/// The shape tool draws with \p look from now on (Watch::changed())
KRITAUI_EXPORT void use(const Look &look);
/// \p look as the shape tool's, with the tool picked
KRITAUI_EXPORT void draw(const Look &look);

/// Tells those showing the shape tool's look that it changed
class KRITAUI_EXPORT Watch : public QObject
{
    Q_OBJECT
public:
    static Watch *instance();
Q_SIGNALS:
    void changed();
};

} // namespace KisSuperellipse

#endif
