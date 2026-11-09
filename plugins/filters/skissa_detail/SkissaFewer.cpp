/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaFewer.h"

#include <QRandomGenerator>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>

#include <kis_properties_configuration.h>

#include "KoCompositeOpMix.h"

namespace SkissaFewer
{

namespace
{

const char *const sourceKeys[] = {"image", "levels", "gray", "palette"};
const char *const ditherKeys[] = {"none", "pattern", "noise", "diffuse"};

/// The most steps of each channel: their colors are numbered in 16 bits
constexpr int mostLevels = 32;
constexpr int mostColors = 256;
/// Candidates a pixel's mix is made of: as many shades between two colors as it can show
constexpr int candidates = 16;

/// Linear to the sRGB curve, from a table read between its steps
float encoded(float linear)
{
    static const std::vector<float> table = []() {
        std::vector<float> values(4097);
        for (int i = 0; i <= 4096; i++) values[i] = KoMixing::srgbOf(i / 4096.0f);
        return values;
    }();
    const float x = std::clamp(linear, 0.0f, 1.0f) * 4096;
    const int i = std::min(int(x), 4095);
    return table[i] + (table[i + 1] - table[i]) * (x - i);
}

float lightnessOf(const float rgb[3])
{
    const float clamped[3] = {std::max(rgb[0], 0.0f), std::max(rgb[1], 0.0f), std::max(rgb[2], 0.0f)};
    float lab[3];
    KoMixing::linearToOklab(clamped, lab);
    return lab[0];
}

/// Bayer's 8 by 8 pattern, each step a 64th apart
const std::array<float, 64> &bayer()
{
    static const std::array<float, 64> pattern = []() {
        std::array<int, 64> m {};
        m[0] = 0;
        // each size made of four copies of the one half as large: 4m, 4m + 2, 4m + 3, 4m + 1
        for (int size = 1; size < 8; size *= 2) {
            std::array<int, 64> next {};
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    const int v = 4 * m[y * 8 + x];
                    next[y * 8 + x] = v;
                    next[y * 8 + x + size] = v + 2;
                    next[(y + size) * 8 + x] = v + 3;
                    next[(y + size) * 8 + x + size] = v + 1;
                }
            }
            m = next;
        }
        std::array<float, 64> out {};
        for (int i = 0; i < 64; i++) out[i] = (m[i] + 0.5f) / 64;
        return out;
    }();
    return pattern;
}

float thresholdAt(Dither dither, int x, int y)
{
    if (dither == Dither::Pattern) return bayer()[(y & 7) * 8 + (x & 7)];
    return blueNoise()[size_t(y & 63) * 64 + (x & 63)];
}

QColor colorOfLab(const float lab[3])
{
    float rgb[3];
    KoMixing::oklabToLinear(lab, rgb);
    return QColor::fromRgbF(KoMixing::srgbOf(std::clamp(rgb[0], 0.0f, 1.0f)), KoMixing::srgbOf(std::clamp(rgb[1], 0.0f, 1.0f)),
                            KoMixing::srgbOf(std::clamp(rgb[2], 0.0f, 1.0f)));
}

} // namespace

/**
 * Blue noise, 64 by 64, made by Ulichney's void and cluster: points are added one at a time where they are
 * loneliest, so any threshold of it is spread evenly, without the clumps of white noise or the grid of a pattern.
 */
