/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaFocus.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <QtMath>

#include <kis_properties_configuration.h>

#include "KoCompositeOpInLinear.h"
#include "SkissaPixels.h"

namespace SkissaFocus
{

namespace
{

/// A blur is made on the coarsest copy of the picture where the aperture is still at least this many cells across
constexpr float finestAperture = 16;
/// A band's blur is a mix of this many blurs of even steps of size, each pixel between the two beside its own
constexpr int bandSteps = 6;

/// Four planes of premultiplied red, green, blue and alpha, \p w by \p h, the first at \p origin in cells of
/// 2^level pixels from the image's corner
struct Grid {
    int level {0};
    QPoint origin;
    int w {0}, h {0};
    std::vector<float> plane[4];
};

/// \p fine halved: each cell the average of the cells of it, fixed to the image's corner so patches agree
Grid halved(const Grid &fine)
{
    auto down = [](int v) { return v >= 0 ? v / 2 : -((-v + 1) / 2); };
    Grid grid;
    grid.level = fine.level + 1;
    grid.origin = QPoint(down(fine.origin.x()), down(fine.origin.y()));
    grid.w = down(fine.origin.x() + fine.w - 1) - grid.origin.x() + 1;
    grid.h = down(fine.origin.y() + fine.h - 1) - grid.origin.y() + 1;
    std::vector<float> counts(size_t(grid.w) * grid.h, 0.0f);
    for (auto &plane : grid.plane) plane.assign(counts.size(), 0.0f);
    for (int y = 0; y < fine.h; y++) {
        const int gy = down(fine.origin.y() + y) - grid.origin.y();
        for (int x = 0; x < fine.w; x++) {
            const size_t to = size_t(gy) * grid.w + (down(fine.origin.x() + x) - grid.origin.x());
            const size_t from = size_t(y) * fine.w + x;
            for (int c = 0; c < 4; c++) grid.plane[c][to] += fine.plane[c][from];
            counts[to] += 1;
        }
    }
    for (size_t i = 0; i < counts.size(); i++) {
        for (auto &plane : grid.plane) plane[i] /= std::max(1.0f, counts[i]);
    }
    return grid;
}

/**
 * The aperture at \p radius cells, as the cells it covers along each row from -reach to reach: a whole run of them
 * and the cells partly covered at either end, each with the share of it covered, so its edge is smooth at any size
 */
struct Spans {
    int reach {0};
    std::vector<int> first, last, left, right;
    std::vector<float> leftShare, rightShare;
    double total {0};
};

Spans spansOf(float radius, int blades, float degrees)
{
    Spans spans;
    spans.reach = int(std::ceil(radius));
    // the polygon's corners, a blade's edge between each two
    std::vector<QPointF> corners;
    if (blades >= 3) {
        for (int k = 0; k < blades; k++) {
            const double a = qDegreesToRadians(double(degrees)) + 2 * M_PI * k / blades - M_PI / 2;
            corners.push_back(QPointF(radius * std::cos(a), radius * std::sin(a)));
        }
    }
    for (int dy = -spans.reach; dy <= spans.reach; dy++) {
        float low = 1, high = -1;
        if (corners.empty()) {
            if (std::abs(dy) <= radius) {
                const float half = std::sqrt(std::max(0.0f, radius * radius - float(dy * dy)));
                low = -half;
                high = half;
            }
        } else {
            for (size_t k = 0; k < corners.size(); k++) {
                const QPointF a = corners[k], b = corners[(k + 1) % corners.size()];
                if ((dy < std::min(a.y(), b.y())) || (dy > std::max(a.y(), b.y())) || std::abs(b.y() - a.y()) < 1e-9) continue;
                const float x = float(a.x() + (b.x() - a.x()) * (dy - a.y()) / (b.y() - a.y()));
                if (low > high) {
                    low = high = x;
                } else {
                    low = std::min(low, x);
                    high = std::max(high, x);
                }
            }
        }
        int first = 1, last = 0, left = 0, right = 0;
        float leftShare = 0, rightShare = 0;
        if (low <= high) {
            // whole cells: those whose span (k - 0.5 to k + 0.5) lies within it
            first = int(std::ceil(low + 0.5f));
            last = int(std::floor(high - 0.5f));
            if (first <= last) {
                left = first - 1;
                leftShare = std::clamp(float(first) - 0.5f - low, 0.0f, 1.0f);
                right = last + 1;
                rightShare = std::clamp(high - (float(last) + 0.5f), 0.0f, 1.0f);
            } else {
                // within one cell, or across the line between two
                left = int(std::floor(low + 0.5f));
                right = int(std::floor(high + 0.5f));
                if (left == right) {
                    leftShare = high - low;
                } else {
                    leftShare = float(left) + 0.5f - low;
                    rightShare = high - (float(right) - 0.5f);
                }
            }
        }
        spans.first.push_back(first);
        spans.last.push_back(last);
        spans.left.push_back(left);
        spans.right.push_back(right);
        spans.leftShare.push_back(leftShare);
        spans.rightShare.push_back(rightShare);
        spans.total += std::max(0, last - first + 1) + leftShare + rightShare;
    }
    return spans;
}

/**
 * \p grid blurred by \p spans over \p area (in the grid's cells), each row read from running sums of it, so a cell
 * costs a few reads a row of the aperture however wide it is; past the grid its edge repeats
 */
void blurOf(const Grid &grid, const Spans &spans, const QRect &area, std::vector<float> out[4])
{
    const int pad = spans.reach + 2;
    const int width = grid.w + 2 * pad;
    const int top = std::max(0, area.top() - spans.reach), bottom = std::min(grid.h - 1, area.bottom() + spans.reach);
    // running sums of each row, its edge repeated past it; doubles, or the darks drown in a bright row's sum
    std::vector<double> sums(size_t(bottom - top + 1) * (width + 1));
    for (int c = 0; c < 4; c++) {
        out[c].assign(size_t(area.width()) * area.height(), 0.0f);
        const std::vector<float> &plane = grid.plane[c];
        for (int y = top; y <= bottom; y++) {
            double *sum = sums.data() + size_t(y - top) * (width + 1);
            const float *row = plane.data() + size_t(y) * grid.w;
            sum[0] = 0;
            for (int i = 0; i < width; i++) sum[i + 1] = sum[i] + row[std::clamp(i - pad, 0, grid.w - 1)];
        }
        auto valueAt = [&](const double *sum, int i) { return float(sum[i + 1] - sum[i]); };
        for (int y = 0; y < area.height(); y++) {
            const int gy = area.top() + y;
            float *to = out[c].data() + size_t(y) * area.width();
            for (int x = 0; x < area.width(); x++) {
                const int at = area.left() + x + pad;
                double total = 0;
                for (int r = 0; r <= 2 * spans.reach; r++) {
                    const int row = std::clamp(gy + r - spans.reach, top, bottom);
                    const double *sum = sums.data() + size_t(row - top) * (width + 1);
                    if (spans.first[r] <= spans.last[r]) total += sum[at + spans.last[r] + 1] - sum[at + spans.first[r]];
                    if (spans.leftShare[r] > 0) total += spans.leftShare[r] * valueAt(sum, at + spans.left[r]);
                    if (spans.rightShare[r] > 0) total += spans.rightShare[r] * valueAt(sum, at + spans.right[r]);
                }
                to[x] = float(total / spans.total);
            }
        }
    }
}

/**
 * \p levels' picture blurred by the aperture at \p radius pixels over \p rect (in pixels at this scale): on the
 * coarsest copy where the aperture is still finestAperture cells across, then enlarged between the cells' middles
 */
void blurredAt(std::vector<Grid> &levels, float radius, int blades, float degrees, const QRect &rect, std::vector<float> out[4])
{
    int level = 0;
    while (radius / std::ldexp(1.0f, level + 1) >= finestAperture / 2 && levels[level].w > 4 && levels[level].h > 4) {
        level++;
        if (level >= int(levels.size())) levels.push_back(halved(levels.back()));
    }
    const Grid &grid = levels[level];
    const float cell = std::ldexp(1.0f, level);
    const Spans spans = spansOf(radius / cell, blades, degrees);
    if (!level) {
        blurOf(grid, spans, rect.translated(-grid.origin), out);
        return;
    }
    // the cells around the rect, one more each way for reading between them
    auto down = [level](int v) { return v >= 0 ? v >> level : -((-v + (1 << level) - 1) >> level); };
    const QRect cells = QRect(QPoint(down(rect.left()) - 1, down(rect.top()) - 1), QPoint(down(rect.right()) + 1, down(rect.bottom()) + 1))
                            .translated(-grid.origin)
        & QRect(0, 0, grid.w, grid.h);
    std::vector<float> coarse[4];
    blurOf(grid, spans, cells, coarse);
    for (int c = 0; c < 4; c++) {
        out[c].resize(size_t(rect.width()) * rect.height());
        for (int y = 0; y < rect.height(); y++) {
            const float gy = std::clamp((rect.top() + y + 0.5f) / cell - 0.5f - grid.origin.y() - cells.top(), 0.0f, float(cells.height() - 1));
            const int y0 = std::min(int(gy), cells.height() - 1), y1 = std::min(y0 + 1, cells.height() - 1);
            const float ty = gy - y0;
            for (int x = 0; x < rect.width(); x++) {
                const float gx = std::clamp((rect.left() + x + 0.5f) / cell - 0.5f - grid.origin.x() - cells.left(), 0.0f, float(cells.width() - 1));
                const int x0 = std::min(int(gx), cells.width() - 1), x1 = std::min(x0 + 1, cells.width() - 1);
                const float tx = gx - x0;
                const float *p = coarse[c].data();
                const float a = p[size_t(y0) * cells.width() + x0] + (p[size_t(y0) * cells.width() + x1] - p[size_t(y0) * cells.width() + x0]) * tx;
                const float b = p[size_t(y1) * cells.width() + x0] + (p[size_t(y1) * cells.width() + x1] - p[size_t(y1) * cells.width() + x0]) * tx;
                out[c][size_t(y) * rect.width() + x] = a + (b - a) * ty;
            }
        }
    }
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings s;
    s.size = float(std::clamp(config.getDouble("size", s.size), 0.0, 1000.0));
    const int blades = config.getInt("blades", s.blades);
    s.blades = blades < 3 ? 0 : std::min(blades, 16);
    s.angle = float(std::clamp(config.getDouble("angle", s.angle), -180.0, 180.0));
    s.band = config.getBool("band", s.band);
    s.center = QPointF(config.getDouble("x", 0.5), config.getDouble("y", 0.5));
    s.bandAngle = float(std::clamp(config.getDouble("band_angle", s.bandAngle), -180.0, 180.0));
    s.bandWidth = float(std::clamp(config.getDouble("band_width", s.bandWidth), 0.0, 2.0));
    return s;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("size", double(size));
    config.setProperty("blades", blades);
    config.setProperty("angle", double(angle));
    config.setProperty("band", band);
    config.setProperty("x", center.x());
    config.setProperty("y", center.y());
    config.setProperty("band_angle", double(bandAngle));
    config.setProperty("band_width", double(bandWidth));
}

bool Settings::sameLook(const Settings &other) const
{
    return size == other.size && blades == other.blades && angle == other.angle && band == other.band && bandAngle == other.bandAngle
        && bandWidth == other.bandWidth;
}

bool Settings::operator==(const Settings &other) const
{
    return sameLook(other) && center == other.center;
}

bool Settings::changesNothing() const
{
    return size < 0.5f;
}

Focus::Focus(const Settings &settings, const QRect &bounds, qreal scale, bool clipped)
    : m_settings(settings)
    , m_scale(scale)
    , m_clipped(clipped)
{
    const QRectF frame = bounds.isEmpty() ? QRectF(0, 0, 1, 1) : QRectF(bounds);
    m_middle = frame.topLeft() + QPointF(settings.center.x() * frame.width(), settings.center.y() * frame.height());
    const double a = qDegreesToRadians(double(settings.bandAngle));
    m_across = QPointF(-std::sin(a), std::cos(a));
    m_half = float(settings.bandWidth * std::hypot(frame.width(), frame.height()) / 4);
    float farthest = 0;
    for (const QPointF &corner : {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight()}) {
        const QPointF d = corner - m_middle;
        farthest = std::max(farthest, float(std::abs(d.x() * m_across.x() + d.y() * m_across.y())));
    }
    m_farthest = farthest;
}

int Focus::reach() const
{
    if (m_settings.changesNothing()) return 0;
    const float radius = float(m_settings.size * m_scale);
    // the blur's own reach, and the cells of the copy it is made on
    const int cell = 1 << std::max(0, int(std::ceil(std::log2(std::max(1.0f, radius / (finestAperture / 2))))));
    return int(std::ceil(radius)) + 2 * cell + 2;
}

float Focus::spreadAt(float x, float y) const
{
    const float radius = float(m_settings.size * m_scale);
    if (!m_settings.band) return radius;
    const float off = std::abs((x - float(m_middle.x())) * float(m_across.x()) + (y - float(m_middle.y())) * float(m_across.y()));
    return radius * std::clamp((off - m_half) / std::max(1.0f, m_farthest - m_half), 0.0f, 1.0f);
}

void Focus::apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const
{
    const Settings &s = m_settings;
    const int w = sourceRect.width(), h = sourceRect.height();
    const QRect inside = rect.translated(-sourceRect.topLeft());
    auto copy = [&]() {
        for (int y = 0; y < rect.height(); y++) {
            std::copy_n(source + (size_t(inside.top() + y) * w + inside.left()) * 4, rect.width() * 4, out + size_t(y) * rect.width() * 4);
        }
    };
    if (s.changesNothing() || w <= 0 || h <= 0) {
        copy();
        return;
    }
    // how far each pixel spreads, and so which blurs it needs
    std::vector<float> spread(size_t(rect.width()) * rect.height());
    float least = 1e9f, most = 0;
    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const float r = spreadAt(rect.left() + x + 0.5f, rect.top() + y + 0.5f);
            spread[size_t(y) * rect.width() + x] = r;
            least = std::min(least, r);
            most = std::max(most, r);
        }
    }
    if (most < 0.5f) {
        copy();
        return;
    }

