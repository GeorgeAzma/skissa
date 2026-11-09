/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAINK_H
#define SKISSAINK_H

#include <QColor>
#include <QImage>
#include <QRect>

class KisPropertiesConfiguration;

/**
 * Ink: a picture drawn as ink, by the extended difference of Gaussians (Winnemöller's XDoG) along the picture's own
 * flow (Kang's flow-based DoG), as Acerola's shader does it.
 *
 * Lightness (Oklab) blurred at a size and at 1.6 times it; the first, sharpened by their difference, is the picture
 * with its edges of that size exaggerated: dark on the dark side, light on the light side. Where that falls below a
 * lightness it becomes ink, softly or sharply. With flow, the structure tensor of the picture gives the direction
 * along its edges at every pixel, and the result is smoothed along it (a line integral), so lines come out long and
 * even, as drawn, and again after the threshold, so their edges are smooth.
 *
 * From there: several thresholds give tones, each tone can be hatched (lines whose width follows how dark it is, a
 * direction for each tone), and the ink lands on paper, alone, over the picture, or in the picture's own colors.
 */
namespace SkissaInk
{

enum class Colors {
    /// Ink on paper
    Paper,
    /// Ink alone, clear between
    Clear,
    /// Ink over the picture
    Over,
    /// The picture's own colors as ink, on paper
    Colored,
};

struct Settings {
    /// The size of what is drawn, in pixels: the smaller blur's sigma
    float size {1.5f};
    /// How strongly edges are drawn
    float lines {0.5f};
    /// How much of the picture is ink, as a share of its area, darkest first
    float ink {0.3f};
    /// The lightness that share reaches in the layer, found by the panel so that every patch and preview agrees;
    /// below 0, the share is taken as the lightness itself
    float level {-1};
    /// From sharp ink to soft gray
    float softness {0.05f};
    /// How much lines and colors follow the picture's shapes
    float flow {0.5f};
    /// How many inks of different darkness
    int tones {1};
    /// How much each tone is drawn as lines rather than flat
    float hatching {0};
    /// Between hatch lines, in pixels
    float spacing {8};
    /// The first tone's hatch direction, in degrees
    float angle {45};
    Colors colors {Colors::Paper};
    QColor inkColor {Qt::black};
    QColor paperColor {Qt::white};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    /// The same drawing, whatever its colors
    bool sameLook(const Settings &other) const;
    bool changesNothing() const;
};

/// The look the effect opens with
Settings inked();

/// Ink at \p scale of the image's size (a level of detail)
class Ink
{
public:
    Ink(const Settings &settings, qreal scale);

    /// How far it reads around what it makes, at this scale
    int reach() const;

    /**
     * \p source (linear sRGB red, green, blue and alpha, covering \p sourceRect, which holds \p rect and reach()
     * around it within the image) inked, written to \p out for \p rect
     */
    void apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const;

private:
    Settings m_settings;
    qreal m_scale {1};
    /// At this scale: the blurs, the structure tensor's smoothing, the flow's and the edges' smoothing along it
    qreal m_sigma {1};
    qreal m_tensorSigma {1};
    qreal m_flowSigma {0};
    qreal m_smoothSigma {0};
};

/// \p image (8-bit sRGB) inked at \p scale
QImage applied(const QImage &image, const Settings &settings, qreal scale);

} // namespace SkissaInk

#endif