const std::vector<float> &blueNoise()
{
    static const std::vector<float> noise = []() {
        constexpr int n = 64, size = n * n, reach = 6;
        constexpr float sigma = 1.5f;
        float kernel[2 * reach + 1][2 * reach + 1];
        for (int dy = -reach; dy <= reach; dy++) {
            for (int dx = -reach; dx <= reach; dx++) kernel[dy + reach][dx + reach] = std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
        }
        struct Field {
            std::vector<char> on = std::vector<char>(size, 0);
            std::vector<float> energy = std::vector<float>(size, 0.0f);
            void put(int p, float sign, const float (&k)[2 * reach + 1][2 * reach + 1])
            {
                on[p] = sign > 0;
                const int px = p % n, py = p / n;
                for (int dy = -reach; dy <= reach; dy++) {
                    for (int dx = -reach; dx <= reach; dx++) {
                        energy[((py + dy + n) % n) * n + (px + dx + n) % n] += sign * k[dy + reach][dx + reach];
                    }
                }
            }
            /// The point in the most crowded place, or the empty place most alone
            int tightest() const
            {
                int best = -1;
                for (int p = 0; p < size; p++) if (on[p] && (best < 0 || energy[p] > energy[best])) best = p;
                return best;
            }
            int loneliest() const
            {
                int best = -1;
                for (int p = 0; p < size; p++) if (!on[p] && (best < 0 || energy[p] < energy[best])) best = p;
                return best;
            }
        };
        // a tenth of the points at random, moved from crowds to voids until they are even
        Field first;
        QRandomGenerator random(7);
        const int seeded = size / 10;
        for (int placed = 0; placed < seeded;) {
            const int p = int(random.bounded(size));
            if (first.on[p]) continue;
            first.put(p, 1, kernel);
            placed++;
        }
        for (int moves = 0; moves < size; moves++) {
            const int crowded = first.tightest();
            first.put(crowded, -1, kernel);
            const int lonely = first.loneliest();
            first.put(lonely, 1, kernel);
            if (lonely == crowded) break;
        }
        std::vector<int> rank(size, 0);
        Field fewer = first;
        for (int r = seeded - 1; r >= 0; r--) {
            const int p = fewer.tightest();
            rank[p] = r;
            fewer.put(p, -1, kernel);
        }
        Field more = first;
        for (int r = seeded; r < size; r++) {
            const int p = more.loneliest();
            rank[p] = r;
            more.put(p, 1, kernel);
        }
        std::vector<float> out(size);
        for (int p = 0; p < size; p++) out[p] = (rank[p] + 0.5f) / size;
        return out;
    }();
    return noise;
}

int cellAt(int pixel, int lod)
{
    return std::max(1, int(std::lround(pixel / std::pow(2.0, lod))));
}

QRect cellsOver(const QRect &rect, int cell)
{
    auto down = [cell](int v) { return v >= 0 ? v / cell : -((-v + cell - 1) / cell); };
    return QRect(QPoint(down(rect.left()), down(rect.top())), QPoint(down(rect.right()), down(rect.bottom())));
}

QRect pixelsOf(const QRect &cells, int cell)
{
    return QRect(cells.left() * cell, cells.top() * cell, cells.width() * cell, cells.height() * cell);
}

