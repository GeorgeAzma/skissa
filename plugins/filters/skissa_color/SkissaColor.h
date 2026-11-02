/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSACOLOR_H
#define SKISSACOLOR_H

#include <QPointF>
#include <QString>
#include <QVector>

#include <array>

class KisPropertiesConfiguration;

/**
 * The Color effect: every pixel's color changed by its color alone, in OKLCH. Curves give each of lightness,
 * chroma and hue by each of them (lightness by lightness is the tone curve, chroma by hue the saturation of one
 * hue), then a white balance, tints for the shadows and highlights, and red, green and blue curves.
 */
namespace SkissaColor
{

enum Axis { Lightness = 0, Chroma = 1, Hue = 2 };

/// The chroma at the right end of the chroma axis: sRGB's most vivid colors reach about this
constexpr float chromaRange = 0.32f;
/// How far a curve at its top or bottom moves lightness (OKLab), and hue (degrees), where it changes them by another axis
constexpr float lightnessShift = 0.3f;
constexpr float hueShift = 60.0f;

/// Points from left to right, both coordinates 0 to 1
struct Curve {
    QVector<QPointF> points;

    /// Krita's curve text, "x,y;x,y;"
    QString toString() const;
    static Curve fromString(const QString &text, const Curve &fallback);
    bool operator==(const Curve &other) const;
    bool operator!=(const Curve &other) const { return !(*this == other); }
};

/// Whether the curve of \p change by \p by gives the new value (its default the diagonal) or a change (flat)
inline bool isMapping(int by, int change) { return by == change; }
Curve defaultCurve(int by, int change);
Curve defaultRgbCurve();
/// \p count values of the curve, evenly across 0 to 1; a curve by hue wraps around
QVector<float> sampled(const Curve &curve, bool wraps, bool mapping, int count);

/// Where OKLCH hue \p degrees is on the hue axis, spaced as an HSV wheel (pure colors evenly), 0 to 1
float axisOfHue(float degrees);
/// The OKLCH hue, in degrees, at \p x on the hue axis
float hueOfAxis(float x);

struct Settings {
    std::array<std::array<Curve, 3>, 3> curves; ///< [by][change]
    std::array<Curve, 3> rgb;
    /// OKLab a and b: the cast added to the whole image (moved toward orange, it warms), and the tints
    QPointF balance;
    QPointF shadows;
    QPointF highlights;

    Settings();
    static Settings from(const KisPropertiesConfiguration &config);
    void writeTo(KisPropertiesConfiguration &config) const;
    bool operator==(const Settings &other) const;
    bool operator!=(const Settings &other) const { return !(*this == other); }
};

QString curveKey(int by, int change);
QString rgbKey(int channel);

/// The settings made into tables, for pixels
class Recipe
{
public:
    explicit Recipe(const Settings &settings);

    /// Changes linear sRGB \p rgb; \p fit keeps the result within sRGB (integer images), giving up chroma
    void apply(float rgb[3], bool fit) const;
    bool changesNothing() const { return m_nothing; }

private:
    static constexpr int size = 1024;
    std::array<std::array<QVector<float>, 3>, 3> m_curves;
    std::array<QVector<float>, 3> m_rgb;
    std::array<std::array<bool, 3>, 3> m_used {};
    bool m_anyRgb {false};
    bool m_oklch {false};
    bool m_byHue {false};
    bool m_turns {false};
    bool m_nothing {true};
    float m_gain[3] {1, 1, 1};
    QPointF m_shadows;
    QPointF m_highlights;
};

} // namespace SkissaColor

#endif
