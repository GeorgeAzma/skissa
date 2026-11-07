/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSALENS_H
#define SKISSALENS_H

#include <QImage>
#include <QPointF>
#include <QRect>
#include <QVector>

#include <array>

class KisPropertiesConfiguration;

/**
 * Lens: what a camera lens does to the picture it makes, worked out from the optics, all in linear light.
 *
 * A lens bends light by refraction, and glass bends short wavelengths (blue) more than long ones (red): its
 * dispersion. So each wavelength makes its own picture, and the colors of a scene land apart:
 *  - Lateral color (Fringes): each wavelength's picture is a little larger or smaller. Nothing moves at the middle,
 *    and the colors part more the farther out a point is, so edges near the corners get rainbow fringes.
 *  - Axial color (Halo): each wavelength comes into focus at its own distance, so when green is sharp, red and blue
 *    are a little blurred, and bright edges get purple halos (green ones on the other side of focus).
 * The rest is the lens's shape and its glass:
 *  - Distortion (Bulge): a lens magnifies the middle more or less than the edges, so straight lines bow outward
 *    (barrel) or inward (pincushion).
 *  - Vignette: light reaching the sensor at an angle θ is spread over more of it and comes through a narrower
 *    opening, cos⁴ θ in all.
 *  - Glow: some light scatters in the glass and the haze in it, spreading around bright light. Light is moved,
 *    not made: an even area stays as it was.
 *
 * Every size is a share of the half diagonal, as a lens's are of the frame it covers: the same settings look the
 * same on any size of image.
 */
namespace SkissaLens
{

struct Settings {
    /// The middle of the lens, as shares of the image's width and height
    QPointF center {0.5, 0.5};
    /// Toward 1 straight lines bow outward (barrel), toward -1 inward (pincushion)
    float bulge {0};
    /// How much smaller blue's picture is than red's (the F and C lines) at the half diagonal: each wavelength reads
    /// from that much further out; negative, larger
    float fringes {0};
    /// How blurred the colors out of focus are: positive, red and blue (purple halos); negative, green
    float halo {0};
    /// How much darker it is at the half diagonal
    float vignette {0};
    /// The share of light scattered
    float glow {0};
    /// How far the scattered light goes
    float glowSize {0.05f};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    /// The same lens, wherever its middle is
    bool sameLens(const Settings &other) const;
    bool changesNothing() const;
};

/// The look the effect opens with: an old lens, a little soft, darker and colored toward the corners
Settings vintage();

/// A lens over an image of \p bounds, at \p scale of it (a level of detail)
class Lens
{
public:
    Lens(const Settings &settings, const QRect &bounds, qreal scale);

    /// The pixels it reads to make \p rect
    QRect sourceOf(const QRect &rect) const;

    /**
     * \p source (linear sRGB red, green, blue and alpha, covering \p sourceRect, which is sourceOf(rect) within the
     * image) through the lens, written to \p out for \p rect
     */
    void apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const;

private:
    /// Where the pixel at \p x, \p y reads the wavelength with \p magnification from (above 1, further out: that
    /// wavelength's picture comes out smaller)
    QPointF from(qreal x, qreal y, float magnification) const;
    /// The pixels the lens's geometry reads for \p rect, before the blurs
    QRect bentSourceOf(const QRect &rect) const;

    QPointF m_center;
    /// The half diagonal, in pixels at this scale
    qreal m_unit {1};
    /// Distortion: r → r (1 + k r²) norm, with r in half diagonals
    float m_k {0};
    float m_norm {1};
    bool m_bends {false};
    /// The wavelengths: each one's magnification and its share of red, green and blue (and none of a fourth, for
    /// reading pixels four floats at a time)
    QVector<float> m_magnification;
    QVector<std::array<float, 4>> m_weights;
    /// Vignette: cos⁴ θ = 1 / (1 + t r²)²
    float m_vignetteT {0};
    /// Halo: the blur of red, green and blue, in pixels
    std::array<qreal, 3> m_halo {};
    int m_haloReach {0};
    /// Glow: what stays, and the light scattered to each size
    float m_keep {1};
    QVector<qreal> m_glowSigmas;
    float m_glowEach {0};
    int m_glowReach {0};
};

/// \p image (8-bit sRGB), a picture of the whole image at \p scale of it, through the lens
QImage applied(const QImage &image, const Settings &settings, qreal scale);

} // namespace SkissaLens

#endif
