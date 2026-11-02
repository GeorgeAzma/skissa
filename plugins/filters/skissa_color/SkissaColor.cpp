/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaColor.h"

#include <QColor>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include <kis_properties_configuration.h>

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

inline float encoded(float c)
{
    return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
}

inline float decoded(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

} // namespace

QString Curve::toString() const
{
    QString text;
    Q_FOREACH (const QPointF &point, points) {
        text += QString::number(point.x(), 'g', 5) + QLatin1Char(',') + QString::number(point.y(), 'g', 5) + QLatin1Char(';');
    }
    return text;
}

Curve Curve::fromString(const QString &text, const Curve &fallback)
{
    Curve curve;
    Q_FOREACH (const QString &pair, text.split(QLatin1Char(';'), Qt::SkipEmptyParts)) {
        const QStringList xy = pair.split(QLatin1Char(','));
        bool okX = false, okY = false;
        const qreal x = xy.value(0).toDouble(&okX);
        const qreal y = xy.value(1).toDouble(&okY);
        if (xy.size() == 2 && okX && okY) curve.points << QPointF(qBound(0.0, x, 1.0), qBound(0.0, y, 1.0));
    }
    if (curve.points.size() < 2) return fallback;
    std::stable_sort(curve.points.begin(), curve.points.end(), [](const QPointF &a, const QPointF &b) { return a.x() < b.x(); });
    return curve;
}

bool Curve::operator==(const Curve &other) const
{
    if (points.size() != other.points.size()) return false;
    for (int i = 0; i < points.size(); i++) {
        if (std::abs(points[i].x() - other.points[i].x()) > 1e-4 || std::abs(points[i].y() - other.points[i].y()) > 1e-4) return false;
    }
    return true;
}

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
    QVector<QPointF> points = curve.points;
    if (points.isEmpty()) points = mapping ? defaultRgbCurve().points : Curve{{QPointF(0, 0.5), QPointF(1, 0.5)}}.points;
    if (wraps) {
        // a turn before and after, so that the curve meets itself at the ends
        QVector<QPointF> around;
        for (int turn = -1; turn <= 1; turn++) {
            Q_FOREACH (const QPointF &point, points) around << QPointF(point.x() + turn, point.y() + (mapping ? turn : 0));
        }
        points = around;
    }
    const int n = points.size();
    QVector<float> values(count);
    if (n == 1) {
        std::fill(values.begin(), values.end(), float(points.first().y()));
        return values;
    }
    // a monotone cubic (Fritsch and Carlson): through every point without overshooting between them
    QVector<double> slope(n - 1), tangent(n);
    for (int i = 0; i < n - 1; i++) {
        const double dx = std::max(points[i + 1].x() - points[i].x(), 1e-6);
        slope[i] = (points[i + 1].y() - points[i].y()) / dx;
    }
    tangent[0] = slope[0];
    tangent[n - 1] = slope[n - 2];
    for (int i = 1; i < n - 1; i++) tangent[i] = slope[i - 1] * slope[i] <= 0 ? 0 : (slope[i - 1] + slope[i]) / 2;
    for (int i = 0; i < n - 1; i++) {
        if (slope[i] == 0) {
            tangent[i] = tangent[i + 1] = 0;
            continue;
        }
        const double a = tangent[i] / slope[i];
        const double b = tangent[i + 1] / slope[i];
        const double s = a * a + b * b;
        if (s > 9) {
            const double t = 3 / std::sqrt(s);
            tangent[i] = t * a * slope[i];
            tangent[i + 1] = t * b * slope[i];
        }
    }
    int segment = 0;
    for (int k = 0; k < count; k++) {
        const double x = double(k) / (count - 1);
        double y;
        if (x <= points.first().x()) {
            y = points.first().y();
        } else if (x >= points.last().x()) {
            y = points.last().y();
        } else {
            while (segment < n - 2 && x > points[segment + 1].x()) segment++;
            const QPointF &p0 = points[segment];
            const QPointF &p1 = points[segment + 1];
            const double h = std::max(p1.x() - p0.x(), 1e-6);
            const double t = (x - p0.x()) / h;
            const double t2 = t * t, t3 = t2 * t;
            y = (2 * t3 - 3 * t2 + 1) * p0.y() + (t3 - 2 * t2 + t) * h * tangent[segment] + (-2 * t3 + 3 * t2) * p1.y()
                + (t3 - t2) * h * tangent[segment + 1];
        }
        values[k] = float(wraps && mapping ? y : std::clamp(y, 0.0, 1.0));
    }
    return values;
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
}

bool Settings::operator==(const Settings &other) const
{
    auto near = [](const QPointF &a, const QPointF &b) { return std::abs(a.x() - b.x()) < 1e-5 && std::abs(a.y() - b.y()) < 1e-5; };
    return curves == other.curves && rgb == other.rgb && near(balance, other.balance) && near(shadows, other.shadows)
        && near(highlights, other.highlights);
}

Recipe::Recipe(const Settings &settings)
    : m_shadows(settings.shadows)
    , m_highlights(settings.highlights)
{
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
    }
    m_nothing = !m_oklch && !m_anyRgb && !balanced;
}

void Recipe::apply(float rgb[3], bool fit) const
{
    if (m_nothing) return;
    if (m_oklch || m_gain[0] != 1 || m_gain[1] != 1 || m_gain[2] != 1) {
        const float cones[3] = {std::cbrt((0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2]) * m_gain[0]),
                                std::cbrt((0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2]) * m_gain[1]),
                                std::cbrt((0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2]) * m_gain[2])};
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
            KoOklch::inGamut(lab, rgb);
        } else {
            KoMixing::oklabToLinear(lab, rgb);
        }
    }
    if (m_anyRgb) {
        for (int i = 0; i < 3; i++) {
            if (m_rgb[i].isEmpty()) continue;
            const float e = encoded(std::max(0.0f, rgb[i]));
            const float changed = e <= 1 ? read(m_rgb[i], e) : read(m_rgb[i], 1) + e - 1;
            rgb[i] = decoded(changed);
        }
    }
}

} // namespace SkissaColor