    // the picture as light, premultiplied: its whites as the brighter light they stand for
    // room for every coarser copy, so the full one stays where it is
    std::vector<Grid> levels(1);
    levels.reserve(32);
    Grid &full = levels[0];
    full.origin = sourceRect.topLeft();
    full.w = w;
    full.h = h;
    for (auto &plane : full.plane) plane.resize(size_t(w) * h);
    for (size_t i = 0; i < size_t(w) * h; i++) {
        const float *pixel = source + i * 4;
        const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
        const float brighter = m_clipped ? SkissaPixels::unclipOf(0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2]) : 1.0f;
        for (int c = 0; c < 3; c++) full.plane[c][i] = std::max(0.0f, pixel[c]) * alpha * brighter;
        full.plane[3][i] = alpha;
    }

    // the blurs of even steps of size that the rect's spreads fall between (only the full one when even)
    const float radius = float(s.size * m_scale);
    const int steps = s.band ? bandSteps : 1;
    const int lowest = s.band ? std::max(0, int(std::floor(least / radius * steps))) : 1;
    const int highest = s.band ? std::min(steps, int(std::floor(most / radius * steps)) + 1) : 1;
    std::vector<std::vector<float>> blurs[bandSteps + 1];
    for (int k = std::max(1, lowest); k <= highest; k++) {
        blurs[k].resize(4);
        std::vector<float> made[4];
        blurredAt(levels, radius * k / steps, s.blades, s.angle, rect, made);
        for (int c = 0; c < 4; c++) blurs[k][c] = std::move(made[c]);
    }

    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const size_t i = size_t(y) * rect.width() + x;
            const float at = std::clamp(spread[i] / radius * steps, 0.0f, float(steps));
            const int k = std::min(int(at), steps - 1);
            const float t = s.band ? at - k : 1.0f;
            float mixed[4];
            for (int c = 0; c < 4; c++) {
                const float sharp = k == 0 ? full.plane[c][size_t(inside.top() + y) * w + inside.left() + x] : blurs[k][c][i];
                const float blurred = blurs[k + 1][c][i];
                mixed[c] = sharp + (blurred - sharp) * t;
            }
            float *to = out + i * 4;
            const float alpha = std::clamp(mixed[3], 0.0f, 1.0f);
            for (int c = 0; c < 3; c++) to[c] = alpha > 1e-6f ? mixed[c] / alpha : 0.0f;
            // back from light to the picture: what wasn't spread is exactly as it was
            if (m_clipped) {
                const float dimmed = SkissaPixels::reclipOf(0.2126f * to[0] + 0.7152f * to[1] + 0.0722f * to[2]);
                for (int c = 0; c < 3; c++) to[c] *= dimmed;
            }
            to[3] = alpha;
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
    Focus(settings, rect, scale).apply(in.data(), rect, rect, out.data());
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

} // namespace SkissaFocus
