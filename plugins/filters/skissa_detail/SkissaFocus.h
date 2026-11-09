/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAFOCUS_H
#define SKISSAFOCUS_H

#include <QImage>
#include <QPointF>
#include <QRect>

class KisPropertiesConfiguration;

/**
 * Lens blur: the picture out of focus, as a lens shows what isn't at the distance it is focused on. Every point of it
 * spreads its light into the shape of the aperture (a disc, or a polygon of its blades) at its own size, so a
 * defocused light is a bright disc or polygon of its own (bokeh), in linear light, with the picture's whitest parts
 * first standing for the brighter light they were clipped from (SkissaPixels::unclipOf) and the light shown again as
 * a picture that stops at white shows it (reclipOf). Out of focus evenly, or growing away from a band in focus across
 * the picture, as a tilted lens (tilt-shift) or a sloping ground makes it.
 */
namespace SkissaFocus
{

struct Settings {
    /// How far a point spreads where it is most out of focus: the aperture's radius in the picture, in pixels
    float size {12};
    /// The aperture: how many blades it has (0: round) and how they are turned, in degrees
    int blades {0};
    float angle {0};
    /// In focus along a band across the picture through \p center (shares of the image's width and height), at
    /// \p bandAngle degrees, \p bandWidth wide (a share of the half diagonal); the blur grows away from it, to its
    /// full size at the farthest corner. Off: out of focus evenly.
    bool band {false};
    QPointF center {0.5, 0.5};
    float bandAngle {0};
    float bandWidth {0.15f};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    /// The same blur, wherever its band is
    bool sameLook(const Settings &other) const;
    bool changesNothing() const;
};

/// The blur at \p scale (a level of detail) of an image of \p bounds (in pixels at that scale)
class Focus
{
public:
    /// \p clipped: the picture stops at white (integers), so its whites stand for brighter light
    Focus(const Settings &settings, const QRect &bounds, qreal scale, bool clipped = true);

    /// How far it reads around what it makes
    int reach() const;

    /**
     * \p source (linear red, green, blue and alpha over \p sourceRect, which holds \p rect and reach() around it)
     * blurred, written to \p out for \p rect
     */
    void apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const;

private:
    /// How far a point at \p x, \p y spreads, in pixels at this scale
    float spreadAt(float x, float y) const;

    Settings m_settings;
    qreal m_scale {1};
    bool m_clipped {true};
    /// The image: nothing past it spreads
    QRect m_bounds;
    /// The band: its line through the middle (a point and the direction across it), its half width and how far the
    /// farthest corner is past it, in pixels at this scale
    QPointF m_middle;
    QPointF m_across;
    float m_half {0};
    float m_farthest {1};
};

/// \p image (8-bit sRGB), the whole image, blurred at \p scale
QImage applied(const QImage &image, const Settings &settings, qreal scale = 1);

} // namespace SkissaFocus

#endif
