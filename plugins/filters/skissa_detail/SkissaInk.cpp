/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaInk.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QtMath>

#include <kis_properties_configuration.h>

#include "KisColorMath.h"
#include "KoCompositeOpInLinear.h"
#include "SkissaDetail.h"

namespace SkissaInk
{

namespace
{

constexpr double tau = 2 * M_PI;
/// The wider blur, as a multiple of the smaller, as in Winnemöller's XDoG
constexpr double wider = 1.6;
/// How much edges are sharpened at Lines 100% (XDoG's p)
constexpr float mostSharpness = 100;
/// Differences of lightness this small aren't edges: sharpened, an 8-bit gradient's steps would show as stripes
constexpr float leastEdge = 0.006f;
/// Each tone's hatch direction after the first's, lightest first: crossing, then between
const float hatchTurns[8] = {0, 90, 45, -45, 22.5f, -67.5f, 67.5f, -22.5f};

QString nameOf(Colors colors)
{
    switch (colors) {
    case Colors::Paper: return QStringLiteral("paper");
    case Colors::Clear: return QStringLiteral("clear");
    case Colors::Over: return QStringLiteral("over");
    case Colors::Colored: return QStringLiteral("colored");
    }
    return QStringLiteral("paper");
}

Colors colorsNamed(const QString &name)
{
    for (Colors colors : {Colors::Paper, Colors::Clear, Colors::Over, Colors::Colored}) {
        if (nameOf(colors) == name) return colors;
    }
    return Colors::Paper;
}

/// Oklab of linear sRGB
inline void oklabOf(float r, float g, float b, float *lab)
{
    const float l = std::cbrt(std::max(0.0f, 0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b));
    const float m = std::cbrt(std::max(0.0f, 0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b));
    const float s = std::cbrt(std::max(0.0f, 0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b));
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

/// A plane read between its pixels (centres at halves), the edge repeating
inline float readAt(const float *plane, int w, int h, float x, float y)
{
    x = std::clamp(x - 0.5f, 0.0f, float(w - 1));
    y = std::clamp(y - 0.5f, 0.0f, float(h - 1));
    const int ix = int(x), iy = int(y);
    const int jx = std::min(ix + 1, w - 1), jy = std::min(iy + 1, h - 1);
    const float tx = x - ix, ty = y - iy;
    const float top = plane[size_t(iy) * w + ix] + (plane[size_t(iy) * w + jx] - plane[size_t(iy) * w + ix]) * tx;
    const float bottom = plane[size_t(jy) * w + ix] + (plane[size_t(jy) * w + jx] - plane[size_t(jy) * w + ix]) * tx;
    return top + (bottom - top) * ty;
}

struct Flow {
    int w {0}, h {0};
    std::vector<float> x, y;
};

/**
 * The direction along the picture's edges at every pixel: the structure tensor of its Oklab colors (Sobel), smoothed
 * by \p sigma, and its eigenvector of the smaller eigenvalue
 */
Flow flowOf(const std::vector<float> lab[3], int w, int h, qreal sigma)
{
    const size_t count = size_t(w) * h;
    std::vector<float> e(count), f(count), g(count), scratch(count);
    for (int y = 0; y < h; y++) {
        const int up = std::max(y - 1, 0), down = std::min(y + 1, h - 1);
        for (int x = 0; x < w; x++) {
            const int left = std::max(x - 1, 0), right = std::min(x + 1, w - 1);
            float ee = 0, ff = 0, gg = 0;
            for (int c = 0; c < 3; c++) {
                const float *p = lab[c].data();
                auto v = [p, w](int px, int py) { return p[size_t(py) * w + px]; };
                const float sx = (v(left, up) + 2 * v(left, y) + v(left, down) - v(right, up) - 2 * v(right, y) - v(right, down)) / 4;
                const float sy = (v(left, up) + 2 * v(x, up) + v(right, up) - v(left, down) - 2 * v(x, down) - v(right, down)) / 4;
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
    if (sigma > 0.3) {
        SkissaDetail::gaussian(e.data(), scratch.data(), w, h, sigma);
        SkissaDetail::gaussian(f.data(), scratch.data(), w, h, sigma);
        SkissaDetail::gaussian(g.data(), scratch.data(), w, h, sigma);
    }
    Flow flow;
    flow.w = w;
    flow.h = h;
    flow.x.resize(count);
    flow.y.resize(count);
    for (size_t i = 0; i < count; i++) {
        const float larger = 0.5f * (e[i] + g[i] + std::sqrt(std::max(0.0f, (e[i] - g[i]) * (e[i] - g[i]) + 4 * f[i] * f[i])));
        // across the larger eigenvector, so along the edge
        float dx = e[i] - larger, dy = f[i];
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 1e-12f) {
            dx /= length;
            dy /= length;
        } else {
            dx = 0;
            dy = 1;
        }
        flow.x[i] = dx;
        flow.y[i] = dy;
    }
    return flow;
}

/**
 * \p planes smoothed along the flow over \p area (in the planes' pixels): each pixel the Gaussian-weighted average along
 * the line through it that follows the flow both ways, a pixel a step (a line integral convolution)
 */
std::vector<std::vector<float>> alongFlow(const std::vector<const float *> &planes, const Flow &flow, qreal sigma, const QRect &area)
{
    const int w = flow.w, h = flow.h;
    // a pixel a step, longer ones for long flows: a smooth flow loses nothing to them
    const float stride = float(std::max(1.0, sigma / 3));
    const int steps = std::max(1, int(std::ceil(2 * sigma / stride)));
    std::vector<float> weights(steps + 1);
    for (int d = 0; d <= steps; d++) weights[d] = float(std::exp(-(d * stride) * (d * stride) / (2 * sigma * sigma)));
    const int count = int(planes.size());
    std::vector<std::vector<float>> out(count, std::vector<float>(size_t(area.width()) * area.height()));
    std::vector<float> sum(count);
    for (int y = area.top(); y <= area.bottom(); y++) {
        for (int x = area.left(); x <= area.right(); x++) {
            const size_t here = size_t(y) * w + x;
            for (int c = 0; c < count; c++) sum[c] = planes[c][here];
            float total = 1;
            for (const float way : {1.0f, -1.0f}) {
                float px = x + 0.5f, py = y + 0.5f;
                float vx = way * flow.x[here], vy = way * flow.y[here];
                for (int d = 1; d <= steps; d++) {
                    px += vx * stride;
                    py += vy * stride;
                    const int ix = int(std::floor(px)), iy = int(std::floor(py));
                    if (ix < 0 || iy < 0 || ix >= w || iy >= h) break;
                    const size_t at = size_t(iy) * w + ix;
                    // the flow has no sign: keep going the way it went
                    float nx = flow.x[at], ny = flow.y[at];
                    if (nx * vx + ny * vy < 0) {
                        nx = -nx;
                        ny = -ny;
                    }
                    const float weight = weights[d];
                    for (int c = 0; c < count; c++) sum[c] += weight * readAt(planes[c], w, h, px, py);
                    total += weight;
                    vx = nx;
                    vy = ny;
                }
            }
            const size_t to = size_t(y - area.top()) * area.width() + (x - area.left());
            for (int c = 0; c < count; c++) out[c][to] = sum[c] / total;
        }
    }
    return out;
}

/// The integral from 0 to \p x of ink(t) = -tanh(t / \p width) below 0 and nothing above (XDoG's soft threshold)
inline double inkIntegral(double x, double width)
{
    if (x >= 0) return 0;
    const double y = -x / width;
    // -width ln cosh(y), written so that it doesn't overflow
    return -width * (y + std::log1p(std::exp(-2 * y)) - M_LN2);
}

/**
 * The ink of a pixel whose difference of Gaussians is \p d and changes by \p slope across it, below \p threshold:
 * the soft threshold averaged over the pixel, so edges come out smooth however sharp the ink is
 */
inline float inkOver(float d, float slope, float threshold, float width)
{
    const double e = d - threshold;
    if (slope < 1e-5f) return e >= 0 ? 0.0f : float(-std::tanh(e / width));
    const double half = 0.5 * slope;
    return float(std::clamp((inkIntegral(e + half, width) - inkIntegral(e - half, width)) / slope, 0.0, 1.0));
}

inline float hashed(quint32 a, quint32 b, quint32 c)
{
    quint32 h = a * 0x9E3779B9u ^ b * 0x85EBCA6Bu ^ c * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    return float(h & 0xFFFFFF) / float(0xFFFFFF);
}

/**
 * A hatch as drawn by hand, at \p along and \p across its direction (in the image's pixels): rows \p spacing apart,
 * each a little off its place and wavering, broken into strokes of their own lengths with tapered ends. A stroke
 * shows where the tone's \p cover passes its own rank, so a light tone has a few strokes and a dark one all of them.
 */
float strokeAt(float along, float across, float cover, int tone, float spacing, float width, float pixel)
{
    const int nearest = int(std::floor(across / spacing + 0.5f));
    float ink = 0;
    for (int row = nearest - 1; row <= nearest + 1; row++) {
        const quint32 r = quint32(row), t = quint32(tone) + 1;
        const float wave = spacing * 0.15f * std::sin(along / (spacing * (4 + 4 * hashed(r, t, 1))) + float(tau) * hashed(r, t, 2));
        const float distance = std::abs(across - (row + (hashed(r, t, 3) - 0.5f) * 0.3f) * spacing - wave);
        if (distance > width + pixel) continue;
        const float length = spacing * (3 + 5 * hashed(r, t, 4));
        const float position = along / length + hashed(r, t, 5);
        const float within = position - std::floor(position);
        const float rank = hashed(r, quint32(int(std::floor(position))), t + 16);
        const float shows = std::clamp((cover - rank) * 8 + 0.5f, 0.0f, 1.0f);
        // a little gap between strokes, and each thinning toward its ends
        const float end = std::clamp(std::min(within - 0.06f, 0.94f - within) / 0.18f, 0.0f, 1.0f);
        if (shows <= 0 || end <= 0) continue;
        const float half = width * 0.5f * (0.35f + 0.65f * std::sqrt(end));
        ink = std::max(ink, shows * std::clamp((half - distance) / pixel + 0.5f, 0.0f, 1.0f));
    }
    return ink;
}

inline QColor colorFrom(const KisPropertiesConfiguration &config, const char *key, const QColor &fallback)
{
    const QColor color(config.getString(key, fallback.name(QColor::HexArgb)));
    return color.isValid() ? color : fallback;
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings settings;
    settings.size = float(std::clamp(config.getDouble("size", 1.5), 0.3, 40.0));
    settings.lines = float(std::clamp(config.getDouble("lines", 0.5), 0.0, 1.0));
    settings.ink = float(std::clamp(config.getDouble("ink", 0.3), 0.0, 1.0));
    settings.level = float(std::clamp(config.getDouble("level", -1), -1.0, 1.5));
    settings.softness = float(std::clamp(config.getDouble("softness", 0.05), 0.0, 1.0));
    settings.flow = float(std::clamp(config.getDouble("flow", 0.5), 0.0, 1.0));
    settings.tones = std::clamp(config.getInt("tones", 1), 1, 8);
    settings.hatching = float(std::clamp(config.getDouble("hatching", 0), 0.0, 1.0));
    settings.spacing = float(std::clamp(config.getDouble("spacing", 8), 2.0, 200.0));
    settings.angle = float(std::clamp(config.getDouble("angle", 45), -180.0, 180.0));
    settings.colors = colorsNamed(config.getString("colors", QStringLiteral("paper")));
    settings.inkColor = colorFrom(config, "ink_color", Qt::black);
    settings.paperColor = colorFrom(config, "paper_color", Qt::white);
    return settings;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("size", double(size));
    config.setProperty("lines", double(lines));
    config.setProperty("ink", double(ink));
    config.setProperty("level", double(level));
    config.setProperty("softness", double(softness));
    config.setProperty("flow", double(flow));
    config.setProperty("tones", tones);
    config.setProperty("hatching", double(hatching));
    config.setProperty("spacing", double(spacing));
    config.setProperty("angle", double(angle));
    config.setProperty("colors", nameOf(colors));
    config.setProperty("ink_color", inkColor.name(QColor::HexArgb));
    config.setProperty("paper_color", paperColor.name(QColor::HexArgb));
}

bool Settings::sameLook(const Settings &other) const
{
    return size == other.size && lines == other.lines && ink == other.ink && softness == other.softness && flow == other.flow
        && tones == other.tones && hatching == other.hatching && (hatching == 0 || (spacing == other.spacing && angle == other.angle))
        && colors == other.colors;
}

bool Settings::operator==(const Settings &other) const
{
    return sameLook(other) && level == other.level && spacing == other.spacing && angle == other.angle && inkColor == other.inkColor
        && paperColor == other.paperColor;
}

bool Settings::changesNothing() const
{
    return colors == Colors::Over && lines == 0 && ink == 0 && flow == 0;
}

Settings inked()
{
    return Settings();
}

Ink::Ink(const Settings &settings, qreal scale)
    : m_settings(settings)
    , m_scale(scale)
{
    m_sigma = settings.size * scale;
    // smoother flow for bolder lines, as their edges are wider
    m_tensorSigma = (1.5 + settings.size) * scale;
    m_flowSigma = settings.flow * (6 + 4 * settings.size) * scale;
    // after the threshold, the lines' edges smoothed along them; fading in with the flow
    m_smoothSigma = std::min(1.0, 4.0 * settings.flow) * scale;
    if (m_flowSigma < 0.3) m_flowSigma = 0;
    if (m_smoothSigma < 0.3) m_smoothSigma = 0;
}

int Ink::reach() const
{
    if (m_settings.changesNothing()) return 0;
    const int blurs = std::max(SkissaDetail::reachOf(wider * m_sigma), m_flowSigma > 0 ? SkissaDetail::reachOf(m_tensorSigma) + 1 : 0);
    return int(std::ceil(2 * m_flowSigma + 2 * m_smoothSigma)) + blurs + 2;
}

void Ink::apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const
{
    const int w = sourceRect.width(), h = sourceRect.height();
    const size_t count = size_t(w) * h;
    const QRect inside = rect.translated(-sourceRect.topLeft());
    const Settings &s = m_settings;
    if (s.changesNothing() || count == 0) {
        for (int y = 0; y < rect.height(); y++) {
            std::copy_n(source + (size_t(inside.top() + y) * w + inside.left()) * 4, rect.width() * 4, out + size_t(y) * rect.width() * 4);
        }
        return;
    }
    const bool flows = m_flowSigma > 0;

    // lightness, and the colors where the flow is found, as seen on white paper
    std::vector<float> lab[3];
    lab[0].resize(count);
    if (flows) {
        lab[1].resize(count);
        lab[2].resize(count);
    }
    for (size_t i = 0; i < count; i++) {
        const float *pixel = source + i * 4;
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        float c[3];
        oklabOf(pixel[0] * alpha + 1 - alpha, pixel[1] * alpha + 1 - alpha, pixel[2] * alpha + 1 - alpha, c);
        lab[0][i] = c[0];
        if (flows) {
            lab[1][i] = c[1];
            lab[2][i] = c[2];
        }
    }

    // the difference of Gaussians, as the picture sharpened by it: edges of this size pushed apart
    std::vector<float> near = lab[0], far = lab[0], scratch(count);
    if (m_sigma > 0.25) SkissaDetail::gaussian(near.data(), scratch.data(), w, h, m_sigma);
    SkissaDetail::gaussian(far.data(), scratch.data(), w, h, wider * std::max(m_sigma, 0.25));
    const float sharpness = mostSharpness * s.lines * s.lines;
    // hatched, the tones come from the picture's lightness alone, so lines stay whole rather than turning to strokes
    const bool hatches = s.hatching > 0 && s.ink > 0;
    std::vector<float> smooth;
    if (hatches) smooth = near;
    std::vector<float> &difference = near;
    for (size_t i = 0; i < count; i++) {
        const float edge = near[i] - far[i];
        difference[i] = near[i] + sharpness * std::copysign(std::max(0.0f, std::abs(edge) - leastEdge), edge);
    }
    far = std::vector<float>();

    // along the flow, over what the smoothing after the threshold reads
    Flow flow;
    const int smoothReach = int(std::ceil(2 * m_smoothSigma)) + 1;
    const QRect work = inside.adjusted(-smoothReach, -smoothReach, smoothReach, smoothReach) & QRect(0, 0, w, h);
    std::vector<float> drawn, toned;
    if (flows) {
        flow = flowOf(lab, w, h, m_tensorSigma);
        std::vector<const float *> planes = {difference.data()};
        if (hatches) planes.push_back(smooth.data());
        std::vector<std::vector<float>> along = alongFlow(planes, flow, m_flowSigma, work);
        drawn = std::move(along[0]);
        if (hatches) toned = std::move(along[1]);
    } else {
        auto cut = [&](const std::vector<float> &plane) {
            std::vector<float> part(size_t(work.width()) * work.height());
            for (int y = 0; y < work.height(); y++) {
                std::copy_n(plane.data() + size_t(work.top() + y) * w + work.left(), work.width(), part.data() + size_t(y) * work.width());
            }
            return part;
        };
        drawn = cut(difference);
        if (hatches) toned = cut(smooth);
    }

    /*
     * Ink below the threshold: 1 + tanh((D - ε) / width) of paper, as XDoG's soft threshold, averaged over each
     * pixel. Tones are thresholds spread evenly below it, each as soft; hatched, each tone is strokes of its own
     * direction, more of them the darker it is.
     */
    const int tones = std::max(1, s.tones);
    const float top = s.ink <= 0 ? 0.0f : s.level >= 0 ? s.level : s.ink;
    const float width = (0.002f + 0.45f * s.softness * s.softness) / tones;
    const float spacing = std::max(2.0f, s.spacing);
    const float strokeWidth = std::max(1.0f, 0.25f * spacing);
    float turnX[8], turnY[8];
    for (int t = 0; t < tones; t++) {
        const double angle = qDegreesToRadians(double(s.angle + hatchTurns[t % 8]));
        turnX[t] = float(std::cos(angle));
        turnY[t] = float(std::sin(angle));
    }
    const float pixel = float(1 / m_scale);
    const int ww = work.width(), wh = work.height();
    // how much a plane changes across a pixel there, for the threshold's average over it
    auto slopeAt = [ww, wh](const std::vector<float> &plane, int x, int y) {
        const float *row = plane.data() + size_t(y) * ww;
        const float across = 0.5f * (row[std::min(x + 1, ww - 1)] - row[std::max(x - 1, 0)]);
        const float down = 0.5f * (plane[size_t(std::min(y + 1, wh - 1)) * ww + x] - plane[size_t(std::max(y - 1, 0)) * ww + x]);
        return std::sqrt(across * across + down * down);
    };
    std::vector<float> coverage(drawn.size());
    for (int y = 0; y < wh; y++) {
        // hatches are fixed to the image's own pixels, whatever the level of detail
        const float imageY = (sourceRect.top() + work.top() + y + 0.5f) * pixel;
        for (int x = 0; x < ww; x++) {
            const size_t i = size_t(y) * ww + x;
            const float d = drawn[i], slope = slopeAt(drawn, x, y);
            float flat = 0;
            for (int t = 0; t < tones; t++) flat += inkOver(d, slope, top * float(tones - t) / tones, width);
            flat /= tones;
            if (!hatches) {
                coverage[i] = flat;
                continue;
            }
            // the lines, whole: what the edges add to the lightest tone
            const float tone = toned[i], toneSlope = slopeAt(toned, x, y);
            const float lines = std::max(0.0f, inkOver(d, slope, top, width) - inkOver(tone, toneSlope, top, width));
            const float imageX = (sourceRect.left() + work.left() + x + 0.5f) * pixel;
            float paper = 1 - lines;
            for (int t = 0; t < tones; t++) {
                // the lightest tone first
                const float cover = inkOver(tone, toneSlope, top * float(tones - t) / tones, width);
                if (cover <= 0) continue;
                const float along = imageX * -turnY[t] + imageY * turnX[t];
                const float across = imageX * turnX[t] + imageY * turnY[t];
                // darker tones press harder: their strokes up to twice as wide, so the darkest close up
                const float pressed = strokeWidth * (1 + float(t) / std::max(1, tones - 1));
                paper *= 1 - strokeAt(along, across, cover, t, spacing, pressed, pixel);
            }
            coverage[i] = flat + s.hatching * (1 - paper - flat);
        }
    }

    // the edges smoothed along the lines, so they come out clean
    std::vector<float> smoothed;
    const QRect inInked = inside.translated(-work.topLeft());
    if (flows && m_smoothSigma > 0) {
        Flow local;
        local.w = work.width();
        local.h = work.height();
        local.x.resize(coverage.size());
        local.y.resize(coverage.size());
        for (int y = 0; y < local.h; y++) {
            for (int x = 0; x < local.w; x++) {
                const size_t from = size_t(work.top() + y) * w + work.left() + x;
                local.x[size_t(y) * local.w + x] = flow.x[from];
                local.y[size_t(y) * local.w + x] = flow.y[from];
            }
        }
        smoothed = std::move(alongFlow({coverage.data()}, local, m_smoothSigma, inInked)[0]);
    } else {
        smoothed.resize(size_t(rect.width()) * rect.height());
        for (int y = 0; y < rect.height(); y++) {
            std::copy_n(coverage.data() + size_t(inInked.top() + y) * work.width() + inInked.left(), rect.width(), smoothed.data() + size_t(y) * rect.width());
        }
    }

    // the picture under the ink, smoothed along the flow as the lines are, so it looks painted with them
    std::vector<std::vector<float>> picture;
    if (s.colors == Colors::Over || s.colors == Colors::Colored) {
        std::vector<float> premultiplied[4];
        for (int c = 0; c < 4; c++) premultiplied[c].resize(count);
        for (size_t i = 0; i < count; i++) {
            const float alpha = std::clamp(source[i * 4 + 3], 0.0f, 1.0f);
            for (int c = 0; c < 3; c++) premultiplied[c][i] = source[i * 4 + c] * alpha;
            premultiplied[3][i] = alpha;
        }
        if (flows) {
            picture = alongFlow({premultiplied[0].data(), premultiplied[1].data(), premultiplied[2].data(), premultiplied[3].data()}, flow, m_flowSigma, inside);
        } else {
            picture.assign(4, std::vector<float>(size_t(rect.width()) * rect.height()));
            for (int c = 0; c < 4; c++) {
                for (int y = 0; y < rect.height(); y++) {
                    std::copy_n(premultiplied[c].data() + size_t(inside.top() + y) * w + inside.left(), rect.width(), picture[c].data() + size_t(y) * rect.width());
                }
            }
        }
    }

    auto linear = [](const QColor &color, float *to) {
        to[0] = float(KisColorMath::toLinear(color.redF()));
        to[1] = float(KisColorMath::toLinear(color.greenF()));
        to[2] = float(KisColorMath::toLinear(color.blueF()));
        to[3] = float(color.alphaF());
    };
    float ink[4], paper[4];
    linear(s.inkColor, ink);
    linear(s.paperColor, paper);
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const size_t i = size_t(y) * rect.width() + x;
            const float cover = std::clamp(smoothed[i], 0.0f, 1.0f);
            float *to = out + i * 4;
            switch (s.colors) {
            case Colors::Paper: {
                // ink over paper, both as opaque as their colors
                const float inkAlpha = cover * ink[3];
                const float alpha = inkAlpha + paper[3] * (1 - inkAlpha);
                for (int c = 0; c < 3; c++) to[c] = alpha > 1e-6f ? (ink[c] * inkAlpha + paper[c] * paper[3] * (1 - inkAlpha)) / alpha : 0.0f;
                to[3] = alpha;
                break;
            }
            case Colors::Clear:
                for (int c = 0; c < 3; c++) to[c] = ink[c];
                to[3] = cover * ink[3];
                break;
            case Colors::Over: {
                const float inkAlpha = cover * ink[3];
                const float alpha = inkAlpha + picture[3][i] * (1 - inkAlpha);
                for (int c = 0; c < 3; c++) to[c] = alpha > 1e-6f ? (ink[c] * inkAlpha + picture[c][i] * (1 - inkAlpha)) / alpha : 0.0f;
                to[3] = alpha;
                break;
            }
            case Colors::Colored: {
                // the picture's color where it has one, the ink's where it is clear
                const float shown = picture[3][i];
                float color[3];
                // pigment laid over itself, as a pencil's color is deeper than the picture's average of it
                for (int c = 0; c < 3; c++) {
                    const float own = shown > 1e-6f ? picture[c][i] / shown : ink[c];
                    color[c] = own * (0.4f + 0.6f * own);
                }
                const float alpha = cover + paper[3] * (1 - cover);
                for (int c = 0; c < 3; c++) to[c] = alpha > 1e-6f ? (color[c] * cover + paper[c] * paper[3] * (1 - cover)) / alpha : 0.0f;
                to[3] = alpha;
                break;
            }
            }
        }
    }
}

QImage applied(const QImage &image, const Settings &settings, qreal scale)
{
    const QImage picture = image.convertToFormat(QImage::Format_ARGB32);
    const QRect rect = picture.rect();
    const Ink ink(settings, scale);
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
    ink.apply(in.data(), rect, rect, out.data());
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

} // namespace SkissaInk
