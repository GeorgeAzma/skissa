/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaShape.h"

#include <algorithm>
#include <cmath>

#include <QtMath>

#include <kis_properties_configuration.h>

#include "KisColorMath.h"
#include "KoCompositeOpInLinear.h"
#include "SkissaDetail.h"

namespace SkissaShape
{

namespace
{

/// Farther than anything asked: no edge in reach
constexpr float none = 1e20f;

/**
 * The squared distance from each of \p count places to the nearest where \p f is 0 (Felzenszwalb and Huttenlocher:
 * the lower envelope of parabolas rooted at each place, \p f high where there is nothing), into \p to, with which
 * place it is into \p from
 */
void lowerEnvelope(const float *f, int count, float *to, int *from, std::vector<int> &roots, std::vector<float> &bounds)
{
    roots.resize(count);
    bounds.resize(count + 1);
    int k = 0;
    roots[0] = 0;
    bounds[0] = -none;
    bounds[1] = none;
    auto meet = [f](int q, int v) { return ((f[q] + float(q) * q) - (f[v] + float(v) * v)) / (2.0f * q - 2.0f * v); };
    for (int q = 1; q < count; q++) {
        float s = meet(q, roots[k]);
        while (s <= bounds[k]) {
            k--;
            s = meet(q, roots[k]);
        }
        k++;
        roots[k] = q;
        bounds[k] = s;
        bounds[k + 1] = none;
    }
    k = 0;
    for (int p = 0; p < count; p++) {
        while (bounds[k + 1] < p) k++;
        const int v = roots[k];
        to[p] = float(p - v) * (p - v) + f[v];
        from[p] = v;
    }
}

/**
 * For each pixel of a \p w by \p h grid, the nearest one that \p isEdge (exactly, as an index), or -1 where there is
 * none
 */
std::vector<int> nearestOf(int w, int h, const std::vector<char> &isEdge)
{
    const size_t count = size_t(w) * h;
    std::vector<float> columns(count), line(std::max(w, h)), out(std::max(w, h));
    std::vector<int> rowOf(count), at(std::max(w, h));
    std::vector<int> roots;
    std::vector<float> bounds;
    // down each column, the squared distance to the nearest in it, and its row
    for (int x = 0; x < w; x++) {
        for (int y = 0; y < h; y++) line[y] = isEdge[size_t(y) * w + x] ? 0.0f : none;
        lowerEnvelope(line.data(), h, out.data(), at.data(), roots, bounds);
        for (int y = 0; y < h; y++) {
            columns[size_t(y) * w + x] = out[y];
            rowOf[size_t(y) * w + x] = at[y];
        }
    }
    // then along each row, over those columns
    std::vector<int> nearest(count, -1);
    for (int y = 0; y < h; y++) {
        lowerEnvelope(columns.data() + size_t(y) * w, w, out.data(), at.data(), roots, bounds);
        for (int x = 0; x < w; x++) {
            if (out[x] >= none * 0.5f) continue;
            const int column = at[x];
            nearest[size_t(y) * w + x] = rowOf[size_t(y) * w + column] * w + column;
        }
    }
    return nearest;
}

inline float coverage(float v)
{
    return std::clamp(v, 0.0f, 1.0f);
}

/// \p top over \p under, both premultiplied red, green, blue and alpha
inline void over(const float *top, float *under)
{
    for (int c = 0; c < 4; c++) under[c] = top[c] + under[c] * (1 - top[3]);
}

/// \p color (straight, linear) laid at \p share over what \p under (premultiplied) covers, and nowhere else
inline void atop(const float *color, float share, float *under)
{
    for (int c = 0; c < 3; c++) under[c] = under[c] * (1 - share) + color[c] * share * under[3];
}

/// The light a bevel is lit by comes this high above the picture (degrees)
constexpr double lightElevation = 30;
/// How steep a bevel of full depth is at its steepest (degrees)
constexpr double steepest = 80;
/// The sky's light beside the sun's (each as it falls on a surface facing it), as on a clear day
constexpr float skyLight = 0.4f;
/// The bevel's gloss: how bright its highlight is, and how tight (a Blinn-Phong exponent)
constexpr float gloss = 0.35f;
constexpr float glossTightness = 24;

const char *const placeKeys[] = {"outside", "center", "inside"};

QColor colorFrom(const KisPropertiesConfiguration &config, const char *key, const QColor &fallback)
{
    const QColor color(config.getString(key, fallback.name(QColor::HexArgb)));
    return color.isValid() ? color : fallback;
}

void linearOf(const QColor &color, float *to)
{
    const float alpha = float(color.alphaF());
    to[0] = float(KisColorMath::toLinear(color.redF())) * alpha;
    to[1] = float(KisColorMath::toLinear(color.greenF())) * alpha;
    to[2] = float(KisColorMath::toLinear(color.blueF())) * alpha;
    to[3] = alpha;
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings s;
    s.grow = float(std::clamp(config.getDouble("grow", 0), -200.0, 200.0));
    s.outline = float(std::clamp(config.getDouble("outline", 0), 0.0, 400.0));
    const QString place = config.getString("place", QStringLiteral("outside"));
    for (int i = 0; i < 3; i++) {
        if (place == QLatin1String(placeKeys[i])) s.place = Place(i);
    }
    s.outlineColor = colorFrom(config, "outline_color", s.outlineColor);
    s.glow = float(std::clamp(config.getDouble("glow", 0), 0.0, 400.0));
    s.glowStrength = float(std::clamp(config.getDouble("glow_strength", s.glowStrength), 0.0, 1.0));
    s.glowColor = colorFrom(config, "glow_color", s.glowColor);
    s.shadow = float(std::clamp(config.getDouble("shadow", 0), 0.0, 400.0));
    s.shadowAngle = float(std::clamp(config.getDouble("shadow_angle", s.shadowAngle), -180.0, 180.0));
    s.shadowSoftness = float(std::clamp(config.getDouble("shadow_softness", s.shadowSoftness), 0.0, 200.0));
    s.shadowColor = colorFrom(config, "shadow_color", s.shadowColor);
    s.solid = float(std::clamp(config.getDouble("solid", 1), 0.02, 1.0));
    s.shadowFall = config.getString("shadow_fall", QStringLiteral("under")) == QLatin1String("inside") ? Fall::Inside : Fall::Under;
    s.bevel = float(std::clamp(config.getDouble("bevel", 0), 0.0, 200.0));
    s.bevelDepth = float(std::clamp(config.getDouble("bevel_depth", s.bevelDepth), 0.0, 1.0));
    s.emboss = config.getBool("emboss", false);
    return s;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("grow", double(grow));
    config.setProperty("outline", double(outline));
    config.setProperty("place", QString::fromLatin1(placeKeys[int(place)]));
    config.setProperty("outline_color", outlineColor.name(QColor::HexArgb));
    config.setProperty("glow", double(glow));
    config.setProperty("glow_strength", double(glowStrength));
    config.setProperty("glow_color", glowColor.name(QColor::HexArgb));
    config.setProperty("shadow", double(shadow));
    config.setProperty("shadow_angle", double(shadowAngle));
    config.setProperty("shadow_softness", double(shadowSoftness));
    config.setProperty("shadow_color", shadowColor.name(QColor::HexArgb));
    config.setProperty("solid", double(solid));
    config.setProperty("shadow_fall", shadowFall == Fall::Inside ? QStringLiteral("inside") : QStringLiteral("under"));
    config.setProperty("bevel", double(bevel));
    config.setProperty("bevel_depth", double(bevelDepth));
    config.setProperty("emboss", emboss);
}

bool Settings::operator==(const Settings &other) const
{
    // the layer's opacity isn't part of the look
    return grow == other.grow && outline == other.outline && place == other.place && outlineColor == other.outlineColor && glow == other.glow
        && glowStrength == other.glowStrength && glowColor == other.glowColor && shadow == other.shadow && shadowAngle == other.shadowAngle
        && shadowSoftness == other.shadowSoftness && shadowColor == other.shadowColor && shadowFall == other.shadowFall && bevel == other.bevel
        && bevelDepth == other.bevelDepth && emboss == other.emboss;
}

bool Settings::changesNothing() const
{
    return grow == 0 && outline <= 0 && (glow <= 0 || glowStrength <= 0) && (shadowColor.alpha() == 0 || shadow <= 0) && (bevel <= 0 || bevelDepth <= 0);
}

Shape::Shape(const Settings &settings, qreal scale)
    : m_settings(settings)
    , m_scale(scale)
{
}

int Shape::reach() const
{
    const Settings &s = m_settings;
    if (s.changesNothing()) return 0;
    // the distance is read as far as the edge's effects reach, around wherever the shadow reads
    const qreal edge = std::abs(s.grow) + s.outline + s.glow + (s.bevel > 0 ? s.bevel : 0);
    const qreal shadow = s.shadowColor.alpha() && s.shadow > 0 ? s.shadow + 1.5 * s.shadowSoftness : 0;
    return int(std::ceil((edge + shadow) * m_scale)) + 3;
}

void Shape::apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const
{
    const Settings &s = m_settings;
    const int w = sourceRect.width(), h = sourceRect.height();
    const size_t count = size_t(w) * h;
    const QRect inside = rect.translated(-sourceRect.topLeft());
    if (s.changesNothing() || count == 0) {
        for (int y = 0; y < rect.height(); y++) {
            std::copy_n(source + (size_t(inside.top() + y) * w + inside.left()) * 4, rect.width() * 4, out + size_t(y) * rect.width() * 4);
        }
        return;
    }
    const float grow = float(s.grow * m_scale), width = float(s.outline * m_scale), glow = float(s.glow * m_scale);

    // how far each pixel is from the edge, between pixels from their alpha (as a share of the layer's solid paint):
    // negative inside
    const float solid = std::clamp(s.solid, 0.02f, 1.0f);
    auto cover = [source, solid](size_t i) { return std::clamp(source[i * 4 + 3] / solid, 0.0f, 1.0f); };
    std::vector<char> opaque(count), clear(count);
    for (size_t i = 0; i < count; i++) {
        const bool in = cover(i) >= 0.5f;
        opaque[i] = in;
        clear[i] = !in;
    }
    const std::vector<int> nearestIn = nearestOf(w, h, opaque);
    const std::vector<int> nearestOut = nearestOf(w, h, clear);
    std::vector<float> distance(count);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const float alpha = cover(i);
            if (!opaque[i]) {
                // past the nearest opaque pixel's edge, which lies as far out of it as it is opaque past a half
                const int q = nearestIn[i];
                const float far = q < 0 ? none : std::hypot(float(x - q % w), float(y - q / w)) + 0.5f - cover(size_t(q));
                // a pixel partly covered beside the inside has the edge within it
                distance[i] = alpha > 0 && far <= 1 ? std::min(far, 0.5f - alpha) : far;
            } else {
                const int r = nearestOut[i];
                const float far = r < 0 ? none : std::hypot(float(x - r % w), float(y - r / w)) - 0.5f + cover(size_t(r));
                // only a pixel beside the outside can hold the edge; further in, paint less than solid is see-through
                // paint, not an edge
                distance[i] = alpha < 1 && far <= 1 ? std::max(-far, 0.5f - alpha) : -far;
            }
        }
    }

