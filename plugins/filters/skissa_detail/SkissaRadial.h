/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSARADIAL_H
#define SKISSARADIAL_H

#include <QImage>
#include <QPointF>
#include <QRect>

class KisPropertiesConfiguration;

/**
 * Radial blur: the picture streaked around a middle, toward it (zoom), around it (spin) or both (a spiral), and
 * light streaming from its bright parts along the same streaks (light rays), all in linear light.
 *
 * Each pixel averages the picture along its streak: the points the picture would pass through if it were zoomed and
 * turned about the middle, from the pixel itself toward the middle. Those streaks are the orbits of one zoom-and-turn,
 * logarithmic spirals that become rays for a zoom alone and circles for a turn alone. Along each the average is read
 * from running sums, so a long streak costs what a short one does.
 *
 * Light rays are physical enough: the bright parts (above a threshold) are averaged along the streaks and added as
 * light, so shapes in front of a light cast shadows through its rays, and the rays fade away from it.
 */
namespace SkissaRadial
{

struct Settings {
    /// The middle, as shares of the image's width and height; it may lie past the edges (a light just out of view)
    QPointF center {0.5, 0.5};
    /// How far each streak reaches toward the middle, as a share of the pixel's distance from it
    float zoom {0};
    /// How far the streaks turn around the middle, in degrees
    float spin {0};
    /// How much the picture itself streaks
    float blur {1};
    /// How strongly the bright parts shine along the streaks
    float light {0};
    /// The brightness light shines from, as a share of the brightest parts' (to the eye)
    float threshold {0.8f};
    /// How bright the layer's brightest parts look (cube root of luminance), found by the panel so that every patch
    /// and preview agrees
    float top {1};
    /// How much the light is broken into beams by angle, as by clouds or leaves in front of it
    float shafts {0};
    /// Which beams
    quint32 seed {1};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    /// The same streaks, wherever their middle is
    bool sameLook(const Settings &other) const;
    bool changesNothing() const;
};

/// The look the effect opens with: a zoom burst
Settings zoomed();

/// A radial blur over an image of \p bounds
class Radial
{
public:
    /// \p clipped: the picture stops at white (integers), so its whitest parts stand for brighter light
    Radial(const Settings &settings, const QRect &bounds, bool clipped = true);

    /// The pixels it reads to make \p rect
    QRect sourceOf(const QRect &rect) const;

    /**
     * \p source (linear sRGB red, green, blue and alpha, covering \p sourceRect, which holds \p rect and
     * sourceOf(rect) within the image) streaked, written to \p out for \p rect
     */
    void apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const;

private:
    Settings m_settings;
    bool m_clipped {true};
    QPointF m_center;
    /// The streak's zoom and turn, as the logarithm of its scale and an angle: a point at σ along it is scaled by
    /// exp(σ m_logScale) and turned by σ m_turn about the middle, σ from -m_back to 1 - m_back
    double m_logScale {0};
    double m_turn {0};
    double m_back {0};
    /// The farthest any streak reaches from the middle, in pixels
    double m_farthest {1};
};

/// \p image (8-bit sRGB), a picture of the whole image, streaked
QImage applied(const QImage &image, const Settings &settings);

} // namespace SkissaRadial

#endif
