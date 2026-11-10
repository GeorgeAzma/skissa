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
#include <limits>
#include <numeric>

#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <kis_paint_device.h>
#include <kis_selection.h>

#include "KisColorMath.h"

namespace
{

using Color = std::array<float, 4>;

/// A Gaussian of 1 px, out to 4 px either side
constexpr int kernelReach = 4;

struct Kernels {
    float smooth[2 * kernelReach + 1];

    Kernels()
    {
        float sum = 0;
        for (int k = -kernelReach; k <= kernelReach; k++) sum += smooth[k + kernelReach] = std::exp(-0.5f * k * k);
        for (int k = -kernelReach; k <= kernelReach; k++) smooth[k + kernelReach] /= sum;
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

/// Levels of the step at a pixel, by ratios from 1e-3 to 1 (Oklab distance), 2.7% apart
constexpr int levels = 256;
constexpr double lowestChange = 1e-3;
constexpr double highestChange = 1.0;
/// How far either side of a pixel its step is read: an edge is a step at every distance, a line only close up, so
/// a pixel's step is the least of these; lines up to about 4 px wide (blurred by the Gaussian) don't part what they
/// lie on
constexpr int distances[] = {3, 5, 7, 9};
constexpr int across = 9;

int levelOf(double change)
{
    if (change <= lowestChange) return 0;
    return std::min(levels - 1, int(levels * std::log(change / lowestChange) / std::log(highestChange / lowestChange)));
}

/// The middle of a level
double stepOfLevel(int level)
{
    return lowestChange * std::pow(highestChange / lowestChange, (level + 0.5) / levels);
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
    /// The step at each pixel, as a level; and how far its own color is from the area around a line it may be on
    std::vector<quint8> level;
    std::vector<quint8> lineLevel;
    /// The tree of areas: each pixel's parent, the node of its own level or, for a node, the area it joins at a
    /// higher level; and each node's size
    std::vector<int> parent;
    std::vector<int> size;

    const quint8 *pixel(int x, int y) const { return pixels.data() + (size_t(y) * w + x) * 4; }
    size_t count() const { return size_t(w) * h; }
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

    // the step at each pixel, on the picture smoothed by a Gaussian of 1 px: how far apart its two sides are, the least
    // over the distances and the most of four directions. Across an edge it is the edge's whole step; across a thin
    // line the sides are alike further out, so a line doesn't part what it lies on; along a gradient it is a small share
    // of its slope
    p->level.resize(count);
    p->lineLevel.resize(count);
    const Kernels kernels;
    const int w = p->w, h = p->h;
    constexpr int band = 32;
    QVector<int> starts;
    for (int y = 0; y < h; y += band) starts.append(y);
    QtConcurrent::blockingMap(starts, [&](int top) {
        const int bottom = std::min(h, top + band);
        // the smoothed rows this band reads, and the picture's rows those need
        const int smoothFirst = std::max(0, top - across), smoothLast = std::min(h - 1, bottom - 1 + across);
        const int first = std::max(0, smoothFirst - kernelReach), last = std::min(h - 1, smoothLast + kernelReach);
        const int rows = last - first + 1;
        std::vector<Color> lab(size_t(rows) * w), along(size_t(rows) * w);
        for (int y = first; y <= last; y++) {
            for (int x = 0; x < w; x++) lab[size_t(y - first) * w + x] = oklabOf(p->pixel(x, y));
        }
        // along rows, then down columns, the edges of the picture repeating
        for (int r = 0; r < rows; r++) {
            const Color *row = lab.data() + size_t(r) * w;
            for (int x = 0; x < w; x++) {
                Color s {0, 0, 0, 0};
                for (int k = -kernelReach; k <= kernelReach; k++) {
                    const Color &c = row[qBound(0, x - k, w - 1)];
                    for (int i = 0; i < 4; i++) s[size_t(i)] += kernels.smooth[k + kernelReach] * c[size_t(i)];
                }
                along[size_t(r) * w + x] = s;
            }
        }
        const int smoothRows = smoothLast - smoothFirst + 1;
        std::vector<Color> smooth(size_t(smoothRows) * w);
        for (int y = smoothFirst; y <= smoothLast; y++) {
            for (int x = 0; x < w; x++) {
                Color s {0, 0, 0, 0};
                for (int k = -kernelReach; k <= kernelReach; k++) {
                    const Color &c = along[size_t(qBound(0, y - k, h - 1) - first) * w + x];
                    for (int i = 0; i < 4; i++) s[size_t(i)] += kernels.smooth[k + kernelReach] * c[size_t(i)];
                }
                smooth[size_t(y - smoothFirst) * w + x] = s;
            }
        }
        auto at = [&](int x, int y) -> const Color & {
            return smooth[size_t(qBound(smoothFirst, y, smoothLast) - smoothFirst) * w + qBound(0, x, w - 1)];
        };
        // across, down and the diagonals. A mark on an area (a line across it) is how far the pixel's own color is from
        // the two sides' where they are most alike, in the direction it is furthest
        const int directions[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
        for (int y = top; y < bottom; y++) {
            for (int x = 0; x < w; x++) {
                const Color &own = lab[size_t(y - first) * w + x];
                float most = 0, mark = 0;
                for (const auto &u : directions) {
                    float least = std::numeric_limits<float>::max();
                    Color sides {0, 0, 0, 0};
                    for (const int r : distances) {
                        const Color &a = at(x + u[0] * r, y + u[1] * r), &b = at(x - u[0] * r, y - u[1] * r);
                        const float step = distance(a, b);
                        if (step < least) {
                            least = step;
                            for (int i = 0; i < 4; i++) sides[size_t(i)] = (a[size_t(i)] + b[size_t(i)]) / 2;
                        }
                    }
                    most = std::max(most, least);
                    mark = std::max(mark, distance(own, sides));
                }
                p->level[size_t(y) * w + x] = quint8(levelOf(most));
                p->lineLevel[size_t(y) * w + x] = quint8(levelOf(mark));
            }
        }
    });

    // the tree of every threshold's areas: pixels taken from the slowest change up, each joining the areas around it
    // (Berger et al.'s union-find), so an area at a level is a node whose pixels change at most that fast
    std::vector<int> order(count);
    {
        std::array<size_t, levels + 1> start {};
        for (size_t i = 0; i < count; i++) start[size_t(p->level[i]) + 1]++;
        for (int l = 0; l < levels; l++) start[size_t(l) + 1] += start[size_t(l)];
        for (size_t i = 0; i < count; i++) order[start[p->level[i]]++] = int(i);
    }
    p->parent.assign(count, -1);
    std::vector<int> root(count);
    auto find = [&](int x) {
        int r = x;
        while (root[size_t(r)] != r) r = root[size_t(r)];
        while (root[size_t(x)] != r) {
            const int next = root[size_t(x)];
            root[size_t(x)] = r;
            x = next;
        }
        return r;
    };
    for (const int at : order) {
        p->parent[size_t(at)] = at;
        root[size_t(at)] = at;
        const int x = at % w, y = at / w;
        const int around[4] = {x > 0 ? at - 1 : -1, x < w - 1 ? at + 1 : -1, y > 0 ? at - w : -1, y < h - 1 ? at + w : -1};
        for (const int n : around) {
            if (n < 0 || p->parent[size_t(n)] < 0) continue;
            const int r = find(n);
            if (r == at) continue;
            p->parent[size_t(r)] = at;
            root[size_t(r)] = at;
        }
    }
    // each pixel's parent the node of its own level, then sizes summed up the tree
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        const int q = p->parent[size_t(*it)];
        if (p->level[size_t(p->parent[size_t(q)])] == p->level[size_t(q)]) p->parent[size_t(*it)] = p->parent[size_t(q)];
    }
    p->size.assign(count, 1);
    for (const int at : order) {
        const int up = p->parent[size_t(at)];
        if (up != at) p->size[size_t(up)] += p->size[size_t(at)];
    }
    d.reset(p);
}

QRect KisColorAreas::bounds() const
{
    return d->bounds;
}

qreal KisColorAreas::autoStep(const QPoint &point, int reach) const
{
    const int w = d->w, h = d->h;
    const QPoint at = point - d->bounds.topLeft();
    constexpr qreal fallback = 0.02;
    if (at.x() < 0 || at.y() < 0 || at.x() >= w || at.y() >= h) return fallback;
    // the most inside pixel under the pointer: the one whose color changes slowest, nearest the point
    int seed = at.y() * w + at.x();
    for (int y = std::max(0, at.y() - reach); y <= std::min(h - 1, at.y() + reach); y++) {
        for (int x = std::max(0, at.x() - reach); x <= std::min(w - 1, at.x() + reach); x++) {
            if ((x - at.x()) * (x - at.x()) + (y - at.y()) * (y - at.y()) > reach * reach) continue;
            if (d->level[size_t(y) * w + x] < d->level[size_t(seed)]) seed = y * w + x;
        }
    }
    // its area's size at every level, up the tree
    std::array<qint64, levels> sizeAt {};
    int node = d->parent[size_t(seed)];
    if (d->level[size_t(node)] != d->level[size_t(seed)]) node = seed;
    for (int l = 0; l < levels; l++) {
        if (l < d->level[size_t(node)]) continue;
        while (d->parent[size_t(node)] != node && d->level[size_t(d->parent[size_t(node)])] <= l) node = d->parent[size_t(node)];
        sizeAt[size_t(l)] = d->size[size_t(node)];
    }
    // stability: how much the size changes over a span of levels either way (a factor of 1.2 in the threshold), against
    // the size; among the levels Spread reaches, at least the pointer's size and less than nearly all the picture
    constexpr int span = 5;
    // a level is stable where that is a local least under 0.25 (MSER's usual span and largest variation); the first
    // such, the finest thing in the picture there: further up, it joins what is beside it
    constexpr double variation = 0.25;
    const qint64 smallest = std::max<qint64>(64, qint64(4) * reach * reach);
    const qint64 most = qint64(d->count()) * 9 / 10;
    // under half the least difference the eye notices (Oklab 0.02) a step can't be a thing's edge: eight-bit color
    // makes slight gradients flat bands a level apart, each of them stable
    const int low = std::max(span, levelOf(0.01));
    const int high = std::min(levels - 1 - span, levelOf(0.25));
    std::array<double, levels> change;
    change.fill(std::numeric_limits<double>::max());
    for (int l = low; l <= high; l++) {
        const qint64 size = sizeAt[size_t(l)];
        if (size < smallest || size > most || sizeAt[size_t(l - span)] == 0) continue;
        change[size_t(l)] = double(sizeAt[size_t(l + span)] - sizeAt[size_t(l - span)]) / double(size);
    }
    for (int l = low; l <= high; l++) {
        if (change[size_t(l)] > variation) continue;
        // down to its least, then the levels about as stable around it, and their middle, well inside the range that
        // finds the area
        int least = l;
        while (least + 1 <= high && change[size_t(least + 1)] <= change[size_t(least)]) least++;
        const double stable = change[size_t(least)] + 0.002;
        int first = least, last = least;
        while (first - 1 >= l && change[size_t(first - 1)] <= stable) first--;
        while (last + 1 <= high && change[size_t(last + 1)] <= stable) last++;
        return stepOfLevel((first + last) / 2);
    }
    return fallback;
}

KisColorAreas::Cover KisColorAreas::cover(const QPoint &point, int reach, qreal step, bool everywhere) const
{
    const int w = d->w, h = d->h;
    const size_t total = d->count();
    Cover result;
    result.mask.assign(total, MIN_SELECTED);
    const QPoint at = point - d->bounds.topLeft();
    if (at.x() < 0 || at.y() < 0 || at.x() >= w || at.y() >= h) return result;
    const int threshold = levelOf(step);
    auto inArea = [&](size_t i) { return d->level[i] <= threshold; };

    // each area its own number, given as it is first reached
    std::vector<int> label(total, -1);
    std::vector<QRect> rects;
    std::vector<qint64> sizes;
    std::vector<int> stack;
    auto flood = [&](int start) {
        const int id = int(rects.size());
        int left = start % w, right = left, top = start / w, bottom = top;
        qint64 size = 0;
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
            size++;
            const int around[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
            for (const int n : around) {
                if (n < 0 || label[size_t(n)] >= 0 || !inArea(size_t(n))) continue;
                label[size_t(n)] = id;
                stack.push_back(n);
            }
        }
        rects.push_back(QRect(QPoint(left, top), QPoint(right, bottom)));
        sizes.push_back(size);
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
                if (!inArea(i)) continue;
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
    if (chosen < 0) return result;

    std::vector<char> taken(rects.size(), 0);
    taken[size_t(chosen)] = 1;
    if (everywhere) {
        for (size_t i = 0; i < total; i++) {
            if (label[i] < 0 && inArea(i)) flood(int(i));
        }
        
        // each area's colors summed, in a band of rows per thread
        struct Sums {
            std::array<double, 4> sum {0, 0, 0, 0};
            double squares {0};
        };
        const int threads = std::max(1, QThread::idealThreadCount());
        QVector<int> parts(threads);
        std::iota(parts.begin(), parts.end(), 0);
        std::vector<std::vector<Sums>> partial(size_t(threads), std::vector<Sums>(rects.size()));
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
        std::vector<Sums> sums(rects.size());
        for (const std::vector<Sums> &mine : partial) {
            for (size_t id = 0; id < rects.size(); id++) {
                for (int k = 0; k < 4; k++) sums[id].sum[size_t(k)] += mine[id].sum[size_t(k)];
                sums[id].squares += mine[id].squares;
            }
        }
        auto meanOf = [&](size_t id) {
            Color m;
            for (int k = 0; k < 4; k++) m[size_t(k)] = float(sums[id].sum[size_t(k)] / double(sizes[id]));
            return m;
        };
        // the root mean square distance of its colors from their mean
        auto spreadOf = [&](size_t id, const Color &mean) {
            double m2 = 0;
            for (int k = 0; k < 4; k++) m2 += double(mean[size_t(k)]) * mean[size_t(k)];
            return float(std::sqrt(std::max(0.0, sums[id].squares / double(sizes[id]) - m2)));
        };
        
        const Color mean = meanOf(size_t(chosen));
        const float spread = spreadOf(size_t(chosen), mean);
        // every area is numbered by now
        taken.assign(rects.size(), 0);
        for (size_t id = 0; id < rects.size(); id++) {
            const Color m = meanOf(id);
            taken[id] = distance(m, mean) <= step && std::abs(spreadOf(id, m) - spread) <= step;
        }
    }
    QRect covers;
    for (size_t id = 0; id < rects.size(); id++) {
        if (taken[id]) covers |= rects[id];
    }
    for (int y = covers.top(); y <= covers.bottom(); y++) {
        for (int x = covers.left(); x <= covers.right(); x++) {
            const size_t i = size_t(y) * w + x;
            if (label[i] >= 0 && taken[size_t(label[i])]) result.mask[i] = MAX_SELECTED;
        }
    }

    // the edges: from the area outwards and from the other areas inwards, each edge pixel learns its nearest pixel of
    // each, a step at a time, up to edgeReach
    const QRect reachable = covers.adjusted(-2 * edgeReach, -2 * edgeReach, 2 * edgeReach, 2 * edgeReach) & QRect(0, 0, w, h);
    auto nearestFrom = [&](auto isSource, const QRect &within) {
        std::vector<int> nearest(total, -1);
        std::vector<int> front, next;
        for (int y = within.top(); y <= within.bottom(); y++) {
            for (int x = within.left(); x <= within.right(); x++) {
                const size_t i = size_t(y) * w + x;
                if (!isSource(i)) continue;
                const int around[4] = {x > 0 ? int(i) - 1 : -1, x < w - 1 ? int(i) + 1 : -1, y > 0 ? int(i) - w : -1, y < h - 1 ? int(i) + w : -1};
                bool border = false;
                for (const int n : around) border = border || (n >= 0 && !inArea(size_t(n)));
                if (!border) continue;
                nearest[i] = int(i);
                front.push_back(int(i));
            }
        }
        for (int layer = 0; layer < edgeReach && !front.empty(); layer++) {
            next.clear();
            for (const int i : front) {
                const int x = i % w, y = i / w;
                const int around[4] = {x > 0 ? i - 1 : -1, x < w - 1 ? i + 1 : -1, y > 0 ? i - w : -1, y < h - 1 ? i + w : -1};
                for (const int n : around) {
                    if (n < 0 || inArea(size_t(n)) || nearest[size_t(n)] >= 0 || !within.contains(n % w, n / w)) continue;
                    nearest[size_t(n)] = nearest[size_t(i)];
                    next.push_back(n);
                }
            }
            front.swap(next);
        }
        return nearest;
    };
    const std::vector<int> inside = nearestFrom([&](size_t i) { return result.mask[i] != 0; }, reachable);
    const std::vector<int> beyond = nearestFrom([&](size_t i) { return inArea(i) && !result.mask[i]; }, reachable);

    // the area's own color across a mark, where both sides are in the area and alike
    auto besideOf = [&](int x, int y) {
        const Color own = oklabOf(d->pixel(x, y));
        int beside = -1;
        float apart = -1;
        for (const auto &u : {std::array<int, 2> {1, 0}, std::array<int, 2> {0, 1}, std::array<int, 2> {1, 1}, std::array<int, 2> {1, -1}}) {
            for (const int r : distances) {
                const int ax = x + u[0] * r, ay = y + u[1] * r, bx = x - u[0] * r, by = y - u[1] * r;
                if (ax < 0 || ay < 0 || bx < 0 || by < 0 || ax >= w || ay >= h || bx >= w || by >= h) continue;
                const size_t a = size_t(ay) * w + ax, b = size_t(by) * w + bx;
                if (!result.mask[a] || !result.mask[b]) continue;
                const Color ca = oklabOf(d->pixel(ax, ay)), cb = oklabOf(d->pixel(bx, by));
                if (distance(ca, cb) >= step) continue;
                const float unlike = distance(own, ca);
                if (unlike > apart) {
                    apart = unlike;
                    beside = int(a);
                }
                break;
            }
        }
        return std::make_pair(beside, apart);
    };
    std::vector<Blend> marks;

    // a soft edge's two colors are its ends, not its nearest whole pixels, which are still part blended: each is read
    // further out the way it lies while that takes it further from the other
    auto further = [&](int from, int end, int other, bool ours) {
        const Color away = oklabOf(d->pixel(other % w, other / w));
        float apart = distance(oklabOf(d->pixel(end % w, end / w)), away);
        const int dx = end % w - from % w, dy = end / w - from / w;
        // no further than an edge is followed, which is as far as a fill reads around a pixel
        for (int k = 2; k <= 4 && (std::abs(dx) + std::abs(dy)) * k <= edgeReach; k++) {
            const int x = from % w + dx * k, y = from / w + dy * k;
            if (x < 0 || y < 0 || x >= w || y >= h) break;
            const size_t n = size_t(y) * w + x;
            if (!inArea(n) || bool(result.mask[n]) != ours) break;
            const float by = distance(oklabOf(d->pixel(x, y)), away);
            if (by <= apart) break;
            apart = by;
            end = int(n);
        }
        return end;
    };
    for (int y = reachable.top(); y <= reachable.bottom(); y++) {
        for (int x = reachable.left(); x <= reachable.right(); x++) {
            const size_t i = size_t(y) * w + x;
            if (inArea(i) || inside[i] < 0) continue;
            if (beyond[i] >= 0) {
                const int outer = further(int(i), beyond[i], inside[i], false);
                result.blends.push_back({int(i), further(int(i), inside[i], outer, true), outer});
                continue;
            }
            // enclosed in the area: within the step of its color it is the area; else a line, blended with its core,
            // the pixel nearby most unlike the area
            const Color own = oklabOf(d->pixel(x, y)), area = oklabOf(d->pixel(inside[i] % w, inside[i] / w));
            if (distance(own, area) < step) {
                result.mask[i] = MAX_SELECTED;
                continue;
            }
            int core = int(i);
            float apart = distance(own, area);
            for (int v = std::max(0, y - 3); v <= std::min(h - 1, y + 3); v++) {
                for (int u = std::max(0, x - 3); u <= std::min(w - 1, x + 3); u++) {
                    const size_t n = size_t(v) * w + u;
                    if (inArea(n)) continue;
                    const float by = distance(oklabOf(d->pixel(u, v)), area);
                    if (by > apart) {
                        apart = by;
                        core = int(n);
                    }
                }
            }
            result.blends.push_back({int(i), inside[i], core});
        }
    }
    // marks on the area (a faint line, a bird): pixels whose own color is a step from the area's around them. They take
    // the new color with their difference from the area kept, read from the area beside them, across the mark
    for (int y = covers.top(); y <= covers.bottom(); y++) {
        for (int x = covers.left(); x <= covers.right(); x++) {
            const size_t i = size_t(y) * w + x;
            if (!result.mask[i] || d->lineLevel[i] <= threshold) continue;
            const auto beside = besideOf(x, y);
            if (beside.first >= 0 && beside.second >= step) marks.push_back({int(i), beside.first, -1});
        }
    }
    for (const Blend &mark : marks) result.mask[size_t(mark.at)] = MIN_SELECTED;
    result.blends.insert(result.blends.end(), marks.begin(), marks.end());
    std::sort(result.blends.begin(), result.blends.end(), [](const Blend &a, const Blend &b) { return a.at < b.at; });

    int left = w, top = h, right = -1, bottom = -1;
    for (int y = reachable.top(); y <= reachable.bottom(); y++) {
        for (int x = reachable.left(); x <= reachable.right(); x++) {
            if (!result.mask[size_t(y) * w + x]) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (right >= 0) result.rect = QRect(QPoint(left, top), QPoint(right, bottom)).translated(d->bounds.topLeft());
    return result;
}
