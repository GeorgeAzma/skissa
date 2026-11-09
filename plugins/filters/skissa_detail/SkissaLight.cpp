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
#include "SkissaPixels.h"

namespace SkissaLight
{

namespace
{

/// The finest glow, in the image's pixels: its sizes start here
constexpr qreal finestGlow = 1.5;
/// The glow's falloff: each octave weighs its size to this power (glare falls off about as r^-2.6)
constexpr qreal glowFalloff = -0.6;
/// How much the longest and shortest waves' spikes reach beyond the middle ones'
constexpr float redReach = 1.2f, blueReach = 0.8f;
/// A spike is followed until it has faded to e^-4
constexpr float spikeEnd = 4;
/// Spikes are made on a grid this many cells along a spike at most, then enlarged
constexpr int spikeCells = 64;

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
    /*
     * Each wavelength's spike its own length, fading exponentially along it, and holding as much light as shone:
     * the fade is a recursive filter, run both ways along the rows of the grid turned to the spike's direction, so
     * a spike of any length costs a few passes over the grid
     */
    const float cellLength = length / cell;
    const float reach[3] = {cellLength * redReach, cellLength, cellLength * blueReach};
    float decay[3], share[3];
    for (int c = 0; c < 3; c++) {
        decay[c] = std::exp(-1 / reach[c]);
        // a fade of a^t from t = 1 on, both ways, adds to 2a / (1 - a)
        share[c] = tint[c] * (1 - decay[c]) / (2 * decay[c]) / directions.size();
    }
    const int cw = cells.width(), ch = cells.height();
    const float middleX = (cw - 1) / 2.0f, middleY = (ch - 1) / 2.0f;
    auto inGrid = [](const std::vector<float> &plane, int w, int h, float x, float y) {
        return x < -0.5f || y < -0.5f || x > w - 0.5f || y > h - 0.5f ? 0.0f : readAt(plane, w, h, x, y);
    };
    Plane3 spikes;
    spikes.resize(cw, ch);
    Plane3 turned;
    std::vector<float> faded;
    for (float degrees : directions) {
        const float dx = float(std::cos(qDegreesToRadians(double(degrees)))), dy = float(std::sin(qDegreesToRadians(double(degrees))));
        // the grid turned so the spike runs along its rows
        const int tw = int(std::ceil(std::abs(dx) * cw + std::abs(dy) * ch)) + 2, th = int(std::ceil(std::abs(dy) * cw + std::abs(dx) * ch)) + 2;
        const float turnedX = (tw - 1) / 2.0f, turnedY = (th - 1) / 2.0f;
        turned.resize(tw, th);
        for (int v = 0; v < th; v++) {
            for (int u = 0; u < tw; u++) {
                const float a = u - turnedX, b = v - turnedY;
                const float x = middleX + a * dx - b * dy, y = middleY + a * dy + b * dx;
                for (int c = 0; c < 3; c++) turned.c[c][size_t(v) * tw + u] = inGrid(coarse.c[c], cw, ch, x, y);
            }
        }
        faded.resize(size_t(tw) * th);
        for (int c = 0; c < 3; c++) {
            for (int v = 0; v < th; v++) {
                const float *row = turned.c[c].data() + size_t(v) * tw;
                float *to = faded.data() + size_t(v) * tw;
                float carried = 0;
                for (int u = 0; u < tw; u++) {
                    to[u] = carried;
                    carried = decay[c] * (carried + row[u]);
                }
                carried = 0;
                for (int u = tw - 1; u >= 0; u--) {
                    to[u] += carried;
                    carried = decay[c] * (carried + row[u]);
                }
            }
            // turned back onto the grid
            for (int y = 0; y < ch; y++) {
                for (int x = 0; x < cw; x++) {
                    const float px = x - middleX, py = y - middleY;
                    const float u = px * dx + py * dy + turnedX, v = -px * dy + py * dx + turnedY;
                    spikes.c[c][size_t(y) * cw + x] += share[c] * inGrid(faded, tw, th, u, v);
                }
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

/// A level of the shine: its planes, and the cell its first one is (in cells of its size from the image's corner)
struct Level {
    QPoint origin;
    Plane3 planes;
};

/// \p finer halved: each of its cells the average of the cells of \p finer it covers
Level halved(const Level &finer)
{
    auto down = [](int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); };
    Level out;
    out.origin = QPoint(down(finer.origin.x()), down(finer.origin.y()));
    const int right = down(finer.origin.x() + finer.planes.w - 1), bottom = down(finer.origin.y() + finer.planes.h - 1);
    out.planes.resize(right - out.origin.x() + 1, bottom - out.origin.y() + 1);
    std::vector<float> counts(size_t(out.planes.w) * out.planes.h, 0.0f);
    for (int y = 0; y < finer.planes.h; y++) {
        const int oy = down(finer.origin.y() + y) - out.origin.y();
        for (int x = 0; x < finer.planes.w; x++) {
            const size_t to = size_t(oy) * out.planes.w + (down(finer.origin.x() + x) - out.origin.x());
            for (int c = 0; c < 3; c++) out.planes.c[c][to] += finer.planes.c[c][size_t(y) * finer.planes.w + x];
            counts[to] += 1;
        }
    }
    for (size_t i = 0; i < counts.size(); i++) {
        for (int c = 0; c < 3; c++) out.planes.c[c][i] /= std::max(1.0f, counts[i]);
    }
    return out;
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

Light::Light(const Settings &settings, qreal scale, bool clipped)
    : m_settings(settings)
    , m_scale(scale)
    , m_clipped(clipped)
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

    // what shines: the light above the threshold, in the picture's own colors
    Plane3 shine;
    shine.resize(w, h);
    const float cutoff = s.threshold * s.top;
    for (size_t i = 0; i < count; i++) {
        const float *pixel = source + i * 4;
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        const float premultiplied[4] = {std::max(0.0f, pixel[0]) * alpha, std::max(0.0f, pixel[1]) * alpha, std::max(0.0f, pixel[2]) * alpha, alpha};
        const float shines = SkissaPixels::shineOf(premultiplied, cutoff, m_clipped);
        if (shines <= 0) continue;
        for (int c = 0; c < 3; c++) shine.c[c][i] = premultiplied[c] * shines;
    }

    Plane3 light;
    light.resize(rect.width(), rect.height());
    if (s.glow > 0) {
        /*
         * Glare as Gaussians of every size from the finest to the size, each weighed by the falloff's power of its
         * size: sampled evenly by ratio, at most an octave apart, each weighed by the share of the range it stands
         * for, so the glow grows smoothly with the size and is the same, scaled, at every level of detail
         */
        const qreal finest = finestGlow * m_scale, largest = std::max(finestGlow, qreal(s.size) / 3) * m_scale;
        const int sizes = std::max(2, int(std::ceil(std::log2(largest / finest))) + 1);
        const qreal step = std::log2(largest / finest) / (sizes - 1);
        std::vector<qreal> sigmas, shares;
        for (int k = 0; k < sizes; k++) {
            sigmas.push_back(finest * std::exp2(k * step));
            shares.push_back(k == 0 || k == sizes - 1 ? 0.5 : 1.0);
        }
        double total = 0;
        for (size_t k = 0; k < sigmas.size(); k++) total += shares[k] * std::pow(sigmas[k], glowFalloff);
        /*
         * A wide glow is made on a smaller copy: levels of the shine each half the size of the one before (2 by 2
         * averages over cells fixed to the image's corner, so patches agree), each octave blurred on the coarsest
         * level where it spans two cells, less the blur the halving and the enlarging add, then enlarged
         */
        std::vector<Level> levels(1);
        levels[0].origin = sourceRect.topLeft();
        levels[0].planes = shine;
        std::vector<float> blurred, scratch;
        for (size_t k = 0; k < sigmas.size(); k++) {
            const qreal sigma = sigmas[k];
            const float weight = float(shares[k] * std::pow(sigma, glowFalloff) / total * s.glow);
            int level = 0;
            while (sigma / std::pow(2.0, level + 1) >= 2 && levels[level].planes.w > 8 && levels[level].planes.h > 8) {
                level++;
                if (level >= int(levels.size())) levels.push_back(halved(levels.back()));
            }
            const Level &at = levels[level];
            const double cell = std::pow(2.0, level);
            const double added = level ? (cell * cell - 1) / 12 + cell * cell / 6 : 0;
            const qreal left = std::sqrt(std::max(0.25, sigma * sigma - added)) / cell;
            const int lw = at.planes.w, lh = at.planes.h;
            blurred.resize(size_t(lw) * lh);
            scratch.resize(blurred.size());
            for (int c = 0; c < 3; c++) {
                blurred = at.planes.c[c];
                SkissaDetail::gaussian(blurred.data(), scratch.data(), lw, lh, left);
                for (int y = 0; y < rect.height(); y++) {
                    float *to = light.c[c].data() + size_t(y) * rect.width();
                    if (!level) {
                        const float *from = blurred.data() + size_t(inside.top() + y) * w + inside.left();
                        for (int x = 0; x < rect.width(); x++) to[x] += weight * from[x];
                        continue;
                    }
                    const float ly = float((rect.top() + y + 0.5) / cell - 0.5 - at.origin.y());
                    for (int x = 0; x < rect.width(); x++) {
                        const float lx = float((rect.left() + x + 0.5) / cell - 0.5 - at.origin.x());
                        to[x] += weight * readAt(blurred, lw, lh, lx, ly);
                    }
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
