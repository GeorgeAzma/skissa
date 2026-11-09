/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSASHAPE_H
#define SKISSASHAPE_H

#include <QColor>
#include <QImage>
#include <QRect>

#include <vector>

class KisPropertiesConfiguration;

/**
 * Shape: what the layer's shape (its opaque part) does around itself, from how far every pixel is from its edge.
 * That distance is exact (Felzenszwalb and Huttenlocher's distance transform, in two passes of lower envelopes of
 * parabolas), and the edge is found between pixels from their alpha, so outlines are smooth at any width and cost
 * the same at any size. From it: the shape grown or shrunk (its colors carried out from the nearest edge), an
 * outline, a glow around it, a shadow under it or inside it, and a bevel: the edge as a rounded slope, lit by the
 * light that casts the shadow.
 */
namespace SkissaShape
{

/// Where an outline lies on the edge
enum class Place { Outside, Center, Inside };

/// Where the shadow falls: under the shape, raised over what is below, or inside it, cut into it
enum class Fall { Under, Inside };

struct Settings {
    /// Grown (above 0) or shrunk, in pixels
    float grow {0};
    /// The outline: how wide, where on the edge, its color
    float outline {0};
    Place place {Place::Outside};
    QColor outlineColor {Qt::black};
    /// The glow around it: how far it reaches, how strong, its color
    float glow {0};
    float glowStrength {0.8f};
    QColor glowColor {QColor(255, 214, 120)};
    /// The shadow under it: how far it falls (none at 0) and which way (degrees, as light from the top left falls at
    /// 45), how soft, its color (and how dark, by the color's alpha)
    float shadow {0};
    float shadowAngle {45};
    float shadowSoftness {8};
    QColor shadowColor {QColor(0, 0, 0, 140)};
    Fall shadowFall {Fall::Under};
    /// The bevel: how wide its slope is in pixels (none at 0), how steep (0 to 1, of 80° at its steepest), and whether it rises inside the
    /// edge or across it (an emboss, half of it outside)
    float bevel {0};
    float bevelDepth {0.7f};
    bool emboss {false};
    /// How opaque the layer's paint is where it is solid, found by the panel so that every patch agrees: the edge lies
    /// at half of it, so a layer painted at low opacity still has a shape
    float solid {1};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    bool changesNothing() const;
};

/// The shape at \p scale of the image's size (a level of detail)
class Shape
{
public:
    Shape(const Settings &settings, qreal scale);

    /// How far it reads around what it makes
    int reach() const;

    /**
     * \p source (linear red, green, blue and alpha over \p sourceRect, which holds \p rect and reach() around it)
     * with the shape's effects, written to \p out for \p rect
     */
    void apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const;

private:
    Settings m_settings;
    qreal m_scale {1};
};

/// \p image (8-bit sRGB) with the shape's effects at \p scale
QImage applied(const QImage &image, const Settings &settings, qreal scale);

} // namespace SkissaShape

#endif
