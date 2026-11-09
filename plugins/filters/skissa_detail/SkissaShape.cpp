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
}

bool Settings::operator==(const Settings &other) const
{
    return grow == other.grow && outline == other.outline && place == other.place && outlineColor == other.outlineColor && glow == other.glow
        && glowStrength == other.glowStrength && glowColor == other.glowColor && shadow == other.shadow && shadowAngle == other.shadowAngle
        && shadowSoftness == other.shadowSoftness && shadowColor == other.shadowColor;
}

bool Settings::changesNothing() const
{
    return grow == 0 && outline <= 0 && (glow <= 0 || glowStrength <= 0) && (shadowColor.alpha() == 0 || shadow <= 0);
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
    const qreal edge = std::abs(s.grow) + s.outline + s.glow;
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

    // how far each pixel is from the edge, between pixels from their alpha: negative inside
    std::vector<char> opaque(count), clear(count);
    for (size_t i = 0; i < count; i++) {
        const bool in = source[i * 4 + 3] >= 0.5f;
        opaque[i] = in;
        clear[i] = !in;
    }
    const std::vector<int> nearestIn = nearestOf(w, h, opaque);
    const std::vector<int> nearestOut = nearestOf(w, h, clear);
    std::vector<float> distance(count);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const float alpha = std::clamp(source[i * 4 + 3], 0.0f, 1.0f);
            if (!opaque[i]) {
                // past the nearest opaque pixel's edge, which lies as far out of it as it is opaque past a half
                const int q = nearestIn[i];
                const float far = q < 0 ? none : std::hypot(float(x - q % w), float(y - q / w)) + 0.5f - std::clamp(source[size_t(q) * 4 + 3], 0.0f, 1.0f);
                // a pixel partly covered has the edge within it
                distance[i] = alpha > 0 ? std::min(far, 0.5f - alpha) : far;
            } else {
                const int r = nearestOut[i];
                const float far = r < 0 ? none : std::hypot(float(x - r % w), float(y - r / w)) - 0.5f + std::clamp(source[size_t(r) * 4 + 3], 0.0f, 1.0f);
                distance[i] = alpha < 1 ? std::max(-far, 0.5f - alpha) : -far;
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
            const float grown = std::max(alpha, coverage(0.5f - d));
            const int q = nearestIn[i];
            const float *edge = q >= 0 ? source + size_t(q) * 4 : own;
            for (int c = 0; c < 3; c++) to[c] = own[c] * alpha + edge[c] * (grown - alpha);
            to[3] = grown;
        } else {
            const float shrunk = std::min(alpha, coverage(0.5f - d));
            for (int c = 0; c < 3; c++) to[c] = own[c] * shrunk;
            to[3] = shrunk;
        }
        silhouette[i] = std::max(to[3], outward > 0 ? coverage(outward - d + 0.5f) : 0.0f);
    }

    // the shadow: the silhouette moved and blurred
    std::vector<float> shadow;
    const bool shadows = s.shadowColor.alpha() > 0 && s.shadow > 0;
    if (shadows) {
        shadow = silhouette;
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
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const int sx = inside.left() + x, sy = inside.top() + y;
            const size_t i = size_t(sy) * w + sx;
            float result[4] = {0, 0, 0, 0};
            float layer[4];
            if (shadows) {
                const float a = shadowAt(sx, sy);
                for (int c = 0; c < 4; c++) layer[c] = shadowColor[c] * a;
                over(layer, result);
            }
            const float d = distance[i] - grow;
            if (glow > 0 && s.glowStrength > 0) {
                // light falling off from the silhouette's edge
                const float t = std::max(0.0f, d - outward) / glow;
                const float a = s.glowStrength * std::exp(-4.6f * t * t);
                for (int c = 0; c < 4; c++) layer[c] = glowColor[c] * a;
                over(layer, result);
            }
            if (width > 0 && s.place == Place::Outside) {
                // under the shape, so the two meet without a seam
                const float a = coverage(width - d + 0.5f);
                for (int c = 0; c < 4; c++) layer[c] = outlineColor[c] * a;
                over(layer, result);
            }
            over(shape.data() + i * 4, result);
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