std::vector<float> cellColors(const float *pixels, const QRect &read, const QRect &bounds, int cell, const QRect &cells)
{
    // each pixel in Oklab times its alpha, and its alpha: a clear pixel is near any other clear one
    const size_t count = size_t(read.width()) * read.height();
    std::vector<std::array<float, 4>> lab(count);
    for (size_t i = 0; i < count; i++) {
        const float *pixel = pixels + i * 4;
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        const float clamped[3] = {std::max(pixel[0], 0.0f), std::max(pixel[1], 0.0f), std::max(pixel[2], 0.0f)};
        float c[3];
        KoMixing::linearToOklab(clamped, c);
        lab[i] = {c[0] * alpha, c[1] * alpha, c[2] * alpha, alpha};
    }
    auto distance = [](const std::array<float, 4> &a, const std::array<float, 4> &b) {
        float d = 0;
        for (int k = 0; k < 4; k++) d += (a[k] - b[k]) * (a[k] - b[k]);
        return d;
    };
    const QRect inside = bounds & read;
    auto pixelsIn = [&](const QPoint &at) { return pixelsOf(QRect(at, QSize(1, 1)), cell) & inside; };
    // every cell's average, and of those around the cells asked for: what a cell stands out from
    const QRect around = cells.adjusted(-1, -1, 1, 1);
    std::vector<std::array<float, 4>> means(size_t(around.width()) * around.height(), {0, 0, 0, 0});
    std::vector<int> counts(means.size(), 0);
    for (int cy = around.top(); cy <= around.bottom(); cy++) {
        for (int cx = around.left(); cx <= around.right(); cx++) {
            const QRect area = pixelsIn(QPoint(cx, cy));
            const size_t m = size_t(cy - around.top()) * around.width() + (cx - around.left());
            for (int y = area.top(); y <= area.bottom(); y++) {
                for (int x = area.left(); x <= area.right(); x++) {
                    const std::array<float, 4> &v = lab[size_t(y - read.top()) * read.width() + (x - read.left())];
                    for (int k = 0; k < 4; k++) means[m][k] += v[k];
                    counts[m]++;
                }
            }
            if (counts[m]) {
                for (float &v : means[m]) v /= counts[m];
            }
        }
    }
    std::vector<float> out(size_t(cells.width()) * cells.height() * 4, 0.0f);
    std::vector<std::array<float, 4>> samples;
    std::vector<int> side;
    for (int cy = cells.top(); cy <= cells.bottom(); cy++) {
        for (int cx = cells.left(); cx <= cells.right(); cx++) {
            const size_t m = size_t(cy - around.top()) * around.width() + (cx - around.left());
            if (!counts[m]) continue;
            samples.clear();
            const QRect area = pixelsIn(QPoint(cx, cy));
            for (int y = area.top(); y <= area.bottom(); y++) {
                for (int x = area.left(); x <= area.right(); x++) samples.push_back(lab[size_t(y - read.top()) * read.width() + (x - read.left())]);
            }
            const std::array<float, 4> &mean = means[m];
            std::array<float, 4> chosen = mean;
            // two kinds of color in the cell: from the one farthest from its average and the one farthest from that
            std::array<float, 4> centers[2];
            float far = -1;
            for (const auto &s : samples) {
                if (distance(s, mean) > far) {
                    far = distance(s, mean);
                    centers[0] = s;
                }
            }
            far = -1;
            for (const auto &s : samples) {
                if (distance(s, centers[0]) > far) {
                    far = distance(s, centers[0]);
                    centers[1] = s;
                }
            }
            if (far > 0.06f * 0.06f) {
                side.assign(samples.size(), 0);
                float shares[2] = {0, 0};
                for (int round = 0; round < 4; round++) {
                    std::array<float, 4> sums[2] = {{0, 0, 0, 0}, {0, 0, 0, 0}};
                    shares[0] = shares[1] = 0;
                    for (size_t i = 0; i < samples.size(); i++) {
                        side[i] = distance(samples[i], centers[1]) < distance(samples[i], centers[0]) ? 1 : 0;
                        for (int k = 0; k < 4; k++) sums[side[i]][k] += samples[i][k];
                        shares[side[i]]++;
                    }
                    for (int j = 0; j < 2; j++) {
                        if (shares[j] > 0) {
                            for (int k = 0; k < 4; k++) centers[j][k] = sums[j][k] / shares[j];
                        }
                    }
                }
                if (distance(centers[0], centers[1]) > 0.06f * 0.06f) {
                    // what is around the cell
                    std::array<float, 4> context = {0, 0, 0, 0};
                    int seen = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            const size_t n = size_t(cy + dy - around.top()) * around.width() + (cx + dx - around.left());
                            if ((dx || dy) && counts[n]) {
                                for (int k = 0; k < 4; k++) context[k] += means[n][k];
                                seen++;
                            }
                        }
                    }
                    if (seen) {
                        for (float &v : context) v /= seen;
                    } else {
                        context = mean;
                    }
                    /*
                     * Of the two, the one that stands for the cell best: the larger, unless the other stands out
                     * from what is around the cell (a line, an edge's dark side) and is more than a speck, so thin
                     * lines stay whole, as a pixel artist keeps them
                     */
                    float best = -1;
                    for (int j = 0; j < 2; j++) {
                        const float share = shares[j] / samples.size();
                        if (share < 0.2f) continue;
                        const float score = share * (std::sqrt(distance(centers[j], context)) + 0.05f);
                        if (score > best) {
                            best = score;
                            chosen = centers[j];
                        }
                    }
                }
            }
            float *to = out.data() + (size_t(cy - cells.top()) * cells.width() + (cx - cells.left())) * 4;
            const float alpha = chosen[3];
            if (alpha > 1e-4f) {
                const float c[3] = {chosen[0] / alpha, chosen[1] / alpha, chosen[2] / alpha};
                KoMixing::oklabToLinear(c, to);
            }
            // a cell is in the picture or out of it, as a pixel artist's pixels are: soft edges would stay soft
            to[3] = alpha >= 0.5f ? 1.0f : 0.0f;
        }
    }
    return out;
}

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings settings;
    const QString source = config.getString("source", QStringLiteral("levels"));
    for (int i = 0; i < 4; i++) {
        if (source == QLatin1String(sourceKeys[i])) settings.source = Source(i);
    }
    const QString dither = config.getString("dither", QStringLiteral("none"));
    for (int i = 0; i < 4; i++) {
        if (dither == QLatin1String(ditherKeys[i])) settings.dither = Dither(i);
    }
    settings.count = std::clamp(config.getInt("count", 4), 2, settings.source == Source::Levels ? mostLevels : mostColors);
    Q_FOREACH (const QString &name, config.getString("colors").split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QColor color(name.trimmed());
        if (color.isValid()) settings.colors << color;
    }
    settings.palette = config.getString("palette");
    settings.amount = float(std::clamp(config.getDouble("amount", 1), 0.0, 1.0));
    settings.lightness = float(std::clamp(config.getDouble("lightness", 0), -0.5, 0.5));
    settings.pixel = std::clamp(config.getInt("pixel", 1), 1, 256);
    return settings;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("source", QString::fromLatin1(sourceKeys[int(source)]));
    config.setProperty("count", count);
    QStringList names;
    if (takesColors()) {
        Q_FOREACH (const QColor &color, colors) names << color.name();
    }
    config.setProperty("colors", names.join(QLatin1Char(',')));
    config.setProperty("palette", source == Source::Palette ? palette : QString());
    config.setProperty("dither", QString::fromLatin1(ditherKeys[int(dither)]));
    config.setProperty("amount", double(amount));
    config.setProperty("lightness", double(lightness));
    config.setProperty("pixel", pixel);
}

