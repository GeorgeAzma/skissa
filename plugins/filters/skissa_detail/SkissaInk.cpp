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
#include "SkissaFewer.h"

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
/// Each hatch direction after the first's, lightest first: crossing, then between
const float hatchTurns[3] = {0, 90, 45};
/// How different in lightness (Oklab) a place along the flow may be and still be smoothed with where the line starts:
/// smoothed regardless, small shapes such as eyes melted into blobs and thin ones were smeared along long ones
constexpr float edgeRange = 0.1f;

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

QString nameOf(Shading shading)
{
    switch (shading) {
    case Shading::Solid: return QStringLiteral("solid");
    case Shading::Soft: return QStringLiteral("soft");
    case Shading::Hatch: return QStringLiteral("hatch");
    case Shading::Strokes: return QStringLiteral("strokes");
    case Shading::Dots: return QStringLiteral("dots");
    case Shading::Screen: return QStringLiteral("screen");
    case Shading::Engrave: return QStringLiteral("engrave");
    case Shading::Contour: return QStringLiteral("contour");
    }
    return QStringLiteral("solid");
}

Shading shadingNamed(const QString &name)
{
    for (Shading shading : {Shading::Solid, Shading::Soft, Shading::Hatch, Shading::Strokes, Shading::Dots, Shading::Screen, Shading::Engrave,
                            Shading::Contour}) {
        if (nameOf(shading) == name) return shading;
    }
    return Shading::Solid;
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
 * by \p sigma, and its eigenvector of the smaller eigenvalue. Where the picture changes less than \p calm across a
 * pixel, it goes toward \p angle (in degrees) instead.
 */
Flow flowOf(const std::vector<float> lab[3], int w, int h, qreal sigma, float calm = 0, float angle = 0)
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
    if (calm > 0) {
        // a change of calm across the direction asked for
        const double a = qDegreesToRadians(double(angle));
        const float across[2] = {float(std::sin(a)), float(-std::cos(a))};
        const float least = calm * calm;
        for (size_t i = 0; i < count; i++) {
            e[i] += least * across[0] * across[0];
            f[i] += least * across[0] * across[1];
            g[i] += least * across[1] * across[1];
        }
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
 * the line through it that follows the flow both ways (a line integral convolution). With \p guide, a sample counts
 * less the more the guide there differs from where the line starts, and nothing past \p range: where a thin shape
 * crosses a long one, the flow runs along the long one, and the line would smear the thin one's edges along it.
 */
std::vector<std::vector<float>> alongFlow(const std::vector<const float *> &planes, const Flow &flow, qreal sigma, const QRect &area,
                                          const float *guide = nullptr, float range = 1)
{
    const int w = flow.w, h = flow.h;
    // a pixel a step, longer ones for long flows: a smooth flow loses nothing to them
    const float stride = float(std::max(1.0, sigma / 3));
    const int steps = std::max(1, int(std::ceil(2 * sigma / stride)));
    std::vector<float> weights(steps + 1);
    for (int d = 0; d <= steps; d++) weights[d] = float(std::exp(-(d * stride) * (d * stride) / (2 * sigma * sigma)));
    const float apart = 1 / (range * range);
    const int count = int(planes.size());
    std::vector<std::vector<float>> out(count, std::vector<float>(size_t(area.width()) * area.height()));
    std::vector<float> sum(count);
    for (int y = area.top(); y <= area.bottom(); y++) {
        for (int x = area.left(); x <= area.right(); x++) {
            const size_t here = size_t(y) * w + x;
            for (int c = 0; c < count; c++) sum[c] = planes[c][here];
            float total = 1;
            const float start = guide ? guide[here] : 0.0f;
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
                    float weight = weights[d];
                    if (guide) {
                        // a biweight of the difference: smooth, and none at all past the range; the guide is smooth
                        // enough to read at the pixel stepped into
                        const float differs = guide[at] - start;
                        const float kept = std::max(0.0f, 1 - differs * differs * apart);
                        weight *= kept * kept;
                    }
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

/**
 * One plane smoothed along a smooth flow, for strokes: as alongFlow, a pixel a step, but reading the pixel stepped
 * into and turning with the flow every third step. Over \p area of the plane.
 */
std::vector<float> strokeAlong(const std::vector<float> &plane, const Flow &flow, qreal sigma, const QRect &area)
{
    const int w = flow.w, h = flow.h;
    const int steps = std::max(1, int(std::ceil(2 * sigma)));
    std::vector<float> weights(steps + 1);
    for (int d = 0; d <= steps; d++) weights[d] = float(std::exp(-d * d / (2 * sigma * sigma)));
    std::vector<float> out(size_t(area.width()) * area.height());
    for (int y = area.top(); y <= area.bottom(); y++) {
        for (int x = area.left(); x <= area.right(); x++) {
            const size_t here = size_t(y) * w + x;
            float sum = plane[here], total = 1;
            for (const float way : {1.0f, -1.0f}) {
                float px = x + 0.5f, py = y + 0.5f;
                float vx = way * flow.x[here], vy = way * flow.y[here];
                for (int d = 1; d <= steps; d++) {
                    px += vx;
                    py += vy;
                    const int ix = int(std::floor(px)), iy = int(std::floor(py));
                    if (ix < 0 || iy < 0 || ix >= w || iy >= h) break;
                    const size_t at = size_t(iy) * w + ix;
                    if (d % 3 == 0) {
                        // the flow has no sign: keep going the way it went
                        float nx = flow.x[at], ny = flow.y[at];
                        if (nx * vx + ny * vy < 0) {
                            nx = -nx;
                            ny = -ny;
                        }
                        vx = nx;
                        vy = ny;
                    }
                    sum += weights[d] * readAt(plane.data(), w, h, px, py);
                    total += weights[d];
                }
            }
            out[size_t(y - area.top()) * area.width() + (x - area.left())] = sum / total;
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
 * Paper's tooth over \p rect (at a level of detail whose pixel is \p pixel image pixels): its height from 0 to 1,
 * even in how often each height comes, grain of a pixel or two and a coarser one, fixed to the image's pixels
 */
std::vector<float> toothOf(const QRect &rect, float pixel)
{
    const int margin = 8;
    const QRect around = rect.adjusted(-margin, -margin, margin, margin);
    const int w = around.width(), h = around.height();
    std::vector<float> fine(size_t(w) * h), coarse(size_t(w) * h), scratch(size_t(w) * h);
    for (int y = 0; y < h; y++) {
        const quint32 imageY = quint32(qint32(std::floor((around.top() + y + 0.5f) * pixel)));
        for (int x = 0; x < w; x++) {
            const quint32 imageX = quint32(qint32(std::floor((around.left() + x + 0.5f) * pixel)));
            fine[size_t(y) * w + x] = hashed(imageX, imageY, 301) - 0.5f;
            coarse[size_t(y) * w + x] = hashed(imageX, imageY, 302) - 0.5f;
        }
    }
    const qreal small = std::max(0.5, 0.7 / pixel), large = std::max(0.8, 1.6 / pixel);
    SkissaDetail::gaussian(fine.data(), scratch.data(), w, h, small);
    SkissaDetail::gaussian(coarse.data(), scratch.data(), w, h, large);
    // white noise of spread 1/12 blurred by a Gaussian keeps about 1 / (4 pi sigma^2) of it
    auto kept = [](qreal sigma) { return std::sqrt(1.0 / 12 / std::max(1.0, 4 * M_PI * sigma * sigma)); };
    const float fineSpread = float(kept(small)), coarseSpread = float(kept(large));
    std::vector<float> out(size_t(rect.width()) * rect.height());
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const size_t from = size_t(y + margin) * w + x + margin;
            const float z = (fine[from] / fineSpread + 0.35f * coarse[from] / coarseSpread) / std::sqrt(1.1225f);
            out[size_t(y) * rect.width() + x] = 0.5f * (1 + std::erf(z / float(M_SQRT2)));
        }
    }
    return out;
}

/// How much of a pixel \p pixel wide a band \p half wide either side of a line covers, \p distance from its middle
inline float bandOver(float distance, float half, float pixel)
{
    const float covered = std::min(distance + half, 0.5f * pixel) - std::max(distance - half, -0.5f * pixel);
    return std::clamp(covered / pixel, 0.0f, 1.0f);
}

/**
 * How much of a pixel \p pixel wide lines at every whole \p u cover, each \p half wide either side (in units of u),
 * where u changes by \p stretch a pixel of the image: the nearest line and the next, which meet where they are wide
 */
inline float bandsOver(float u, float half, float stretch, float pixel)
{
    const float off = u - std::floor(u);
    return std::min(1.0f, bandOver(off / stretch, half / stretch, pixel) + bandOver((1 - off) / stretch, half / stretch, pixel));
}

/// The inverse of the normal distribution (Abramowitz and Stegun 26.2.23, within 0.00045)
inline float normalQuantile(float p)
{
    p = std::clamp(p, 1e-4f, 1 - 1e-4f);
    const float q = std::min(p, 1 - p);
    const float t = std::sqrt(-2 * std::log(q));
    const float x = t - (2.515517f + t * (0.802853f + t * 0.010328f)) / (1 + t * (1.432788f + t * (0.189269f + t * 0.001308f)));
    return p < 0.5f ? -x : x;
}

/**
 * Hatching as drawn with a pen, at \p along and \p across its direction (in the image's pixels). Where it is darkest,
 * rows are \p spacing apart; lighter, every second, then every fourth (each row's rank), so light hatching is sparse,
 * not broken. A row comes in thin where \p cover passes its rank and its full \p width a quarter later, so it tapers
 * where the shading ends. Each row is long strokes with small gaps, each a little off its place and tilted; pressed
 * \p press times harder (\p width is the pressed one), the gaps close in as the strokes widen.
 */
float hatchAt(float along, float across, float cover, int direction, float spacing, float width, float press, float pixel)
{
    static const float rankOfRow[4] = {0, 0.5f, 0.25f, 0.75f};
    const int nearest = int(std::floor(across / spacing + 0.5f));
    const quint32 d = quint32(direction) + 1;
    float ink = 0;
    for (int row = nearest - 1; row <= nearest + 1; row++) {
        const float strength = std::clamp((cover - rankOfRow[row & 3]) * 4, 0.0f, 1.0f);
        if (strength <= 0) continue;
        const quint32 r = quint32(row);
        const float length = spacing * (8 + 8 * hashed(r, d, 4));
        const float position = along / length + hashed(r, d, 5);
        const float index = std::floor(position);
        const quint32 stroke = quint32(qint32(index));
        const float within = (position - index) * length;
        // each stroke a little off the row and tilted, wavering slowly
        const float offset = (hashed(r, stroke, d + 20) - 0.5f) * 0.2f * spacing;
        const float tilt = (hashed(r, stroke, d + 21) - 0.5f) * 0.04f;
        const float wave = spacing * 0.05f * std::sin(along / (spacing * (3 + 3 * hashed(r, d, 1))) + float(tau) * hashed(r, d, 2));
        const float middle = row * spacing + offset + tilt * (within - 0.5f * length) + wave;
        // a small gap between strokes, and each thinning toward its ends; pressed harder, both shorter
        const float fromEnd = std::min(within, length - within) - 0.25f * spacing / press;
        const float taper = std::clamp(fromEnd * press / spacing, 0.0f, 1.0f);
        if (taper <= 0) continue;
        const float half = 0.5f * width * std::sqrt(strength) * (0.3f + 0.7f * std::sqrt(taper));
        ink = std::max(ink, bandOver(std::abs(across - middle), half, pixel));
    }
    return ink;
}

/**
 * Hatching that covers \p dark of the paper with strokes \p width (in spacings) wide: how far each direction's rows
 * have come in (hatchAt's cover) and how much harder all are pressed. The first direction fills in, then the crossing
 * ones over it, then all widen until they close up.
 */
void hatchCovers(float dark, float width, float cover[3], float *press)
{
    // the rows' sum by how far they have come in (each row's width grows as the root of its strength), inverted
    static const std::vector<float> rowsFor = []() {
        constexpr int n = 256;
        auto rows = [](float k) {
            float sum = 0;
            for (int j = 0; j < 4; j++) sum += std::sqrt(std::clamp(4 * k - j, 0.0f, 1.0f));
            return sum / 4;
        };
        std::vector<float> out(n + 1);
        for (int i = 0; i <= n; i++) {
            const float share = float(i) / n;
            float low = 0, high = 1;
            for (int step = 0; step < 24; step++) {
                const float middle = 0.5f * (low + high);
                (rows(middle) < share ? low : high) = middle;
            }
            out[i] = high;
        }
        return out;
    }();
    // one direction with every row in: the strokes' gaps and tapering ends leave about a twelfth of each row
    const float full = std::min(0.916f * width, 0.95f);
    *press = 1;
    cover[0] = cover[1] = cover[2] = 0;
    float laid = 0;
    for (int t = 0; t < 3; t++) {
        // this direction's share over what the ones before laid
        const float wanted = 1 - (1 - dark) / (1 - laid);
        if (wanted <= 0) break;
        if (wanted < full) {
            const float at = wanted / full * 256;
            const int i = std::min(int(at), 255);
            cover[t] = rowsFor[i] + (rowsFor[i + 1] - rowsFor[i]) * (at - i);
            break;
        }
        cover[t] = 1;
        laid = 1 - (1 - laid) * (1 - full);
    }
    if (dark > laid && cover[2] >= 1) {
        // every direction pressed alike, each covering its share, at most as wide as the space between rows
        const float each = 1 - std::cbrt(std::max(0.0f, 1 - dark));
        *press = std::clamp(each / full, 1.0f, 1 / width);
    }
}

/// A stipple dot: where it lies (in the image's pixels) and how far it reaches; none where it reaches nothing
struct Dot {
    float x {0}, y {0}, reach {0};
};

/**
 * Stippling: dots on a grid of cells \p radius × 2.4 wide, each a little off its cell's middle, there where the
 * darkness under it passes its cell's rank in blue noise, so that dots are spread evenly at any density, without
 * clumps. A dot comes in small and grows to its size; once every cell has one, they grow until they close up.
 */
Dot dotOf(int column, int row, float radius, float dark)
{
    const std::vector<float> &ranks = SkissaFewer::blueNoise();
    const float cell = radius * 2.4f;
    // how much is covered once every cell has its dot
    const float full = float(M_PI) * radius * radius / (cell * cell);
    const quint32 a = quint32(column), b = quint32(row);
    Dot dot;
    dot.x = (column + 0.5f + 0.5f * (hashed(a, b, 41) - 0.5f)) * cell;
    dot.y = (row + 0.5f + 0.5f * (hashed(a, b, 42) - 0.5f)) * cell;
    if (dark <= 0) return dot;
    const float own = radius * (0.85f + 0.3f * hashed(a, b, 44));
    if (dark < full) {
        const float past = dark / full - ranks[size_t(row & 63) * 64 + size_t(column & 63)];
        if (past > 0) dot.reach = own * std::min(1.0f, std::sqrt(past * 8));
    } else {
        dot.reach = own * std::sqrt(dark / full) * (1 + 0.4f * (dark - full) / (1 - full));
    }
    return dot;
}

/**
 * The darkness to ask of dotOf for its dots to cover \p dark: past where every cell has its dot they overlap as they
 * grow, and cover less than their area. Measured once by laying dots out over the blue noise's 64 × 64 cells.
 */
float dotsFor(float dark)
{
    static const std::vector<float> covered = []() {
        constexpr int levels = 32, cells = 64, samples = 4;
        const float radius = 1, cell = radius * 2.4f;
        std::vector<float> out(levels + 1);
        std::vector<Dot> dots(size_t(cells + 2) * (cells + 2));
        for (int level = 0; level <= levels; level++) {
            const float asked = float(level) / levels;
            for (int j = -1; j <= cells; j++) {
                for (int i = -1; i <= cells; i++) dots[size_t(j + 1) * (cells + 2) + (i + 1)] = dotOf(i, j, radius, asked);
            }
            int inked = 0;
            for (int j = 0; j < cells; j++) {
                for (int i = 0; i < cells; i++) {
                    for (int sy = 0; sy < samples; sy++) {
                        for (int sx = 0; sx < samples; sx++) {
                            const float x = (i + (sx + 0.5f) / samples) * cell, y = (j + (sy + 0.5f) / samples) * cell;
                            bool in = false;
                            for (int b = j - 1; b <= j + 1 && !in; b++) {
                                for (int a = i - 1; a <= i + 1 && !in; a++) {
                                    const Dot &dot = dots[size_t(b + 1) * (cells + 2) + (a + 1)];
                                    const float dx = x - dot.x, dy = y - dot.y;
                                    in = dx * dx + dy * dy < dot.reach * dot.reach;
                                }
                            }
                            inked += in;
                        }
                    }
                }
            }
            out[level] = float(inked) / (cells * cells * samples * samples);
        }
        return out;
    }();
    if (dark <= 0) return 0;
    const int levels = int(covered.size()) - 1;
    for (int level = 1; level <= levels; level++) {
        if (covered[level] >= dark) {
            const float low = covered[level - 1], high = covered[level];
            return (level - 1 + (high > low ? (dark - low) / (high - low) : 1)) / levels;
        }
    }
    return 1;
}

/// Where the darkness under a dot is read: its own place, which lies within a cell and a half of its cell
inline QPointF dotPlace(int column, int row, float radius)
{
    const Dot dot = dotOf(column, row, radius, 0);
    return QPointF(dot.x, dot.y);
}

/// The Euclidean dot, from -1 to 1: round dots in light tones, a checkerboard at half, round holes in dark ones
inline float spotAt(float u, float v)
{
    return 0.5f * (std::cos(float(tau) * u) + std::cos(float(tau) * v));
}

/// The spot's value that \p dark of a cell's area lies above
float spotLevel(float dark)
{
    static const std::vector<float> levels = []() {
        constexpr int n = 512;
        std::vector<float> spots;
        spots.reserve(n * n);
        for (int y = 0; y < n; y++) {
            for (int x = 0; x < n; x++) spots.push_back(spotAt((x + 0.5f) / n, (y + 0.5f) / n));
        }
        std::sort(spots.begin(), spots.end());
        std::vector<float> out(1025);
        for (int i = 0; i <= 1024; i++) out[i] = spots[std::min(size_t(n) * n - 1, size_t(double(i) / 1024 * n * n))];
        return out;
    }();
    const float at = std::clamp(1 - dark, 0.0f, 1.0f) * 1024;
    const int i = std::min(int(at), 1023);
    return levels[i] + (levels[i + 1] - levels[i]) * (at - i);
}

/**
 * A printer's screen at a pixel (in the image's pixels): a grid \p spacing apart turned to (\p cosine, \p sine),
 * inked where its spot passes the level that covers \p dark of a cell
 */
float screenAt(float imageX, float imageY, float dark, float spacing, float cosine, float sine, float pixel)
{
    if (dark <= 0) return 0;
    if (dark >= 1) return 1;
    const float u = (imageX * cosine + imageY * sine) / spacing, v = (imageY * cosine - imageX * sine) / spacing;
    const float su = std::sin(float(tau) * u), sv = std::sin(float(tau) * v);
    const float slope = float(M_PI) * pixel / spacing * std::sqrt(su * su + sv * sv);
    return std::clamp((spotAt(u, v) - spotLevel(dark)) / std::max(slope, 1e-4f) + 0.5f, 0.0f, 1.0f);
}

/**
 * Stripes along \p flow on a grid of \p noise: filtered across the flow into stripes \p spacing apart (a broad
 * Gabor kernel, so the strokes vary as drawn ones do rather than lining up as a weave) and smoothed along it into
 * strokes (a line integral, \p sigma), then brought to unit spread around each place. Over \p area of the grid.
 */
std::vector<float> stripesOnGrid(const Flow &flow, const std::vector<float> &noise, qreal spacing, qreal sigma, const QRect &area)
{
    const int w = flow.w, h = flow.h;
    const double band = 0.25 * spacing;
    const int reach = int(std::ceil(3 * band));
    std::vector<float> kernel(2 * reach + 1);
    double mean = 0;
    for (int t = -reach; t <= reach; t++) {
        kernel[t + reach] = float(std::cos(tau * t / spacing) * std::exp(-t * t / (2 * band * band)));
        mean += kernel[t + reach];
    }
    for (float &k : kernel) k -= float(mean / kernel.size());
    const int even = int(std::ceil(3 * spacing)), along = int(std::ceil(2 * sigma)) + 1;
    const QRect bounds(0, 0, w, h);
    const QRect evened = area.adjusted(-even, -even, even, even) & bounds;
    const QRect wide = evened.adjusted(-along, -along, along, along) & bounds;
    std::vector<float> across(size_t(w) * h, 0.0f);
    for (int y = wide.top(); y <= wide.bottom(); y++) {
        for (int x = wide.left(); x <= wide.right(); x++) {
            const size_t i = size_t(y) * w + x;
            const float ax = -flow.y[i], ay = flow.x[i];
            float sum = 0;
            for (int t = -reach; t <= reach; t++) sum += kernel[t + reach] * readAt(noise.data(), w, h, x + 0.5f + t * ax, y + 0.5f + t * ay);
            across[i] = sum;
        }
    }
    std::vector<float> field = strokeAlong(across, flow, sigma, evened);
    const int ew = evened.width(), eh = evened.height();
    std::vector<float> power(field.size()), scratch(field.size());
    for (size_t i = 0; i < field.size(); i++) power[i] = field[i] * field[i];
    SkissaDetail::gaussian(power.data(), scratch.data(), ew, eh, spacing);
    std::vector<float> out(size_t(area.width()) * area.height());
    for (int y = 0; y < area.height(); y++) {
        for (int x = 0; x < area.width(); x++) {
            const size_t from = size_t(area.top() + y - evened.top()) * ew + (area.left() + x - evened.left());
            out[size_t(y) * area.width() + x] = field[from] / std::sqrt(power[from] + 1e-12f);
        }
    }
    return out;
}

/**
 * Strokes along the flow, as drawn: stripes of white noise fixed to the image's pixels (stripesOnGrid), so that a
 * threshold covers as much as the normal distribution says. Over \p area (in \p flow's pixels, which cover
 * \p sourceRect). Stripes are smooth across many pixels: wide ones are made on a grid of cells a fifth of a
 * stripe apart, fixed to the image's corner, and enlarged (fewer cells to a stripe, the strokes turn blotchy).
 */
std::vector<float> stripesOf(const Flow &flow, const QRect &sourceRect, float pixel, qreal spacing, qreal sigma, const QRect &area,
                             quint32 seed)
{
    const int w = flow.w, h = flow.h;
    const int cell = std::max(1, int(spacing / 5));
    auto down = [cell](int v) { return v >= 0 ? v / cell : -((-v + cell - 1) / cell); };
    const QRect cells(QPoint(down(sourceRect.left()), down(sourceRect.top())), QPoint(down(sourceRect.right()), down(sourceRect.bottom())));
    const int cw = cells.width(), ch = cells.height();
    // each cell's noise, by the image's pixel at its middle, and the flow there
    Flow grid;
    grid.w = cw;
    grid.h = ch;
    grid.x.resize(size_t(cw) * ch);
    grid.y.resize(size_t(cw) * ch);
    std::vector<float> noise(size_t(cw) * ch);
    for (int y = 0; y < ch; y++) {
        const int middleY = (cells.top() + y) * cell + cell / 2;
        const quint32 imageY = quint32(qint32(std::floor((middleY + 0.5f) * pixel)));
        const int sy = std::clamp(middleY - sourceRect.top(), 0, h - 1);
        for (int x = 0; x < cw; x++) {
            const int middleX = (cells.left() + x) * cell + cell / 2;
            const quint32 imageX = quint32(qint32(std::floor((middleX + 0.5f) * pixel)));
            const int sx = std::clamp(middleX - sourceRect.left(), 0, w - 1);
            const size_t i = size_t(y) * cw + x;
            noise[i] = hashed(imageX, imageY, seed) - 0.5f;
            grid.x[i] = flow.x[size_t(sy) * w + sx];
            grid.y[i] = flow.y[size_t(sy) * w + sx];
        }
    }
    // the cells over the area, with one around for reading between them
    const QRect onImage = area.translated(sourceRect.topLeft());
    const QRect areaCells = QRect(QPoint(down(onImage.left()) - 1, down(onImage.top()) - 1), QPoint(down(onImage.right()) + 1, down(onImage.bottom()) + 1))
                                .translated(-cells.topLeft())
        & QRect(0, 0, cw, ch);
    const std::vector<float> coarse = stripesOnGrid(grid, noise, spacing / cell, sigma / cell, areaCells);
    if (cell == 1 && areaCells == area) return coarse;
    std::vector<float> out(size_t(area.width()) * area.height());
    for (int y = 0; y < area.height(); y++) {
        const float gy = (onImage.top() + y + 0.5f) / cell - 0.5f - cells.top() - areaCells.top();
        for (int x = 0; x < area.width(); x++) {
            const float gx = (onImage.left() + x + 0.5f) / cell - 0.5f - cells.left() - areaCells.left();
            out[size_t(y) * area.width() + x] = readAt(coarse.data(), areaCells.width(), areaCells.height(), gx + 0.5f, gy + 0.5f);
        }
    }
    return out;
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
    settings.flow = float(std::clamp(config.getDouble("flow", 0.5), 0.0, 1.0));
    settings.shading = shadingNamed(config.getString("shading", QStringLiteral("solid")));
    settings.steps = std::clamp(config.getInt("steps", 1), 1, 8);
    settings.softness = float(std::clamp(config.getDouble("softness", 0.5), 0.0, 1.0));
    settings.grain = float(std::clamp(config.getDouble("grain", 0), 0.0, 1.0));
    settings.spacing = float(std::clamp(config.getDouble("spacing", 8), 2.0, 200.0));
    settings.angle = float(std::clamp(config.getDouble("angle", 45), -180.0, 180.0));
    settings.length = float(std::clamp(config.getDouble("length", 16), 2.0, 200.0));
    settings.dot = float(std::clamp(config.getDouble("dot", 3), 1.0, 100.0));
    settings.levels = std::clamp(config.getInt("levels", 12), 2, 64);
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
    config.setProperty("flow", double(flow));
    config.setProperty("shading", nameOf(shading));
    config.setProperty("steps", steps);
    config.setProperty("softness", double(softness));
    config.setProperty("grain", double(grain));
    config.setProperty("spacing", double(spacing));
    config.setProperty("angle", double(angle));
    config.setProperty("length", double(length));
    config.setProperty("dot", double(dot));
    config.setProperty("levels", levels);
    config.setProperty("colors", nameOf(colors));
    config.setProperty("ink_color", inkColor.name(QColor::HexArgb));
    config.setProperty("paper_color", paperColor.name(QColor::HexArgb));
}

bool Settings::sameShading(const Settings &other) const
{
    if (shading != other.shading) return false;
    switch (shading) {
    case Shading::Solid: return steps == other.steps;
    case Shading::Soft: return softness == other.softness && grain == other.grain;
    case Shading::Hatch:
    case Shading::Screen:
    case Shading::Engrave: return spacing == other.spacing && angle == other.angle;
    case Shading::Contour: return levels == other.levels;
    case Shading::Strokes: return spacing == other.spacing && length == other.length;
    case Shading::Dots: return dot == other.dot;
    }
    return true;
}

bool Settings::sameLook(const Settings &other) const
{
    return size == other.size && lines == other.lines && ink == other.ink && flow == other.flow && colors == other.colors
        && (ink == 0 || sameShading(other));
}

bool Settings::operator==(const Settings &other) const
{
    return sameLook(other) && shading == other.shading && steps == other.steps && softness == other.softness && grain == other.grain
        && spacing == other.spacing && angle == other.angle && length == other.length && dot == other.dot && levels == other.levels
        && level == other.level && inkColor == other.inkColor && paperColor == other.paperColor;
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
    if (settings.ink > 0 && settings.shading == Shading::Strokes) {
        m_strokeSigma = 0.5 * settings.length * scale;
        m_stripeSpacing = std::max(2.5, settings.spacing * scale);
        // strokes follow the larger shapes, not every bump
        m_strokeTensorSigma = (2 + 0.5 * settings.length) * scale;
    }
    m_needsFlow = m_flowSigma > 0 || m_strokeSigma > 0;
}

int Ink::reach() const
{
    if (m_settings.changesNothing()) return 0;
    const int tensor = std::max(m_needsFlow ? SkissaDetail::reachOf(m_tensorSigma) + 1 : 0,
                                m_strokeSigma > 0 ? SkissaDetail::reachOf(m_strokeTensorSigma) + 1 : 0);
    const int blurs = std::max(SkissaDetail::reachOf(wider * m_sigma), tensor);
    // strokes read noise across them, along them, and around them for their spread
    const qreal strokes = m_strokeSigma > 0 ? 2 * m_strokeSigma + 4.5 * m_stripeSpacing + 3 : 0;
    const int engraved = m_settings.ink > 0 && m_settings.shading == Shading::Engrave ? SkissaDetail::reachOf(broadSigma()) + 1 : 0;
    return int(std::ceil(2 * m_flowSigma + 2 * m_smoothSigma + strokes + dotReach())) + std::max(blurs, engraved) + 2;
}

qreal Ink::broadSigma() const
{
    return std::max(1.0, 0.75 * m_settings.spacing * m_scale);
}

qreal Ink::dotReach() const
{
    // a dot's middle can lie a cell and a half away, the darkness read there
    return m_settings.ink > 0 && m_settings.shading == Shading::Dots ? 4 * m_settings.dot * m_scale : 0;
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
    if (m_needsFlow) {
        lab[1].resize(count);
        lab[2].resize(count);
    }
    for (size_t i = 0; i < count; i++) {
        const float *pixel = source + i * 4;
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        float c[3];
        oklabOf(pixel[0] * alpha + 1 - alpha, pixel[1] * alpha + 1 - alpha, pixel[2] * alpha + 1 - alpha, c);
        lab[0][i] = c[0];
        if (m_needsFlow) {
            lab[1][i] = c[1];
            lab[2][i] = c[2];
        }
    }

    // the difference of Gaussians, as the picture sharpened by it: edges of this size pushed apart
    std::vector<float> near = lab[0], far = lab[0], scratch(count);
    if (m_sigma > 0.25) SkissaDetail::gaussian(near.data(), scratch.data(), w, h, m_sigma);
    SkissaDetail::gaussian(far.data(), scratch.data(), w, h, wider * std::max(m_sigma, 0.25));
    const float sharpness = mostSharpness * s.lines * s.lines;
    // shading drawn as marks takes its darkness from the picture's lightness alone, so lines stay whole rather than
    // turning to marks
    const bool marked = s.ink > 0 && s.shading != Shading::Solid && s.shading != Shading::Soft;
    // the picture's lightness at the lines' size: the marks' tone, and what the flow's lines keep to
    std::vector<float> smooth;
    if (marked || flows) smooth = near;
    std::vector<float> &difference = near;
    for (size_t i = 0; i < count; i++) {
        const float edge = near[i] - far[i];
        difference[i] = near[i] + sharpness * std::copysign(std::max(0.0f, std::abs(edge) - leastEdge), edge);
    }
    far = std::vector<float>();

    // along the flow, over what the smoothing after the threshold reads
    Flow flow;
    const int smoothReach = int(std::ceil(std::max(2 * m_smoothSigma, dotReach()))) + 1;
    const QRect work = inside.adjusted(-smoothReach, -smoothReach, smoothReach, smoothReach) & QRect(0, 0, w, h);
    std::vector<float> drawn, toned;
    if (m_needsFlow) flow = flowOf(lab, w, h, m_tensorSigma);
    if (flows) {
        std::vector<const float *> planes = {difference.data()};
        if (marked) planes.push_back(smooth.data());
        std::vector<std::vector<float>> along = alongFlow(planes, flow, m_flowSigma, work, smooth.data(), edgeRange);
        drawn = std::move(along[0]);
        if (marked) toned = std::move(along[1]);
    } else {
        auto cut = [&](const std::vector<float> &plane) {
            std::vector<float> part(size_t(work.width()) * work.height());
            for (int y = 0; y < work.height(); y++) {
                std::copy_n(plane.data() + size_t(work.top() + y) * w + work.left(), work.width(), part.data() + size_t(y) * work.width());
            }
            return part;
        };
        drawn = cut(difference);
        if (marked) toned = cut(smooth);
    }

    /*
     * Ink below the threshold: 1 + tanh((D - ε) / width) of paper, as XDoG's soft threshold, averaged over each
     * pixel. Solid shading is thresholds spread evenly below it (its steps); soft, one as wide as asked. Marks (hatching,
     * strokes, dots, a screen) take their darkness from the picture's lightness alone, and the lines are what the edges
     * add to it, so they stay whole over the marks.
     */
    const int tones = s.shading == Shading::Solid ? std::max(1, s.steps) : 1;
    const float top = s.ink <= 0 ? 0.0f : s.level >= 0 ? s.level : s.ink;
    const float width = 0.002f + (s.shading == Shading::Soft ? 0.45f * s.softness * s.softness : 0.0f);
    // the darkness marks draw: as dark as the picture is below the shading's level, evenly
    auto darkness = [top](float lightness) { return std::clamp((top - lightness) / std::max(top, 0.05f), 0.0f, 1.0f); };
    // a screen prints the picture's tones: its ink covers what the picture's light, under the level's, leaves dark
    auto printed = [](float dark) {
        const float light = 1 - dark;
        return 1 - light * light * light;
    };
    const float pixel = float(1 / m_scale);
    const int ww = work.width(), wh = work.height();
    // how much a plane changes across a pixel there, for the threshold's average over it
    auto slopeAt = [](const std::vector<float> &plane, int pw, int ph, int x, int y) {
        const float *row = plane.data() + size_t(y) * pw;
        const float across = 0.5f * (row[std::min(x + 1, pw - 1)] - row[std::max(x - 1, 0)]);
        const float down = 0.5f * (plane[size_t(std::min(y + 1, ph - 1)) * pw + x] - plane[size_t(std::max(y - 1, 0)) * pw + x]);
        return std::sqrt(across * across + down * down);
    };
    // under marks, the lines are the edges alone, drawn as they would be on the paper's white, so they stay whole over
    // the darkest marks: added to the picture's own tone, they would show only where it is lighter than the level
    std::vector<float> edges;
    if (marked) {
        edges.resize(drawn.size());
        for (size_t i = 0; i < drawn.size(); i++) edges[i] = 1 + drawn[i] - toned[i];
    }
    std::vector<float> coverage(drawn.size());
    for (int y = 0; y < wh; y++) {
        for (int x = 0; x < ww; x++) {
            const size_t i = size_t(y) * ww + x;
            if (marked) {
                coverage[i] = inkOver(edges[i], slopeAt(edges, ww, wh, x, y), top, width);
                continue;
            }
            const float d = drawn[i], slope = slopeAt(drawn, ww, wh, x, y);
            float flat = 0;
            for (int t = 0; t < tones; t++) flat += inkOver(d, slope, top * float(tones - t) / tones, width / tones);
            coverage[i] = flat / tones;
        }
    }

    // the marks, over what is made, fixed to the image's own pixels whatever the level of detail
    const QRect inWork = inside.translated(-work.topLeft());
    const int rw = rect.width(), rh = rect.height();
    std::vector<float> marks;
    std::vector<float> stripes[2];
    // engraved lines bend with the picture's lightness blurred at about their spacing, by this many spacings across
    // the range from black to white
    constexpr float engraveBend = 3;
    std::vector<float> broad;
    if (marked && s.shading == Shading::Engrave) {
        broad = lab[0];
        SkissaDetail::gaussian(broad.data(), scratch.data(), w, h, broadSigma());
    }
    if (marked && s.shading == Shading::Strokes) {
        // strokes follow the larger shapes, and where the picture has none, the way a right hand hatches
        Flow along = flowOf(lab, w, h, m_strokeTensorSigma, 0.02f, 45);
        stripes[0] = stripesOf(along, sourceRect, pixel, m_stripeSpacing, m_strokeSigma, inside, 101);
        // the darker parts crossed by strokes turned a third of the way around: square to them, they read as a weave
        const float turnCos = 0.5f, turnSin = float(std::sqrt(0.75));
        for (size_t i = 0; i < along.x.size(); i++) {
            const float x = along.x[i];
            along.x[i] = x * turnCos - along.y[i] * turnSin;
            along.y[i] = x * turnSin + along.y[i] * turnCos;
        }
        stripes[1] = stripesOf(along, sourceRect, pixel, m_stripeSpacing, m_strokeSigma, inside, 202);
    }
    const bool colorScreen = s.shading == Shading::Screen && s.colors == Colors::Colored;
    // the dots that reach the rect, each found once
    std::vector<Dot> dots;
    QRect dotCells;
    const float dotRadius = 0.5f * s.dot;
    if (marked && s.shading == Shading::Dots) {
        const float cell = dotRadius * 2.4f;
        auto cellOf = [cell](float v) { return int(std::floor(v / cell)); };
        dotCells = QRect(QPoint(cellOf(rect.left() * pixel) - 1, cellOf(rect.top() * pixel) - 1),
                         QPoint(cellOf((rect.right() + 1) * pixel) + 1, cellOf((rect.bottom() + 1) * pixel) + 1));
        dots.resize(size_t(dotCells.width()) * dotCells.height());
        for (int j = dotCells.top(); j <= dotCells.bottom(); j++) {
            for (int i = dotCells.left(); i <= dotCells.right(); i++) {
                const QPointF place = dotPlace(i, j, dotRadius);
                const int px = std::clamp(int(std::floor(place.x() / pixel - sourceRect.left() - work.left())), 0, ww - 1);
                const int py = std::clamp(int(std::floor(place.y() / pixel - sourceRect.top() - work.top())), 0, wh - 1);
                dots[size_t(j - dotCells.top()) * dotCells.width() + (i - dotCells.left())] = dotOf(i, j, dotRadius, dotsFor(darkness(toned[size_t(py) * ww + px])));
            }
        }
    }
    if (marked && !colorScreen) {
        marks.resize(size_t(rw) * rh);
        const float spacing = std::max(2.0f, s.spacing);
        const float strokeWidth = std::max(1.0f, 0.25f * spacing);
        float turnX[3], turnY[3];
        for (int t = 0; t < 3; t++) {
            const double angle = qDegreesToRadians(double(s.angle + hatchTurns[t]));
            turnX[t] = float(std::cos(angle));
            turnY[t] = float(std::sin(angle));
        }
        for (int y = 0; y < rh; y++) {
            const float imageY = (rect.top() + y + 0.5f) * pixel;
            for (int x = 0; x < rw; x++) {
                const float imageX = (rect.left() + x + 0.5f) * pixel;
                const size_t i = size_t(y) * rw + x;
                const float dark = darkness(toned[size_t(inWork.top() + y) * ww + inWork.left() + x]);
                float paper = 1;
                switch (s.shading) {
                case Shading::Hatch: {
                    float cover[3], press;
                    hatchCovers(dark, strokeWidth / spacing, cover, &press);
                    for (int t = 0; t < 3 && cover[t] > 0; t++) {
                        const float along = imageX * -turnY[t] + imageY * turnX[t], across = imageX * turnX[t] + imageY * turnY[t];
                        paper *= 1 - hatchAt(along, across, cover[t], t, spacing, strokeWidth * press, press, pixel);
                    }
                    break;
                }
                case Shading::Strokes: {
                    // the first strokes cover up to a little over half, those crossing them the rest
                    const float first = std::min(dark, 0.55f), second = dark > first ? 1 - (1 - dark) / (1 - first) : 0.0f;
                    const float shares[2] = {first, second};
                    for (int layer = 0; layer < 2; layer++) {
                        if (shares[layer] <= 0) continue;
                        const float value = stripes[layer][i];
                        paper *= 1 - inkOver(value, slopeAt(stripes[layer], rw, rh, x, y), normalQuantile(shares[layer]), 0.02f);
                    }
                    break;
                }
                case Shading::Dots: {
                    const float cell = dotRadius * 2.4f;
                    const int column = int(std::floor(imageX / cell)), row = int(std::floor(imageY / cell));
                    float ink = 0;
                    for (int j = row - 1; j <= row + 1; j++) {
                        for (int i = column - 1; i <= column + 1; i++) {
                            const Dot &dot = dots[size_t(j - dotCells.top()) * dotCells.width() + (i - dotCells.left())];
                            if (dot.reach <= 0) continue;
                            const float dx = imageX - dot.x, dy = imageY - dot.y;
                            ink = std::max(ink, std::clamp((dot.reach - std::sqrt(dx * dx + dy * dy)) / pixel + 0.5f, 0.0f, 1.0f));
                        }
                    }
                    paper = 1 - ink;
                    break;
                }
                case Shading::Screen:
                    paper = 1 - screenAt(imageX, imageY, printed(dark), spacing, turnX[0], turnY[0], pixel);
                    break;
                case Shading::Engrave: {
                    // lines across the angle, pushed aside by the picture's broad tones, each as wide as its share of
                    // the space between them is dark
                    const size_t at = size_t(rect.top() - sourceRect.top() + y) * w + (rect.left() - sourceRect.left() + x);
                    const int sx = rect.left() - sourceRect.left() + x, sy = rect.top() - sourceRect.top() + y;
                    const float gx = 0.5f * (broad[size_t(sy) * w + std::min(sx + 1, w - 1)] - broad[size_t(sy) * w + std::max(sx - 1, 0)]) / pixel;
                    const float gy = 0.5f * (broad[size_t(std::min(sy + 1, h - 1)) * w + sx] - broad[size_t(std::max(sy - 1, 0)) * w + sx]) / pixel;
                    const float u = (imageX * turnX[0] + imageY * turnY[0]) / spacing + engraveBend * broad[at];
                    const float ux = turnX[0] / spacing + engraveBend * gx, uy = turnY[0] / spacing + engraveBend * gy;
                    const float stretch = std::max(std::sqrt(ux * ux + uy * uy), 1e-6f);
                    paper = 1 - bandsOver(u, 0.5f * printed(dark), stretch, pixel);
                    break;
                }
                case Shading::Contour: {
                    // lines where the darkness passes each of its steps, bolder where it is darker
                    const int tx = inWork.left() + x, ty = inWork.top() + y;
                    const float u = dark * float(s.levels);
                    const float stretch = float(s.levels) * slopeAt(toned, ww, wh, tx, ty) / std::max(top, 0.05f) / pixel;
                    if (dark <= 0 || stretch < 1e-6f) break;
                    const float pen = std::max(1.0f, 0.7f * s.size) * (0.6f + 0.8f * dark);
                    paper = 1 - bandsOver(u, 0.5f * pen * stretch, stretch, pixel);
                    break;
                }
                default:
                    break;
                }
                marks[i] = 1 - paper;
            }
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
            picture = alongFlow({premultiplied[0].data(), premultiplied[1].data(), premultiplied[2].data(), premultiplied[3].data()}, flow, m_flowSigma,
                                inside, smooth.data(), edgeRange);
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
    const std::vector<float> tooth = s.shading == Shading::Soft && s.grain > 0 ? toothOf(rect, pixel) : std::vector<float>();
    // a color screen: cyan, magenta and yellow at their own angles, black (the ink) at the angle asked
    float screenX[4], screenY[4];
    for (int k = 0; k < 4; k++) {
        static const float turns[4] = {-30, 30, -45, 0};
        const double angle = qDegreesToRadians(double(s.angle + turns[k]));
        screenX[k] = float(std::cos(angle));
        screenY[k] = float(std::sin(angle));
    }
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const size_t i = size_t(y) * rect.width() + x;
            float cover = std::clamp(smoothed[i], 0.0f, 1.0f);
            if (!tooth.empty()) {
                // the ink catches on the paper's peaks first: the tooth's height against what is left of the paper,
                // softly, though never so softly that bare paper catches some or full ink leaves some
                const float soft = std::max(std::min({0.3f, 2 * cover, 2 * (1 - cover)}), 1e-4f);
                const float caught = std::clamp((cover - (1 - tooth[i])) / soft + 0.5f, 0.0f, 1.0f);
                cover = cover + s.grain * (caught - cover);
            }
            if (!marks.empty()) cover = 1 - (1 - cover) * (1 - marks[i]);
            float *to = out + i * 4;
            if (colorScreen) {
                const float shown = picture[3][i];
                float own[3];
                for (int c = 0; c < 3; c++) own[c] = shown > 1e-6f ? std::max(0.0f, picture[c][i] / shown) : 1.0f;
                const float most = std::max({own[0], own[1], own[2], 1e-6f});
                const float imageX = (rect.left() + x + 0.5f) * pixel, imageY = (rect.top() + y + 0.5f) * pixel;
                const float spacing = std::max(2.0f, s.spacing);
                // black takes what the brightest channel lacks, under the shading's level; the colors the rest
                const float light = std::cbrt(most);
                const float black = shown > 1e-6f ? screenAt(imageX, imageY, printed(darkness(light)), spacing, screenX[3], screenY[3], pixel) : 0.0f;
                float color[3], kept = 1;
                for (int c = 0; c < 3; c++) {
                    const float screened = shown > 1e-6f ? screenAt(imageX, imageY, 1 - own[c] / most, spacing, screenX[c], screenY[c], pixel) : 0.0f;
                    color[c] = 1 - screened;
                    kept *= 1 - screened;
                }
                // the paper under it, or white where it is clear, filtered by the inks laid on it
                const float alpha = 1 - (1 - paper[3]) * kept * (1 - black * ink[3]) * (1 - cover * ink[3]);
                const float inked = black * ink[3] + (1 - black * ink[3]) * cover * ink[3];
                for (int c = 0; c < 3; c++) {
                    const float base = paper[c] * paper[3] + (1 - paper[3]);
                    to[c] = base * color[c] * (1 - inked) + ink[c] * inked;
                }
                to[3] = alpha;
                continue;
            }
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
