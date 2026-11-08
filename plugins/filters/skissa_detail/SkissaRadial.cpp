/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaRadial.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include <QtMath>

#include <kis_properties_configuration.h>

#include "KoCompositeOpInLinear.h"

namespace SkissaRadial
{

namespace
{

constexpr double tau = 2 * M_PI;
/// Streaks this far apart side by side (at the farthest) blend without lines between them
constexpr double lineSpacing = 0.8;
/// Samples this far apart along a streak, in pixels
constexpr double sampleStep = 1.0;
/// Pixels this close to the middle have no streak
constexpr double leastRadius = 0.5;
constexpr double mostZoom = 0.98;
/// The light the bright parts give, at 100%: an average along a streak is faint next to the light it came from
constexpr float lightGain = 4.0f;
/// What a streak sums: the premultiplied picture (4), its light (3) and the weight
constexpr int sums = 8;
/// Each times 1, s and s²: weights along a streak are squares of lines, so streaks fade out smoothly
constexpr int powers = 3;
constexpr int stored = sums * powers;

inline float luminance(const float *rgb)
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

/// The picture at \p x, \p y (in the source's pixels, centres at halves) premultiplied, the edge repeating beyond it
inline void readAt(const float *source, int w, int h, double x, double y, float *to)
{
    x = std::clamp(x - 0.5, 0.0, double(w - 1));
    y = std::clamp(y - 0.5, 0.0, double(h - 1));
    const int ix = int(x), iy = int(y);
    const int jx = std::min(ix + 1, w - 1), jy = std::min(iy + 1, h - 1);
    const float tx = float(x - ix), ty = float(y - iy);
    const float weights[4] = {(1 - tx) * (1 - ty), tx * (1 - ty), (1 - tx) * ty, tx * ty};
    const float *pixels[4] = {source + (size_t(iy) * w + ix) * 4, source + (size_t(iy) * w + jx) * 4, source + (size_t(jy) * w + ix) * 4,
                              source + (size_t(jy) * w + jx) * 4};
    to[0] = to[1] = to[2] = to[3] = 0;
    for (int k = 0; k < 4; k++) {
        const float alpha = std::clamp(pixels[k][3], 0.0f, 1.0f) * weights[k];
        for (int c = 0; c < 3; c++) to[c] += pixels[k][c] * alpha;
        to[3] += alpha;
    }
}

/// The light of a premultiplied color: what of it is brighter than \p threshold
inline void lightOf(const float *premultiplied, float threshold, float *to)
{
    const float alpha = premultiplied[3];
    const float brightness = alpha > 1e-6f ? luminance(premultiplied) / alpha : 0.0f;
    const float share = std::max(0.0f, brightness - threshold) / (1 - threshold);
    for (int c = 0; c < 3; c++) to[c] = std::max(0.0f, premultiplied[c]) * share;
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings settings;
    settings.center = QPointF(std::clamp(config.getDouble("x", 0.5), -1.0, 2.0), std::clamp(config.getDouble("y", 0.5), -1.0, 2.0));
    settings.zoom = float(std::clamp(config.getDouble("zoom", 0), 0.0, mostZoom));
    settings.spin = float(std::clamp(config.getDouble("spin", 0), -180.0, 180.0));
    settings.blur = float(std::clamp(config.getDouble("blur", 1), 0.0, 1.0));
    settings.light = float(std::clamp(config.getDouble("light", 0), 0.0, 4.0));
    settings.threshold = float(std::clamp(config.getDouble("threshold", 0.6), 0.0, 0.99));
    return settings;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("x", center.x());
    config.setProperty("y", center.y());
    config.setProperty("zoom", double(zoom));
    config.setProperty("spin", double(spin));
    config.setProperty("blur", double(blur));
    config.setProperty("light", double(light));
    config.setProperty("threshold", double(threshold));
}

bool Settings::sameLook(const Settings &other) const
{
    return zoom == other.zoom && spin == other.spin && blur == other.blur && light == other.light
        && (light == 0 || threshold == other.threshold);
}

bool Settings::operator==(const Settings &other) const
{
    return center == other.center && sameLook(other) && threshold == other.threshold;
}

bool Settings::changesNothing() const
{
    return (zoom == 0 && spin == 0) || (blur == 0 && light == 0);
}

Settings zoomed()
{
    Settings settings;
    settings.zoom = 0.3f;
    return settings;
}

Radial::Radial(const Settings &settings, const QRect &bounds)
    : m_settings(settings)
{
    const QRectF frame = bounds.isEmpty() ? QRectF(0, 0, 1, 1) : QRectF(bounds);
    m_center = frame.topLeft() + QPointF(settings.center.x() * frame.width(), settings.center.y() * frame.height());
    m_logScale = std::log(1.0 - std::clamp(double(settings.zoom), 0.0, mostZoom));
    m_turn = qDegreesToRadians(double(settings.spin));
    // a turn alone streaks both ways, so shapes stay where they are; a zoom streaks toward the middle, outward from
    // the shapes, as a zoom burst and light rays do; both at once, between
    const double turn = std::abs(m_turn), zoom = std::abs(m_logScale);
    m_back = turn + zoom > 0 ? 0.5 * turn / (turn + zoom) : 0;
    double farthest = 1;
    for (const QPointF &corner : {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight()}) {
        farthest = std::max(farthest, std::hypot(corner.x() - m_center.x(), corner.y() - m_center.y()));
    }
    m_farthest = farthest * std::exp(-m_back * m_logScale);
}

QRect Radial::sourceOf(const QRect &rect) const
{
    if (m_settings.changesNothing() || rect.isEmpty()) return rect;
    const QPointF corners[4] = {rect.topLeft(), QPointF(rect.right() + 1, rect.top()), QPointF(rect.left(), rect.bottom() + 1),
                                QPointF(rect.right() + 1, rect.bottom() + 1)};
    qreal left = rect.left(), top = rect.top(), right = rect.right() + 1, bottom = rect.bottom() + 1;
    double reach = 0;
    // the streaks of a rectangle sweep copies of it, zoomed and turned about the middle
    constexpr int steps = 64;
    for (int i = 0; i <= steps; i++) {
        const double along = -m_back + double(i) / steps;
        const double scale = std::exp(along * m_logScale), angle = along * m_turn;
        const double cosine = scale * std::cos(angle), sine = scale * std::sin(angle);
        for (const QPointF &corner : corners) {
            const QPointF v = corner - m_center;
            const QPointF at = m_center + QPointF(cosine * v.x() - sine * v.y(), sine * v.x() + cosine * v.y());
            left = std::min(left, at.x());
            right = std::max(right, at.x());
            top = std::min(top, at.y());
            bottom = std::max(bottom, at.y());
            reach = std::max(reach, std::hypot(v.x(), v.y()) * std::max(1.0, scale));
        }
    }
    // corners turn along arcs, which bulge past the steps
    const int margin = 3 + int(std::ceil(reach * (1 - std::cos(std::abs(m_turn) / steps / 2))));
    return QRect(QPoint(int(std::floor(left)) - margin, int(std::floor(top)) - margin),
                 QPoint(int(std::ceil(right)) + margin, int(std::ceil(bottom)) + margin));
}

void Radial::apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const
{
    const int w = rect.width(), h = rect.height();
    const size_t count = size_t(w) * h;
    const int sw = sourceRect.width(), sh = sourceRect.height();
    const float blur = m_settings.blur;
    const float light = m_settings.light * lightGain;
    const float threshold = std::min(m_settings.threshold, 0.99f);

    auto original = [&](int x, int y) {
        return source + (size_t(rect.top() + y - sourceRect.top()) * sw + (rect.left() + x - sourceRect.left())) * 4;
    };
    // the pixel mixed with its streak's average (premultiplied, then its light), the light added
    auto finish = [&](int x, int y, const float *streak) {
        const float *pixel = original(x, y);
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        float mixed[4] = {pixel[0] * alpha, pixel[1] * alpha, pixel[2] * alpha, alpha};
        for (int c = 0; c < 4; c++) mixed[c] += blur * (streak[c] - mixed[c]);
        float rays[3];
        for (int c = 0; c < 3; c++) rays[c] = light * streak[4 + c];
        // light over clear pixels is as opaque as it is bright, so over black it adds up as light does
        const float shine = std::clamp(std::max({rays[0], rays[1], rays[2]}), 0.0f, 1.0f);
        const float covered = std::clamp(mixed[3], 0.0f, 1.0f);
        const float result = covered + (1 - covered) * shine;
        float *to = out + (size_t(y) * w + x) * 4;
        for (int c = 0; c < 3; c++) to[c] = result > 1e-6f ? (mixed[c] + rays[c]) / result : 0.0f;
        to[3] = result;
    };
    // a pixel without a streak: itself, and its own light
    auto alone = [&](int x, int y) {
        const float *pixel = original(x, y);
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        float streak[7] = {pixel[0] * alpha, pixel[1] * alpha, pixel[2] * alpha, alpha};
        lightOf(streak, threshold, streak + 4);
        finish(x, y, streak);
    };

    if (m_settings.changesNothing()) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) std::copy_n(original(x, y), 4, out + (size_t(y) * w + x) * 4);
        }
        return;
    }

    /*
     * In log-polar coordinates (u = ln r, θ) a streak is a straight line: a point σ along it is at u + σ L, θ + σ A.
     * So each pixel lies on one streak, named by λ = θ L - u A (the same all along it), at s = (u L + θ A) / D²
     * along it (σ itself). Streaks a fixed step of λ apart are summed once each, and every pixel reads the two either
     * side of it.
     */
    const double L = m_logScale, A = m_turn;
    const double D2 = L * L + A * A, D = std::sqrt(D2);
    const bool zooms = L != 0;
    const double back = m_back < 1e-4 ? 0.0 : m_back;

    std::vector<double> named(count), along(count);
    const QPointF middle = QRectF(rect).center() - m_center;
    const double middleAngle = std::atan2(middle.y(), middle.x());
    double farthestHere = 0;
    double least = std::numeric_limits<double>::infinity(), most = -std::numeric_limits<double>::infinity();
    for (int y = 0; y < h; y++) {
        const double dy = rect.top() + y + 0.5 - m_center.y();
        for (int x = 0; x < w; x++) {
            const double dx = rect.left() + x + 0.5 - m_center.x();
            const double r = std::hypot(dx, dy);
            const size_t i = size_t(y) * w + x;
            if (r < leastRadius) {
                named[i] = std::numeric_limits<double>::quiet_NaN();
                continue;
            }
            // angles kept continuous across this rectangle (unless it holds the middle)
            const double angle = middleAngle + std::remainder(std::atan2(dy, dx) - middleAngle, tau);
            const double u = std::log(r);
            named[i] = angle * L - u * A;
            along[i] = (u * L + angle * A) / D2;
            least = std::min(least, named[i]);
            most = std::max(most, named[i]);
            farthestHere = std::max(farthestHere, r);
        }
    }
    if (least > most) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) alone(x, y);
        }
        return;
    }

    /*
     * The streaks summed are the same in every patch, so patches meet without seams: a step of λ that puts them
     * lineSpacing apart at the farthest any streak reaches, a whole number of them around a turn (an angle and the
     * same angle a turn later name the same streak), doubled where this patch's own streaks reach less far.
     */
    double step;
    {
        const double finest = D * lineSpacing / m_farthest;
        const double here = farthestHere * std::exp(-back * L);
        const int coarser = std::max(0, int(std::floor(std::log2(m_farthest / std::max(here, 1.0)))));
        if (zooms) {
            const double around = tau * std::abs(L);
            // q 2^k of them, q from 8 to 15: halved up to k times, never more than an eighth too many
            const double needed = std::max(8.0, around / finest);
            const int doublings = std::min(24, int(std::floor(std::log2(needed / 8))));
            const long q = long(std::ceil(needed / std::ldexp(1.0, doublings)));
            step = around / double(q << (doublings - std::min(coarser, doublings)));
        } else {
            step = finest * std::ldexp(1.0, coarser);
        }
    }
    const long firstLine = long(std::floor(least / step));
    const long lastLine = long(std::floor(most / step)) + 1;
    const int lineCount = int(lastLine - firstLine + 1);

    // pixels by the streak below them
    std::vector<int> pixelsOf(lineCount + 1, 0);
    std::vector<float> between(count);
    std::vector<double> lowest(lineCount, std::numeric_limits<double>::infinity()), highest(lineCount, -std::numeric_limits<double>::infinity());
    std::vector<int> lineOf(count, -1);
    for (size_t i = 0; i < count; i++) {
        if (std::isnan(named[i])) continue;
        const double position = named[i] / step;
        const long line = std::clamp(long(std::floor(position)), firstLine, lastLine - 1);
        const int index = int(line - firstLine);
        lineOf[i] = index;
        between[i] = float(std::clamp(position - line, 0.0, 1.0));
        lowest[index] = std::min(lowest[index], along[i]);
        highest[index] = std::max(highest[index], along[i]);
        pixelsOf[index + 1]++;
    }
    for (int i = 0; i < lineCount; i++) pixelsOf[i + 1] += pixelsOf[i];
    std::vector<int> order(pixelsOf.back());
    {
        std::vector<int> next(pixelsOf.begin(), pixelsOf.end() - 1);
        for (size_t i = 0; i < count; i++) {
            if (lineOf[i] >= 0) order[next[lineOf[i]]++] = int(i);
        }
    }

    /*
     * A streak's running sums of the picture times the weight, and of that times s and s² (for weights that rise
     * and fall along it). Samples are about a pixel apart: along a zoom the radius halves every ln 2 / |L| of s, so each such
     * octave has its own even step, half as many samples as the one before; along a turn alone one step does. Every
     * sample's place follows from the one before by a complex multiplication.
     */
    struct Line {
        double ref {0};
        /// zoom: octaves first..last of s, each counts[k] samples, the sample at its start at bases[k] (it may lie
        /// before the line's first sample); turn alone: one run at s = (first + i) step
        long first {0};
        std::vector<int> counts;
        std::vector<long> bases;
        double step {1};
        int count {0};
        std::vector<double> sums;
    };
    const double octave = zooms ? M_LN2 / std::abs(L) : 0;
    auto positionOf = [&](const Line &line, double s) {
        double position;
        if (zooms) {
            const double o = s / octave;
            const long k = std::clamp(long(std::floor(o)), line.first, line.first + long(line.counts.size()) - 1);
            const size_t i = size_t(k - line.first);
            position = line.bases[i] + (o - k) * line.counts[i];
        } else {
            position = s / line.step - line.first;
        }
        return std::clamp(position, 0.0, double(line.count - 1));
    };
    const bool shines = light > 0;
    auto make = [&](Line &line, long index, double from, double to) {
        const double lambda = index * step;
        const double u0 = -A * lambda / D2, angle0 = L * lambda / D2;
        const double radius = std::exp(u0);
        line.ref = from;
        line.counts.clear();
        line.bases.clear();
        line.sums.clear();
        double before[stored] = {};
        double running[stored] = {};
        double lastS = 0;
        int written = 0;
        // samples from \p s on, \p steps of \p ds apart
        auto run = [&](double s, int steps, double ds) {
            const double scale = std::exp(u0 + L * s), angle = angle0 + A * s;
            double zx = scale * std::cos(angle), zy = scale * std::sin(angle);
            const double grow = std::exp(L * ds), turn = A * ds;
            const double wx = grow * std::cos(turn), wy = grow * std::sin(turn);
            for (int j = 0; j < steps; j++, s += ds) {
                float sample[sums];
                readAt(source, sw, sh, m_center.x() + zx - sourceRect.left(), m_center.y() + zy - sourceRect.top(), sample);
                if (shines) {
                    lightOf(sample, threshold, sample + 4);
                } else {
                    sample[4] = sample[5] = sample[6] = 0;
                }
                sample[7] = 1;
                // each sample stands for the length of streak around it: r ds
                const double weight = std::hypot(zx, zy);
                double here[stored];
                const double relative = s - line.ref;
                for (int c = 0; c < sums; c++) {
                    here[c] = sample[c] * weight;
                    here[sums + c] = here[c] * relative;
                    here[2 * sums + c] = here[sums + c] * relative;
                }
                if (written > 0) {
                    const double span = 0.5 * (s - lastS);
                    for (int c = 0; c < stored; c++) running[c] += span * (before[c] + here[c]);
                }
                for (int c = 0; c < stored; c++) {
                    line.sums.push_back(running[c]);
                    before[c] = here[c];
                }
                lastS = s;
                written++;
                const double nx = zx * wx - zy * wy;
                zy = zx * wy + zy * wx;
                zx = nx;
            }
        };
        if (zooms) {
            line.first = long(std::floor(from / octave));
            const long last = long(std::floor(to / octave));
            for (long k = line.first; k <= last; k++) {
                // an octave's length: from its start's radius to half that, |L| of s per unit of ln r
                const double arc = radius * std::exp(L * k * octave) * D / (2 * std::abs(L));
                const int n = std::clamp(int(std::ceil(arc / sampleStep)), 4, 1 << 16);
                const int startAt = k == line.first ? int(std::floor((from / octave - k) * n)) : 0;
                const int endAt = k == last ? int(std::ceil((to / octave - k) * n)) : n - 1;
                line.counts.push_back(n);
                line.bases.push_back(written - startAt);
                run((k + double(startAt) / n) * octave, endAt - startAt + 1, octave / n);
            }
        } else {
            line.step = (tau / std::abs(A)) / std::max(1.0, std::ceil(tau * radius / sampleStep));
            line.first = long(std::floor(from / line.step));
            const long last = std::max(line.first + 1, long(std::ceil(to / line.step)));
            run(line.first * line.step, int(std::min<long>(last - line.first + 1, 1 << 20)), line.step);
        }
        line.count = written;
    };
    // the running sums at s, between samples
    auto sumsAt = [&](const Line &line, double s, double *into) {
        const double position = positionOf(line, s);
        const int i = std::min(int(position), line.count - 2);
        const double t = position - i;
        const double *a = line.sums.data() + size_t(i) * stored, *b = a + stored;
        for (int c = 0; c < stored; c++) into[c] = a[c] + t * (b[c] - a[c]);
    };
    // the streak of a pixel at s on a line: fading from the pixel toward the middle, and to it from beyond; a weight
    // (p + q s')² over a stretch is p² ΣS0 + 2pq ΣS1 + q² ΣS2
    auto streak = [&](const Line &line, double s, double *result) {
        double start[stored], here[stored], end[stored];
        sumsAt(line, s, here);
        sumsAt(line, s + 1 - back, end);
        const double relative = s - line.ref;
        auto add = [&](const double *from, const double *to, double p, double q, bool first) {
            for (int c = 0; c < sums; c++) {
                const double value = p * p * (to[c] - from[c]) + 2 * p * q * (to[sums + c] - from[sums + c])
                    + q * q * (to[2 * sums + c] - from[2 * sums + c]);
                result[c] = first ? value : result[c] + value;
            }
        };
        const double fall = 1 / (1 - back);
        add(here, end, 1 + relative * fall, -fall, true);
        if (back > 0) {
            sumsAt(line, s - back, start);
            const double rise = 1 / back;
            add(start, here, 1 - relative * rise, rise, false);
        }
    };

    Line lines[2];
    for (int index = 0; index < lineCount; index++) {
        const bool below = index > 0 && pixelsOf[index] > pixelsOf[index - 1];
        const bool above = pixelsOf[index + 1] > pixelsOf[index];
        if (below || above) {
            double from = std::numeric_limits<double>::infinity(), to = -std::numeric_limits<double>::infinity();
            for (int k : {index - 1, index}) {
                if (k < 0 || k >= lineCount || pixelsOf[k + 1] == pixelsOf[k]) continue;
                from = std::min(from, lowest[k]);
                to = std::max(to, highest[k]);
            }
            make(lines[index % 2], firstLine + index, from - back, to + 1 - back);
        }
        if (!below) continue;
        const Line &low = lines[(index - 1) % 2], &high = lines[index % 2];
        for (int p = pixelsOf[index - 1]; p < pixelsOf[index]; p++) {
            const int i = order[p];
            double a[sums], b[sums];
            streak(low, along[i], a);
            streak(high, along[i], b);
            const double t = between[i];
            double total[sums];
            for (int c = 0; c < sums; c++) total[c] = a[c] + t * (b[c] - a[c]);
            float average[7];
            const double weight = total[7];
            for (int c = 0; c < 7; c++) average[c] = weight > 1e-12 ? float(total[c] / weight) : 0.0f;
            finish(i % w, i / w, average);
        }
    }
    for (size_t i = 0; i < count; i++) {
        if (lineOf[i] < 0) alone(int(i % w), int(i / w));
    }
}

QImage applied(const QImage &image, const Settings &settings)
{
    const QImage picture = image.convertToFormat(QImage::Format_ARGB32);
    const QRect rect = picture.rect();
    const Radial radial(settings, rect);
    const QRect source = radial.sourceOf(rect) & rect;
    const float *decode = KoMixing::srgbToLinearTable();
    const quint8 *encode = KoMixing::linearToSrgbTable();
    std::vector<float> in(size_t(source.width()) * source.height() * 4);
    for (int y = 0; y < source.height(); y++) {
        const QRgb *line = reinterpret_cast<const QRgb *>(picture.constScanLine(source.top() + y)) + source.left();
        for (int x = 0; x < source.width(); x++) {
            float *pixel = in.data() + (size_t(y) * source.width() + x) * 4;
            pixel[0] = decode[qRed(line[x])];
            pixel[1] = decode[qGreen(line[x])];
            pixel[2] = decode[qBlue(line[x])];
            pixel[3] = qAlpha(line[x]) / 255.0f;
        }
    }
    std::vector<float> out(size_t(rect.width()) * rect.height() * 4);
    radial.apply(in.data(), source, rect, out.data());
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

} // namespace SkissaRadial
