/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaLens.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <kis_properties_configuration.h>

#include "KoCompositeOpInLinear.h"
#include "SkissaDetail.h"

namespace SkissaLens
{

namespace
{

/**
 * Glass is described by Fraunhofer's lines: how much it bends yellow d (587.6 nm), and how much more it bends blue F
 * (486.1 nm) than red C (656.3 nm). Cauchy's law, n = A + B / λ², is close for clear glass, so a wavelength's
 * dispersion is its bend past d's in units of F's past C's: 0 at d, 1 more at F than at C.
 */
constexpr double lineD = 587.6;
constexpr double lineF = 486.1;
constexpr double lineC = 656.3;
const double perFC = 1 / (lineF * lineF) - 1 / (lineC * lineC);

double dispersionOf(double nm)
{
    return (1 / (nm * nm) - 1 / (lineD * lineD)) / perFC;
}

double wavelengthOf(double dispersion)
{
    return 1 / std::sqrt(dispersion * perFC + 1 / (lineD * lineD));
}

/// The light we see
constexpr double shortest = 400;
constexpr double longest = 700;

/// How strongly a wavelength stirs the eye, as CIE 1931 XYZ (Wyman, Sloan and Shirley's fit of the tables, 2013)
double lobe(double nm, double middle, double below, double above)
{
    const double t = (nm - middle) / (nm < middle ? below : above);
    return std::exp(-0.5 * t * t);
}

/// The linear sRGB of a wavelength; colors purer than sRGB's come out partly negative
std::array<double, 3> rgbOf(double nm)
{
    const double x = 1.056 * lobe(nm, 599.8, 37.9, 31.0) + 0.362 * lobe(nm, 442.0, 16.0, 26.7) - 0.065 * lobe(nm, 501.1, 20.4, 26.2);
    const double y = 0.821 * lobe(nm, 568.8, 46.9, 40.5) + 0.286 * lobe(nm, 530.9, 16.3, 31.1);
    const double z = 1.217 * lobe(nm, 437.0, 11.8, 36.0) + 0.681 * lobe(nm, 459.0, 26.0, 13.8);
    return {3.2406 * x - 1.5372 * y - 0.4986 * z, -0.9689 * x + 1.8758 * y + 0.0415 * z, 0.0557 * x - 0.2040 * y + 1.0570 * z};
}

/// Wavelengths this many pixels apart at the farthest corner blend into fringes without steps
constexpr qreal wavelengthStep = 1.0;
constexpr int fewestWavelengths = 8;
constexpr int mostWavelengths = 48;

/// With green in focus, how far red is out of it next to blue (their dispersions' distances from 550 nm's)
constexpr qreal haloRed = 0.43;
/// Blurs smaller than this are no blur
constexpr qreal leastBlur = 0.3;

inline int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

/// An image as premultiplied planes over \p cells, each cell 2^shift pixels across
struct Grid {
    int shift {0};
    QRect cells;
    std::vector<float> plane[4];
};

/// The finest glow reads this far around the work area, at full size
constexpr int nearRoom = 12;

/// \p source (straight red, green, blue and alpha over \p sourceRect) premultiplied, on \p cells of 2^shift
/// pixels, each the average of the pixels of it that were read
Grid premultiplied(const float *source, const QRect &sourceRect, const QRect &cells, int shift, int planes)
{
    Grid grid;
    grid.shift = shift;
    grid.cells = cells;
    const int w = cells.width(), h = cells.height(), size = 1 << shift;
    for (int p = 0; p < planes; p++) grid.plane[p].assign(size_t(w) * h, 0.0f);
    for (int j = 0; j < h; j++) {
        const int top = std::max((cells.top() + j) * size, sourceRect.top());
        const int bottom = std::min((cells.top() + j) * size + size - 1, sourceRect.bottom());
        for (int i = 0; i < w; i++) {
            const int left = std::max((cells.left() + i) * size, sourceRect.left());
            const int right = std::min((cells.left() + i) * size + size - 1, sourceRect.right());
            float sum[4] = {0, 0, 0, 0};
            for (int y = top; y <= bottom; y++) {
                const float *pixel = source + (size_t(y - sourceRect.top()) * sourceRect.width() + (left - sourceRect.left())) * 4;
                for (int x = left; x <= right; x++, pixel += 4) {
                    const float alpha = std::clamp(pixel[3], 0.0f, 1.0f);
                    for (int c = 0; c < 3; c++) sum[c] += pixel[c] * alpha;
                    sum[3] += alpha;
                }
            }
            const float share = 1.0f / ((right - left + 1) * (bottom - top + 1));
            for (int p = 0; p < planes; p++) grid.plane[p][size_t(j) * w + i] = sum[p] * share;
        }
    }
    return grid;
}

/// \p fine averaged two by two, on cells lined up with the image's, so neighbouring patches agree
Grid halved(const Grid &fine, int planes)
{
    Grid grid;
    grid.shift = fine.shift + 1;
    grid.cells = QRect(QPoint(floorDiv(fine.cells.left(), 2), floorDiv(fine.cells.top(), 2)),
                       QPoint(floorDiv(fine.cells.right(), 2), floorDiv(fine.cells.bottom(), 2)));
    const int w = grid.cells.width(), h = grid.cells.height(), fw = fine.cells.width();
    for (int p = 0; p < planes; p++) grid.plane[p].assign(size_t(w) * h, 0.0f);
    for (int j = 0; j < h; j++) {
        const int y0 = std::max(2 * (grid.cells.top() + j), fine.cells.top()) - fine.cells.top();
        const int y1 = std::min(2 * (grid.cells.top() + j) + 1, fine.cells.bottom()) - fine.cells.top();
        for (int i = 0; i < w; i++) {
            const int x0 = std::max(2 * (grid.cells.left() + i), fine.cells.left()) - fine.cells.left();
            const int x1 = std::min(2 * (grid.cells.left() + i) + 1, fine.cells.right()) - fine.cells.left();
            const float share = 1.0f / ((x1 - x0 + 1) * (y1 - y0 + 1));
            for (int p = 0; p < planes; p++) {
                const float *from = fine.plane[p].data();
                float sum = 0;
                for (int y = y0; y <= y1; y++) {
                    for (int x = x0; x <= x1; x++) sum += from[size_t(y) * fw + x];
                }
                grid.plane[p][size_t(j) * w + i] = sum * share;
            }
        }
    }
    return grid;
}

/// The level of the grids a blur of \p sigma pixels is made on: coarse enough that it spans a few cells
int levelFor(qreal sigma)
{
    return sigma < 3 ? 0 : int(std::floor(std::log2(sigma / 1.5)));
}

/// The blur to make on \p level for one of \p sigma on the image: less what averaging down and back up blur
qreal sigmaOnLevel(qreal sigma, int level)
{
    const qreal cells = std::ldexp(1.0, 2 * level);
    const qreal lost = level > 0 ? (cells - 1) / 12 + cells / 6 : 0;
    return std::sqrt(std::max(0.0, sigma * sigma - lost) / cells);
}

/// The room around a pixel a blur of \p sigma on the image reads
int reachOfBlur(qreal sigma)
{
    const int level = levelFor(sigma);
    return (SkissaDetail::reachOf(sigmaOnLevel(sigma, level)) + 2) << level;
}

} // namespace

Settings Settings::from(const KisPropertiesConfiguration &config)
{
    Settings settings;
    settings.center = QPointF(std::clamp(config.getDouble("x", 0.5), 0.0, 1.0), std::clamp(config.getDouble("y", 0.5), 0.0, 1.0));
    settings.bulge = float(std::clamp(config.getDouble("bulge", 0), -1.0, 1.0));
    settings.fringes = float(std::clamp(config.getDouble("fringes", 0), -0.05, 0.05));
    settings.halo = float(std::clamp(config.getDouble("halo", 0), -0.02, 0.02));
    settings.vignette = float(std::clamp(config.getDouble("vignette", 0), 0.0, 0.95));
    settings.glow = float(std::clamp(config.getDouble("glow", 0), 0.0, 1.0));
    settings.glowSize = float(std::clamp(config.getDouble("glow_size", 0.05), 0.001, 1.0));
    return settings;
}

void Settings::writeTo(KisPropertiesConfiguration &config) const
{
    config.setProperty("x", center.x());
    config.setProperty("y", center.y());
    config.setProperty("bulge", double(bulge));
    config.setProperty("fringes", double(fringes));
    config.setProperty("halo", double(halo));
    config.setProperty("vignette", double(vignette));
    config.setProperty("glow", double(glow));
    config.setProperty("glow_size", double(glowSize));
}

bool Settings::sameLens(const Settings &other) const
{
    return bulge == other.bulge && fringes == other.fringes && halo == other.halo && vignette == other.vignette && glow == other.glow
        && glowSize == other.glowSize;
}

bool Settings::operator==(const Settings &other) const
{
    return center == other.center && sameLens(other);
}

bool Settings::changesNothing() const
{
    return bulge == 0 && fringes == 0 && halo == 0 && vignette == 0 && glow == 0;
}

Settings vintage()
{
    Settings settings;
    settings.fringes = 0.003f;
    settings.halo = 0.0015f;
    settings.vignette = 0.4f;
    settings.glow = 0.12f;
    settings.glowSize = 0.04f;
    return settings;
}

Lens::Lens(const Settings &settings, const QRect &bounds, qreal scale)
{
    const QRectF frame = bounds.isEmpty() ? QRectF(0, 0, 1, 1) : QRectF(bounds);
    m_center = frame.topLeft() + QPointF(settings.center.x() * frame.width(), settings.center.y() * frame.height());
    m_unit = std::max(1.0, std::hypot(frame.width(), frame.height()) / 2);
    qreal farthest = 0;
    for (const QPointF &corner : {frame.topLeft(), frame.topRight(), frame.bottomLeft(), frame.bottomRight()}) {
        const QPointF to = (corner - m_center) / m_unit;
        farthest = std::max(farthest, to.x() * to.x() + to.y() * to.y());
    }

    // a barrel keeps the farthest corner in place, so the image stays full; a pincushion stops short of folding over
    if (settings.bulge > 0) {
        m_k = settings.bulge;
        m_norm = float(1 / (1 + m_k * farthest));
    } else if (settings.bulge < 0) {
        m_k = float(std::max(0.3 * settings.bulge, -0.95 / (3 * std::max(farthest, 1e-6))));
    }

    // each wavelength's picture is magnified by 1 + fringes × its dispersion, so nothing moves at the middle and
    // the colors part in proportion to the distance from it; wavelengths are taken at even steps of that, so the
    // fringes are as smooth everywhere, each standing for the light of its stretch of the spectrum
    const double low = dispersionOf(longest), high = dispersionOf(shortest);
    int count = 1;
    if (settings.fringes != 0) {
        const qreal spread = std::sqrt(farthest) * m_unit * std::abs(settings.fringes) * (high - low);
        count = std::clamp(int(std::ceil(spread / wavelengthStep)) + 1, fewestWavelengths, mostWavelengths);
    }
    if (count == 1) {
        m_magnification = {1.0f};
        m_weights = {{1.0f, 1.0f, 1.0f, 0.0f}};
    } else {
        QVector<std::array<double, 3>> shares;
        std::array<double, 3> sums {};
        for (int i = 0; i < count; i++) {
            const double dispersion = low + (i + 0.5) / count * (high - low);
            const double nm = wavelengthOf(dispersion);
            // the stretch of wavelengths a step of dispersion spans grows as λ³
            const double stretch = nm * nm * nm;
            std::array<double, 3> rgb = rgbOf(nm);
            for (int c = 0; c < 3; c++) {
                // what sRGB can't show of a pure color is left out; each channel's shares still add up to all of it
                rgb[c] = std::max(0.0, rgb[c]) * stretch;
                sums[c] += rgb[c];
            }
            shares << rgb;
            m_magnification << float(1 + settings.fringes * dispersion);
        }
        for (const std::array<double, 3> &rgb : shares) m_weights << std::array<float, 4> {float(rgb[0] / sums[0]), float(rgb[1] / sums[1]), float(rgb[2] / sums[2]), 0.0f};
    }
    m_bends = m_k != 0 || count > 1;

    // cos⁴ θ, with tan θ = r / the focal length, made as dark as asked at the half diagonal
    if (settings.vignette > 0) m_vignetteT = float(1 / std::sqrt(1 - settings.vignette) - 1);

    if (settings.halo != 0) {
        const qreal size = std::abs(settings.halo) * m_unit;
        m_halo = settings.halo > 0 ? std::array<qreal, 3> {haloRed * size, 0, size} : std::array<qreal, 3> {0, size, 0};
        for (qreal &sigma : m_halo) {
            if (sigma < leastBlur) sigma = 0;
            m_haloReach = std::max(m_haloReach, SkissaDetail::reachOf(sigma));
        }
    }

    // scattered light: equal shares at sizes an octave apart, from the largest down to a pixel of the whole image,
    // so it fades as the square of the distance, as a lens's stray light does
    m_keep = 1 - settings.glow;
    if (settings.glow > 0) {
        const qreal largest = settings.glowSize * m_unit / scale;
        QVector<qreal> sizes;
        for (qreal sigma = largest; sigma >= 1; sigma /= 2) sizes << sigma;
        if (sizes.isEmpty()) sizes << largest;
        m_glowEach = settings.glow / sizes.size();
        Q_FOREACH (qreal sigma, sizes) {
            const qreal here = sigma * scale;
            if (here < 0.5) {
                m_keep += m_glowEach;
                continue;
            }
            m_glowSigmas << here;
            m_glowReach = std::max(m_glowReach, reachOfBlur(here));
        }
    }
}

QPointF Lens::from(qreal x, qreal y, float magnification) const
{
    const qreal dx = x - m_center.x(), dy = y - m_center.y();
    const qreal r2 = (dx * dx + dy * dy) / (m_unit * m_unit);
    const qreal factor = (1 + m_k * r2) * m_norm * magnification;
    return m_center + QPointF(dx, dy) * factor;
}

QRect Lens::bentSourceOf(const QRect &rect) const
{
    if (!m_bends || rect.isEmpty()) return rect;
    const auto [least, most] = std::minmax_element(m_magnification.cbegin(), m_magnification.cend());
    qreal left = HUGE_VAL, top = HUGE_VAL, right = -HUGE_VAL, bottom = -HUGE_VAL;
    auto add = [&](qreal x, qreal y) {
        for (float magnification : {*least, *most}) {
            const QPointF at = from(x, y, magnification);
            left = std::min(left, at.x());
            right = std::max(right, at.x());
            top = std::min(top, at.y());
            bottom = std::max(bottom, at.y());
        }
    };
    // a lens keeps the order of distances from its middle, so the edges' pictures enclose the inside's
    constexpr int step = 8;
    for (int x = rect.left();; x = std::min(x + step, rect.right())) {
        add(x + 0.5, rect.top() + 0.5);
        add(x + 0.5, rect.bottom() + 0.5);
        if (x == rect.right()) break;
    }
    for (int y = rect.top();; y = std::min(y + step, rect.bottom())) {
        add(rect.left() + 0.5, y + 0.5);
        add(rect.right() + 0.5, y + 0.5);
        if (y == rect.bottom()) break;
    }
    if (QRectF(rect).contains(m_center)) add(m_center.x(), m_center.y());
    // pixel centres are at halves; bilinear reads the next one too
    return QRect(QPoint(int(std::floor(left - 0.5)) - 2, int(std::floor(top - 0.5)) - 2),
                 QPoint(int(std::floor(right - 0.5)) + 3, int(std::floor(bottom - 0.5)) + 3));
}

QRect Lens::sourceOf(const QRect &rect) const
{
    const int blur = m_haloReach + m_glowReach;
    return bentSourceOf(rect).adjusted(-blur, -blur, blur, blur);
}

void Lens::apply(const float *source, const QRect &sourceRect, const QRect &rect, float *out) const
{
    const size_t written = size_t(rect.width()) * rect.height();
    if (sourceRect.isEmpty()) {
        std::fill(out, out + written * 4, 0.0f);
        return;
    }
    // where the geometry reads, with room for the halo's blur, within what was read
    QRect work = bentSourceOf(rect).adjusted(-m_haloReach, -m_haloReach, m_haloReach, m_haloReach) & sourceRect;
    if (work.isEmpty()) work = sourceRect;

    const size_t count = size_t(sourceRect.width()) * sourceRect.height();
    bool opaque = true;
    for (size_t i = 0; i < count && opaque; i++) opaque = source[i * 4 + 3] >= 1;
    const int planes = opaque ? 3 : 4;

    // full size only around the work area, where the geometry and the finest glow read; the far glow from halves
    const QRect near = work.adjusted(-nearRoom, -nearRoom, nearRoom, nearRoom) & sourceRect;
    std::vector<Grid> levels;
    levels.push_back(premultiplied(source, sourceRect, near, 0, planes));

    const int ww = work.width(), wh = work.height();
    const size_t workCount = size_t(ww) * wh;
    std::vector<float> plane[4];
    for (int p = 0; p < planes; p++) {
        plane[p].resize(workCount);
        const Grid &grid = levels.front();
        for (int y = 0; y < wh; y++) {
            const float *from = grid.plane[p].data() + size_t(work.top() - near.top() + y) * near.width() + (work.left() - near.left());
            float *to = plane[p].data() + size_t(y) * ww;
            for (int x = 0; x < ww; x++) to[x] = m_keep * from[x];
        }
    }

    // glow: the light scattered to each size, each blurred on a grid coarse enough that it costs the same
    if (!m_glowSigmas.isEmpty()) {
        if (levelFor(m_glowSigmas.front()) > 0) {
            const QRect halves(QPoint(floorDiv(sourceRect.left(), 2), floorDiv(sourceRect.top(), 2)),
                               QPoint(floorDiv(sourceRect.right(), 2), floorDiv(sourceRect.bottom(), 2)));
            levels.push_back(premultiplied(source, sourceRect, halves, 1, planes));
        }
        std::vector<float> buffer, scratch;
        std::vector<int> x0(ww), x1(ww), y0(wh), y1(wh);
        std::vector<float> fx(ww), fy(wh);
        Q_FOREACH (qreal sigma, m_glowSigmas) {
            const int level = levelFor(sigma);
            while (int(levels.size()) <= level && levels.back().cells.width() > 1 && levels.back().cells.height() > 1) {
                levels.push_back(halved(levels.back(), planes));
            }
            const Grid &grid = levels[std::min<size_t>(level, levels.size() - 1)];
            const int cell = 1 << grid.shift;
            const qreal blur = sigmaOnLevel(sigma, grid.shift);
            const int room = SkissaDetail::reachOf(blur) + 1;
            QRect around(QPoint(floorDiv(work.left(), cell) - 1, floorDiv(work.top(), cell) - 1),
                         QPoint(floorDiv(work.right(), cell) + 1, floorDiv(work.bottom(), cell) + 1));
            around = around.adjusted(-room, -room, room, room) & grid.cells;
            if (around.isEmpty()) continue;
            const int aw = around.width(), ah = around.height();
            // where each pixel of the work area falls between the cells
            auto place = [cell](int pixel, int first, int size, int &low, int &high, float &t) {
                const double u = std::clamp((pixel + 0.5) / cell - 0.5 - first, 0.0, double(size - 1));
                low = int(u);
                high = std::min(low + 1, size - 1);
                t = float(u - low);
            };
            for (int x = 0; x < ww; x++) place(work.left() + x, around.left(), aw, x0[x], x1[x], fx[x]);
            for (int y = 0; y < wh; y++) place(work.top() + y, around.top(), ah, y0[y], y1[y], fy[y]);
            buffer.resize(size_t(aw) * ah);
            scratch.resize(buffer.size());
            for (int p = 0; p < planes; p++) {
                for (int y = 0; y < ah; y++) {
                    std::copy_n(grid.plane[p].data() + size_t(around.top() - grid.cells.top() + y) * grid.cells.width() + (around.left() - grid.cells.left()), aw,
                                buffer.data() + size_t(y) * aw);
                }
                if (blur > 0.2) SkissaDetail::gaussian(buffer.data(), scratch.data(), aw, ah, blur);
                for (int y = 0; y < wh; y++) {
                    const float *r0 = buffer.data() + size_t(y0[y]) * aw;
                    const float *r1 = buffer.data() + size_t(y1[y]) * aw;
                    float *to = plane[p].data() + size_t(y) * ww;
                    for (int x = 0; x < ww; x++) {
                        const float top = r0[x0[x]] + (r0[x1[x]] - r0[x0[x]]) * fx[x];
                        const float bottom = r1[x0[x]] + (r1[x1[x]] - r1[x0[x]]) * fx[x];
                        to[x] += m_glowEach * (top + (bottom - top) * fy[y]);
                    }
                }
            }
        }
    }

    // halo: each color blurred by how far out of focus it is, its coverage with it
    std::vector<float> alphaOf[3];
    if (m_haloReach > 0) {
        std::vector<float> scratch(workCount);
        for (int c = 0; c < 3; c++) {
            if (m_halo[c] <= 0) continue;
            SkissaDetail::gaussian(plane[c].data(), scratch.data(), ww, wh, m_halo[c]);
            if (opaque) continue;
            alphaOf[c] = plane[3];
            SkissaDetail::gaussian(alphaOf[c].data(), scratch.data(), ww, wh, m_halo[c]);
        }
    }

    // side by side, four floats a pixel for the reads of the geometry: red, green, blue, then each one's coverage
    const int stride = opaque ? 4 : 8;
    std::vector<float> pixels(workCount * stride, 0.0f);
    for (size_t i = 0; i < workCount; i++) {
        float *to = pixels.data() + i * stride;
        for (int c = 0; c < 3; c++) to[c] = plane[c][i];
        if (opaque) continue;
        for (int c = 0; c < 3; c++) to[4 + c] = alphaOf[c].empty() ? plane[3][i] : alphaOf[c][i];
    }

    const int wavelengths = m_magnification.size();
    const float unit2 = float(m_unit * m_unit);
    const float lastX = float(ww - 1), lastY = float(wh - 1);
    for (int y = 0; y < rect.height(); y++) {
        const double dy = rect.top() + y + 0.5 - m_center.y();
        for (int x = 0; x < rect.width(); x++) {
            const double dx = rect.left() + x + 0.5 - m_center.x();
            const float r2 = float(dx * dx + dy * dy) / unit2;
            const float bend = (1 + m_k * r2) * m_norm;
            // positions in the work area for a magnification of 1, and how far each step of it moves them
            const float baseU = float(m_center.x() - 0.5 - work.left()), baseV = float(m_center.y() - 0.5 - work.top());
            const float stepU = float(dx) * bend, stepV = float(dy) * bend;
            float sum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
            for (int i = 0; i < wavelengths; i++) {
                const float u = std::clamp(baseU + stepU * m_magnification[i], 0.0f, lastX);
                const float v = std::clamp(baseV + stepV * m_magnification[i], 0.0f, lastY);
                const int ix = int(u), iy = int(v);
                const int jx = std::min(ix + 1, ww - 1), jy = std::min(iy + 1, wh - 1);
                const float tu = u - ix, tv = v - iy;
                const float *p00 = pixels.data() + (size_t(iy) * ww + ix) * stride;
                const float *p01 = pixels.data() + (size_t(iy) * ww + jx) * stride;
                const float *p10 = pixels.data() + (size_t(jy) * ww + ix) * stride;
                const float *p11 = pixels.data() + (size_t(jy) * ww + jx) * stride;
                const float w00 = (1 - tu) * (1 - tv), w01 = tu * (1 - tv), w10 = (1 - tu) * tv, w11 = tu * tv;
                const float *share = m_weights[i].data();
                for (int c = 0; c < 4; c++) sum[c] += share[c] * (w00 * p00[c] + w01 * p01[c] + w10 * p10[c] + w11 * p11[c]);
                if (opaque) continue;
                for (int c = 4; c < 8; c++) sum[c] += share[c - 4] * (w00 * p00[c] + w01 * p01[c] + w10 * p10[c] + w11 * p11[c]);
            }
            const float alpha = opaque ? 1.0f : std::clamp(std::max({sum[4], sum[5], sum[6]}), 0.0f, 1.0f);
            const float shade = m_vignetteT > 0 ? 1 / ((1 + m_vignetteT * r2) * (1 + m_vignetteT * r2)) : 1.0f;
            float *pixel = out + (size_t(y) * rect.width() + x) * 4;
            for (int c = 0; c < 3; c++) pixel[c] = alpha > 1e-6f ? sum[c] * shade / alpha : 0.0f;
            pixel[3] = alpha;
        }
    }
}

QImage applied(const QImage &image, const Settings &settings, qreal scale)
{
    const QImage picture = image.convertToFormat(QImage::Format_ARGB32);
    const QRect rect = picture.rect();
    const Lens lens(settings, rect, scale);
    const QRect source = lens.sourceOf(rect) & rect;
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
    lens.apply(in.data(), source, rect, out.data());
    QImage result(rect.size(), QImage::Format_ARGB32);
    auto encoded = [encode](float c) { return int(encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]); };
    for (int y = 0; y < rect.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < rect.width(); x++) {
            const float *pixel = out.data() + (size_t(y) * rect.width() + x) * 4;
            line[x] = qRgba(encoded(pixel[0]), encoded(pixel[1]), encoded(pixel[2]), qRound(pixel[3] * 255));
        }
    }
    return result;
}

} // namespace SkissaLens