bool Settings::operator==(const Settings &other) const
{
    return source == other.source && count == other.count && (!takesColors() || colors == other.colors) && dither == other.dither
        && (dither == Dither::None || amount == other.amount) && lightness == other.lightness && pixel == other.pixel;
}

Quantizer::Quantizer(const Settings &settings)
    : m_source(settings.source)
    , m_count(settings.count)
{
    if (settings.takesColors() && settings.colors.isEmpty()) {
        m_source = Source::Levels;
        m_count = std::clamp(m_count, 2, mostLevels);
    }
    switch (m_source) {
    case Source::Levels: {
        std::vector<float> steps(m_count);
        for (int i = 0; i < m_count; i++) steps[i] = KoMixing::linearOf(float(i) / (m_count - 1));
        for (int b = 0; b < m_count; b++) {
            for (int g = 0; g < m_count; g++) {
                for (int r = 0; r < m_count; r++) m_rgb.insert(m_rgb.end(), {steps[r], steps[g], steps[b]});
            }
        }
        break;
    }
    case Source::Gray:
        // evenly in Oklab lightness, whose gray is its cube in linear light
        for (int i = 0; i < m_count; i++) {
            const float l = float(i) / (m_count - 1);
            m_rgb.insert(m_rgb.end(), {l * l * l, l * l * l, l * l * l});
        }
        break;
    default: {
        struct Entry {
            float lab[3];
            float rgb[3];
        };
        std::vector<Entry> entries;
        Q_FOREACH (const QColor &color, settings.colors) {
            Entry entry;
            entry.rgb[0] = KoMixing::linearOf(float(color.redF()));
            entry.rgb[1] = KoMixing::linearOf(float(color.greenF()));
            entry.rgb[2] = KoMixing::linearOf(float(color.blueF()));
            KoMixing::linearToOklab(entry.rgb, entry.lab);
            entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.lab[0] < b.lab[0]; });
        for (const Entry &entry : entries) {
            m_rgb.insert(m_rgb.end(), entry.rgb, entry.rgb + 3);
            m_lab.insert(m_lab.end(), entry.lab, entry.lab + 3);
        }
        break;
    }
    }
    for (int i = 0; i < size(); i++) m_lightness.push_back(SkissaFewer::lightnessOf(colorOf(i)));
}

