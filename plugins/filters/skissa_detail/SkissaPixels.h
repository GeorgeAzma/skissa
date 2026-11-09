/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAPIXELS_H
#define SKISSAPIXELS_H

#include <QRect>

#include <algorithm>
#include <cmath>
#include <vector>

#include <kis_types.h>

class KoColorSpace;

/// Reading and writing a layer's pixels for an effect
namespace SkissaPixels
{

/**
 * The bytes of \p rect as they were when the effect began. An effect runs in patches side by side on one layer, and
 * a neighbour's may already be written: Krita's own filters read its old data for this.
 */
void readOriginal(KisPaintDeviceSP device, const QRect &rect, quint8 *out);

/// A number that changes with any byte of \p rect as it was when the effect began
quint64 originalKey(KisPaintDeviceSP device, const QRect &rect);

/// \p rect as it was when the effect began, as linear sRGB red, green, blue and alpha
std::vector<float> readLinear(KisPaintDeviceSP device, const QRect &rect);

/// \p pixels (linear sRGB red, green, blue and alpha) into \p rect, clamped to 0..1 unless the device holds floats
void writeLinear(KisPaintDeviceSP device, const QRect &rect, const float *pixels);

/// Whether \p cs holds values past white (floats)
bool holdsPastWhite(const KoColorSpace *cs);

/// How much brighter than itself the light a color of \p luminance stands for, where the picture stops at white: its
/// whitest parts stand for light far brighter than white that was clipped (the inverse of a Reinhard curve, up to ten
/// times)
inline float unclipOf(float luminance)
{
    return 1 / (1 - 0.9f * std::clamp(luminance, 0.0f, 1.0f));
}

/**
 * How much light a color (linear, premultiplied red, green, blue and alpha) gives off past \p cutoff, as a factor of
 * the color: what of its light looks brighter than the cutoff (the cube root of luminance, with a soft knee), and,
 * where the picture stops at white (\p clipped), as the brighter light it stands for (unclipOf). Light and Radial
 * blur's rays share it, so a threshold means the same in each.
 */
inline float shineOf(const float *premultiplied, float cutoff, bool clipped)
{
    const float alpha = premultiplied[3];
    if (alpha <= 1e-6f) return 0;
    const float luminance = std::max(0.0f, (0.2126f * premultiplied[0] + 0.7152f * premultiplied[1] + 0.0722f * premultiplied[2]) / alpha);
    const float seen = std::cbrt(luminance);
    constexpr float knee = 0.08f;
    const float over = seen - cutoff;
    const float excess = over <= -knee ? 0.0f : over >= knee ? over : (over + knee) * (over + knee) / (4 * knee);
    if (excess <= 0 || seen <= 1e-4f) return 0;
    // the share of its light above the cutoff, as light: the cube of what is seen
    const float below = std::max(0.0f, seen - excess) / seen;
    const float share = 1 - below * below * below;
    return clipped ? share * unclipOf(luminance) : share;
}

} // namespace SkissaPixels

#endif
