/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaLight.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <QtMath>

#include <kis_properties_configuration.h>

#include "KisColorMath.h"
#include "KoCompositeOpInLinear.h"
#include "SkissaDetail.h"

namespace SkissaLight
{

namespace
{

/// The finest glow, in pixels: the octaves start here
constexpr qreal finestGlow = 1.5;
/// The glow's falloff: each octave weighs its size to this power (glare falls off about as r^-2.6)
constexpr qreal glowFalloff = -0.6;
/// How much the longest and shortest waves' spikes reach beyond the middle ones'
constexpr float redReach = 1.2f, blueReach = 0.8f;
/// A spike is followed until it has faded to e^-4
constexpr float spikeEnd = 4;
/// Spikes are made on a grid this many cells along a spike at most, then enlarged: they are smooth along it
constexpr int spikeCells = 24;

struct Plane3 {
    int w {0}, h {0};
    std::vector<float> c[3];
    void resize(int width, int height)
    {
        w = width;
        h = height;
        for (auto &plane : c) plane.assign(size_t(w) * h, 0.0f);
    }
};

inline float readAt(const std::vector<float> &plane, int w, int h, float x, float y)
{
    x = std::clamp(x, 0.0f, float(w - 1));
    y = std::clamp(y, 0.0f, float(h - 1));
    const int ix = std::min(int(x), std::max(0, w - 2)), iy = std::min(int(y), std::max(0, h - 2));
    const int jx = std::min(ix + 1, w - 1), jy = std::min(iy + 1, h - 1);
    const float tx = x - ix, ty = y - iy;
    const float top = plane[size_t(iy) * w + ix] + (plane[size_t(iy) * w + jx] - plane[size_t(iy) * w + ix]) * tx;
    const float bottom = plane[size_t(jy) * w + ix] + (plane[size_t(jy) * w + jx] - plane[size_t(jy) * w + ix]) * tx;
    return top + (bottom - top) * ty;
}

/**
 * Spikes from every bright part of \p shine along \p directions (degrees, both ways), each \p length long (pixels
 * of \p shine, which covers \p rect), their light in \p tint, added to \p light over \p area (both in \p rect's
 * pixels). Made on a grid of cells fixed to the image's pixels, coarse enough that a spike spans few of them.
 */
void addSpikes(const Plane3 &shine, const QRect &rect, const QRect &area, const std::vector<float> &directions, float length, const float tint[3],
               Plane3 &light)
{
    if (length < 1 || directions.empty()) return;
    const int cell = std::max(1, int(std::ceil(length / spikeCells)));
    auto down = [cell](int v) { return v >= 0 ? v / cell : -((-v + cell - 1) / cell); };
    // the cells over the source, each the average of its pixels
    const QRect cells(QPoint(down(rect.left()), down(rect.top())), QPoint(down(rect.right()), down(rect.bottom())));
    Plane3 coarse;
    coarse.resize(cells.width(), cells.height());
    std::vector<float> counts(size_t(cells.width()) * cells.height(), 0.0f);
    for (int y = 0; y < shine.h; y++) {
        const int cy = down(rect.top() + y) - cells.top();
        for (int x = 0; x < shine.w; x++) {
            const size_t to = size_t(cy) * cells.width() + (down(rect.left() + x) - cells.left());
            for (int c = 0; c < 3; c++) coarse.c[c][to] += shine.c[c][size_t(y) * shine.w + x];
            counts[to] += 1;
        }
    }
    for (size_t i = 0; i < counts.size(); i++) {
        if (counts[i] > 0) {
            for (int c = 0; c < 3; c++) coarse.c[c][i] /= counts[i];
        }
    }
    // each wavelength's spike its own length; each spike holds as much light as shone, shared out along it
    const float cellLength = length / cell;
    const float reach[3] = {cellLength * redReach, cellLength, cellLength * blueReach};
    const int steps = int(std::ceil(spikeEnd * reach[0]));
    std::vector<std::array<float, 3>> weights(steps + 1);
    for (int c = 0; c < 3; c++) {
        double total = 0;
        for (int t = 1; t <= steps; t++) total += 2 * std::exp(-t / reach[c]);
        for (int t = 1; t <= steps; t++) weights[t][c] = float(std::exp(-t / reach[c]) / std::max(1e-9, total)) * tint[c];
    }
    Plane3 spikes;
    spikes.resize(cells.width(), cells.height());
    for (float degrees : directions) {
        const float dx = float(std::cos(qDegreesToRadians(double(degrees)))), dy = float(std::sin(qDegreesToRadians(double(degrees))));
        for (int y = 0; y < cells.height(); y++) {
            for (int x = 0; x < cells.width(); x++) {
                float sum[3] = {0, 0, 0};
                for (int t = 1; t <= steps; t++) {
                    for (const float way : {1.0f, -1.0f}) {
                        const float px = x + way * t * dx, py = y + way * t * dy;
                        if (px < 0 || py < 0 || px > cells.width() - 1 || py > cells.height() - 1) continue;
                        for (int c = 0; c < 3; c++) sum[c] += weights[t][c] * readAt(coarse.c[c], cells.width(), cells.height(), px, py);
                    }
                }
                for (int c = 0; c < 3; c++) spikes.c[c][size_t(y) * cells.width() + x] += sum[c] / directions.size();
            }
        }
    }
    // enlarged back over the pixels, between the cells' middles
    for (int y = 0; y < area.height(); y++) {
        const float cy = (float(area.top() + y) + 0.5f) / cell - 0.5f - cells.top();
        for (int x = 0; x < area.width(); x++) {
            const float cx = (float(area.left() + x) + 0.5f) / cell - 0.5f - cells.left();
            const size_t to = size_t(y) * light.w + x;
            for (int c = 0; c < 3; c++) light.c[c][to] += readAt(spikes.c[c], cells.width(), cells.height(), cx, cy);
        }
    }
}

void linearOf(const QColor &color, float *to)
{
    to[0] = float(KisColorMath::toLinear(color.redF()));
    to[1] = float(KisColorMath::toLinear(color.greenF()));
    to[2] = float(KisColorMath::toLinear(color.blueF()));
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings s;
    s.threshold = float(std::clamp(config.getDouble("threshold", s.threshold), 0.0, 1.0));
    s.top = float(std::clamp(config.getDouble("top", s.top), 0.05, 4.0));
    s.glow = float(std::clamp(config.getDouble("glow", s.glow), 0.0, 4.0));
    s.size = float(std::clamp(config.getDouble("size", s.size), 2.0, 1000.0));
    s.star = float(std::clamp(config.getDouble("star", s.star), 0.0, 4.0));
    s.length = float(std::clamp(config.getDouble("length", s.length), 2.0, 2000.0));
    s.points = std::clamp(config.getInt("points", s.points), 2, 16) / 2 * 2;
    s.angle = float(std::clamp(config.getDouble("angle", s.angle), -180.0, 180.0));
    s.streak = float(std::clamp(config.getDouble("streak", s.streak), 0.0, 4.0));
    s.streakLength = float(std::clamp(config.getDouble("streak_length", s.streakLength), 2.0, 4000.0));
    s.streakAngle = float(std::clamp(config.getDouble("streak_angle", s.streakAngle), -180.0, 180.0));
    const QColor color(config.getString("streak_color", s.streakColor.name()));
    if (color.isValid()) s.streakColor = color;
    return s;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("threshold", double(threshold));
    config.setProperty("top", double(top));
    config.setProperty("glow", double(glow));
    config.setProperty("size", double(size));
    config.setProperty("star", double(star));
    config.setProperty("length", double(length));
    config.setProperty("points", points);
    config.setProperty("angle", double(angle));
    config.setProperty("streak", double(streak));
    config.setProperty("streak_length", double(streakLength));
    config.setProperty("streak_angle", double(streakAngle));
    config.setProperty("streak_color", streakColor.name());
}

bool Settings::operator==(const Settings &other) const
{
    return threshold == other.threshold && top == other.top && glow == other.glow && size == other.size && star == other.star && length == other.length
        && points == other.points && angle == other.angle && streak == other.streak && streakLength == other.streakLength
        && streakAngle == other.streakAngle && streakColor == other.streakColor;
}

bool Settings::changesNothing() const
{
    return glow <= 0 && star <= 0 && streak <= 0;
}

Light::Light(const Settings &settings, qreal scale)
    : m_settings(settings)
    , m_scale(scale)
{
}

int Light::reach() const
{
    const Settings &s = m_settings;
    if (s.changesNothing()) return 0;
    qreal reach = 0;
    if (s.glow > 0) reach = s.size;
    if (s.star > 0) reach = std::max<qreal>(reach, spikeEnd * redReach * s.length);
    if (s.streak > 0) reach = std::max<qreal>(reach, spikeEnd * redReach * s.streakLength);
    return int(std::ceil(reach * m_scale)) + 4;
}

void Light::apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const
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

