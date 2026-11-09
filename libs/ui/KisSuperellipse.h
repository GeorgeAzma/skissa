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
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#include "kritaui_export.h"

/**
 * One shape for every box: a regular polygon of a few sides, turned, stretched to the box, its corners Lamé curves
 * (superellipses) between the two sides that meet there. Four sides at no turn is the box itself.
 *
 * Roundness is how far each corner reaches along its two sides, as a share of the shortest half side, so corners are
 * as round on a long box as on a square one: none is the polygon, all of it a pill (a circle from a square). Curve is
 * the corners' exponent in the frame of their two sides: 2 round, more flatter and squarer (4 to 5 a squircle's), 1
 * a straight cut, under 1 pinched inward, a star whose points are the sides' middles.
 *
 * Stretched, the shape is made in a square and stretched to its box, corners and all, Roundness then a share of the
 * longest half side: an ellipse, a diamond, a star fill any box.
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
    /// Made in a square and stretched to the box, corners too
    bool stretched {false};
    bool operator==(const Look &other) const
    {
        return sides == other.sides && qAbs(roundness - other.roundness) < 1e-3 && qAbs(curve - other.curve) < 1e-3
            && qAbs(angle - other.angle) < 1e-3 && stretched == other.stretched;
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

/// The settings registry's ids of the look's parts, in the order of Look's members
KRITAUI_EXPORT QStringList settingIds();

/// The look the shape tool draws with
KRITAUI_EXPORT Look current();
/// The shape tool draws with \p look from now on (Watch::changed())
KRITAUI_EXPORT void use(const Look &look);
/// A part of the look was written (or taken back): Watch::changed() once, after the step's other parts
KRITAUI_EXPORT void written();
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
