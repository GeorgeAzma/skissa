/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaColor.h"

#include <QColor>

#include <algorithm>
#include <cmath>

#include <kis_properties_configuration.h>

#include "KoCompositeOpMix.h"
#include "KoCompositeOpOklch.h"

namespace SkissaColor
{

namespace
{

constexpr float degrees = float(180.0 / M_PI);
const char axisLetters[] = "lch";
const char rgbLetters[] = "rgb";

/// The OKLCH hues of the pure colors around an HSV wheel, a quarter degree apart, unwrapped and always rising
const QVector<float> &wheelHues()
{
    static const QVector<float> hues = []() {
        const int count = 1441;
        QVector<float> hues(count);
        float turns = 0;
        for (int i = 0; i < count; i++) {
            const QColor pure = QColor::fromHsvF(std::fmod(i / 1440.0, 1.0), 1.0, 1.0);
            const float rgb[3] = {float(std::pow((pure.redF() + 0.055) / 1.055, 2.4)),
                                  float(std::pow((pure.greenF() + 0.055) / 1.055, 2.4)),
                                  float(std::pow((pure.blueF() + 0.055) / 1.055, 2.4))};
            float lab[3];
            KoMixing::linearToOklab(rgb, lab);
            float h = std::atan2(lab[2], lab[1]) * degrees + turns;
            if (i > 0 && h < hues[i - 1] - 180) {
                turns += 360;
                h += 360;
            }
            // around blue the hues stall and fall back a little: rising slowly instead, each has one place
            hues[i] = i > 0 ? std::max(h, hues[i - 1] + 0.01f) : h;
        }
        return hues;
    }();
    return hues;
}

/// Where each tenth of a degree of OKLCH hue, from the first wheel hue on, is on the axis
const QVector<float> &axisTable()
{
    static const QVector<float> table = []() {
        const QVector<float> &hues = wheelHues();
        QVector<float> table(3601);
        int i = 0;
        for (int k = 0; k <= 3600; k++) {
            const float h = hues.first() + k * 0.1f;
            while (i < hues.size() - 2 && hues[i + 1] < h) i++;
            const float span = hues[i + 1] - hues[i];
            const float along = span > 0 ? std::clamp((h - hues[i]) / span, 0.0f, 1.0f) : 0.0f;
            table[k] = std::min(1.0f, (i + along) / 1440.0f);
        }
        return table;
    }();
    return table;
}

inline float read(const QVector<float> &table, float x)
{
    const float at = std::clamp(x, 0.0f, 1.0f) * (table.size() - 1);
    const int i = std::min(int(at), table.size() - 2);
    return table[i] + (table[i + 1] - table[i]) * (at - i);
}

/// The sRGB curve; \p quick reads it from tables between 0 and 1 (off it by at most 1e-5)
inline float encoded(float c, bool quick)
{
    if (quick && c >= 0 && c <= 1) return KoMixing::readTable<KoMixing::Curves::toSrgbSteps>(KoMixing::curves().toSrgb, c);
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
}

inline float decoded(float c, bool quick)
{
    if (quick && c >= 0 && c <= 1) return KoMixing::readTable<KoMixing::Curves::toLinearSteps>(KoMixing::curves().toLinear, c);
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

Curve defaultCurve(int by, int change)
{
    return isMapping(by, change) ? defaultRgbCurve() : Curve{{QPointF(0, 0.5), QPointF(1, 0.5)}};
}

Curve defaultRgbCurve()
{
    return Curve{{QPointF(0, 0), QPointF(1, 1)}};
}

QVector<float> sampled(const Curve &curve, bool wraps, bool mapping, int count)
{
    if (curve.points.isEmpty()) return sampled(mapping ? defaultRgbCurve() : Curve{{QPointF(0, 0.5), QPointF(1, 0.5)}}, wraps, mapping, count);
    return curve.sampled(count, wraps, wraps && mapping);
}

float axisOfHue(float hue)
{
    const QVector<float> &table = axisTable();
    float h = std::fmod(hue - wheelHues().first(), 360.0f);
    if (h < 0) h += 360;
    const float at = h * 10;
    const int i = std::min(int(at), table.size() - 2);
    return table[i] + (table[i + 1] - table[i]) * (at - i);
}

float hueOfAxis(float x)
{
    const QVector<float> &hues = wheelHues();
    float along = std::fmod(x, 1.0f);
    if (along < 0) along += 1;
    const float at = along * 1440;
    const int i = std::min(int(at), hues.size() - 2);
    return hues[i] + (hues[i + 1] - hues[i]) * (at - i);
}

void mapAt(const Stops &stops, bool steps, float at, float lab[3])
{
    auto labOf = [](const QColor &color, float *out) {
        const float rgb[3] = {KoMixing::linearOf(float(color.redF())), KoMixing::linearOf(float(color.greenF())), KoMixing::linearOf(float(color.blueF()))};
        KoMixing::linearToOklab(rgb, out);
    };
    if (stops.isEmpty()) {
        lab[0] = at;
        lab[1] = lab[2] = 0;
        return;
    }
    int after = 0;
    while (after < stops.size() && stops[after].first < at) after++;
    if (after == 0 || after == stops.size()) {
        labOf(stops[after == 0 ? 0 : stops.size() - 1].second, lab);
        return;
    }
    const qreal span = std::max(1e-6, stops[after].first - stops[after - 1].first);
    const float t = float((at - stops[after - 1].first) / span);
    if (steps) {
        labOf(stops[t < 0.5f ? after - 1 : after].second, lab);
        return;
    }
    float from[3], to[3];
    labOf(stops[after - 1].second, from);
    labOf(stops[after].second, to);
    for (int c = 0; c < 3; c++) lab[c] = from[c] + (to[c] - from[c]) * t;
}

QString curveKey(int by, int change)
{
    return QStringLiteral("curve_") + QLatin1Char(axisLetters[by]) + QLatin1Char(axisLetters[change]);
}

QString rgbKey(int channel)
{
    return QStringLiteral("curve_") + QLatin1Char(rgbLetters[channel]);
}

Settings::Settings()
{
    for (int by = 0; by < 3; by++) {
        for (int change = 0; change < 3; change++) curves[by][change] = defaultCurve(by, change);
        rgb[by] = defaultRgbCurve();
    }
}

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings settings;
    for (int by = 0; by < 3; by++) {
        for (int change = 0; change < 3; change++) {
            settings.curves[by][change] = Curve::fromString(config.getString(curveKey(by, change)), defaultCurve(by, change));
        }
        settings.rgb[by] = Curve::fromString(config.getString(rgbKey(by)), defaultRgbCurve());
    }
    auto point = [&config](const char *name) {
        return QPointF(config.getDouble(QLatin1String(name) + QStringLiteral("_a"), 0.0),
                       config.getDouble(QLatin1String(name) + QStringLiteral("_b"), 0.0));
    };
    settings.balance = point("balance");
    settings.shadows = point("shadows");
    settings.highlights = point("highlights");
    // the map as "lightness:#color" pairs
    Q_FOREACH (const QString &stop, config.getString("map").split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const int colon = stop.indexOf(QLatin1Char(':'));
        if (colon < 0) continue;
        const QColor color(stop.mid(colon + 1).trimmed());
        if (color.isValid()) settings.map << qMakePair(qBound(0.0, stop.left(colon).toDouble(), 1.0), color);
    }
    std::stable_sort(settings.map.begin(), settings.map.end(), [](const QPair<qreal, QColor> &a, const QPair<qreal, QColor> &b) { return a.first < b.first; });
    settings.mapSteps = config.getBool("map_steps", false);
    settings.mapKeepsLightness = config.getBool("map_keep", false);
    settings.mapStrength = float(qBound(0.0, config.getDouble("map_strength", 1.0), 1.0));
    return settings;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    // only what differs from the defaults, so that a file says what was changed
    auto curve = [&config](const QString &key, const Curve &value, const Curve &fallback) {
        if (value == fallback) config.removeProperty(key);
        else config.setProperty(key, value.toString());
    };
    for (int by = 0; by < 3; by++) {
        for (int change = 0; change < 3; change++) curve(curveKey(by, change), curves[by][change], defaultCurve(by, change));
        curve(rgbKey(by), rgb[by], defaultRgbCurve());
    }
    auto point = [&config](const char *name, const QPointF &value) {
        const QString a = QLatin1String(name) + QStringLiteral("_a");
        const QString b = QLatin1String(name) + QStringLiteral("_b");
        if (value.isNull()) {
            config.removeProperty(a);
            config.removeProperty(b);
        } else {
            config.setProperty(a, value.x());
            config.setProperty(b, value.y());
        }
    };
    point("balance", balance);
    point("shadows", shadows);
    point("highlights", highlights);
    QStringList stops;
    for (const auto &stop : map) stops << QString::number(stop.first, 'g', 4) + QLatin1Char(':') + stop.second.name();
    for (const char *key : {"map", "map_steps", "map_keep", "map_strength"}) config.removeProperty(QLatin1String(key));
    if (!map.isEmpty()) {
        config.setProperty("map", stops.join(QLatin1Char(';')));
        if (mapSteps) config.setProperty("map_steps", true);
        if (mapKeepsLightness) config.setProperty("map_keep", true);
        if (mapStrength != 1) config.setProperty("map_strength", double(mapStrength));
    }
}

bool Settings::operator==(const Settings &other) const
{
    auto near = [](const QPointF &a, const QPointF &b) { return std::abs(a.x() - b.x()) < 1e-5 && std::abs(a.y() - b.y()) < 1e-5; };
    return curves == other.curves && rgb == other.rgb && near(balance, other.balance) && near(shadows, other.shadows)
        && near(highlights, other.highlights) && map == other.map
        && (map.isEmpty() || (mapSteps == other.mapSteps && mapKeepsLightness == other.mapKeepsLightness && mapStrength == other.mapStrength));
}

Recipe::Recipe(const Settings &settings)
    : m_mapKeepsLightness(settings.mapKeepsLightness)
    , m_mapStrength(settings.mapStrength)
    , m_shadows(settings.shadows)
    , m_highlights(settings.highlights)
{
    if (!settings.map.isEmpty() && settings.mapStrength > 0) {
        m_map.resize(size * 3);
        for (int i = 0; i < size; i++) mapAt(settings.map, settings.mapSteps, float(i) / (size - 1), m_map.data() + i * 3);
    }
    for (int by = 0; by < 3; by++) {
        for (int change = 0; change < 3; change++) {
            m_used[by][change] = settings.curves[by][change] != defaultCurve(by, change);
            if (m_used[by][change]) {
                m_curves[by][change] = sampled(settings.curves[by][change], by == Hue, isMapping(by, change), size);
                m_oklch = true;
            }
        }
        if (settings.rgb[by] != defaultRgbCurve()) {
            m_rgb[by] = sampled(settings.rgb[by], false, true, size);
            m_anyRgb = true;
        }
    }
    m_byHue = m_used[Hue][Lightness] || m_used[Hue][Chroma] || m_used[Hue][Hue];
    m_turns = m_used[Hue][Hue] || m_used[Lightness][Hue] || m_used[Chroma][Hue];
    if (!m_shadows.isNull() || !m_highlights.isNull()) m_oklch = true;

    // white balance as the eye adapts (von Kries), in the cone responses Oklab is made from: the color opposite the
    // cast, at full lightness, becomes white
    const bool balanced = !settings.balance.isNull();
    if (balanced) {
        const float a = -settings.balance.x();
        const float b = -settings.balance.y();
        const float cones[3] = {1 + 0.3963377774f * a + 0.2158037573f * b, 1 - 0.1055613458f * a - 0.0638541728f * b,
                                1 - 0.0894841775f * a - 1.2914855480f * b};
        for (int i = 0; i < 3; i++) m_gain[i] = 1 / std::max(1e-3f, cones[i] * cones[i] * cones[i]);
        // where the picture stops at white, white itself is kept inside it, so paper takes the cast the grays beside
        // it take instead of a channel being cut at white
        const float white[3] = {4.0767416621f * m_gain[0] - 3.3077115913f * m_gain[1] + 0.2309699292f * m_gain[2],
                                -1.2684380046f * m_gain[0] + 2.6097574011f * m_gain[1] - 0.3413193965f * m_gain[2],
                                -0.0041960863f * m_gain[0] - 0.7034186147f * m_gain[1] + 1.7076147010f * m_gain[2]};
        m_whiteKept = 1 / std::max(1.0f, std::max({white[0], white[1], white[2]}));
    }
    m_nothing = !m_oklch && !m_anyRgb && !balanced && m_map.isEmpty();
}

/// \p lab in a picture that stops at white: as much of its chroma as fits at its lightness, and past white as a
/// near-white is fitted, not cut channel by channel into a tint the near-whites beside it don't have
inline void fitted(float lab[3], float rgb[3])
{
    lab[0] = std::min(lab[0], 0.99999f);
    KoOklch::inGamut(lab, rgb);
}

void Recipe::apply(float rgb[3], bool fit) const
{
    if (m_nothing) return;
    if (!m_map.isEmpty()) {
        // the map first: the pixel's lightness becomes the map's color there
        const float clamped[3] = {std::max(rgb[0], 0.0f), std::max(rgb[1], 0.0f), std::max(rgb[2], 0.0f)};
        float lab[3];
        KoMixing::linearToOklab(clamped, lab);
        const float at = std::clamp(lab[0], 0.0f, 1.0f) * (size - 1);
        const int i = std::min(int(at), size - 2);
        const float t = at - i;
        float mapped[3];
        for (int c = 0; c < 3; c++) mapped[c] = m_map[i * 3 + c] + (m_map[(i + 1) * 3 + c] - m_map[i * 3 + c]) * t;
        if (m_mapKeepsLightness) mapped[0] = lab[0];
        for (int c = 0; c < 3; c++) lab[c] += (mapped[c] - lab[c]) * m_mapStrength;
        if (fit) {
            fitted(lab, rgb);
        } else {
            KoMixing::oklabToLinear(lab, rgb);
        }
    }
    if (m_oklch || m_gain[0] != 1 || m_gain[1] != 1 || m_gain[2] != 1) {
        const float kept = fit ? m_whiteKept : 1.0f;
        const float cones[3] = {std::cbrt((0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2]) * m_gain[0] * kept),
                                std::cbrt((0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2]) * m_gain[1] * kept),
                                std::cbrt((0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2]) * m_gain[2] * kept)};
        float lab[3] = {0.2104542553f * cones[0] + 0.7936177850f * cones[1] - 0.0040720468f * cones[2],
                        1.9779984951f * cones[0] - 2.4285922050f * cones[1] + 0.4505937099f * cones[2],
                        0.0259040371f * cones[0] + 0.7827717662f * cones[1] - 0.8086757660f * cones[2]};
        if (m_oklch) {
            const float lightness = lab[0];
            const float chroma = std::hypot(lab[1], lab[2]);
            const float c = chroma / chromaRange;
            const float x = m_byHue || m_turns ? axisOfHue(std::atan2(lab[2], lab[1]) * degrees) : 0.0f;
            // a gray's hue is noise: curves by hue reach a color as it gains chroma
            const float colored = std::min(1.0f, chroma / 0.04f);

            float newLightness = lightness;
            if (m_used[Lightness][Lightness]) {
                newLightness = lightness <= 1 ? read(m_curves[Lightness][Lightness], lightness)
                                              : read(m_curves[Lightness][Lightness], 1) + lightness - 1;
            }
            if (m_used[Chroma][Lightness]) newLightness += (read(m_curves[Chroma][Lightness], c) - 0.5f) * 2 * lightnessShift;
            if (m_used[Hue][Lightness]) newLightness += colored * (read(m_curves[Hue][Lightness], x) - 0.5f) * 2 * lightnessShift;
            newLightness = std::max(0.0f, newLightness);

            float newChroma = chroma;
            if (m_used[Chroma][Chroma]) {
                newChroma = c <= 1 ? read(m_curves[Chroma][Chroma], c) * chromaRange : chroma * read(m_curves[Chroma][Chroma], 1);
            }
            if (m_used[Lightness][Chroma]) newChroma *= 2 * read(m_curves[Lightness][Chroma], lightness);
            if (m_used[Hue][Chroma]) newChroma *= 1 + colored * (2 * read(m_curves[Hue][Chroma], x) - 1);

            if (m_turns) {
                float hue = hueOfAxis(m_used[Hue][Hue] ? read(m_curves[Hue][Hue], x) : x);
                if (m_used[Lightness][Hue]) hue += (read(m_curves[Lightness][Hue], lightness) - 0.5f) * 2 * hueShift;
                if (m_used[Chroma][Hue]) hue += (read(m_curves[Chroma][Hue], c) - 0.5f) * 2 * hueShift;
                lab[1] = newChroma * std::cos(hue / degrees);
                lab[2] = newChroma * std::sin(hue / degrees);
            } else if (chroma > 1e-7f) {
                lab[1] *= newChroma / chroma;
                lab[2] *= newChroma / chroma;
            }
            lab[0] = newLightness;

            // the tints fade toward the middle, each strongest at its end
            const float s = std::clamp(newLightness, 0.0f, 1.0f);
            const float toShadows = (1 - s) * (1 - s);
            const float toHighlights = s * s;
            lab[1] += float(m_shadows.x()) * toShadows + float(m_highlights.x()) * toHighlights;
            lab[2] += float(m_shadows.y()) * toShadows + float(m_highlights.y()) * toHighlights;
        }
        if (fit) {
            fitted(lab, rgb);
        } else {
            KoMixing::oklabToLinear(lab, rgb);
        }
    }
    if (m_anyRgb) {
        // integer images can't show the tables' error; floats keep the exact curve
        for (int i = 0; i < 3; i++) {
            if (m_rgb[i].isEmpty()) continue;
            const float e = encoded(std::max(0.0f, rgb[i]), fit);
            const float changed = e <= 1 ? read(m_rgb[i], e) : read(m_rgb[i], 1) + e - 1;
            rgb[i] = decoded(changed, fit);
        }
    }
}

} // namespace SkissaColor
