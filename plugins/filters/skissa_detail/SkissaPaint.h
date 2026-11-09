/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAPAINT_H
#define SKISSAPAINT_H

#include <QImage>
#include <QRect>

class KisPropertiesConfiguration;

/**
 * Painting: the picture as paint, by the anisotropic Kuwahara filter (Kyprianidis, Kang and Döllner), with
 * polynomial sector weights (Kyprianidis 2011). Around each pixel an ellipse stretched along the picture's flow (its
 * structure tensor) is split into eight sectors; each pixel takes the sectors' average colors, weighed toward the
 * calmest of them (least spread in Oklab). Flat areas become strokes that follow the shapes, and edges stay sharp,
 * because the sector across an edge is the busiest and counts least.
 */
namespace SkissaPaint
{

struct Settings {
    /// The brush: the ellipse's radius, in pixels
    float size {6};
    /// How strictly the calmest sectors win: soft blends, sharp keeps the strokes' edges crisp
    float sharpness {0.6f};
    /// How much the strokes stretch along the shapes
    float flow {0.7f};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
};

/// Painting at \p scale of the image's size (a level of detail)
class Paint
{
public:
    Paint(const Settings &settings, qreal scale);

    int reach() const;
    /// \p source (linear red, green, blue and alpha over \p sourceRect, which holds \p rect and reach() around it)
    /// painted, written to \p out for \p rect
    void apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const;

private:
    Settings m_settings;
    qreal m_scale {1};
};

/// \p image (8-bit sRGB) painted at \p scale
QImage applied(const QImage &image, const Settings &settings, qreal scale);

} // namespace SkissaPaint

#endif