    // the shape grown or shrunk, its new pixels the color of the nearest edge; and with an outline outside, the
    // whole silhouette that casts the shadow and glows
    std::vector<float> shape(count * 4), silhouette(count);
    const float outward = s.outline > 0 ? (s.place == Place::Outside ? width : s.place == Place::Center ? width / 2 : 0.0f) : 0.0f;
    for (size_t i = 0; i < count; i++) {
        const float *own = source + i * 4;
        const float alpha = std::clamp(own[3], 0.0f, 1.0f);
        float *to = shape.data() + i * 4;
        const float d = distance[i] - grow;
        if (grow == 0) {
            for (int c = 0; c < 3; c++) to[c] = own[c] * alpha;
            to[3] = alpha;
        } else if (grow > 0) {
            // as opaque as the paint it grows from
            const float grown = std::max(alpha, solid * coverage(0.5f - d));
            const int q = nearestIn[i];
            const float *edge = q >= 0 ? source + size_t(q) * 4 : own;
            for (int c = 0; c < 3; c++) to[c] = own[c] * alpha + edge[c] * (grown - alpha);
            to[3] = grown;
        } else {
            const float shrunk = std::min(alpha, solid * coverage(0.5f - d));
            for (int c = 0; c < 3; c++) to[c] = own[c] * shrunk;
            to[3] = shrunk;
        }
        silhouette[i] = std::max(to[3], outward > 0 ? coverage(outward - d + 0.5f) : 0.0f);
    }

