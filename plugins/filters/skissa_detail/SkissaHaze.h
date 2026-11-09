/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAHAZE_H
#define SKISSAHAZE_H

#include <QColor>
#include <QImage>
#include <QRect>

class KisPropertiesConfiguration;

/**
 * Haze: the air between the eye and what is far (Koschmieder's law). Of each pixel's light, e^(-βd) comes through and
 * the rest is the air's own color, lit by the sky: I = J e^(-βd) + A (1 - e^(-βd)), in linear light. Air scatters
 * blue more than red (Rayleigh, as 1/λ⁴, here half of the scattering, the rest the gray of haze's droplets), so far
 * things fade toward the air bluer first. A layer is one distance, as a painter's planes are, or the distance grows
 * across it toward the horizon.
 */
namespace SkissaHaze
{

struct Settings {
    /// How much of the air's color the farthest of the layer takes (in green): 1 - e^(-βd)
    float amount {0.35f};
    /// The air's color, as the sky lights it
    QColor air {QColor(196, 210, 228)};
    /// Farther toward \p angle degrees (as -90 is up), from none at the near edge to all of it at the far one;
    /// off: the whole layer at one distance
    bool gradient {false};
    float angle {-90};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    bool changesNothing() const;
};

/// \p pixels (linear red, green, blue and alpha over \p rect) in haze, in place, in an image of \p bounds (in the
/// same pixels)
void apply(const Settings &settings, const QRect &bounds, const QRect &rect, float *pixels);

/// \p image (8-bit sRGB), the whole image, in haze
QImage applied(const QImage &image, const Settings &settings);

} // namespace SkissaHaze

#endif
