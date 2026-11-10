/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisColorAreas.h"

#include <QThread>
#include <QVector>
#include <QtConcurrent>

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>

#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <kis_paint_device.h>
#include <kis_selection.h>

#include "KisColorMath.h"

namespace
{

using Color = std::array<float, 4>;

/// The derivative of a Gaussian of 1 px, and the Gaussian itself, out to 4 px either side
constexpr int kernelReach = 4;

struct Kernels {
    float smooth[2 * kernelReach + 1];
    float slope[2 * kernelReach + 1];

    Kernels()
    {
        float sum = 0;
        for (int k = -kernelReach; k <= kernelReach; k++) sum += smooth[k + kernelReach] = std::exp(-0.5f * k * k);
        for (int k = -kernelReach; k <= kernelReach; k++) {
            smooth[k + kernelReach] /= sum;
            // a ramp of slope 1 gives 1, so the gradient is in color per pixel
            slope[k + kernelReach] = k * smooth[k + kernelReach];
        }
    }
};

/// sRGB bytes in linear light
const float *linearTable()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t;
        for (int i = 0; i < 256; i++) t[size_t(i)] = float(KisColorMath::toLinear(i / 255.0));
        return t;
    }();
    return table.data();
}

/// A pixel (blue, green, red, alpha) as Oklab premultiplied by its opacity, with the opacity
Color oklabOf(const quint8 *pixel)
{
    const float *linear = linearTable();
    const KisColorMath::Oklab lab = KisColorMath::oklabFromLinear(linear[pixel[2]], linear[pixel[1]], linear[pixel[0]]);
    const float alpha = pixel[3] / 255.0f;
    return {float(lab.l) * alpha, float(lab.a) * alpha, float(lab.b) * alpha, alpha};
}

float distance(const Color &a, const Color &b)
{
    float d2 = 0;
    for (int c = 0; c < 4; c++) d2 += (a[size_t(c)] - b[size_t(c)]) * (a[size_t(c)] - b[size_t(c)]);
    return std::sqrt(d2);
}

} // namespace

struct KisColorAreas::Private {
    QRect bounds;
    int w {0};
    int h {0};
    /// The picture as 8-bit sRGB (blue, green, red, alpha)
    std::vector<quint8> pixels;
    /// How fast the color changes at each pixel, in Oklab distance per pixel
    std::vector<float> gradient;
    /// The gradient noise alone gives: its median, as most of a picture is smooth
    float noise {0};

    const quint8 *pixel(int x, int y) const { return pixels.data() + (size_t(y) * w + x) * 4; }
};

KisColorAreas::KisColorAreas(KisPaintDeviceSP device, const QRect &bounds)
{
    auto *p = new Private;
    p->bounds = bounds;
    p->w = bounds.width();
    p->h = bounds.height();
    const size_t count = size_t(p->w) * p->h;
    const KoColorSpace *srgb = KoColorSpaceRegistry::instance()->rgb8();
    p->pixels.resize(count * 4);
    if (*device->colorSpace() == *srgb) {
        device->readBytes(p->pixels.data(), bounds);
    } else {
        std::vector<quint8> raw(count * device->pixelSize());
        device->readBytes(raw.data(), bounds);
        device->colorSpace()->convertPixelsTo(raw.data(), p->pixels.data(), srgb, int(count), KoColorConversionTransformation::internalRenderingIntent(),
                                              KoColorConversionTransformation::internalConversionFlags());
    }

    // the gradient: each channel through the Gaussian's derivative one way and the Gaussian the other, in bands of rows
    p->gradient.resize(count);
    const Kernels kernels;
    const int w = p->w, h = p->h;
    constexpr int band = 32;
    QVector<int> starts;
    for (int y = 0; y < h; y += band) starts.append(y);
    QtConcurrent::blockingMap(starts, [&](int top) {
        const int bottom = std::min(h, top + band);
        const int first = std::max(0, top - kernelReach), last = std::min(h - 1, bottom - 1 + kernelReach);
        const int rows = last - first + 1;
        std::vector<Color> lab(size_t(rows) * w), along(size_t(rows) * w), across(size_t(rows) * w);
        for (int y = first; y <= last; y++) {
            for (int x = 0; x < w; x++) lab[size_t(y - first) * w + x] = oklabOf(p->pixel(x, y));
        }
        // along rows: smoothed and differentiated, the edges of the picture repeating
        for (int r = 0; r < rows; r++) {
            const Color *row = lab.data() + size_t(r) * w;
            for (int x = 0; x < w; x++) {
                Color s {0, 0, 0, 0}, d {0, 0, 0, 0};
                for (int k = -kernelReach; k <= kernelReach; k++) {
                    const Color &c = row[qBound(0, x - k, w - 1)];
                    for (int i = 0; i < 4; i++) {
                        s[size_t(i)] += kernels.smooth[k + kernelReach] * c[size_t(i)];
                        d[size_t(i)] += kernels.slope[k + kernelReach] * c[size_t(i)];
                    }
                }
                along[size_t(r) * w + x] = s;
                across[size_t(r) * w + x] = d;
            }
        }
        // down columns: the change across is the smoothed rows differentiated, the change along the differentiated ones smoothed
        for (int y = top; y < bottom; y++) {
            for (int x = 0; x < w; x++) {
                Color gx {0, 0, 0, 0}, gy {0, 0, 0, 0};
                for (int k = -kernelReach; k <= kernelReach; k++) {
                    const int r = qBound(0, y - k, h - 1) - first;
                    const Color &s = along[size_t(r) * w + x];
                    const Color &d = across[size_t(r) * w + x];
                    for (int i = 0; i < 4; i++) {
                        gx[size_t(i)] += kernels.smooth[k + kernelReach] * d[size_t(i)];
                        gy[size_t(i)] += kernels.slope[k + kernelReach] * s[size_t(i)];
                    }
                }
                float g2 = 0;
                for (int i = 0; i < 4; i++) g2 += gx[size_t(i)] * gx[size_t(i)] + gy[size_t(i)] * gy[size_t(i)];
                p->gradient[size_t(y) * w + x] = std::sqrt(g2);
            }
        }
    });
    // the median of a sample, every 7th pixel
    std::vector<float> sample;
    sample.reserve(count / 7 + 1);
    for (size_t i = 0; i < count; i += 7) sample.push_back(p->gradient[i]);
    if (!sample.empty()) {
        std::nth_element(sample.begin(), sample.begin() + sample.size() / 2, sample.end());
        p->noise = sample[sample.size() / 2];
    }
    d.reset(p);
}

