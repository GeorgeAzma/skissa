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

/// A point spreads on the coarsest grid where its aperture is still this many cells across
constexpr float finestAperture = 16;
/// Apertures are made at sizes this many steps a cell apart, and each point spreads by the nearest
constexpr int sizeSteps = 8;

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
    , m_bounds(bounds)
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
    // how far the points that may reach rect spread at most (the spread is least along a line, so most at a corner)
    float most = 0;
    for (const QPoint &corner : {sourceRect.topLeft(), sourceRect.topRight(), sourceRect.bottomLeft(), sourceRect.bottomRight()}) {
        most = std::max(most, spreadAt(corner.x() + 0.5f, corner.y() + 0.5f));
    }
    if (most < 0.5f) {
        copy();
        return;
    }

    /*
     * Each point spreads its light into the aperture at its own size, as light out of focus does; a pixel shows the
     * light that reaches it over how much light of any color could (the weights reaching it), so the picture stays as
     * bright as it was however the sizes change, and its edges, where nothing past the image spreads, stay whole.
     * Points spread on grids fixed to the image's corner, each on the coarsest one where its aperture is still
     * finestAperture cells across, a few writes a row of it (where its spans start and end, run through each row).
     */
    struct Level {
        QRect cells; // the cells that hold what reaches rect, one more each way for reading between them
        std::vector<double> sums[5]; // premultiplied light, alpha, weight
        bool used {false};
    };
    auto levelOf = [](float r) { return r < finestAperture ? 0 : int(std::floor(std::log2(r / (finestAperture / 2)))); };
    auto down = [](int v, int level) { return v >= 0 ? v >> level : -((-v + (1 << level) - 1) >> level); };
    const int levels = levelOf(most) + 1;
    std::vector<Level> grids(levels);
    for (int level = 0; level < levels; level++) {
        Level &grid = grids[level];
        grid.cells = QRect(QPoint(down(rect.left(), level) - 1, down(rect.top(), level) - 1), QPoint(down(rect.right(), level) + 1, down(rect.bottom(), level) + 1));
        for (auto &plane : grid.sums) plane.assign(size_t(grid.cells.height()) * (grid.cells.width() + 1), 0.0);
    }

    // the points, summed into the cells of their own grid: light, alpha, how many, their sizes
    struct Cell {
        float value[5] {0, 0, 0, 0, 0};
        float size {0};
    };
    std::vector<std::vector<Cell>> cells(levels);
    std::vector<QRect> cellRects(levels);
    for (int level = 0; level < levels; level++) {
        cellRects[level] = QRect(QPoint(down(sourceRect.left(), level), down(sourceRect.top(), level)),
                                 QPoint(down(sourceRect.right(), level), down(sourceRect.bottom(), level)));
        cells[level].resize(size_t(cellRects[level].width()) * cellRects[level].height());
    }
    const QRect frame = m_bounds.isEmpty() ? sourceRect : m_bounds;
    for (int y = 0; y < h; y++) {
        const int py = sourceRect.top() + y;
        if (py < frame.top() || py > frame.bottom()) continue;
        for (int x = 0; x < w; x++) {
            const int px = sourceRect.left() + x;
            if (px < frame.left() || px > frame.right()) continue;
            const float *pixel = source + (size_t(y) * w + x) * 4;
            const float r = std::max(0.5f, spreadAt(px + 0.5f, py + 0.5f));
            const int level = std::min(levels - 1, levelOf(r));
            const QRect &from = cellRects[level];
            Cell &cell = cells[level][size_t(down(py, level) - from.top()) * from.width() + down(px, level) - from.left()];
            // the picture as light: its whites as the brighter light they stand for
            const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
            const float brighter = m_clipped ? SkissaPixels::unclipOf(0.2126f * pixel[0] + 0.7152f * pixel[1] + 0.0722f * pixel[2]) : 1.0f;
            for (int c = 0; c < 3; c++) cell.value[c] += std::max(0.0f, pixel[c]) * alpha * brighter;
            cell.value[3] += alpha;
            cell.value[4] += 1;
            cell.size += r;
        }
    }

    std::vector<Spans> apertures;
    std::vector<char> made;
    for (int level = 0; level < levels; level++) {
        Level &grid = grids[level];
        const QRect &from = cellRects[level];
        const float cellSize = std::ldexp(1.0f, level);
        const int width = grid.cells.width();
        for (int cy = 0; cy < from.height(); cy++) {
            for (int cx = 0; cx < from.width(); cx++) {
                const Cell &cell = cells[level][size_t(cy) * from.width() + cx];
                if (cell.value[4] <= 0) continue;
                const float cellRadius = cell.size / cell.value[4] / cellSize;
                const int step = std::max(sizeSteps / 2, int(std::lround(cellRadius * sizeSteps)));
                const int x = from.left() + cx - grid.cells.left(), y = from.top() + cy - grid.cells.top();
                const int reach = int(std::ceil(float(step) / sizeSteps)) + 1;
                if (y + reach < 0 || y - reach >= grid.cells.height() || x + reach < 0 || x - reach >= width) continue;
                if (step >= int(apertures.size())) {
                    apertures.resize(step + 1);
                    made.resize(step + 1, 0);
                }
                if (!made[step]) {
                    apertures[step] = spansOf(float(step) / sizeSteps, s.blades, s.angle);
                    made[step] = 1;
                }
                const Spans &spans = apertures[step];
                grid.used = true;
                const double share = 1 / spans.total;
                for (int r = 0; r <= 2 * spans.reach; r++) {
                    const int row = y + r - spans.reach;
                    if (row < 0 || row >= grid.cells.height()) continue;
                    // a run of cells from first to last, each getting part of the point
                    auto add = [&](int first, int last, double part) {
                        first = std::max(first + x, 0);
                        last = std::min(last + x, width - 1);
                        if (first > last) return;
                        for (int c = 0; c < 5; c++) {
                            double *line = grid.sums[c].data() + size_t(row) * (width + 1);
                            const double v = cell.value[c] * part;
                            line[first] += v;
                            line[last + 1] -= v;
                        }
                    };
                    if (spans.first[r] <= spans.last[r]) add(spans.first[r], spans.last[r], share);
                    if (spans.leftShare[r] > 0) add(spans.left[r], spans.left[r], share * spans.leftShare[r]);
                    if (spans.rightShare[r] > 0) add(spans.right[r], spans.right[r], share * spans.rightShare[r]);
                }
            }
        }
        // the starts and ends run through each row
        if (grid.used) {
            for (auto &plane : grid.sums) {
                for (int row = 0; row < grid.cells.height(); row++) {
                    double *line = plane.data() + size_t(row) * (width + 1);
                    for (int i = 1; i < width; i++) line[i] += line[i - 1];
                }
            }
        }
    }

    for (int y = 0; y < rect.height(); y++) {
        for (int x = 0; x < rect.width(); x++) {
            const int px = rect.left() + x, py = rect.top() + y;
            double total[5] = {0, 0, 0, 0, 0};
            for (int level = 0; level < levels; level++) {
                const Level &grid = grids[level];
                if (!grid.used) continue;
                const int width = grid.cells.width();
                auto at = [&](int c, int gx, int gy) { return grid.sums[c][size_t(gy) * (width + 1) + gx]; };
                if (!level) {
                    for (int c = 0; c < 5; c++) total[c] += at(c, px - grid.cells.left(), py - grid.cells.top());
                    continue;
                }
                // between the cells' middles, each cell's light shared by the pixels it covers
                const float cellSize = std::ldexp(1.0f, level);
                const float gx = (px + 0.5f) / cellSize - 0.5f - grid.cells.left();
                const float gy = (py + 0.5f) / cellSize - 0.5f - grid.cells.top();
                const int x0 = int(std::floor(gx)), y0 = int(std::floor(gy));
                const double tx = gx - x0, ty = gy - y0, per = 1.0 / (double(cellSize) * cellSize);
                for (int c = 0; c < 5; c++) {
                    const double top = at(c, x0, y0) + (at(c, x0 + 1, y0) - at(c, x0, y0)) * tx;
                    const double bottom = at(c, x0, y0 + 1) + (at(c, x0 + 1, y0 + 1) - at(c, x0, y0 + 1)) * tx;
                    total[c] += (top + (bottom - top) * ty) * per;
                }
            }
            float *to = out + (size_t(y) * rect.width() + x) * 4;
            if (total[4] <= 1e-9) {
                std::copy_n(source + (size_t(inside.top() + y) * w + inside.left() + x) * 4, 4, to);
                continue;
            }
            const float alpha = std::clamp(float(total[3] / total[4]), 0.0f, 1.0f);
            for (int c = 0; c < 3; c++) to[c] = total[3] > 1e-9 ? std::max(0.0f, float(total[c] / total[3])) : 0.0f;
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