int Quantizer::nearest(const float rgb[3]) const
{
    switch (m_source) {
    case Source::Levels: {
        int index = 0;
        for (int c = 2; c >= 0; c--) index = index * m_count + int(encoded(rgb[c]) * (m_count - 1) + 0.5f);
        return index;
    }
    case Source::Gray:
        return int(std::clamp(SkissaFewer::lightnessOf(rgb), 0.0f, 1.0f) * (m_count - 1) + 0.5f);
    default: {
        const float clamped[3] = {std::max(rgb[0], 0.0f), std::max(rgb[1], 0.0f), std::max(rgb[2], 0.0f)};
        float lab[3];
        KoMixing::linearToOklab(clamped, lab);
        // outward from the colors as light as it, until lightness alone is farther than the best
        const int n = size();
        const auto start = std::lower_bound(m_lightness.begin(), m_lightness.end(), lab[0]);
        int up = int(start - m_lightness.begin()), down = up - 1;
        int best = std::min(up, n - 1);
        float bestDistance = HUGE_VALF;
        auto test = [&](int i) {
            const float *c = m_lab.data() + size_t(i) * 3;
            const float d = (c[0] - lab[0]) * (c[0] - lab[0]) + (c[1] - lab[1]) * (c[1] - lab[1]) + (c[2] - lab[2]) * (c[2] - lab[2]);
            if (d < bestDistance) {
                bestDistance = d;
                best = i;
            }
        };
        while (up < n || down >= 0) {
            const float upGap = up < n ? m_lab[size_t(up) * 3] - lab[0] : HUGE_VALF;
            const float downGap = down >= 0 ? lab[0] - m_lab[size_t(down) * 3] : HUGE_VALF;
            if (std::min(upGap, downGap) * std::min(upGap, downGap) >= bestDistance) break;
            if (upGap <= downGap) {
                test(up++);
            } else {
                test(down--);
            }
        }
        return best;
    }
    }
}

void shift(float rgb[3], float lightness)
{
    if (lightness == 0) return;
    const float clamped[3] = {std::max(rgb[0], 0.0f), std::max(rgb[1], 0.0f), std::max(rgb[2], 0.0f)};
    float lab[3];
    KoMixing::linearToOklab(clamped, lab);
    lab[0] = std::max(0.0f, lab[0] + lightness);
    KoMixing::oklabToLinear(lab, rgb);
}