    // what shines: the light above the threshold, with a soft knee, in the picture's own colors
    Plane3 shine;
    shine.resize(w, h);
    const float cutoff = s.threshold * s.top, knee = 0.08f;
    for (size_t i = 0; i < count; i++) {
        const float *pixel = source + i * 4;
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        const float rgb[3] = {std::max(0.0f, pixel[0]) * alpha, std::max(0.0f, pixel[1]) * alpha, std::max(0.0f, pixel[2]) * alpha};
        const float seen = std::cbrt(0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2]);
        const float over = seen - cutoff;
        const float excess = over <= -knee ? 0.0f : over >= knee ? over : (over + knee) * (over + knee) / (4 * knee);
        if (excess <= 0 || seen <= 1e-4f) continue;
        // the share of its light above the cutoff, as light (the cube of what is seen); a picture's whitest parts
        // stand for light far brighter than white, clipped (the inverse of a Reinhard curve, up to ten times)
        const float below = std::max(0.0f, seen - excess) / seen;
        const float share = 1 - below * below * below;
        const float luminance = std::min(1.0f, seen * seen * seen);
        const float unclipped = 1 / (1 - 0.9f * luminance);
        for (int c = 0; c < 3; c++) shine.c[c][i] = rgb[c] * share * unclipped;
    }

