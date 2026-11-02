/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSANOISE_H
#define SKISSANOISE_H

#include <QImage>
#include <QRect>
#include <QVector>

#include <widgets/KisStudioCurve.h>

class KisPropertiesConfiguration;

/**
 * Noise: random variation made of the same sizes Detail works with. White noise, the same at a pixel wherever and
 * however often it is drawn (a hash of the pixel and the seed), is split into half-octave bands, each brought to
 * the strength the size curve gives it. The bands of large sizes are made on coarser grids and smoothly enlarged,
 * so clouds cost no more than grain. One such field goes where each target asks: lightness, color (two fields, in
 * Oklab's two color directions), transparency, or the pixels' place (two fields, sideways and down: scatter or
 * wobble).
 */
namespace SkissaNoise
{

/// Grain is Noise with a few sliders, its own effect
enum class Kind { Grain, Noise };

enum Target { Lightness, Color, Transparency, Displace, Targets };

struct Settings {
    /// How strong the noise of each size is: x as Detail's radius axis, y from none to full
    KisStudioCurve sizes;
    /// Lightness and color as fractions of their full strength, transparency likewise, displacement in pixels
    float amount[Targets] {0.3f, 0, 0, 0};
    /// 0: as strong in every tone; 1: only in the midtones, as film grain
    float mids {0.6f};
    quint32 seed {0};

    Settings();
    static Settings from(Kind kind, const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }

    bool changesNothing() const;
    /// How far from a pixel it reads the image, at \p scale
    int reach(qreal scale) const;
};

KisStudioCurve fineSizes();

/// A noise field of unit strength whose sizes follow a curve, at a scale of the image (a level of detail)
class Field
{
public:
    Field(const KisStudioCurve &sizes, qreal scale);

    bool isEmpty() const { return m_levels.isEmpty(); }
    /// The field over \p rect (image pixels at the scale) into \p out, row by row; \p key tells fields apart
    void render(const QRect &rect, quint32 key, float *out) const;

private:
    struct Level {
        int step {1}; ///< pixels between its grid points
        bool anchored {false}; ///< its pixels are the full image's, not this scale's
        QVector<qreal> sigmas; ///< in grid points, finest first; 0 is the white noise itself
        QVector<float> weights;
    };
    QVector<Level> m_levels;
    float m_norm {1};
    qreal m_scale {1};
};

/**
 * \p settings applied at \p scale to the linear sRGB pixels (red, green, blue, alpha) of \p source, which covers
 * \p sourceRect, written to \p out for \p rect (inside it). \p fit keeps colors within sRGB.
 */
void apply(const Settings &settings, qreal scale, const float *source, const QRect &sourceRect, const QRect &rect, float *out, bool fit);

/// \p image (8-bit sRGB), its top left at \p corner of the image, with \p settings applied
QImage applied(const QImage &image, const QPoint &corner, const Settings &settings);

} // namespace SkissaNoise

#endif