void apply(const Settings &settings, const Quantizer &quantizer, float *pixels, const QRect &rect)
{
    const bool dithered = settings.dither != Dither::None && settings.dither != Dither::Diffuse && settings.amount > 0;
    // a pixel's mix depends on its color alone, and pictures repeat colors: the mixes of the colors met last are kept
    struct Mix {
        bool made {false};
        float rgb[3];
        quint16 colors[candidates];
    };
    constexpr int kept = 1024;
    std::vector<Mix> mixes(dithered ? kept : 0);
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            float *pixel = pixels + (size_t(y) * rect.width() + x) * 4;
            if (pixel[3] <= 0) continue;
            float goal[3] = {pixel[0], pixel[1], pixel[2]};
            int chosen;
            if (!dithered) {
                shift(goal, settings.lightness);
                for (float &c : goal) c = std::clamp(c, 0.0f, 1.0f);
                chosen = quantizer.nearest(goal);
            } else {
                quint32 bits[3];
                std::memcpy(bits, pixel, sizeof(bits));
                const quint32 hash = (bits[0] * 0x9e3779b1u) ^ (bits[1] * 0x85ebca77u) ^ (bits[2] * 0xc2b2ae3du);
                Mix &mix = mixes[(hash ^ hash >> 15) % kept];
                if (!mix.made || std::memcmp(mix.rgb, pixel, sizeof(mix.rgb)) != 0) {
                    mix.made = true;
                    std::copy_n(pixel, 3, mix.rgb);
                    shift(goal, settings.lightness);
                    for (float &c : goal) c = std::clamp(c, 0.0f, 1.0f);
                    // Knoll's pattern dithering: colors chosen one after another, each making up for what those
                    // before missed, so together they average to the pixel; the threshold picks one of them, from
                    // dark to light
                    float error[3] = {0, 0, 0};
                    for (int i = 0; i < candidates; i++) {
                        float attempt[3];
                        for (int c = 0; c < 3; c++) attempt[c] = std::clamp(goal[c] + error[c] * settings.amount, 0.0f, 1.0f);
                        const int candidate = quantizer.nearest(attempt);
                        const float *color = quantizer.colorOf(candidate);
                        for (int c = 0; c < 3; c++) error[c] += goal[c] - color[c];
                        int at = i;
                        while (at > 0 && quantizer.lightnessOf(mix.colors[at - 1]) > quantizer.lightnessOf(candidate)) {
                            mix.colors[at] = mix.colors[at - 1];
                            at--;
                        }
                        mix.colors[at] = quint16(candidate);
                    }
                }
                const float t = thresholdAt(settings.dither, rect.left() + x, rect.top() + y);
                chosen = mix.colors[std::min(candidates - 1, int(t * candidates))];
            }
            std::copy_n(quantizer.colorOf(chosen), 3, pixel);
        }
    }
}

Diffuser::Diffuser(const Settings &settings, const Quantizer &quantizer, int width)
    : m_settings(settings)
    , m_quantizer(quantizer)
    , m_width(width)
    , m_error(size_t(width + 2) * 3, 0.0f)
    , m_nextError(size_t(width + 2) * 3, 0.0f)
{
}

void Diffuser::row(const float *row, quint16 *out)
{
    // every other row right to left, so the error doesn't drift one way
    const bool back = m_y++ % 2;
    std::fill(m_nextError.begin(), m_nextError.end(), 0.0f);
    const float amount = m_settings.amount;
    for (int i = 0; i < m_width; i++) {
        const int x = back ? m_width - 1 - i : i;
        const float *pixel = row + size_t(x) * 4;
        if (pixel[3] <= 0) {
            out[x] = none;
            continue;
        }
        float *error = m_error.data() + size_t(x + 1) * 3;
        float goal[3] = {pixel[0], pixel[1], pixel[2]};
        shift(goal, m_settings.lightness);
        for (int c = 0; c < 3; c++) goal[c] = std::clamp(goal[c] + error[c], 0.0f, 1.0f);
        const int chosen = m_quantizer.nearest(goal);
        out[x] = quint16(chosen);
        const float *color = m_quantizer.colorOf(chosen);
        // Floyd and Steinberg's shares: ahead 7, behind below 3, below 5, ahead below 1
        const int ahead = back ? -1 : 1;
        for (int c = 0; c < 3; c++) {
            const float e = (goal[c] - color[c]) * amount;
            m_error[size_t(x + 1 + ahead) * 3 + c] += e * 7 / 16;
            m_nextError[size_t(x + 1 - ahead) * 3 + c] += e * 3 / 16;
            m_nextError[size_t(x + 1) * 3 + c] += e * 5 / 16;
            m_nextError[size_t(x + 1 + ahead) * 3 + c] += e * 1 / 16;
        }
    }
    std::swap(m_error, m_nextError);
}

