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
 * opacity, so a step of color means the same anywhere, to the eye and against clear. An edge is a sudden change: the
 * color's gradient, taken through derivatives of a Gaussian of 1 px, is high there. Across a gradient (a sky, water,
 * a glow) the color changes slowly, so a gradient is one area. The gradient is worked out once for a picture; edges
 * and areas for any step are quick from it.
 */
class KisColorAreas
{
public:
    /// \p device read over \p bounds
    KisColorAreas(KisPaintDeviceSP device, const QRect &bounds);

    QRect bounds() const;

    /**
     * Where the edges are when an edge is a color step of at least \p step (Oklab distance): the gradient is at least
     * what such a step gives through the Gaussian (0.4 of it), and above what noise gives (three times its median);
     * and, as in Canny's hysteresis, half as much where that continues such an edge, so an edge that fades in one
     * place (two colors that meet in a gradient of each) still holds there. MAX_SELECTED at an edge, over bounds().
     */
    std::vector<quint8> edges(qreal step) const;

    /**
     * The area under a pointer covering \p reach px around \p point (the one most of it is on), MAX_SELECTED over
     * bounds(); \p rect gets what it covers. With \p everywhere, every area of the same color too: one whose mean
     * color and whose spread of colors are each within \p step of its, so a flat shape finds flat shapes of its color
     * and not gradients passing through it.
     */
    std::vector<quint8> area(const std::vector<quint8> &edges, const QPoint &point, int reach, bool everywhere, qreal step, QRect *rect) const;

private:
    struct Private;
    QSharedPointer<const Private> d;
};

#endif
