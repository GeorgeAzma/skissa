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
 * Drawing: a picture drawn by hand, its lines by the extended difference of Gaussians (Winnemöller's XDoG) along the
 * picture's own flow (Kang's flow-based DoG), as Acerola's shader does it, and its shading in one of a few ways.
 *
 * Three choices make every look. What is drawn: the size of what makes a line, how strongly edges are drawn, how much
 * of the picture is shaded, and how much lines follow the shapes. How the shading is drawn: solid ink in steps, soft
 * gray, straight hatching, strokes along the shapes, or dots. Where the ink goes: on paper, alone, over the picture,
 * or in the picture's own colors.
 *
 * Lines: lightness (Oklab) blurred at a size and at 1.6 times it; the first, sharpened by their difference, is the
 * picture with its edges of that size exaggerated, dark on the dark side, light on the light side; where that falls
 * below a lightness it is ink. With flow, the structure tensor of the picture gives the direction along its edges at
 * every pixel, and the result is smoothed along it (a line integral), so lines come out long and even, as drawn.
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

/// How the shading is drawn
enum class Shading {
    /// Ink, flat, in a few steps of darkness
    Solid,
    /// Gray, as soft as asked: pencil, charcoal
    Soft,
    /// Straight strokes, crossing where darker
    Hatch,
    /// Strokes along the shapes, as dense as it is dark
    Strokes,
    /// Dots, as dense as it is dark
    Dots,
};

struct Settings {
    /// The size of what makes a line, in pixels: the smaller blur's sigma
    float size {1.5f};
    /// How strongly edges are drawn
    float lines {0.5f};
    /// How much of the picture is shaded, as a share of its area, darkest first
    float ink {0.3f};
    /// The lightness that share reaches in the layer, found by the panel so that every patch and preview agrees;
    /// below 0, the share is taken as the lightness itself
    float level {-1};
    /// How much lines and colors follow the picture's shapes
    float flow {0.5f};
    Shading shading {Shading::Solid};
    /// Solid: how many steps of darkness
    int steps {1};
    /// Soft: from sharp to soft gray
    float softness {0.5f};
    /// Hatch: between strokes, in pixels, and the first strokes' direction in degrees
    float spacing {8};
    float angle {45};
    /// Strokes: how long, in pixels
    float length {8};
    /// Dots: how wide, in pixels
    float dot {3};
    Colors colors {Colors::Paper};
    QColor inkColor {Qt::black};
    QColor paperColor {Qt::white};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    /// The same drawing, whatever its colors
    bool sameLook(const Settings &other) const;
    /// The same shading, by the setting its kind has
    bool sameShading(const Settings &other) const;
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
    /// How far stippling reads around a pixel, at this scale
    qreal dotReach() const;

    Settings m_settings;
    qreal m_scale {1};
    /// At this scale: the blurs, the structure tensor's smoothing, the flow's and the edges' smoothing along it
    qreal m_sigma {1};
    qreal m_tensorSigma {1};
    qreal m_flowSigma {0};
    qreal m_smoothSigma {0};
    /// Strokes along the shapes: how long they are
    qreal m_strokeSigma {0};
    /// The flow is needed: for lines that follow it, or for strokes along it
    bool m_needsFlow {false};
};

/// \p image (8-bit sRGB) drawn at \p scale
QImage applied(const QImage &image, const Settings &settings, qreal scale);

} // namespace SkissaInk

#endif
