/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaNoise.h"

#include <QMap>

#include <algorithm>
#include <cmath>
#include <vector>

#include <kis_properties_configuration.h>

#include "KoCompositeOpInLinear.h"
#include "KoCompositeOpOklch.h"
#include "SkissaDetail.h"

namespace SkissaNoise
{

namespace
{

/// The bands, every half octave of radius from 1 px (as Detail's curve)
constexpr int bands = 21;
/// A band is made on a grid coarse enough that its finer blur spans this many grid points
constexpr qreal finestOnGrid = 2.5;
/// What full strength is: lightness and each color direction of Oklab, and transparency, as the noise's spread
constexpr float lightnessAtFull = 0.2f;
constexpr float colorAtFull = 0.06f;
constexpr float transparencyAtFull = 0.5f;

const char *const amountKeys[Targets] = {"lightness", "color", "transparency", "displace"};

inline quint32 mixed(quint32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/// White noise of unit spread: three uniform values summed, close to a Gaussian
inline float whiteAt(int i, int j, quint32 key)
{
    const quint32 h = mixed(quint32(i) * 0x8da6b343u ^ mixed(quint32(j) * 0xd8163841u ^ key));
    return (float((h & 1023) + ((h >> 10) & 1023) + ((h >> 20) & 1023)) / 1023.0f - 1.5f) * 2.0f;
}

inline int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/// The 2D correlation of two separable Gaussians, from their 1D kernels (both centered)
double overlap(const std::vector<float> &a, const std::vector<float> &b)
{
    const int ha = int(a.size()) / 2, hb = int(b.size()) / 2;
    const int h = std::min(ha, hb);
    double sum = 0;
    for (int k = -h; k <= h; k++) sum += double(a[k + ha]) * b[k + hb];
    return sum * sum;
}

struct Built {
    struct Band {
        int step;
        bool anchored;
        qreal fine, coarse; ///< sigmas in grid points
        float weight;
    };
    QVector<Band> bands;
    double variance {0};
};

/**
 * The bands of \p sizes at \p scale, each of the strength the curve gives it. A band whose grid is no finer than
 * the pixels at this scale is anchored to the full image's pixels, so a preview at a lower level of detail shows
 * the same clouds the image gets; finer ones are made of this scale's own pixels, where the grain is new but alike.
 */
Built build(const KisStudioCurve &sizes, qreal scale)
{
    const QVector<float> heights = sizes.sampled(1025);
    Built built;
    // what each grid's bands add up to, for the spread of the whole
    QMap<int, QVector<QPair<std::vector<float>, float>>> terms;
    for (int k = 0; k < bands; k++) {
        const float strength = heights[qBound(0, qRound(qreal(k) / (bands - 1) * 1024), 1024)];
        // the band from the boundary below its radius to the one above
        const qreal low = k == 0 ? 0 : std::pow(2.0, (k - 0.5) / 2);
        const qreal high = std::pow(2.0, (k + 0.5) / 2);
        const qreal fineFull = low > 0 ? SkissaDetail::sigmaOf(low) : 0;
        const int step = fineFull <= 0 ? 1 : std::max(1, 1 << int(std::floor(std::log2(std::max(1.0, fineFull / finestOnGrid)))));
        const bool anchored = step * scale >= 1 && (step > 1 || scale == 1);
        float wanted = strength;
        qreal fine, coarse;
        if (anchored) {
            fine = fineFull / step;
            coarse = SkissaDetail::sigmaOf(high) / step;
        } else {
            // sizes under a pixel at this scale average away
            wanted *= float(std::min(1.0, high * scale));
            fine = low > 0 ? SkissaDetail::sigmaOf(low * scale) : 0;
            coarse = SkissaDetail::sigmaOf(high * scale);
        }
        if (wanted < 1e-4f) continue;
        const std::vector<float> a = SkissaDetail::kernel1D(fine);
        const std::vector<float> b = SkissaDetail::kernel1D(coarse);
        const double spread = std::sqrt(std::max(1e-12, overlap(a, a) + overlap(b, b) - 2 * overlap(a, b)));
        const float weight = float(wanted / spread);
        const int grid = anchored ? step : 1;
        built.bands << Built::Band{grid, anchored, fine, coarse, weight};
        terms[grid * 2 + (anchored ? 1 : 0)] << qMakePair(a, weight) << qMakePair(b, -weight);
    }
    // bands on one grid share its white noise; grids are apart
    for (auto it = terms.cbegin(); it != terms.cend(); ++it) {
        const auto &list = it.value();
        for (int i = 0; i < list.size(); i++) {
            for (int j = 0; j < list.size(); j++) built.variance += double(list[i].second) * list[j].second * overlap(list[i].first, list[j].first);
        }
    }
    return built;
}

} // namespace

KisStudioCurve fineSizes()
{
    return KisStudioCurve{{QPointF(0, 1), QPointF(0.06, 1), QPointF(0.16, 0), QPointF(1, 0)}};
}

Settings::Settings()
    : sizes(fineSizes())
{
}

Settings Settings::from(Kind kind, const KisPropertiesConfiguration &config)
{
    Settings settings;
    if (kind == Kind::Grain) {
        // full strength up to the size, gone an octave and a half above it
        const qreal size = std::max(0.5, config.getDouble("size", 1));
        const qreal x = SkissaDetail::xOfRadius(size);
        KisStudioCurve sizes;
        sizes.points << QPointF(0, 1);
        if (x > 0.01) sizes.points << QPointF(x, 1);
        const qreal gone = std::min(1.0, x + 0.15);
        sizes.points << QPointF(gone, 0);
        if (gone < 1) sizes.points << QPointF(1, 0);
        settings.sizes = sizes;
        const float amount = float(std::clamp(config.getDouble("amount", 0.3), 0.0, 4.0));
        settings.amount[Lightness] = amount;
        settings.amount[Color] = amount * float(std::clamp(config.getDouble("color", 0), 0.0, 1.0));
        settings.mids = 0;
        return settings;
    }
    settings.sizes = KisStudioCurve::fromString(config.getString("sizes"), fineSizes());
    for (int t = 0; t < Targets; t++) settings.amount[t] = float(std::max(0.0, config.getDouble(amountKeys[t], t == Lightness ? 0.3 : 0)));
    settings.mids = float(std::clamp(config.getDouble("mids", 0.6), 0.0, 1.0));
    settings.seed = quint32(config.getInt("seed", 0));
    return settings;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("sizes", sizes.toString());
    for (int t = 0; t < Targets; t++) config.setProperty(amountKeys[t], double(amount[t]));
    config.setProperty("mids", double(mids));
    config.setProperty("seed", int(seed));
}

bool Settings::operator==(const Settings &other) const
{
    for (int t = 0; t < Targets; t++) {
        if (std::abs(amount[t] - other.amount[t]) > 1e-4f) return false;
    }
    return sizes == other.sizes && std::abs(mids - other.mids) < 1e-4f && seed == other.seed;
}

bool Settings::changesNothing() const
{
    return std::all_of(std::begin(amount), std::end(amount), [](float a) { return a <= 0; });
}

int Settings::reach(qreal scale) const
{
    return amount[Displace] > 0 ? int(std::ceil(3 * amount[Displace] * scale)) + 2 : 0;
}

Field::Field(const KisStudioCurve &sizes, qreal scale)
{
    const Built whole = build(sizes, 1);
    if (whole.variance <= 1e-12) return;
    // as strong at every scale as in the full image
    m_norm = float(1 / std::sqrt(whole.variance));
    m_scale = scale;
    const Built built = scale == 1 ? whole : build(sizes, scale);
    Q_FOREACH (const Built::Band &band, built.bands) {
        auto level = std::find_if(m_levels.begin(), m_levels.end(),
                                  [&band](const Level &l) { return l.step == band.step && l.anchored == band.anchored; });
        if (level == m_levels.end()) {
            m_levels << Level{band.step, band.anchored, {}, {}};
            level = m_levels.end() - 1;
        }
        // neighbouring bands meet at one blur
        for (const auto &term : {qMakePair(band.fine, band.weight), qMakePair(band.coarse, -band.weight)}) {
            const int at = level->sigmas.indexOf(term.first);
            if (at >= 0) {
                level->weights[at] += term.second;
            } else {
                level->sigmas << term.first;
                level->weights << term.second;
            }
        }
    }
}

void Field::render(const QRect &rect, quint32 key, float *out) const
{
    const int w = rect.width(), h = rect.height();
    std::fill(out, out + size_t(w) * h, 0.0f);
    Q_FOREACH (const Level &level, m_levels) {
        // an anchored grid's points are every step pixels of the full image; the others are this scale's pixels
        const qreal toImage = level.anchored ? 1 / m_scale : 1;
        const int d = level.step;
        int reach = 0;
        Q_FOREACH (qreal sigma, level.sigmas) reach = std::max(reach, SkissaDetail::reachOf(sigma));
        const int i0 = int(std::floor(rect.left() * toImage / d)) - reach - 1;
        const int j0 = int(std::floor(rect.top() * toImage / d)) - reach - 1;
        const int gw = int(std::ceil((rect.right() + 1) * toImage / d)) + reach + 2 - i0 + 1;
        const int gh = int(std::ceil((rect.bottom() + 1) * toImage / d)) + reach + 2 - j0 + 1;
        const size_t n = size_t(gw) * gh;
        const quint32 levelKey = mixed(key ^ (quint32(d) * 0x9e3779b9u) ^ (level.anchored ? 0u : 0x51ed270bu));
        std::vector<float> white(n), sum(n, 0.0f), blurred(n), scratch(n);
        for (int j = 0; j < gh; j++) {
            for (int i = 0; i < gw; i++) white[size_t(j) * gw + i] = whiteAt(i0 + i, j0 + j, levelKey);
        }
        for (int s = 0; s < level.sigmas.size(); s++) {
            const float weight = level.weights[s];
            if (std::abs(weight) < 1e-7f) continue;
            if (level.sigmas[s] <= 0) {
                for (size_t i = 0; i < n; i++) sum[i] += weight * white[i];
                continue;
            }
            blurred = white;
            SkissaDetail::gaussian(blurred.data(), scratch.data(), gw, gh, level.sigmas[s]);
            for (size_t i = 0; i < n; i++) sum[i] += weight * blurred[i];
        }
        // onto the pixels, smoothly between grid points
        const qreal perPixel = toImage / d;
        for (int y = 0; y < h; y++) {
            float *row = out + size_t(y) * w;
            if (perPixel == 1) {
                const float *from = sum.data() + size_t(rect.top() + y - j0) * gw + (rect.left() - i0);
                for (int x = 0; x < w; x++) row[x] += m_norm * from[x];
                continue;
            }
            const double v = (rect.top() + y + 0.5) * perPixel - 0.5 - j0;
            const int jv = int(std::floor(v));
            const float tv = float(v - jv);
            const float *r0 = sum.data() + size_t(jv) * gw;
            const float *r1 = r0 + gw;
            for (int x = 0; x < w; x++) {
                const double u = (rect.left() + x + 0.5) * perPixel - 0.5 - i0;
                const int iu = int(std::floor(u));
                const float tu = float(u - iu);
                const float top = r0[iu] + (r0[iu + 1] - r0[iu]) * tu;
                const float bottom = r1[iu] + (r1[iu + 1] - r1[iu]) * tu;
                row[x] += m_norm * (top + (bottom - top) * tv);
            }
        }
    }
}

void apply(const Settings &settings, qreal scale, const float *source, const QRect &sourceRect, const QRect &rect, float *out, bool fit)
{
    const int w = rect.width(), h = rect.height();
    const size_t n = size_t(w) * h;
    const Field field(settings.sizes, scale);
    const float *amount = settings.amount;
    auto noise = [&](bool wanted, quint32 which) {
        std::vector<float> values;
        if (!wanted || field.isEmpty()) return values;
        values.resize(n);
        field.render(rect, mixed(settings.seed * 0x632be5abu + which), values.data());
        return values;
    };
    const std::vector<float> light = noise(amount[Lightness] > 0, 1);
    const std::vector<float> green = noise(amount[Color] > 0, 2);
    const std::vector<float> blue = noise(amount[Color] > 0, 3);
    const std::vector<float> clear = noise(amount[Transparency] > 0, 4);
    const std::vector<float> across = noise(amount[Displace] > 0, 5);
    const std::vector<float> down = noise(amount[Displace] > 0, 6);
    const float shift = float(amount[Displace] * scale);

    const int sw = sourceRect.width(), sh = sourceRect.height();
    auto at = [&](int x, int y) { return source + (size_t(std::clamp(y, 0, sh - 1)) * sw + std::clamp(x, 0, sw - 1)) * 4; };
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const size_t i = size_t(y) * w + x;
            const int sx = rect.left() + x - sourceRect.left();
            const int sy = rect.top() + y - sourceRect.top();
            float pixel[4];
            if (!across.empty()) {
                // the pixel taken from beside it, blended between its four nearest as their colors weighed by coverage
                const float fx = sx + shift * across[i], fy = sy + shift * down[i];
                const int ix = int(std::floor(fx)), iy = int(std::floor(fy));
                const float tx = fx - ix, ty = fy - iy;
                float sum[4] = {0, 0, 0, 0};
                const float weights[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
                const float *corners[4] = {at(ix, iy), at(ix + 1, iy), at(ix, iy + 1), at(ix + 1, iy + 1)};
                for (int c = 0; c < 4; c++) {
                    const float coverage = weights[c] * corners[c][3];
                    for (int k = 0; k < 3; k++) sum[k] += coverage * corners[c][k];
                    sum[3] += coverage;
                }
                for (int k = 0; k < 3; k++) pixel[k] = sum[3] > 1e-6f ? sum[k] / sum[3] : 0.0f;
                pixel[3] = sum[3];
            } else {
                std::copy_n(at(sx, sy), 4, pixel);
            }
            if (pixel[3] > 0 && (!light.empty() || !green.empty())) {
                float lab[3];
                KoMixing::linearToOklab(pixel, lab);
                if (!light.empty()) {
                    const float tone = std::clamp(lab[0], 0.0f, 1.0f);
                    const float weight = 1 - settings.mids + settings.mids * 4 * tone * (1 - tone);
                    float spread = amount[Lightness] * lightnessAtFull * weight;
                    // near black, and near white where the picture stops there, the noise has room on one side only:
                    // cut there it would darken whites and lift blacks, so it stays within two spreads of the end
                    spread = std::min(spread, (fit ? std::min(tone, 1 - tone) : tone) / 2);
                    lab[0] += spread * light[i];
                }
                if (!green.empty()) {
                    lab[1] += amount[Color] * colorAtFull * green[i];
                    lab[2] += amount[Color] * colorAtFull * blue[i];
                }
                if (fit) {
                    KoOklch::inGamut(lab, pixel);
                } else {
                    KoMixing::oklabToLinear(lab, pixel);
                }
            }
            if (!clear.empty()) pixel[3] *= std::clamp(1 + amount[Transparency] * transparencyAtFull * clear[i], 0.0f, 1.0f);
            std::copy_n(pixel, 4, out + i * 4);
        }
    }
}

QImage applied(const QImage &image, const QPoint &corner, const Settings &settings)
{
    const QImage source = image.convertToFormat(QImage::Format_ARGB32);
    const int w = source.width(), h = source.height();
    const float *decode = KoMixing::srgbToLinearTable();
    const quint8 *encode = KoMixing::linearToSrgbTable();
    std::vector<float> in(size_t(w) * h * 4), out(in.size());
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(source.constScanLine(y));
        for (int x = 0; x < w; x++) {
            float *pixel = in.data() + (size_t(y) * w + x) * 4;
            pixel[0] = decode[qRed(line[x])];
            pixel[1] = decode[qGreen(line[x])];
            pixel[2] = decode[qBlue(line[x])];
            pixel[3] = qAlpha(line[x]) / 255.0f;
        }
    }
    const QRect rect(corner, source.size());
    apply(settings, 1, in.data(), rect, rect, out.data(), true);
    QImage result(w, h, QImage::Format_ARGB32);
    auto encoded = [encode](float c) { return int(encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]); };
    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < w; x++) {
            const float *pixel = out.data() + (size_t(y) * w + x) * 4;
            line[x] = qRgba(encoded(pixel[0]), encoded(pixel[1]), encoded(pixel[2]), qRound(std::clamp(pixel[3], 0.0f, 1.0f) * 255));
        }
    }
    return result;
}

} // namespace SkissaNoise
