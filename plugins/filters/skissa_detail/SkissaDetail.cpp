/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaDetail.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#include <kis_properties_configuration.h>

namespace SkissaDetail
{

namespace
{

/// Exact Gaussians (Krita's kernel) up to this sigma; wider ones are three box blurs, the same work at any size
constexpr qreal boxFrom = 8;
/// The plan's half-octave samples of a curve
constexpr int samplesPerOctave = 2;

/// A box blur of radius r with the next pixel either side weighed alpha: any variance, not only whole widths
struct Box {
    int r {0};
    double alpha {0};
};

Box boxFor(double variance)
{
    Box box;
    box.r = std::max(0, int(std::floor((std::sqrt(1 + 12 * variance) - 1) / 2)));
    const double r = box.r;
    const double inner = r * (r + 1) * (2 * r + 1) / 3;
    box.alpha = std::clamp(((2 * r + 1) * variance - inner) / (2 * (r + 1) * (r + 1) - 2 * variance), 0.0, 1.0);
    return box;
}

} // namespace

int reachOf(qreal sigma)
{
    if (sigma <= 0) return 0;
    if (sigma <= boxFrom) return 3 * int(std::ceil(sigma));
    return 3 * (boxFor(sigma * sigma / 3).r + 1);
}

namespace
{

/// Krita's Gaussian kernel: 6 ceil(sigma) + 1 taps
std::vector<float> kernelOf(qreal sigma)
{
    const int half = 3 * int(std::ceil(sigma));
    std::vector<float> kernel(2 * half + 1);
    double sum = 0;
    for (int k = -half; k <= half; k++) sum += kernel[k + half] = float(std::exp(-k * k / (2 * sigma * sigma)));
    for (float &value : kernel) value = float(value / sum);
    return kernel;
}

void boxRows(float *data, int w, int h, const Box &box)
{
    const int r = box.r;
    const int pad = r + 1;
    const double width = 2 * r + 1 + 2 * box.alpha;
    std::vector<float> line(size_t(w) + 2 * pad);
    for (int y = 0; y < h; y++) {
        float *row = data + size_t(y) * w;
        for (int i = 0; i < w + 2 * pad; i++) line[i] = row[std::clamp(i - pad, 0, w - 1)];
        double sum = 0;
        for (int k = -r; k <= r; k++) sum += line[pad + k];
        for (int x = 0; x < w; x++) {
            const int p = x + pad;
            row[x] = float((sum + box.alpha * (line[p - r - 1] + line[p + r + 1])) / width);
            sum += line[p + r + 1] - line[p - r];
        }
    }
}

void boxColumns(const float *in, float *out, int w, int h, const Box &box)
{
    const int r = box.r;
    const double width = 2 * r + 1 + 2 * box.alpha;
    const float alpha = float(box.alpha);
    const float scale = float(1 / width);
    auto row = [in, w, h](int y) { return in + size_t(std::clamp(y, 0, h - 1)) * w; };
    std::vector<double> sum(w, 0.0);
    for (int k = -r; k <= r; k++) {
        const float *add = row(k);
        for (int x = 0; x < w; x++) sum[x] += add[x];
    }
    for (int y = 0; y < h; y++) {
        const float *before = row(y - r - 1);
        const float *after = row(y + r + 1);
        const float *leaving = row(y - r);
        float *target = out + size_t(y) * w;
        for (int x = 0; x < w; x++) {
            target[x] = (float(sum[x]) + alpha * (before[x] + after[x])) * scale;
            sum[x] += after[x] - leaving[x];
        }
    }
}

} // namespace

namespace
{

/// Each row blurred by \p sigma, back into \p data
void blurAcross(float *data, int w, int h, qreal sigma)
{
    if (sigma <= 0) return;
    if (sigma > boxFrom) {
        const Box box = boxFor(sigma * sigma / 3);
        for (int pass = 0; pass < 3; pass++) boxRows(data, w, h, box);
        return;
    }
    // whole rows at a time, which vectorize, with the taps either side of the middle taken together
    const std::vector<float> kernel = kernelOf(sigma);
    const int half = int(kernel.size()) / 2;
    std::vector<float> line(size_t(w) + 2 * half);
    for (int y = 0; y < h; y++) {
        float *row = data + size_t(y) * w;
        std::fill(line.begin(), line.begin() + half, row[0]);
        std::copy_n(row, w, line.begin() + half);
        std::fill(line.begin() + half + w, line.end(), row[w - 1]);
        const float *middle = line.data() + half;
        const float centre = kernel[half];
        for (int x = 0; x < w; x++) row[x] = centre * middle[x];
        for (int k = 1; k <= half; k++) {
            const float weight = kernel[half + k];
            const float *before = middle - k;
            const float *after = middle + k;
            for (int x = 0; x < w; x++) row[x] += weight * (before[x] + after[x]);
        }
    }
}

/// Each column blurred by \p sigma, back into \p data
void blurDown(float *data, float *scratch, int w, int h, qreal sigma)
{
    if (sigma <= 0) return;
    if (sigma > boxFrom) {
        const Box box = boxFor(sigma * sigma / 3);
        boxColumns(data, scratch, w, h, box);
        boxColumns(scratch, data, w, h, box);
        boxColumns(data, scratch, w, h, box);
        std::memcpy(data, scratch, sizeof(float) * size_t(w) * h);
        return;
    }
    std::memcpy(scratch, data, sizeof(float) * size_t(w) * h);
    const std::vector<float> kernel = kernelOf(sigma);
    const int half = int(kernel.size()) / 2;
    for (int y = 0; y < h; y++) {
        float *target = data + size_t(y) * w;
        const float *centre = scratch + size_t(y) * w;
        const float middle = kernel[half];
        for (int x = 0; x < w; x++) target[x] = middle * centre[x];
        for (int k = 1; k <= half; k++) {
            const float *above = scratch + size_t(std::clamp(y - k, 0, h - 1)) * w;
            const float *below = scratch + size_t(std::clamp(y + k, 0, h - 1)) * w;
            const float weight = kernel[half + k];
            for (int x = 0; x < w; x++) target[x] += weight * (above[x] + below[x]);
        }
    }
}

} // namespace

void gaussian(float *data, float *scratch, int w, int h, qreal sigmaAcross, qreal sigmaDown)
{
    if (w <= 0 || h <= 0) return;
    blurAcross(data, w, h, sigmaAcross);
    blurDown(data, scratch, w, h, sigmaDown);
}

void gaussian(float *data, float *scratch, int w, int h, qreal sigma)
{
    gaussian(data, scratch, w, h, sigma, sigma);
}

std::vector<float> kernel1D(qreal sigma)
{
    if (sigma <= 0) return {1.0f};
    if (sigma <= boxFrom) return kernelOf(sigma);
    const Box box = boxFor(sigma * sigma / 3);
    std::vector<float> one(2 * box.r + 3, 1.0f);
    one.front() = one.back() = float(box.alpha);
    const float sum = float(2 * box.r + 1 + 2 * box.alpha);
    for (float &value : one) value /= sum;
    std::vector<float> kernel = {1.0f};
    for (int pass = 0; pass < 3; pass++) {
        std::vector<float> wider(kernel.size() + one.size() - 1, 0.0f);
        for (size_t i = 0; i < kernel.size(); i++) {
            for (size_t j = 0; j < one.size(); j++) wider[i + j] += kernel[i] * one[j];
        }
        kernel.swap(wider);
    }
    return kernel;
}

namespace
{

/**
 * Wide blurs are made on smaller copies of the image: a pyramid of levels, each half the size of the one before,
 * made by a binomial filter of six taps (1 5 10 10 5 1), which softens it enough that next to nothing finer than a
 * cell folds into the next. A blur is made on the coarsest level where it still spans two cells, exactly, and
 * enlarged by a cubic B-spline, whose own small blur is taken off what is left to make. Any size of blur then
 * costs what a few pixels' does, within a 40th of a level of 8 bits of an exact Gaussian on the sharpest edge.
 */
constexpr qreal fewestCells = 2.0;

/// The variance a level's grid has been blurred by on its way down, in pixels: each halving's filter adds 1.25 of
/// the cells it halves
double varianceAt(int level)
{
    return 1.25 * (std::ldexp(1.0, 2 * level) - 1) / 3;
}

/// What enlarging a level's grid by a cubic B-spline blurs, in pixels
double splineVarianceAt(int level)
{
    return level > 0 ? std::ldexp(1.0, 2 * level) / 3 : 0;
}

int levelFor(qreal sigma)
{
    int level = 0;
    while (level < 12) {
        const double cell = std::ldexp(1.0, level + 1);
        if (sigma * sigma - varianceAt(level + 1) - splineVarianceAt(level + 1) < fewestCells * fewestCells * cell * cell) break;
        level++;
    }
    return level;
}

inline int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/// The cells of \p level that hold \p rect, and \p margin more around them
QRect cellsOf(const QRect &rect, int level, int margin)
{
    const int cell = 1 << level;
    return QRect(QPoint(floorDiv(rect.left(), cell) - margin, floorDiv(rect.top(), cell) - margin),
                 QPoint(floorDiv(rect.right(), cell) + margin, floorDiv(rect.bottom(), cell) + margin));
}

/// Row \p y of \p data over \p from, \p width values from column \p left, with its edge repeating beyond it
void rowOf(const float *data, const QRect &from, int y, int left, int width, float *target)
{
    // the part inside, copied whole; the edge repeated either side of it
    const float *row = data + size_t(std::clamp(y, from.top(), from.bottom()) - from.top()) * from.width();
    const int inside = std::clamp(from.left() - left, 0, width);
    const int end = std::clamp(from.right() + 1 - left, inside, width);
    std::fill(target, target + inside, row[0]);
    if (end > inside) std::copy_n(row + (left + inside - from.left()), end - inside, target + inside);
    std::fill(target + end, target + width, row[from.width() - 1]);
}

/// \p data over \p from, read for \p to with its edge repeating beyond it
std::vector<float> cropped(const float *data, const QRect &from, const QRect &to)
{
    std::vector<float> out(size_t(to.width()) * to.height());
    for (int y = 0; y < to.height(); y++) rowOf(data, from, to.top() + y, to.left(), to.width(), out.data() + size_t(y) * to.width());
    return out;
}

std::vector<float> cropped(const std::vector<float> &data, const QRect &from, const QRect &to)
{
    return cropped(data.data(), from, to);
}

/// \p data over \p cells blurred by \p sigma cells, kept over \p wanted (inside \p cells' reach)
std::vector<float> blurredOver(const float *data, const QRect &cells, qreal sigma, const QRect &wanted)
{
    if (sigma <= 0) return cropped(data, cells, wanted);
    const int reach = reachOf(sigma);
    const QRect room = wanted.adjusted(-reach, -reach, reach, reach);
    std::vector<float> work = cropped(data, cells, room), scratch(work.size());
    gaussian(work.data(), scratch.data(), room.width(), room.height(), sigma);
    return cropped(work, room, wanted);
}

std::vector<float> blurredOver(const std::vector<float> &data, const QRect &cells, qreal sigma, const QRect &wanted)
{
    return blurredOver(data.data(), cells, sigma, wanted);
}

/// One plane of an image at successively halved sizes, each made when first asked for
class Pyramid
{
public:
    /// \p plane holds \p rect of the image (kept while the pyramid is), read over the full-size grid's \p cells
    /// with its edge repeating
    Pyramid(const float *plane, const QRect &rect, const QRect &cells)
        : m_base(plane)
        , m_baseRect(rect)
        , m_baseCells(cells)
    {
    }

