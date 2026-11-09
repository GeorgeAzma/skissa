/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSALIGHT_H
#define SKISSALIGHT_H

#include <QColor>
#include <QImage>
#include <QRect>

class KisPropertiesConfiguration;

/**
 * Light: the bright parts of the picture scattering light as an eye or a lens does, added in linear light. What
 * shines is what looks brighter (cube root of luminance) than a share of the layer's brightest parts, with a soft
 * knee; the whitest parts shine most, as the clipped light they stand for (an inverse Reinhard curve). It spreads
 * three ways:
 *
 * - Glow: the glare of scattered light, falling off as a power of the distance (about r^-2.6, as the eye's glare
 *   does), made of Gaussians an octave apart whose weights follow that power, up to Size.
 * - Star: the diffraction of an aperture's Blades: each straight edge spreads light in a line square to it, so an even
 *   count makes as many spikes and an odd count twice as many, falling off along them about as 1/r²; the longer
 *   waves spread further (diffraction grows with wavelength), so the spikes' ends turn red.
 * - Streak: the long line of an anamorphic lens, one direction, in its own color.
 * - Halation: film's red halo, the light the film's base reflects back fogging its red layer around what shines.
 */
namespace SkissaLight
{

struct Settings {
    /// What shines: brighter than this share of the brightest parts
    float threshold {0.75f};
    /// How bright the layer's brightest parts look (cube root of luminance), found by the panel so that every patch
    /// and preview agrees
    float top {1};
    /// Glow: how strong, and how far it reaches in pixels
    float glow {1};
    float size {60};
    /// Star: how strong, how long its spikes are in pixels, the aperture's blades (3 to 16), turned by how many degrees
    float star {0};
    float length {80};
    int blades {6};
    float angle {15};
    /// Streak: how strong, how long in pixels, at which angle, in which color
    float streak {0};
    float streakLength {300};
    float streakAngle {0};
    QColor streakColor {QColor(110, 170, 255)};
    /// Halation: how strong, and how far it reaches in pixels
    float halation {0};
    float halationSize {40};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    bool changesNothing() const;
};

/// Light at \p scale of the image's size (a level of detail)
class Light
{
public:
    /// \p clipped: the picture stops at white (integers), so its whitest parts stand for brighter light
    Light(const Settings &settings, qreal scale, bool clipped = true);

    /// How far it reads around what it makes
    int reach() const;

    /**
     * \p source (linear red, green, blue and alpha over \p sourceRect, which holds \p rect and reach() around it)
     * with the light added, written to \p out for \p rect
     */
    void apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const;

private:
    Settings m_settings;
    qreal m_scale {1};
    bool m_clipped {true};
};

/// \p image (8-bit sRGB) with light at \p scale
QImage applied(const QImage &image, const Settings &settings, qreal scale);

} // namespace SkissaLight

#endif
