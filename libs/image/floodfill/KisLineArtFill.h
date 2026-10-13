/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISLINEARTFILL_H
#define KISLINEARTFILL_H

#include <functional>

#include <QRect>

#include "kritaimage_export.h"
#include "kis_types.h"

class KoColor;
class KoColorSpace;

/**
 * Fills that know about line art. A plain flood fill stops where a line begins to darken, which
 * leaves a pale fringe along every line; these fills reach the middle of the line instead, where
 * the region on its other side meets them, as a watershed on how much each pixel differs.
 */
class KRITAIMAGE_EXPORT KisLineArtFill
{
public:
    /**
     * Grows \p region (a flood fill from a pixel of \p seedColor in \p reference) under the lines
     * around it, up to where they meet the areas next to it: the middle of a thin line, the solid
     * part of a thick one. Areas smaller than \p speckSize pixels across (dust, paper between
     * hatching) have no say and are taken in by what surrounds them; so are the corners a gap
     * closing of \p closeGap cut off. With \p underLines false only the open ones are taken in. Ink
     * on \p target, the layer filled, is never covered: when it holds the lines itself, they stay.
     */
    static void grow(KisPixelSelectionSP region, KisPaintDeviceSP reference, const KoColor &seedColor, int threshold,
                     bool underLines, int speckSize, int closeGap, const QRect &bounds, KisPaintDeviceSP target = nullptr);

    /**
     * How wide the lines near \p point are, in pixels: the dark ink, whatever colors are around it. Half
     * of the line crossings (or the share \p share of them) are at most this wide.
     */
    static int lineWidthNear(KisPaintDeviceSP reference, const QPoint &point, const QRect &bounds, qreal share = 0.5);

    /**
     * Flat colors for line art: every area enclosed by the lines of \p lines gets its own color, reaching
     * the middle of the lines. Gaps up to \p closeGap pixels close; areas smaller than \p speckSize
     * across join their neighbors. The largest area touching the edge (the paper around the drawing)
     * stays empty. Returns a device in \p colorSpace, or null if \p cancelled says so.
     */
    static KisPaintDeviceSP flats(KisPaintDeviceSP lines, const QRect &bounds, int closeGap, int speckSize,
                                  const KoColorSpace *colorSpace, const std::function<bool()> &cancelled = {});
};

#endif