    // the shadow: the silhouette moved and blurred, or inside, what is around the shape moved into it
    std::vector<float> shadow;
    const bool shadows = s.shadowColor.alpha() > 0 && s.shadow > 0;
    const bool cutIn = s.shadowFall == Fall::Inside;
    if (shadows) {
        shadow = silhouette;
        if (cutIn) {
            for (size_t i = 0; i < count; i++) shadow[i] = 1 - shape[i * 4 + 3];
        }
        std::vector<float> scratch(count);
        const qreal sigma = 0.5 * s.shadowSoftness * m_scale;
        if (sigma > 0.3) SkissaDetail::gaussian(shadow.data(), scratch.data(), w, h, sigma);
    }
    const double angle = qDegreesToRadians(double(s.shadowAngle));
    const float shiftX = float(std::cos(angle) * s.shadow * m_scale), shiftY = float(std::sin(angle) * s.shadow * m_scale);
    auto shadowAt = [&](int x, int y) {
        // read between pixels, nothing past the source
        const float fx = x - shiftX, fy = y - shiftY;
        const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
        const float tx = fx - ix, ty = fy - iy;
        auto at = [&](int px, int py) { return px < 0 || py < 0 || px >= w || py >= h ? 0.0f : shadow[size_t(py) * w + px]; };
        return (at(ix, iy) * (1 - tx) + at(ix + 1, iy) * tx) * (1 - ty) + (at(ix, iy + 1) * (1 - tx) + at(ix + 1, iy + 1) * tx) * ty;
    };

