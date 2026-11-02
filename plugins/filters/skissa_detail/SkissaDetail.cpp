/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaDetail.h"

#include <algorithm>
#include <cstring>

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

int reachOf(qreal sigma)
{
    if (sigma <= boxFrom) return 3 * int(std::ceil(sigma));
    return 3 * (boxFor(sigma * sigma / 3).r + 1);
}

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

/// Blurs \p data, \p w by \p h, in place; \p scratch is as large
void gaussian(float *data, float *scratch, int w, int h, qreal sigma)
{
    if (w <= 0 || h <= 0 || sigma <= 0) return;
    if (sigma <= boxFrom) {
        const std::vector<float> kernel = kernelOf(sigma);
        const int half = int(kernel.size()) / 2;
        std::vector<float> line(size_t(w) + 2 * half);
        for (int y = 0; y < h; y++) {
            const float *row = data + size_t(y) * w;
            for (int i = 0; i < w + 2 * half; i++) line[i] = row[std::clamp(i - half, 0, w - 1)];
            float *target = scratch + size_t(y) * w;
            for (int x = 0; x < w; x++) {
                float sum = 0;
                for (int k = 0; k <= 2 * half; k++) sum += kernel[k] * line[x + k];
                target[x] = sum;
            }
        }
        for (int y = 0; y < h; y++) {
            float *target = data + size_t(y) * w;
            std::fill(target, target + w, 0.0f);
            for (int k = 0; k <= 2 * half; k++) {
                const float *row = scratch + size_t(std::clamp(y + k - half, 0, h - 1)) * w;
                const float weight = kernel[k];
                for (int x = 0; x < w; x++) target[x] += weight * row[x];
            }
        }
        return;
    }
    const Box box = boxFor(sigma * sigma / 3);
    for (int pass = 0; pass < 3; pass++) boxRows(data, w, h, box);
    boxColumns(data, scratch, w, h, box);
    boxColumns(scratch, data, w, h, box);
    boxColumns(data, scratch, w, h, box);
    std::memcpy(data, scratch, sizeof(float) * size_t(w) * h);
}

/// The lightness the edge-aware blurs follow, its local mean and variance at one size
struct Guide {
    std::vector<float> light;
    std::vector<float> mean;
    std::vector<float> variance;
};

/**
 * \p data blurred keeping edges (He, Sun and Tang's guided filter, with Gaussian windows): in each window the plane
 * is fitted as a line of the guide's lightness, so where the lightness has a strong edge the plane keeps it, and
 * where it varies less than \p epsilon's square root it is evened out
 */
void guided(float *data, float *scratch, int w, int h, qreal sigma, const Guide &guide, double epsilon)
{
    const size_t n = size_t(w) * h;
    std::vector<float> mean(data, data + n);
    gaussian(mean.data(), scratch, w, h, sigma);
    std::vector<float> slope(n);
    for (size_t i = 0; i < n; i++) slope[i] = guide.light[i] * data[i];
    gaussian(slope.data(), scratch, w, h, sigma);
    for (size_t i = 0; i < n; i++) {
        const float a = float((slope[i] - guide.mean[i] * mean[i]) / (guide.variance[i] + epsilon));
        slope[i] = a;
        mean[i] -= a * guide.mean[i];
    }
    gaussian(slope.data(), scratch, w, h, sigma);
    gaussian(mean.data(), scratch, w, h, sigma);
    for (size_t i = 0; i < n; i++) data[i] = slope[i] * guide.light[i] + mean[i];
}

} // namespace

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

int Plan::reach() const
{
    int reach = 0;
    Q_FOREACH (const Step &step, steps) reach = std::max(reach, reachOf(sigmaOf(step.radius)));
    // the edge-aware blur blurs what it blurred once more
    return edges > 0 ? 2 * reach : reach;
}