QRect KisColorAreas::bounds() const
{
    return d->bounds;
}

std::vector<quint8> KisColorAreas::edges(qreal step) const
{
    const int w = d->w, h = d->h;
    const size_t count = size_t(w) * h;
    // a step of height s through the Gaussian's derivative peaks at s / sqrt(2 pi)
    const float strong = std::max(float(step / std::sqrt(2 * M_PI)), 3 * d->noise);
    const float faint = strong / 2;
    std::vector<quint8> edge(count, MIN_SELECTED);
    std::vector<int> stack;
    for (size_t i = 0; i < count; i++) {
        if (d->gradient[i] >= strong) {
            edge[i] = MAX_SELECTED;
            stack.push_back(int(i));
        }
    }
    while (!stack.empty()) {
        const int at = stack.back();
        stack.pop_back();
        const int x = at % w, y = at / w;
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                const int qx = x + dx, qy = y + dy;
                if (qx < 0 || qy < 0 || qx >= w || qy >= h) continue;
                const size_t q = size_t(qy) * w + qx;
                if (edge[q] || d->gradient[q] < faint) continue;
                edge[q] = MAX_SELECTED;
                stack.push_back(int(q));
            }
        }
    }
    return edge;
}

std::vector<quint8> KisColorAreas::area(const std::vector<quint8> &edges, const QPoint &point, int reach, bool everywhere, qreal step, QRect *rect) const
{
    const int w = d->w, h = d->h;
    const size_t count = size_t(w) * h;
    std::vector<quint8> mask(count, MIN_SELECTED);
    if (rect) *rect = QRect();
    const QPoint at = point - d->bounds.topLeft();
    if (at.x() < 0 || at.y() < 0 || at.x() >= w || at.y() >= h) return mask;

    // each area its own number, given as it is first reached; with its color's sum, sum of squares and size
    std::vector<int> label(count, -1);
    struct Stats {
        std::array<double, 4> sum {0, 0, 0, 0};
        double squares {0};
        qint64 size {0};
        QRect rect;
    };
    std::vector<Stats> stats;
    std::vector<int> stack;
    auto flood = [&](int start) {
        const int id = int(stats.size());
        stats.push_back(Stats());
        Stats &s = stats.back();
        int left = start % w, right = left, top = start / w, bottom = top;
        label[size_t(start)] = id;
        stack.push_back(start);
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const int x = i % w, y = i / w;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
            s.size++;
            const int around[4][2] = {{x - 1, y}, {x + 1, y}, {x, y - 1}, {x, y + 1}};
            for (const auto &q : around) {
                if (q[0] < 0 || q[1] < 0 || q[0] >= w || q[1] >= h) continue;
                const size_t n = size_t(q[1]) * w + q[0];
                if (label[n] >= 0 || edges[n]) continue;
                label[n] = id;
                stack.push_back(int(n));
            }
        }
        s.rect = QRect(QPoint(left, top), QPoint(right, bottom));
        return id;
    };

    // the areas under the pointer, counted by how much of it each covers; none there (all edge): the nearest
    int chosen = -1;
    for (int r = std::max(1, reach); chosen < 0 && r <= std::max(1, reach) * 8 + 16; r *= 2) {
        std::vector<qint64> covered;
        for (int y = std::max(0, at.y() - r); y <= std::min(h - 1, at.y() + r); y++) {
            for (int x = std::max(0, at.x() - r); x <= std::min(w - 1, at.x() + r); x++) {
                if ((x - at.x()) * (x - at.x()) + (y - at.y()) * (y - at.y()) > r * r) continue;
                const size_t i = size_t(y) * w + x;
                if (edges[i]) continue;
                if (label[i] < 0) flood(int(i));
                if (covered.size() <= size_t(label[i])) covered.resize(size_t(label[i]) + 1, 0);
                covered[size_t(label[i])]++;
            }
        }
        qint64 most = 0;
        for (size_t id = 0; id < covered.size(); id++) {
            if (covered[id] > most) {
                most = covered[id];
                chosen = int(id);
            }
        }
    }
    if (chosen < 0) return mask;

    std::vector<char> taken(stats.size(), 0);
    taken[size_t(chosen)] = 1;
    if (everywhere) {
        for (size_t i = 0; i < count; i++) {
            if (label[i] < 0 && !edges[i]) flood(int(i));
        }
        // each area's colors summed, in a band of rows per thread
        const int threads = std::max(1, QThread::idealThreadCount());
        QVector<int> parts(threads);
        std::iota(parts.begin(), parts.end(), 0);
        struct Sums {
            std::array<double, 4> sum {0, 0, 0, 0};
            double squares {0};
        };
        std::vector<std::vector<Sums>> partial(size_t(threads), std::vector<Sums>(stats.size()));
        QtConcurrent::blockingMap(parts, [&](int part) {
            std::vector<Sums> &mine = partial[size_t(part)];
            for (int y = h * part / threads; y < h * (part + 1) / threads; y++) {
                for (int x = 0; x < w; x++) {
                    const int id = label[size_t(y) * w + x];
                    if (id < 0) continue;
                    const Color c = oklabOf(d->pixel(x, y));
                    Sums &s = mine[size_t(id)];
                    for (int k = 0; k < 4; k++) {
                        s.sum[size_t(k)] += c[size_t(k)];
                        s.squares += double(c[size_t(k)]) * c[size_t(k)];
                    }
                }
            }
        });
        for (const std::vector<Sums> &mine : partial) {
            for (size_t id = 0; id < stats.size(); id++) {
                for (int k = 0; k < 4; k++) stats[id].sum[size_t(k)] += mine[id].sum[size_t(k)];
                stats[id].squares += mine[id].squares;
            }
        }
        taken.assign(stats.size(), 0);
        auto meanOf = [](const Stats &s) {
            Color m;
            for (int k = 0; k < 4; k++) m[size_t(k)] = float(s.sum[size_t(k)] / double(s.size));
            return m;
        };
        // the root mean square distance of its colors from their mean
        auto spreadOf = [](const Stats &s, const Color &mean) {
            double m2 = 0;
            for (int k = 0; k < 4; k++) m2 += double(mean[size_t(k)]) * mean[size_t(k)];
            return float(std::sqrt(std::max(0.0, s.squares / double(s.size) - m2)));
        };
        const Color mean = meanOf(stats[size_t(chosen)]);
        const float spread = spreadOf(stats[size_t(chosen)], mean);
        for (size_t id = 0; id < stats.size(); id++) {
            const Color m = meanOf(stats[id]);
            taken[id] = distance(m, mean) <= step && std::abs(spreadOf(stats[id], m) - spread) <= step;
        }
    }
    QRect covers;
    for (size_t id = 0; id < stats.size(); id++) {
        if (taken[id]) covers |= stats[id].rect;
    }
    for (int y = covers.top(); y <= covers.bottom(); y++) {
        for (int x = covers.left(); x <= covers.right(); x++) {
            const size_t i = size_t(y) * w + x;
            if (label[i] >= 0 && taken[size_t(label[i])]) mask[i] = MAX_SELECTED;
        }
    }
    if (rect) *rect = covers.translated(d->bounds.topLeft());
    return mask;
}