QVector<QColor> colorsOf(const QImage &image, int count)
{
    const QImage picture = image.convertToFormat(QImage::Format_ARGB32);
    struct Sample {
        float lab[3];
        float weight;
    };
    std::vector<Sample> samples;
    // at most this many pixels, taken evenly
    constexpr int mostSamples = 16384;
    const qint64 pixels = qint64(picture.width()) * picture.height();
    const int step = std::max(1, int(std::ceil(std::sqrt(double(pixels) / mostSamples))));
    const float *decode = KoMixing::srgbToLinearTable();
    for (int y = 0; y < picture.height(); y += step) {
        const QRgb *line = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
        for (int x = 0; x < picture.width(); x += step) {
            if (!qAlpha(line[x])) continue;
            Sample sample;
            const float rgb[3] = {decode[qRed(line[x])], decode[qGreen(line[x])], decode[qBlue(line[x])]};
            KoMixing::linearToOklab(rgb, sample.lab);
            sample.weight = qAlpha(line[x]) / 255.0f;
            samples.push_back(sample);
        }
    }
    QVector<QColor> colors;
    if (samples.empty()) {
        for (int i = 0; i < count; i++) colors << QColor::fromRgbF(qreal(i) / std::max(1, count - 1), qreal(i) / std::max(1, count - 1), qreal(i) / std::max(1, count - 1));
        return colors;
    }
    auto distance = [](const float *a, const float *b) {
        return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]);
    };
    // k-means++: each new center drawn with odds by how far a sample is from those so far, the same every time
    QRandomGenerator random(1);
    std::vector<std::array<float, 3>> centers;
    std::vector<float> nearestDistance(samples.size(), HUGE_VALF);
    const size_t first = random.bounded(quint32(samples.size()));
    centers.push_back({samples[first].lab[0], samples[first].lab[1], samples[first].lab[2]});
    while (int(centers.size()) < count) {
        double total = 0;
        for (size_t i = 0; i < samples.size(); i++) {
            nearestDistance[i] = std::min(nearestDistance[i], distance(samples[i].lab, centers.back().data()));
            total += nearestDistance[i] * samples[i].weight;
        }
        if (total <= 0) break;
        double pick = random.generateDouble() * total;
        size_t chosen = 0;
        for (; chosen + 1 < samples.size(); chosen++) {
            pick -= nearestDistance[chosen] * samples[chosen].weight;
            if (pick <= 0) break;
        }
        centers.push_back({samples[chosen].lab[0], samples[chosen].lab[1], samples[chosen].lab[2]});
    }
    std::vector<int> owner(samples.size(), 0);
    // the centers by lightness: a sample looks outward from its own, until lightness alone is farther than the
    // nearest found; of centers as near, the first one is its owner
    const int k = int(centers.size());
    std::vector<int> byLightness(size_t(k), 0);
    std::vector<float> lightness(size_t(k), 0.0f);
    for (int round = 0; round < 12; round++) {
        bool moved = false;
        std::iota(byLightness.begin(), byLightness.end(), 0);
        std::stable_sort(byLightness.begin(), byLightness.end(), [&centers](int a, int b) { return centers[a][0] < centers[b][0]; });
        for (int j = 0; j < k; j++) lightness[size_t(j)] = centers[size_t(byLightness[size_t(j)])][0];
        for (size_t i = 0; i < samples.size(); i++) {
            const float l = samples[i].lab[0];
            int up = int(std::lower_bound(lightness.begin(), lightness.end(), l) - lightness.begin()), down = up - 1;
            int best = 0;
            float bestDistance = HUGE_VALF;
            while (up < k || down >= 0) {
                const float upGap = up < k ? lightness[size_t(up)] - l : HUGE_VALF;
                const float downGap = down >= 0 ? l - lightness[size_t(down)] : HUGE_VALF;
                const float gap = std::min(upGap, downGap);
                if (gap * gap > bestDistance) break;
                const int c = upGap <= downGap ? byLightness[size_t(up++)] : byLightness[size_t(down--)];
                const float d = distance(samples[i].lab, centers[size_t(c)].data());
                if (d < bestDistance || (d == bestDistance && c < best)) {
                    bestDistance = d;
                    best = c;
                }
            }
            moved |= owner[i] != best;
            owner[i] = best;
        }
        if (!moved && round > 0) break;
        std::vector<std::array<double, 4>> sums(centers.size(), {0, 0, 0, 0});
        for (size_t i = 0; i < samples.size(); i++) {
            for (int k = 0; k < 3; k++) sums[owner[i]][k] += samples[i].lab[k] * samples[i].weight;
            sums[owner[i]][3] += samples[i].weight;
        }
        for (size_t c = 0; c < centers.size(); c++) {
            if (sums[c][3] <= 0) continue;
            for (int k = 0; k < 3; k++) centers[c][k] = float(sums[c][k] / sums[c][3]);
        }
    }
    std::sort(centers.begin(), centers.end(), [](const std::array<float, 3> &a, const std::array<float, 3> &b) { return a[0] < b[0]; });
    for (const auto &center : centers) {
        const QColor color = colorOfLab(center.data());
        if (!colors.contains(color)) colors << color;
    }
    return colors;
}