    Plane3 light;
    light.resize(rect.width(), rect.height());
    if (s.glow > 0) {
        // Gaussians an octave apart up to the size, weighed by the falloff's power of their size
        const qreal largest = std::max(finestGlow, s.size * m_scale / 3);
        std::vector<qreal> sigmas;
        for (qreal sigma = finestGlow; sigma < largest * 1.01; sigma *= 2) sigmas.push_back(std::min(sigma, largest));
        double total = 0;
        for (qreal sigma : sigmas) total += std::pow(sigma, glowFalloff);
        std::vector<float> blurred(count), scratch(count);
        for (qreal sigma : sigmas) {
            const float weight = float(std::pow(sigma, glowFalloff) / total * s.glow);
            for (int c = 0; c < 3; c++) {
                blurred = shine.c[c];
                SkissaDetail::gaussian(blurred.data(), scratch.data(), w, h, sigma);
                for (int y = 0; y < rect.height(); y++) {
                    const float *from = blurred.data() + size_t(inside.top() + y) * w + inside.left();
                    float *to = light.c[c].data() + size_t(y) * rect.width();
                    for (int x = 0; x < rect.width(); x++) to[x] += weight * from[x];
                }
            }
        }
    }
    if (s.star > 0) {
        std::vector<float> directions;
        const int pairs = std::max(1, s.points / 2);
        for (int j = 0; j < pairs; j++) directions.push_back(s.angle + 180.0f * j / pairs);
        const float tint[3] = {s.star, s.star, s.star};
        addSpikes(shine, sourceRect, rect, directions, float(s.length * m_scale), tint, light);
    }
    if (s.streak > 0) {
        float tint[3];
        linearOf(s.streakColor, tint);
        // the color's hue at the streak's strength: a lighter color doesn't shine more
        const float most = std::max({tint[0], tint[1], tint[2], 1e-4f});
        for (float &c : tint) c = c / most * s.streak;
        addSpikes(shine, sourceRect, rect, {s.streakAngle}, float(s.streakLength * m_scale), tint, light);
    }

    // added as light; over what is clear, as opaque as it is bright
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const float *pixel = source + (size_t(inside.top() + y) * w + inside.left() + x) * 4;
            const size_t i = size_t(y) * rect.width() + x;
            const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
            const float added[3] = {light.c[0][i], light.c[1][i], light.c[2][i]};
            const float shown = std::min(1.0f, std::max({added[0], added[1], added[2]}));
            const float newAlpha = alpha + (1 - alpha) * shown;
            float *to = out + i * 4;
            for (int c = 0; c < 3; c++) to[c] = newAlpha > 1e-6f ? (pixel[c] * alpha + added[c]) / newAlpha : 0.0f;
            to[3] = newAlpha;
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
    Light(settings, scale).apply(in.data(), rect, rect, out.data());
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

} // namespace SkissaLight