    float outlineColor[4], glowColor[4], shadowColor[4];
    linearOf(s.outlineColor, outlineColor);
    linearOf(s.glowColor, glowColor);
    linearOf(s.shadowColor, shadowColor);

    /*
     * The bevel: the edge as a rounded slope (a quarter of a sine, steepest at the edge), rising inward over its width,
     * or across the edge for an emboss, at most as steep as its depth asks. Its normal leans the way the distance grows
     * (the distance field's gradient), and it is lit as a surface is outdoors: the sun from the shadow's opposite side,
     * lightElevation up (Lambert), the sky from above (a hemisphere), and a glossy highlight (Blinn-Phong), each as
     * much as it differs from a flat surface's, so the paint as it is stays the paint on the flat.
     */
    const float bevel = float(s.bevel * m_scale);
    const bool bevels = bevel > 0 && s.bevelDepth > 0;
    const double turn = qDegreesToRadians(double(s.shadowAngle)), up = qDegreesToRadians(lightElevation);
    const float toLight[3] = {float(-std::cos(turn) * std::cos(up)), float(-std::sin(turn) * std::cos(up)), float(std::sin(up))};
    // halfway between the light and the eye, straight above
    const float halfLength = std::hypot(toLight[0], toLight[1], toLight[2] + 1);
    const float halfway[3] = {toLight[0] / halfLength, toLight[1] / halfLength, (toLight[2] + 1) / halfLength};
    auto lightOn = [&](const float *n) { return std::max(0.0f, n[0] * toLight[0] + n[1] * toLight[1] + n[2] * toLight[2]) + skyLight * (1 + n[2]) / 2; };
    auto shineOn = [&](const float *n) { return gloss * std::pow(std::max(0.0f, n[0] * halfway[0] + n[1] * halfway[1] + n[2] * halfway[2]), glossTightness); };
    const float flatNormal[3] = {0, 0, 1};
    const float flatLight = lightOn(flatNormal), flatShine = shineOn(flatNormal);
    const float steepestSlope = float(std::tan(qDegreesToRadians(steepest * s.bevelDepth)));
    // the slope is read from the distance smoothed a little: exact distances step by whole pixels' roots, and their
    // differences streaked round edges
    std::vector<float> smooth;
    if (bevels) {
        smooth.resize(count);
        for (size_t i = 0; i < count; i++) smooth[i] = std::clamp(distance[i], -bevel - 4, bevel + 4);
        std::vector<float> scratch(count);
        SkissaDetail::gaussian(smooth.data(), scratch.data(), w, h, 1.5);
    }
    const float shadowAlpha = std::max(shadowColor[3], 1e-6f);
    const float shadowStraight[3] = {shadowColor[0] / shadowAlpha, shadowColor[1] / shadowAlpha, shadowColor[2] / shadowAlpha};
    // how a pixel of the slope is lit beside the flat: a factor for the paint and a highlight added to it; false off
    // the slope
    auto litAt = [&](int sx, int sy, float d, float &factor, float &highlight) {
        const float from = s.emboss ? -bevel / 2 : -bevel;
        const float t = (d - from) / bevel;
        if (t <= 0 || t >= 1) return false;
        const size_t i = size_t(sy) * w + sx;
        const float gx = 0.5f * (smooth[size_t(sy) * w + std::min(sx + 1, w - 1)] - smooth[size_t(sy) * w + std::max(sx - 1, 0)]);
        const float gy = 0.5f * (smooth[size_t(std::min(sy + 1, h - 1)) * w + sx] - smooth[size_t(std::max(sy - 1, 0)) * w + sx]);
        const float length = std::hypot(gx, gy);
        if (length < 0.3f || std::abs(distance[i]) >= none / 2) return false;
        // the height falls outward as cos: its slope is steepest at the outer end
        const float slope = steepestSlope * std::sin(t * float(M_PI) / 2);
        const float lean = 1 / std::sqrt(slope * slope + 1);
        const float normal[3] = {slope * gx / length * lean, slope * gy / length * lean, lean};
        factor = lightOn(normal) / flatLight;
        highlight = shineOn(normal) - flatShine;
        return true;
    };
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const int sx = inside.left() + x, sy = inside.top() + y;
            const size_t i = size_t(sy) * w + sx;
            float result[4] = {0, 0, 0, 0};
            float layer[4];
            if (shadows && !cutIn) {
                const float a = shadowAt(sx, sy);
                for (int c = 0; c < 4; c++) layer[c] = shadowColor[c] * a;
                over(layer, result);
            }
            const float d = distance[i] - grow;
            if (glow > 0 && s.glowStrength > 0) {
                // light falling off from the silhouette's edge, under the shape only as far as its soft edge, so a
                // see-through shape isn't tinted
                const float t = std::max(0.0f, d - outward) / glow;
                const float a = s.glowStrength * std::exp(-4.6f * t * t) * coverage(d + 3);
                for (int c = 0; c < 4; c++) layer[c] = glowColor[c] * a;
                over(layer, result);
            }
            if (width > 0 && s.place == Place::Outside) {
                // under the shape's soft edge, so the two meet without a seam; no further in, or a see-through shape
                // is tinted with it
                const float a = coverage(width - d + 0.5f) * coverage(d + 3);
                for (int c = 0; c < 4; c++) layer[c] = outlineColor[c] * a;
                over(layer, result);
            }
            over(shape.data() + i * 4, result);
            if (shadows && cutIn) atop(shadowStraight, shadowAt(sx, sy) * shadowColor[3], result);
            float factor, highlight;
            if (bevels && litAt(sx, sy, d, factor, highlight)) {
                if (d < 0) {
                    // the paint itself lit
                    for (int c = 0; c < 3; c++) result[c] = std::max(0.0f, result[c] * factor + highlight * result[3]);
                } else {
                    // an emboss's outer half, sloping down to what is around, which may lie on layers below: black
                    // over it darkens anything by exactly the factor, and white lights a middle gray by it
                    const float shade = factor < 1 ? 1 - factor : 0.0f;
                    const float light = std::clamp(std::max(0.0f, factor - 1) + std::max(0.0f, highlight), 0.0f, 1.0f);
                    for (int c = 0; c < 4; c++) layer[c] = c < 3 ? light : light + shade;
                    over(layer, result);
                }
            }
            if (width > 0 && s.place != Place::Outside) {
                const float a = s.place == Place::Inside ? coverage(d + width + 0.5f) * shape[i * 4 + 3]
                                                         : coverage(d + width / 2 + 0.5f) * coverage(width / 2 - d + 0.5f);
                for (int c = 0; c < 4; c++) layer[c] = outlineColor[c] * a;
                over(layer, result);
            }
            float *to = out + (size_t(y) * rect.width() + x) * 4;
            for (int c = 0; c < 3; c++) to[c] = result[3] > 1e-6f ? result[c] / result[3] : 0.0f;
            to[3] = result[3];
        }
    }
}