    Pyramid(std::vector<float> plane, const QRect &rect, const QRect &cells)
        : m_own(std::move(plane))
        , m_base(m_own.data())
        , m_baseRect(rect)
        , m_baseCells(cells)
    {
    }

    Pyramid(const Pyramid &) = delete;
    Pyramid &operator=(const Pyramid &) = delete;

    /// The grid of \p level blurred by a further \p sigma of its cells, over \p wanted of its cells
    std::vector<float> blurred(int level, qreal sigma, const QRect &wanted)
    {
        if (level == 0) return blurredOver(m_base, m_baseRect, sigma, wanted);
        while (int(m_levels.size()) < level) {
            Level coarse = m_levels.empty() ? halved(m_base, m_baseRect, m_baseCells)
                                            : halved(m_levels.back().data.data(), m_levels.back().cells, m_levels.back().cells);
            m_levels.push_back(std::move(coarse));
        }
        const Level &grid = m_levels[size_t(level - 1)];
        return blurredOver(grid.data, grid.cells, sigma, wanted);
    }

private:
    struct Level {
        QRect cells;
        std::vector<float> data;
    };

    /// Half the size of \p cells, whose values \p fine holds over \p rect: coarse cell i made of fine cells 2i - 2
    /// to 2i + 3, on cells lined up with the image's, so neighbouring patches agree; beyond the fine grid its edge
    /// repeats
    static Level halved(const float *fine, const QRect &rect, const QRect &cells)
    {
        static const float taps[6] = {1 / 32.0f, 5 / 32.0f, 10 / 32.0f, 10 / 32.0f, 5 / 32.0f, 1 / 32.0f};
        Level coarse;
        coarse.cells = QRect(QPoint(floorDiv(cells.left(), 2), floorDiv(cells.top(), 2)), QPoint(floorDiv(cells.right(), 2), floorDiv(cells.bottom(), 2)));
        const int w = coarse.cells.width(), h = coarse.cells.height();
        // across: every fine row at the coarse columns, a row at a time
        const int wideLeft = 2 * coarse.cells.left() - 2;
        const int wideWidth = 2 * coarse.cells.right() + 3 - wideLeft + 1;
        std::vector<float> wide(static_cast<size_t>(wideWidth));
        std::vector<float> across(size_t(w) * cells.height());
        for (int y = 0; y < cells.height(); y++) {
            rowOf(fine, rect, cells.top() + y, wideLeft, wideWidth, wide.data());
            const float *row = wide.data();
            float *out = across.data() + size_t(y) * w;
            for (int i = 0; i < w; i++) {
                const float *at = row + 2 * i;
                out[i] = taps[0] * (at[0] + at[5]) + taps[1] * (at[1] + at[4]) + taps[2] * (at[2] + at[3]);
            }
        }
        // down: each coarse row from six of those, the edge rows repeating
        coarse.data.assign(size_t(w) * h, 0.0f);
        for (int j = 0; j < h; j++) {
            float *out = coarse.data.data() + size_t(j) * w;
            for (int t = 0; t < 6; t++) {
                const int y = std::clamp(2 * (coarse.cells.top() + j) - 2 + t, cells.top(), cells.bottom()) - cells.top();
                const float *row = across.data() + size_t(y) * w;
                for (int i = 0; i < w; i++) out[i] += taps[t] * row[i];
            }
        }
        return coarse;
    }

