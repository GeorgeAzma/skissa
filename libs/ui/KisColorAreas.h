/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISCOLORAREAS_H
#define KISCOLORAREAS_H

#include <QRect>
#include <QSharedPointer>

#include <vector>

#include <kis_types.h>

/**
 * A picture as smooth areas parted by edges, for color drops. Colors are compared in Oklab, premultiplied by their
 * opacity, so a step of color means the same anywhere, to the eye and against clear. The step at a pixel is how far
 * apart the picture's colors are 3 px either side of it (smoothed by a Gaussian of 1 px), the most of four directions:
 * across an edge it is the edge's whole step, across a thin line the two sides are alike (a line lies on an area, it
 * doesn't part it), along a gradient it is a small share of its slope. For a step, the areas are the pieces where the
 * step is smaller and the rest is edge. Pixels are ranked once by their step and joined into the tree of every step's
 * areas (a min-tree, as for maximally stable regions), so the area under a pointer is known at every step at once.
 */
class KisColorAreas
{
public:
    /// \p device read over \p bounds
    KisColorAreas(KisPaintDeviceSP device, const QRect &bounds);

    QRect bounds() const;

    /**
     * The step at which the area under a pointer covering \p reach px around \p point is most stable: the one whose
     * size changes least, relative to it, as the threshold moves (Matas's maximally stable regions). An area that holds
     * over a long range of thresholds is a thing in the picture; one that grows steadily is a part of something larger.
     */
    qreal autoStep(const QPoint &point, int reach) const;

    /// A pixel at an edge of the area, read as a blend of the color inside (its nearest pixel in the area) and the
    /// color beyond (its nearest pixel in another area, or a line's core); or, with no beyond (-1), a mark on the area,
    /// which keeps its difference from the area beside it (inside); indexes over bounds()
    struct Blend {
        int at;
        int inside;
        int beyond;
    };

    struct Cover {
        /// MAX_SELECTED in the area, over bounds()
        std::vector<quint8> mask;
        /// What the area covers, in the image
        QRect rect;
        /// Sorted by pixel
        std::vector<Blend> blends;
    };

    /**
     * What a drop covers at \p step: the area most of the pointer is on, and with \p everywhere every area of the same
     * color (its mean color and spread of colors each within \p step, so a flat shape finds flat shapes and not a
     * gradient passing through its color). Its edges are followed to the nearest area on each side, as far as an edge
     * blends (32 px); an edge enclosed in it whose color stays within \p step of the area's (a crease of a gradient)
     * is the area, one that doesn't is a line, blended with its core.
     */
    Cover cover(const QPoint &point, int reach, qreal step, bool everywhere) const;

    /// How far an edge is followed for the colors on its two sides
    static constexpr int edgeReach = 32;

private:
    struct Private;
    QSharedPointer<const Private> d;
};

#endif