QImage applied(const QImage &image, const Settings &settings, qreal scale)
{
    const QImage picture = image.convertToFormat(QImage::Format_ARGB32);
    const QRect rect = picture.rect();
    const float *decode = KoMixing::srgbToLinearTable();
    const quint8 *encode = KoMixing::linearToSrgbTable();
    std::vector<float> in(size_t(rect.width()) * rect.height() * 4);
    for (int y = 0; y < rect.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
        for (int x = 0; x < rect.width(); x++) {
            float *pixel = in.data() + (size_t(y) * rect.width() + x) * 4;
            pixel[0] = decode[qRed(line[x])];
            pixel[1] = decode[qGreen(line[x])];
            pixel[2] = decode[qBlue(line[x])];
            pixel[3] = qAlpha(line[x]) / 255.0f;
        }
    }
    std::vector<float> out(in.size());
    Shape(settings, scale).apply(in.data(), rect, rect, out.data());
    QImage result(rect.size(), QImage::Format_ARGB32);
    auto encoded = [encode](float c) { return int(encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]); };
    for (int y = 0; y < rect.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < rect.width(); x++) {
            const float *pixel = out.data() + (size_t(y) * rect.width() + x) * 4;
            line[x] = qRgba(encoded(pixel[0]), encoded(pixel[1]), encoded(pixel[2]), qRound(std::clamp(pixel[3], 0.0f, 1.0f) * 255));
        }
    }
    return result;
}

} // namespace SkissaShape