    std::vector<float> m_own;
    const float *m_base;
    QRect m_baseRect;
    QRect m_baseCells;
    /// From the first halving on
    std::vector<Level> m_levels;
};

/// The cubic B-spline's weights for the four cells around a point \p t of the way past the second
inline void splineWeights(float t, float w[4])
{
    const float t2 = t * t, t3 = t2 * t;
    w[0] = (1 - t) * (1 - t) * (1 - t) / 6;
    w[1] = (3 * t3 - 6 * t2 + 4) / 6;
    w[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6;
    w[3] = t3 / 6;
}

/// \p grid (\p level's \p cells) enlarged to the pixels of \p target by a cubic B-spline; the pixels themselves at level 0
std::vector<float> enlarged(const std::vector<float> &grid, const QRect &cells, int level, const QRect &target)
{
    if (level == 0) return cropped(grid, cells, target);
    const int cell = 1 << level;
    const int w = target.width(), h = target.height(), cw = cells.width();
    // across first, for every row of cells the target reaches
    const QRect rows = cellsOf(target, level, 2) & cells;
    std::vector<float> across(size_t(w) * rows.height());
    std::vector<int> first(w);
    std::vector<std::array<float, 4>> weights(w);
    for (int x = 0; x < w; x++) {
        const float u = (target.left() + x + 0.5f) / cell - 0.5f;
        const int j = int(std::floor(u));
        first[x] = j - 1;
        splineWeights(u - j, weights[x].data());
    }
    for (int r = 0; r < rows.height(); r++) {
        const float *row = grid.data() + size_t(rows.top() + r - cells.top()) * cw;
        float *out = across.data() + size_t(r) * w;
        for (int x = 0; x < w; x++) {
            float sum = 0;
            for (int k = 0; k < 4; k++) sum += weights[x][k] * row[std::clamp(first[x] + k, cells.left(), cells.right()) - cells.left()];
            out[x] = sum;
        }
    }
    std::vector<float> out(size_t(w) * h, 0.0f);
    for (int y = 0; y < h; y++) {
        const float v = (target.top() + y + 0.5f) / cell - 0.5f;
        const int j = int(std::floor(v));
        float wy[4];
        splineWeights(v - j, wy);
        float *target_row = out.data() + size_t(y) * w;
        for (int k = 0; k < 4; k++) {
            const float *row = across.data() + size_t(std::clamp(j - 1 + k, rows.top(), rows.bottom()) - rows.top()) * w;
            for (int x = 0; x < w; x++) target_row[x] += wy[k] * row[x];
        }
    }
    return out;
}

/// The blur that is left to make on \p level's cells for one of \p sigma, before or after enlarging
qreal blurOnLevel(qreal sigma, int level, bool enlarging)
{
    const double rest = sigma * sigma - varianceAt(level) - (enlarging ? splineVarianceAt(level) : 0);
    return std::sqrt(std::max(0.0, rest)) / std::ldexp(1.0, level);
}

} // namespace

int Plan::reach() const
{
    int reach = 0;
    Q_FOREACH (const Step &step, steps) {
        if (step.radiusDown >= 0) {
            // two ways apart: at full size
            reach = std::max({reach, reachOf(sigmaOf(step.radius)), reachOf(sigmaOf(step.radiusDown))});
            continue;
        }
        const qreal sigma = sigmaOf(step.radius);
        const int level = levelFor(sigma);
        // at full size Krita's kernel; on a level the Gaussian's three sigmas, and a few of the level's cells for
        // the halving filters and the spline
        reach = std::max(reach, level == 0 ? reachOf(sigma) : int(std::ceil(3 * sigma)) + (3 << level));
    }
    // the edge-aware blur blurs what it blurred once more
    return edges > 0 ? 2 * reach : reach;
}

qreal xOfRadius(qreal radius)
{
    return std::clamp(std::log2(std::max(radius, 1e-3)) / octaves, 0.0, 1.0);
}

float gainOf(qreal y)
{
    return float(y <= 0.5 ? 2 * y : 1 + 4 * (y - 0.5));
}

qreal yOfGain(float gain)
{
    return std::clamp(gain <= 1 ? gain / 2.0 : 0.5 + (gain - 1) / 4.0, 0.0, 1.0);
}

KisStudioCurve flatCurve()
{
    return KisStudioCurve{{QPointF(0, 0.5), QPointF(1, 0.5)}};
}

float Plan::coarse() const
{
    float gain = fine;
    Q_FOREACH (const Step &step, steps) gain += step.gain;
    return gain;
}

bool Plan::changesNothing() const
{
    if (std::abs(fine - 1) > 1e-4f) return false;
    Q_FOREACH (const Step &step, steps) {
        if (std::abs(step.gain) > 1e-4f) return false;
    }
    return true;
}

Plan Plan::scaled(qreal factor) const
{
    Plan plan = *this;
    for (Step &step : plan.steps) {
        step.radius *= factor;
        if (step.radiusDown >= 0) step.radiusDown *= factor;
    }
    return plan;
}

Plan planOfCurve(const KisStudioCurve &curve, qreal edges)
{
    const int count = int(octaves) * samplesPerOctave + 1;
    const QVector<float> heights = curve.sampled(1025);
    auto gainAt = [&heights](qreal x) { return gainOf(heights[qBound(0, qRound(x * 1024), 1024)]); };
    Plan plan;
    plan.edges = edges;
    plan.fine = gainAt(0);
    float level = plan.fine;
    for (int i = 1; i < count; i++) {
        const float next = gainAt(qreal(i) / (count - 1));
        if (std::abs(next - level) < 0.002f) continue;
        // between this sample and the one before
        plan.steps << Step{radiusAt((i - 0.5) / (count - 1)), next - level};
        level = next;
    }
    return plan;
}

Plan planOf(Kind kind, const KisPropertiesConfiguration &config)
{
    Plan plan;
    switch (kind) {
    case Kind::Blur: {
        // Krita's Gaussian blur's settings; "radius" in Skissa's own from before
        const qreal across = config.getDouble("horizRadius", config.getDouble("radius", 5));
        const qreal down = config.getDouble("vertRadius", across);
        if (across <= 0 && down <= 0) break;
        plan.fine = 0;
        plan.steps << Step{across, 1, std::abs(down - across) > 1e-6 ? down : -1.0};
        break;
    }
    case Kind::Smooth: {
        const qreal radius = config.getDouble("radius", 4);
        if (radius <= 0) break;
        plan.fine = 0;
        plan.steps << Step{radius, 1};
        plan.edges = std::clamp(config.getDouble("edges", 0.4), 0.0, 1.0);
        break;
    }
    case Kind::Sharpen: {
        // Krita's unsharp mask's
        const float amount = float(config.getDouble("amount", 1));
        plan.fine = 1 + amount;
        plan.steps << Step{config.getDouble("halfSize", config.getDouble("radius", 2)), -amount};
        break;
    }
    case Kind::HighPass:
        // Krita's Gaussian high pass's
        plan.steps << Step{config.getDouble("blurAmount", config.getDouble("radius", 4)), -1};
        break;
    case Kind::Detail:
        plan = planOfCurve(KisStudioCurve::fromString(config.getString("curve"), flatCurve()), std::clamp(config.getDouble("edges", 0), 0.0, 1.0));
        break;
    }
    return plan;
}

void Planes::resize(int w, int h)
{
    width = w;
    height = h;
    for (std::vector<float> &values : plane) values.assign(size_t(w) * h, 0.0f);
}

void apply(const Plan &plan, const Planes &source, const QRect &sourceRect, const QRect &resultRect, Planes &result)
{
    const int w = source.width;
    result.resize(resultRect.width(), resultRect.height());
    const QPoint offset = resultRect.topLeft() - sourceRect.topLeft();
    const size_t count = size_t(resultRect.width()) * resultRect.height();

    // transparency only ever blurs: its gains go no further than 0 to 1, and its largest shapes stay
    QVector<float> alphaLevels;
    float level = plan.fine;
    alphaLevels << std::clamp(level, 0.0f, 1.0f);
    Q_FOREACH (const Step &step, plan.steps) alphaLevels << std::clamp(level += step.gain, 0.0f, 1.0f);
    alphaLevels.last() = 1;
    // a layer without transparency keeps none, whatever its alpha's gains: it isn't blurred at all
    const bool opaque = std::all_of(source.plane[3].begin(), source.plane[3].end(), [](float alpha) { return alpha >= 1; });
    const int planeCount = opaque ? 3 : 4;
    if (opaque) std::fill(result.plane[3].begin(), result.plane[3].end(), 1.0f);

    for (int c = 0; c < planeCount; c++) {
        const float gain = c < 3 ? plan.fine : alphaLevels.first();
        if (gain == 0) continue;
        for (int y = 0; y < resultRect.height(); y++) {
            const float *from = source.plane[c].data() + size_t(y + offset.y()) * w + offset.x();
            float *to = result.plane[c].data() + size_t(y) * resultRect.width();
            for (int x = 0; x < resultRect.width(); x++) to[x] = gain * from[x];
        }
    }

    // each plane's pyramid, and for the edge-aware blur the lightness it follows and its products with each plane;
    // past what was read (the image's edge) its edge repeats, as Krita's blurs do
    const int reach = plan.reach();
    const QRect padded = resultRect.adjusted(-reach, -reach, reach, reach) | sourceRect;
    const bool edges = plan.edges > 0;
    const double epsilon = edges ? std::pow(0.02 / plan.edges, 2) : 0;
    std::unique_ptr<Pyramid> planes[4];
    std::unique_ptr<Pyramid> light, lightSquared, lightTimes[4];
    std::vector<float> lightAll, lightHere;
    auto pyramidOf = [&](std::unique_ptr<Pyramid> &pyramid, auto &&make) -> Pyramid & {
        if (!pyramid) pyramid.reset(new Pyramid(make(), sourceRect, padded));
        return *pyramid;
    };
    auto lightOf = [&]() { return lightAll; };
    if (edges) {
        lightAll.resize(source.plane[0].size());
        for (size_t i = 0; i < lightAll.size(); i++) {
            lightAll[i] = 0.2126f * source.plane[0][i] + 0.7152f * source.plane[1][i] + 0.0722f * source.plane[2][i];
        }
        lightHere = cropped(lightAll, sourceRect, resultRect);
    }

    for (int s = 0; s < plan.steps.size(); s++) {
        const Step &step = plan.steps[s];
        const float alphaGain = alphaLevels[s + 1] - alphaLevels[s];
        if (std::abs(step.gain) < 1e-6f && std::abs(alphaGain) < 1e-6f) continue;
        const qreal sigma = sigmaOf(step.radius);
        if (step.radiusDown >= 0) {
            // across and down apart, at full size
            const qreal down = sigmaOf(step.radiusDown);
            const int reachHere = std::max(reachOf(sigma), reachOf(down));
            const QRect room = resultRect.adjusted(-reachHere, -reachHere, reachHere, reachHere);
            for (int c = 0; c < planeCount; c++) {
                const float gain = c < 3 ? step.gain : alphaGain;
                if (std::abs(gain) < 1e-6f) continue;
                std::vector<float> work = cropped(source.plane[c], sourceRect, room), scratch(work.size());
                gaussian(work.data(), scratch.data(), room.width(), room.height(), step.radius > 0 ? sigma : 0, step.radiusDown > 0 ? down : 0);
                const std::vector<float> blurred = cropped(work, room, resultRect);
                float *out = result.plane[c].data();
                for (size_t i = 0; i < count; i++) out[i] += gain * blurred[i];
            }
            continue;
        }
        const int at = levelFor(sigma);
        if (!edges) {
            const QRect cells = cellsOf(resultRect, at, at > 0 ? 2 : 0);
            for (int c = 0; c < planeCount; c++) {
                const float gain = c < 3 ? step.gain : alphaGain;
                if (std::abs(gain) < 1e-6f) continue;
                Pyramid &pyramid = pyramidOf(planes[c], [&]() { return source.plane[c].data(); });
                const std::vector<float> blurred = enlarged(pyramid.blurred(at, blurOnLevel(sigma, at, true), cells), cells, at, resultRect);
                float *out = result.plane[c].data();
                for (size_t i = 0; i < count; i++) out[i] += gain * blurred[i];
            }
            continue;
        }
        // He, Sun and Tang's guided filter, with Gaussian windows: in each window a plane is fitted as a line of the
        // lightness, so where the lightness has a strong edge the plane keeps it, and where it varies less than
        // epsilon's square root it is evened out. The line's two numbers are smooth, so they are found on the
        // level's cells and enlarged; the lightness they multiply is the image's own.
        const qreal second = blurOnLevel(sigma, 0, false) / std::ldexp(1.0, at);
        const qreal secondCells = std::sqrt(std::max(0.0, second * second - splineVarianceAt(at) / std::ldexp(1.0, 2 * at)));
        const QRect cells = cellsOf(resultRect, at, at > 0 ? 2 : 0);
        const QRect around = cells.adjusted(-reachOf(secondCells), -reachOf(secondCells), reachOf(secondCells), reachOf(secondCells));
        const qreal first = blurOnLevel(sigma, at, false);
        const std::vector<float> meanLight = pyramidOf(light, [&]() { return static_cast<const float *>(lightAll.data()); }).blurred(at, first, around);
        const std::vector<float> meanSquared = pyramidOf(lightSquared, [&]() {
            std::vector<float> values = lightOf();
            for (float &v : values) v *= v;
            return values;
        }).blurred(at, first, around);
        for (int c = 0; c < planeCount; c++) {
            const float gain = c < 3 ? step.gain : alphaGain;
            if (std::abs(gain) < 1e-6f) continue;
            const std::vector<float> mean = pyramidOf(planes[c], [&]() { return source.plane[c].data(); }).blurred(at, first, around);
            const std::vector<float> meanProduct = pyramidOf(lightTimes[c], [&]() {
                std::vector<float> values = lightOf();
                for (size_t i = 0; i < values.size(); i++) values[i] *= source.plane[c][i];
                return values;
            }).blurred(at, first, around);
            std::vector<float> a(mean.size()), b(mean.size());
            for (size_t i = 0; i < mean.size(); i++) {
                const float variance = std::max(0.0f, meanSquared[i] - meanLight[i] * meanLight[i]);
                a[i] = float((meanProduct[i] - meanLight[i] * mean[i]) / (variance + epsilon));
                b[i] = mean[i] - a[i] * meanLight[i];
            }
            const std::vector<float> slope = enlarged(blurredOver(a, around, secondCells, cells), cells, at, resultRect);
            const std::vector<float> base = enlarged(blurredOver(b, around, secondCells, cells), cells, at, resultRect);
            float *out = result.plane[c].data();
            for (size_t i = 0; i < count; i++) out[i] += gain * (slope[i] * lightHere[i] + base[i]);
        }
    }

    // what was taken of the largest shapes leaves middle gray, as a high pass does
    const float gray = 0.5f * (1 - plan.coarse());
    if (std::abs(gray) > 1e-6f) {
        for (int c = 0; c < 3; c++) {
            for (size_t i = 0; i < count; i++) result.plane[c][i] += gray * result.plane[3][i];
        }
    }
}

QImage applied(const QImage &image, const Plan &plan)
{
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    const int w = source.width(), h = source.height();
    Planes planes;
    planes.resize(w, h);
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const float alpha = qAlpha(line[x]) / 255.0f;
            planes.plane[0][i] = qRed(line[x]) / 255.0f * alpha;
            planes.plane[1][i] = qGreen(line[x]) / 255.0f * alpha;
            planes.plane[2][i] = qBlue(line[x]) / 255.0f * alpha;
            planes.plane[3][i] = alpha;
        }
    }
    Planes result;
    apply(plan, planes, QRect(0, 0, w, h), QRect(0, 0, w, h), result);
    QImage out(w, h, QImage::Format_ARGB32);
    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(out.scanLine(y));
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const float alpha = std::clamp(result.plane[3][i], 0.0f, 1.0f);
            auto channel = [&](int c) { return alpha > 1e-6f ? qBound(0, qRound(result.plane[c][i] / alpha * 255), 255) : 0; };
            line[x] = qRgba(channel(0), channel(1), channel(2), qRound(alpha * 255));
        }
    }
    return out;
}

