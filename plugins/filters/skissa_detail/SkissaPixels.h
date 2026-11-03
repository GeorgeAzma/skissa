/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAPIXELS_H
#define SKISSAPIXELS_H

#include <QRect>

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

/// \p rect as it was when the effect began, as linear sRGB red, green, blue and alpha
std::vector<float> readLinear(KisPaintDeviceSP device, const QRect &rect);

/// \p pixels (linear sRGB red, green, blue and alpha) into \p rect, clamped to 0..1 unless the device holds floats
void writeLinear(KisPaintDeviceSP device, const QRect &rect, const float *pixels);

/// Whether \p cs holds values past white (floats)
bool holdsPastWhite(const KoColorSpace *cs);

} // namespace SkissaPixels

#endif
