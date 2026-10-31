/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISRESAMPLING_H
#define KISRESAMPLING_H

#include <QRect>
#include <QString>

#include "kis_filter_strategy.h"
#include "kis_types.h"
#include "kritaimage_export.h"

class KoColorSpace;

/**
 * Smooth: Lanczos3, each new pixel kept within the range of the two source pixels
 * nearest it, so it stays sharp without halos; shrinking in linear light, so thin
 * bright lines keep their brightness.
 */
class KRITAIMAGE_EXPORT KisSmoothFilterStrategy : public KisFilterStrategy
{
public:
    KisSmoothFilterStrategy();
    QString description() override;
    qreal valueAt(qreal t, qreal weightsPositionScale) const override;
    bool clampsToNeighbours() const override { return true; }
    bool shrinksInLinearLight() const override { return true; }
};

/**
 * Pixel: each new pixel takes the source pixels it covers, by how much it covers
 * them. At whole-number scales that copies pixels exactly; at others every pixel
 * keeps one size and only the seams between them blend.
 */
class KRITAIMAGE_EXPORT KisPixelFilterStrategy : public KisFilterStrategy
{
public:
    KisPixelFilterStrategy();
    QString description() override;
    qreal valueAt(qreal t, qreal weightsPositionScale) const override;
    bool knowsEnlargement() const override { return true; }
};

namespace KisResampling
{
/// Keeps \p pixel's channels within those of \p a and \p b
KRITAIMAGE_EXPORT void clampBetween(const KoColorSpace *cs, quint8 *pixel, const quint8 *a, const quint8 *b);

/// Whether \p device looks like pixel art within \p rect: few colors and hard edges
KRITAIMAGE_EXPORT bool looksLikePixelArt(KisPaintDeviceSP device, const QRect &rect);

/// A copy of \p device in linear light when shrinking it with \p filter should be done
/// there, or null
KRITAIMAGE_EXPORT KisPaintDeviceSP linearCopy(KisPaintDeviceSP device, KisFilterStrategy *filter, qreal xscale, qreal yscale);

/// Writes \p linear, a linearCopy() since transformed, back into \p device: \p cleared is
/// what the transform took from, \p written what it wrote
KRITAIMAGE_EXPORT void writeBack(KisPaintDeviceSP linear, KisPaintDeviceSP device, const QRect &cleared, const QRect &written);

/// The id Krita itself knows, for saving where only Krita's filters can be read
KRITAIMAGE_EXPORT QString kritaFilterId(const QString &id);
}

#endif