qreal busiestRadius(const QImage &image, const QPoint &at)
{
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    const int w = source.width(), h = source.height();
    if (w <= 0 || h <= 0) return 4;
    const size_t n = size_t(w) * h;
    std::vector<float> light(n);
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < w; x++) light[size_t(y) * w + x] = qGray(line[x]) / 255.0f * qAlpha(line[x]) / 255.0f;
    }
    const QPoint center(qBound(0, at.x(), w - 1), qBound(0, at.y(), h - 1));
    const QRect bounds(0, 0, w, h);
    constexpr int bands = 15;
    auto radiusAt = [](int i) { return std::pow(2.0, i / 2.0); };
    // a band's strength is measured over a window as wide as its features
    auto windowAt = [&](int i) { return std::max(2.0, 2 * sigmaOf(radiusAt(i))); };
    auto windowRect = [&](int i) {
        const int reach = int(std::ceil(2 * windowAt(i)));
        return QRect(center - QPoint(reach, reach), center + QPoint(reach, reach)) & bounds;
    };
    // the image blurred to band i's radius where its window and the next one (holding it) read it: only the
    // blur's reach around that is read, so it comes out as a blur of the whole image would
    auto blurredAt = [&](int i, QRect &kept) {
        const qreal sigma = sigmaOf(radiusAt(i));
        kept = windowRect(std::min(i + 1, bands - 1));
        const int margin = reachOf(sigma);
        const QRect room = kept.adjusted(-margin, -margin, margin, margin) & bounds;
        std::vector<float> work = cropped(light, bounds, room), scratch(work.size());
        gaussian(work.data(), scratch.data(), room.width(), room.height(), sigma);
        return cropped(work, room, kept);
    };
    // each half octave's band, between the image blurred to its radius and to the one before
    std::vector<float> finer = light;
    QRect finerRect = bounds;
    qreal best = 4;
    double bestEnergy = -1;
    for (int i = 0; i < bands; i++) {
        QRect coarserRect;
        std::vector<float> coarser = blurredAt(i, coarserRect);
        const qreal window = windowAt(i);
        const QRect area = windowRect(i);
        const int reach = int(std::ceil(2 * window));
        std::vector<double> falloff(size_t(2 * reach + 1));
        for (int d = -reach; d <= reach; d++) falloff[size_t(d + reach)] = std::exp(-double(d) * d / (2 * window * window));
        double energy = 0, weights = 0;
        for (int y = area.top(); y <= area.bottom(); y++) {
            const double down = falloff[size_t(y - center.y() + reach)];
            const float *fine = finer.data() + size_t(y - finerRect.top()) * finerRect.width();
            const float *coarse = coarser.data() + size_t(y - coarserRect.top()) * coarserRect.width();
            for (int x = area.left(); x <= area.right(); x++) {
                const double weight = down * falloff[size_t(x - center.x() + reach)];
                const double band = fine[x - finerRect.left()] - coarse[x - coarserRect.left()];
                energy += weight * band * band;
                weights += weight;
            }
        }
        energy /= std::max(weights, 1e-9);
        if (energy > bestEnergy) {
            bestEnergy = energy;
            best = radiusAt(i);
        }
        finer.swap(coarser);
        finerRect = coarserRect;
    }
    return best;
}

} // namespace SkissaDetail
