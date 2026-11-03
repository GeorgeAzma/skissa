/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAFEWER_H
#define SKISSAFEWER_H

#include <QColor>
#include <QImage>
#include <QRect>
#include <QVector>

#include <vector>

class KisPropertiesConfiguration;

/**
 * Fewer colors: every pixel takes the nearest of a few colors, measured in Oklab, so near means looks near. The
 * colors are steps of each channel (posterize), grays, colors found in the image, or a palette's. Dithering mixes
 * neighbouring colors so that from a step back they average to the pixel's own color, in linear light: a pattern
 * (Bayer's) or noise (blue noise, without clumps), both fixed to the image's pixels, or error diffusion
 * (Floyd and Steinberg's), which runs through the whole image in order.
 */
namespace SkissaFewer
{

enum class Source { Image, Levels, Gray, Palette };
enum class Dither { None, Pattern, Noise, Diffuse };

struct Settings {
    Source source {Source::Levels};
    /// Steps of each channel, grays, or colors taken from the image
    int count {4};
    /// The image's or the palette's colors
    QVector<QColor> colors;
    /// The palette's name, to show which it was
    QString palette;
    Dither dither {Dither::None};
    /// How fully dithering mixes colors
    float amount {1};
    /// Added to the Oklab lightness before choosing: moves where the steps fall
    float lightness {0};

    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
    bool takesColors() const { return source == Source::Image || source == Source::Palette; }
};

/// The colors a pixel chooses among
class Quantizer
{
public:
    explicit Quantizer(const Settings &settings);

    int size() const { return int(m_rgb.size() / 3); }
    /// The color nearest \p rgb (linear sRGB) in Oklab
    int nearest(const float rgb[3]) const;
    const float *colorOf(int index) const { return m_rgb.data() + size_t(index) * 3; }
    float lightnessOf(int index) const { return m_lightness[size_t(index)]; }

private:
    Source m_source;
    int m_count {2};
    /// Linear sRGB and Oklab lightness of each color; the image's and palette's sorted by lightness
    std::vector<float> m_rgb;
    std::vector<float> m_lightness;
    std::vector<float> m_lab;
};

/// The pixel moved by the lightness setting, in place
void shift(float rgb[3], float lightness);

/**
 * \p pixels (linear red, green, blue and alpha over \p rect, in image pixels) with fewer colors, in place: no
 * dithering, a pattern or noise. What is fully transparent is left as it is.
 */
void apply(const Settings &settings, const Quantizer &quantizer, float *pixels, const QRect &rect);

/// Error diffusion, a row at a time from the top, each the other way across
class Diffuser
{
public:
    Diffuser(const Settings &settings, const Quantizer &quantizer, int width);

    /// The colors chosen for \p row (linear red, green, blue and alpha); \p none where it is fully transparent
    void row(const float *row, quint16 *out);

    static constexpr quint16 none = 0xffff;

private:
    const Settings &m_settings;
    const Quantizer &m_quantizer;
    int m_width;
    int m_y {0};
    std::vector<float> m_error;
    std::vector<float> m_nextError;
};

/// \p count colors that stand for \p image's, found by k-means in Oklab, darkest first
QVector<QColor> colorsOf(const QImage &image, int count);

/// \p image (8-bit sRGB, its top left at \p corner of the image) with fewer colors
QImage applied(const QImage &image, const QPoint &corner, const Settings &settings);

} // namespace SkissaFewer

#endif
