/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaPaint.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

#include <QtMath>

#include <kis_properties_configuration.h>

#include "KoCompositeOpInLinear.h"
#include "KoCompositeOpMix.h"
#include "SkissaDetail.h"

namespace SkissaPaint
{

namespace
{

constexpr int sectors = 8;
/// Points the ellipse is read at, spread evenly over it (a sunflower's spiral)
constexpr int points = 48;
/// The sectors' polynomial weights (Kyprianidis 2011): each sector's edge is a parabola through the disk's edge
/// this far (radians) from the sector's middle, its tip this far behind the disk's middle, so the sectors overlap there
constexpr float zeta = 0.15f;
constexpr float zeroCrossing = 0.58f;

struct Point {
    /// Where it lies in the unit disk
    float x, y;
    /// The sectors it counts in (most count in two or three) and its weight in each, with the Gaussian falloff
    int count {0};
    int sector[sectors];
    float weight[sectors];
};

const std::vector<Point> &pattern()
{
    static const std::vector<Point> spread = []() {
        std::vector<Point> out;
        const float eta = (zeta + std::cos(zeroCrossing)) / (std::sin(zeroCrossing) * std::sin(zeroCrossing));
        const double golden = M_PI * (3 - std::sqrt(5.0));
        for (int i = 0; i < points; i++) {
            Point p;
            const double radius = std::sqrt((i + 0.5) / points), angle = i * golden;
            p.x = float(radius * std::cos(angle));
            p.y = float(radius * std::sin(angle));
            const float vx = p.x, vy = p.y;
            float w[sectors];
            auto lobes = [eta](float x, float y, float *out) {
                const float vxx = zeta - eta * x * x, vyy = zeta - eta * y * y;
                out[0] = std::pow(std::max(0.0f, y + vxx), 2.0f);
                out[2] = std::pow(std::max(0.0f, -x + vyy), 2.0f);
                out[4] = std::pow(std::max(0.0f, -y + vxx), 2.0f);
                out[6] = std::pow(std::max(0.0f, x + vyy), 2.0f);
            };
            float even[sectors], odd[sectors];
            lobes(vx, vy, even);
            const float r = float(M_SQRT1_2);
            lobes(r * (vx - vy), r * (vx + vy), odd);
            float sum = 0;
            for (int k = 0; k < sectors; k++) {
                w[k] = k % 2 ? odd[k - 1] : even[k];
                sum += w[k];
            }
            const float g = std::exp(-0.78f * (vx * vx + vy * vy)) / std::max(sum, 1e-6f);
            for (int k = 0; k < sectors; k++) {
                if (w[k] * g <= 1e-6f) continue;
                p.sector[p.count] = k;
                p.weight[p.count++] = w[k] * g;
            }
            out.push_back(p);
        }
        return out;
    }();
    return spread;
}

/// \p x to the power \p y, for x above 0, within a few parts in a thousand (the sectors' weights need no more): the
/// exponent and a quadratic in the mantissa for log2, a quadratic for 2 to the fraction
inline float roughPow(float x, float y)
{
    if (x <= 1e-30f) return 0;
    int bits;
    std::memcpy(&bits, &x, sizeof(bits));
    const float mantissa = float(bits & 0x7fffff) / float(0x800000);
    const float log2 = float((bits >> 23) & 0xff) - 127 + mantissa * (1.3465557f - 0.34655572f * mantissa);
    const float e = std::clamp(y * log2, -126.0f, 126.0f);
    const float whole = std::floor(e), fraction = e - whole;
    const float power = 1 + fraction * (0.6565f + 0.3435f * fraction);
    int out;
    std::memcpy(&out, &power, sizeof(out));
    out += int(whole) << 23;
    float result;
    std::memcpy(&result, &out, sizeof(result));
    return result;
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings s;
    s.size = float(std::clamp(config.getDouble("size", s.size), 1.0, 100.0));
    s.sharpness = float(std::clamp(config.getDouble("sharpness", s.sharpness), 0.0, 1.0));
    s.flow = float(std::clamp(config.getDouble("flow", s.flow), 0.0, 1.0));
    return s;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("size", double(size));
    config.setProperty("sharpness", double(sharpness));
    config.setProperty("flow", double(flow));
}

bool Settings::operator==(const Settings &other) const
{
    return size == other.size && sharpness == other.sharpness && flow == other.flow;
}

Paint::Paint(const Settings &settings, qreal scale)
    : m_settings(settings)
    , m_scale(scale)
{
}

int Paint::reach() const
{
    // the ellipse stretches to twice the radius along the flow; the tensor and the blur read around it
    const qreal radius = m_settings.size * m_scale;
    return int(std::ceil(2 * radius + 3 * (1 + 0.25 * radius) + 0.3 * radius)) + 3;
}

void Paint::apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const
{
    const Settings &s = m_settings;
    const int w = sourceRect.width(), h = sourceRect.height();
    const size_t count = size_t(w) * h;
    const QRect inside = rect.translated(-sourceRect.topLeft());
    const float radius = float(std::max(0.5, s.size * m_scale));

    // Oklab and alpha, the colors weighed by their alpha
    std::vector<float> lab(count * 4);
    for (size_t i = 0; i < count; i++) {
        const float *pixel = source + i * 4;
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        const float rgb[3] = {std::max(0.0f, pixel[0]), std::max(0.0f, pixel[1]), std::max(0.0f, pixel[2])};
        float c[3];
        KoMixing::linearToOklab(rgb, c);
        for (int k = 0; k < 3; k++) lab[i * 4 + k] = c[k] * alpha;
        lab[i * 4 + 3] = alpha;
    }

    // the flow: the structure tensor of the colors (Oklab) and the coverage, smoothed: lightness alone has no edge
    // between colors as light as each other, nor around a white shape on a clear layer
    std::vector<float> e(count), f(count), g(count), scratch(count);
    for (int y = 0; y < h; y++) {
        const int up = std::max(y - 1, 0), down = std::min(y + 1, h - 1);
        for (int x = 0; x < w; x++) {
            const int left = std::max(x - 1, 0), right = std::min(x + 1, w - 1);
            float ee = 0, ff = 0, gg = 0;
            for (int k = 0; k < 4; k++) {
                auto v = [&](int px, int py) { return lab[(size_t(py) * w + px) * 4 + k]; };
                const float sx = (v(right, up) + 2 * v(right, y) + v(right, down) - v(left, up) - 2 * v(left, y) - v(left, down)) / 4;
                const float sy = (v(left, down) + 2 * v(x, down) + v(right, down) - v(left, up) - 2 * v(x, up) - v(right, up)) / 4;
                ee += sx * sx;
                ff += sx * sy;
                gg += sy * sy;
            }
            const size_t i = size_t(y) * w + x;
            e[i] = ee;
            f[i] = ff;
            g[i] = gg;
        }
    }
    const qreal tensorSigma = 1 + 0.25 * radius;
    SkissaDetail::gaussian(e.data(), scratch.data(), w, h, tensorSigma);
    SkissaDetail::gaussian(f.data(), scratch.data(), w, h, tensorSigma);
    SkissaDetail::gaussian(g.data(), scratch.data(), w, h, tensorSigma);

    // a large brush reads its ellipse at fewer points than it covers: what lies between them is blurred in first
    if (radius > 8) {
        std::vector<float> plane(count);
        for (int k = 0; k < 4; k++) {
            for (size_t i = 0; i < count; i++) plane[i] = lab[i * 4 + k];
            SkissaDetail::gaussian(plane.data(), scratch.data(), w, h, 0.1 * radius);
            for (size_t i = 0; i < count; i++) lab[i * 4 + k] = plane[i];
        }
    }

    const std::vector<Point> &spread = pattern();
    const float q = 1 + 17 * s.sharpness;
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const int sx = inside.left() + x, sy = inside.top() + y;
            const size_t i = size_t(sy) * w + sx;
            const float lambda1 = 0.5f * (e[i] + g[i] + std::sqrt(std::max(0.0f, (e[i] - g[i]) * (e[i] - g[i]) + 4 * f[i] * f[i])));
            const float lambda2 = 0.5f * (e[i] + g[i] - std::sqrt(std::max(0.0f, (e[i] - g[i]) * (e[i] - g[i]) + 4 * f[i] * f[i])));
            // along the flow, and how strongly the picture has one
            float tx = lambda1 - e[i], ty = -f[i];
            const float length = std::hypot(tx, ty);
            if (length > 1e-9f) {
                tx /= length;
                ty /= length;
            } else {
                tx = 0;
                ty = 1;
            }
            const float anisotropy = s.flow * (lambda1 + lambda2 > 1e-9f ? (lambda1 - lambda2) / (lambda1 + lambda2) : 0.0f);
            const float along = radius * (1 + anisotropy), across = radius / (1 + anisotropy);

            float m[sectors][4] = {}, sq[sectors][4] = {}, total[sectors] = {};
            for (const Point &p : spread) {
                // the disk's point stretched into the ellipse and turned along the flow
                const float ox = p.x * along, oy = p.y * across;
                const float px = sx + 0.5f + ox * tx - oy * ty, py = sy + 0.5f + ox * ty + oy * tx;
                // the nearest pixel: 48 points over the ellipse average out where between pixels they fall
                const int nx = std::clamp(int(px), 0, w - 1), ny = std::clamp(int(py), 0, h - 1);
                const float *c = lab.data() + (size_t(ny) * w + nx) * 4;
                const float squares[4] = {c[0] * c[0], c[1] * c[1], c[2] * c[2], c[3] * c[3]};
                for (int j = 0; j < p.count; j++) {
                    const int k = p.sector[j];
                    const float weight = p.weight[j];
                    for (int ch = 0; ch < 4; ch++) {
                        m[k][ch] += c[ch] * weight;
                        sq[k][ch] += squares[ch] * weight;
                    }
                    total[k] += weight;
                }
            }
            // each sector's average, weighed toward the calmest
            float result[4] = {0, 0, 0, 0}, weights = 0;
            for (int k = 0; k < sectors; k++) {
                if (total[k] <= 0) continue;
                float spread2 = 0, mean[4];
                for (int ch = 0; ch < 4; ch++) {
                    mean[ch] = m[k][ch] / total[k];
                    spread2 += std::max(0.0f, sq[k][ch] / total[k] - mean[ch] * mean[ch]);
                }
                const float weight = 1 / (1 + roughPow(1000 * spread2, 0.5f * q));
                for (int ch = 0; ch < 4; ch++) result[ch] += mean[ch] * weight;
                weights += weight;
            }
            float *to = out + (size_t(y) * rect.width() + x) * 4;
            if (weights <= 0) {
                std::copy_n(source + i * 4, 4, to);
                continue;
            }
            for (float &v : result) v /= weights;
            const float alpha = std::clamp(result[3], 0.0f, 1.0f);
            if (alpha <= 1e-5f) {
                to[0] = to[1] = to[2] = to[3] = 0;
                continue;
            }
            const float c[3] = {result[0] / alpha, result[1] / alpha, result[2] / alpha};
            KoMixing::oklabToLinear(c, to);
            to[3] = alpha;
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
    Paint(settings, scale).apply(in.data(), rect, rect, out.data());
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

} // namespace SkissaPaint