Plan Plan::scaled(qreal factor) const
{
    Plan plan = *this;
    for (Step &step : plan.steps) step.radius *= factor;
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
    case Kind::Blur:
    case Kind::Smooth: {
        const qreal radius = config.getDouble("radius", kind == Kind::Blur ? 5 : 4);
        if (radius <= 0) break;
        plan.fine = 0;
        plan.steps << Step{radius, 1};
        if (kind == Kind::Smooth) plan.edges = std::clamp(config.getDouble("edges", 0.4), 0.0, 1.0);
        break;
    }
    case Kind::Sharpen: {
        const float amount = float(config.getDouble("amount", 1));
        plan.fine = 1 + amount;
        plan.steps << Step{config.getDouble("radius", 2), -amount};
        break;
    }
    case Kind::HighPass:
        plan.steps << Step{config.getDouble("radius", 4), -1};
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
    const int w = source.width, h = source.height;
    result.resize(resultRect.width(), resultRect.height());
    const QPoint offset = resultRect.topLeft() - sourceRect.topLeft();
    auto forEach = [&](auto &&f) {
        for (int y = 0; y < resultRect.height(); y++) {
            const size_t from = size_t(y + offset.y()) * w + offset.x();
            const size_t to = size_t(y) * resultRect.width();
            for (int x = 0; x < resultRect.width(); x++) f(from + x, to + x);
        }
    };

    // transparency only ever blurs: its gains go no further than 0 to 1, and its largest shapes stay
    QVector<float> alphaLevels;
    float level = plan.fine;
    alphaLevels << std::clamp(level, 0.0f, 1.0f);
    Q_FOREACH (const Step &step, plan.steps) alphaLevels << std::clamp(level += step.gain, 0.0f, 1.0f);
    alphaLevels.last() = 1;

    for (int c = 0; c < 4; c++) {
        const float gain = c < 3 ? plan.fine : alphaLevels.first();
        const std::vector<float> &in = source.plane[c];
        std::vector<float> &out = result.plane[c];
        if (gain != 0) forEach([&](size_t from, size_t to) { out[to] = gain * in[from]; });
    }

    const size_t n = size_t(w) * h;
    std::vector<float> blurred(n), scratch(n);
    Guide guide;
    const double epsilon = plan.edges > 0 ? std::pow(0.02 / plan.edges, 2) : 0;
    if (plan.edges > 0) {
        guide.light.resize(n);
        for (size_t i = 0; i < n; i++) {
            guide.light[i] = 0.2126f * source.plane[0][i] + 0.7152f * source.plane[1][i] + 0.0722f * source.plane[2][i];
        }
    }
    for (int s = 0; s < plan.steps.size(); s++) {
        const Step &step = plan.steps[s];
        const float alphaGain = alphaLevels[s + 1] - alphaLevels[s];
        if (std::abs(step.gain) < 1e-6f && std::abs(alphaGain) < 1e-6f) continue;
        const qreal sigma = sigmaOf(step.radius);
        if (plan.edges > 0) {
            guide.mean = guide.light;
            gaussian(guide.mean.data(), scratch.data(), w, h, sigma);
            guide.variance.resize(n);
            for (size_t i = 0; i < n; i++) guide.variance[i] = guide.light[i] * guide.light[i];
            gaussian(guide.variance.data(), scratch.data(), w, h, sigma);
            for (size_t i = 0; i < n; i++) guide.variance[i] = std::max(0.0f, guide.variance[i] - guide.mean[i] * guide.mean[i]);
        }
        for (int c = 0; c < 4; c++) {
            const float gain = c < 3 ? step.gain : alphaGain;
            if (std::abs(gain) < 1e-6f) continue;
            std::copy(source.plane[c].begin(), source.plane[c].end(), blurred.begin());
            if (plan.edges > 0) guided(blurred.data(), scratch.data(), w, h, sigma, guide, epsilon);
            else gaussian(blurred.data(), scratch.data(), w, h, sigma);
            std::vector<float> &out = result.plane[c];
            forEach([&](size_t from, size_t to) { out[to] += gain * blurred[from]; });
        }
    }

    // what was taken of the largest shapes leaves middle gray, as a high pass does
    const float gray = 0.5f * (1 - plan.coarse());
    if (std::abs(gray) > 1e-6f) {
        const size_t count = size_t(result.width) * result.height;
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
    std::vector<float> light(n), scratch(n);
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < w; x++) light[size_t(y) * w + x] = qGray(line[x]) / 255.0f * qAlpha(line[x]) / 255.0f;
    }
    const QPoint center(qBound(0, at.x(), w - 1), qBound(0, at.y(), h - 1));
    // each half octave's band, between the image blurred to its radius and to the one before
    std::vector<float> finer = light;
    qreal best = 4;
    double bestEnergy = -1;
    for (int i = 0; i <= 14; i++) {
        const qreal radius = std::pow(2.0, i / 2.0);
        std::vector<float> coarser = light;
        gaussian(coarser.data(), scratch.data(), w, h, sigmaOf(radius));
        // its strength around the point, over a window as wide as the features
        const qreal window = std::max(2.0, 2 * sigmaOf(radius));
        const int reach = int(std::ceil(2 * window));
        double energy = 0, weights = 0;
        for (int y = std::max(0, center.y() - reach); y <= std::min(h - 1, center.y() + reach); y++) {
            for (int x = std::max(0, center.x() - reach); x <= std::min(w - 1, center.x() + reach); x++) {
                const double d2 = double(x - center.x()) * (x - center.x()) + double(y - center.y()) * (y - center.y());
                const double weight = std::exp(-d2 / (2 * window * window));
                const size_t i2 = size_t(y) * w + x;
                const double band = finer[i2] - coarser[i2];
                energy += weight * band * band;
                weights += weight;
            }
        }
        energy /= std::max(weights, 1e-9);
        if (energy > bestEnergy) {
            bestEnergy = energy;
            best = radius;
        }
        finer.swap(coarser);
    }
    return best;
}

} // namespace SkissaDetail