QImage applied(const QImage &image, const QPoint &corner, const Settings &settings, qreal scale)
{
    const QImage picture = image.convertToFormat(QImage::Format_ARGB32);
    const int w = picture.width(), h = picture.height();
    const float *decode = KoMixing::srgbToLinearTable();
    const quint8 *encode = KoMixing::linearToSrgbTable();
    std::vector<float> pixels(size_t(w) * h * 4);
    for (int y = 0; y < h; y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(picture.constScanLine(y));
        for (int x = 0; x < w; x++) {
            float *pixel = pixels.data() + (size_t(y) * w + x) * 4;
            pixel[0] = decode[qRed(line[x])];
            pixel[1] = decode[qGreen(line[x])];
            pixel[2] = decode[qBlue(line[x])];
            pixel[3] = qAlpha(line[x]) / 255.0f;
        }
    }
    const Quantizer quantizer(settings);
    const int cell = std::max(1, int(std::lround(settings.pixel * scale)));
    if (cell > 1) {
        // the cells' colors, with fewer colors as one pixel each, then spread over their cells
        const QRect rect(corner, picture.size());
        const QRect cells = cellsOver(rect, cell);
        std::vector<float> colors = cellColors(pixels.data(), rect, rect, cell, cells);
        const int cw = cells.width(), ch = cells.height();
        if (settings.dither == Dither::Diffuse) {
            Diffuser diffuser(settings, quantizer, cw);
            std::vector<quint16> chosen(cw);
            for (int y = 0; y < ch; y++) {
                float *row = colors.data() + size_t(y) * cw * 4;
                diffuser.row(row, chosen.data());
                for (int x = 0; x < cw; x++) {
                    if (chosen[x] != Diffuser::none) std::copy_n(quantizer.colorOf(chosen[x]), 3, row + size_t(x) * 4);
                }
            }
        } else {
            apply(settings, quantizer, colors.data(), cells);
        }
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const QPoint at = cellsOver(QRect(corner + QPoint(x, y), QSize(1, 1)), cell).topLeft() - cells.topLeft();
                std::copy_n(colors.data() + (size_t(at.y()) * cw + at.x()) * 4, 4, pixels.data() + (size_t(y) * w + x) * 4);
            }
        }
    } else if (settings.dither == Dither::Diffuse) {
        Diffuser diffuser(settings, quantizer, w);
        std::vector<quint16> chosen(w);
        for (int y = 0; y < h; y++) {
            float *row = pixels.data() + size_t(y) * w * 4;
            diffuser.row(row, chosen.data());
            for (int x = 0; x < w; x++) {
                if (chosen[x] != Diffuser::none) std::copy_n(quantizer.colorOf(chosen[x]), 3, row + size_t(x) * 4);
            }
        }
    } else {
        apply(settings, quantizer, pixels.data(), QRect(corner, picture.size()));
    }
    QImage result(w, h, QImage::Format_ARGB32);
    auto encoded8 = [encode](float c) { return int(encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]); };
    for (int y = 0; y < h; y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < w; x++) {
            const float *pixel = pixels.data() + (size_t(y) * w + x) * 4;
            line[x] = qRgba(encoded8(pixel[0]), encoded8(pixel[1]), encoded8(pixel[2]), qRound(std::clamp(pixel[3], 0.0f, 1.0f) * 255));
        }
    }
    return result;
}

} // namespace SkissaFewer
