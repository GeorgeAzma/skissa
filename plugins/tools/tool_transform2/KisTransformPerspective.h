/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISTRANSFORMPERSPECTIVE_H
#define KISTRANSFORMPERSPECTIVE_H

#include <QLineF>
#include <QPolygonF>
#include <QTransform>
#include <QVector>

#include "kritatooltransform_export.h"

class QPainter;
class QPen;
class KoSnapGuide;
class KisCoordinatesConverter;
class ToolTransformArgs;
class TransformTransactionProperties;

/**
 * Perspective inside free transform. Quads are four image points in the
 * order top left, top right, bottom right, bottom left of the box.
 */
namespace KisTransformPerspective
{

/**
 * Sets free transform \p args (scale, shear, rotations and position; bounds
 * rotation and camera kept) so that \p src lands on \p dst. Fails when no
 * free transform does, such as for a quad that isn't convex.
 */
KRITATOOLTRANSFORM_EXPORT bool fit(ToolTransformArgs &args, const QPolygonF &src, const QPolygonF &dst);

/// Where the quad's top and bottom edges (\p across) or its sides meet
KRITATOOLTRANSFORM_EXPORT bool vanishingPoint(const QPolygonF &quad, bool across, QPointF *point);

/// The quad with its vanishing point moved to \p pos: the far edge stays, the near corners slide along the near edge
KRITATOOLTRANSFORM_EXPORT QPolygonF withVanishingPoint(const QPolygonF &quad, bool across, const QPointF &pos);

/**
 * The transform showing \p quad, placed on something seen at an angle,
 * straight on: an upright rectangle with the proportions it has in the
 * world (estimated around \p principal, the photo's center), as large as
 * the quad's longer edges.
 */
KRITATOOLTRANSFORM_EXPORT bool flattening(const QPolygonF &quad, const QPointF &principal, qreal fallbackFocal, QTransform *result);

/// The quad's edges drawn on out to where they meet
void paintVanishingLines(QPainter &gc, const QPolygonF &quad, const QTransform &imageToView, const QPen &pen);

/// What a dragged corner catches on: guides, the canvas edges and other layers' corners
class Catcher
{
public:
    void collect(KoSnapGuide *snapGuide, const TransformTransactionProperties &transaction, const KisCoordinatesConverter *converter);
    void clear();

    /**
     * \p pos caught within \p radius: on a point, where two lines cross, or
     * on a line; lines include those from each of \p neighbours (the
     * corner's fixed neighbours) to the guides' vanishing points.
     */
    QPointF corner(const QPointF &pos, qreal radius, const QVector<QPointF> &neighbours) const;

private:
    QVector<QPointF> m_points;
    QVector<QLineF> m_lines;
    QVector<QPointF> m_vanishing;
};

}

#endif
