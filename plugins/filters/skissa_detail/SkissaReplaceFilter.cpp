/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaReplaceFilter.h"

#include <QColor>
#include <QPoint>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <KisEffectPanels.h>
#include <KoColor.h>
#include <KoColorConversionTransformation.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaPixels.h"

namespace
{

struct Settings {
    /// The color replaced
    QColor color {Qt::white};
    /// What takes its place, see-through by its alpha
    QColor with {Qt::transparent};
    /// How far from the color still counts as the color itself (scans, compression): 0 to 1 in each channel
    float range {0.02f};
    /// The color lies under everything, as paper does, and every pixel is read as paint over it; otherwise only the
    /// color itself and its blends with what lies beside it are replaced
    bool background {true};

    static Settings from(const KisPropertiesConfiguration &config)
    {
        Settings s;
        auto colorOf = [&config](const char *key, const QColor &fallback) {
            const QColor color(config.getString(key, fallback.name(QColor::HexArgb)));
            return color.isValid() ? color : fallback;
        };
        s.color = colorOf("color", s.color);
        s.with = colorOf("with", s.with);
        s.range = float(qBound(0.0, config.getDouble("range", s.range), 1.0));
        s.background = config.getBool("background", s.background);
        return s;
    }

    void writeTo(KisPropertiesConfiguration &config) const
    {
        config.setProperty("color", color.name(QColor::HexArgb));
        config.setProperty("with", with.name(QColor::HexArgb));
        config.setProperty("range", double(range));
        config.setProperty("background", background);
    }

    bool changesNothing() const
    {
        QColor opaque = color;
        opaque.setAlpha(255);
        return with == opaque && range <= 0;
    }
};

/// How far the color mode follows a blend each way, in pixels at full size
constexpr int edgeReach = 64;

int reachAt(qreal scale)
{
    return std::max(2, int(std::ceil(edgeReach * scale)));
}

enum class Layout { U8, U16, F32, Converted };

/// A layer's pixels as red, green, blue and alpha floats, keeping its own encoding (its profile's curve), and back
class Coding
{
public:
    explicit Coding(const KoColorSpace *cs)
        : m_cs(cs)
    {
        const bool rgb = cs->colorModelId() == RGBAColorModelID;
        const QString depth = cs->colorDepthId().id();
        m_layout = !rgb ? Layout::Converted
            : depth == Integer8BitsColorDepthID.id() ? Layout::U8
            : depth == Integer16BitsColorDepthID.id() ? Layout::U16
            : depth == Float32BitsColorDepthID.id() ? Layout::F32
                                                     : Layout::Converted;
        if (m_layout == Layout::Converted) {
            m_work = KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(),
                                                                  rgb ? cs->profile() : KoColorSpaceRegistry::instance()->rgb8()->profile());
        }
    }

    bool valid() const
    {
        return m_layout != Layout::Converted || m_work;
    }

    void decode(const quint8 *raw, size_t count, float *out) const
    {
        switch (m_layout) {
        case Layout::U8:
            // blue, green, red, alpha, as Krita keeps integer RGB
            for (size_t i = 0; i < count; i++) {
                const quint8 *pixel = raw + i * 4;
                float *to = out + i * 4;
                to[0] = pixel[2] / 255.0f;
                to[1] = pixel[1] / 255.0f;
                to[2] = pixel[0] / 255.0f;
                to[3] = pixel[3] / 255.0f;
            }
            break;
        case Layout::U16:
            for (size_t i = 0; i < count; i++) {
                const quint16 *pixel = reinterpret_cast<const quint16 *>(raw) + i * 4;
                float *to = out + i * 4;
                to[0] = pixel[2] / 65535.0f;
                to[1] = pixel[1] / 65535.0f;
                to[2] = pixel[0] / 65535.0f;
                to[3] = pixel[3] / 65535.0f;
            }
            break;
        case Layout::F32: std::memcpy(out, raw, count * 4 * sizeof(float)); break;
        case Layout::Converted:
            m_cs->convertPixelsTo(raw, reinterpret_cast<quint8 *>(out), m_work, int(count), KoColorConversionTransformation::internalRenderingIntent(),
                                  KoColorConversionTransformation::internalConversionFlags());
            break;
        }
    }

    void encode(const float *in, size_t count, quint8 *raw) const
    {
        auto u8 = [](float v) { return quint8(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
        auto u16 = [](float v) { return quint16(std::clamp(v, 0.0f, 1.0f) * 65535 + 0.5f); };
        switch (m_layout) {
        case Layout::U8:
            for (size_t i = 0; i < count; i++) {
                const float *from = in + i * 4;
                quint8 *pixel = raw + i * 4;
                pixel[2] = u8(from[0]);
                pixel[1] = u8(from[1]);
                pixel[0] = u8(from[2]);
                pixel[3] = u8(from[3]);
            }
            break;
        case Layout::U16:
            for (size_t i = 0; i < count; i++) {
                const float *from = in + i * 4;
                quint16 *pixel = reinterpret_cast<quint16 *>(raw) + i * 4;
                pixel[2] = u16(from[0]);
                pixel[1] = u16(from[1]);
                pixel[0] = u16(from[2]);
                pixel[3] = u16(from[3]);
            }
            break;
        case Layout::F32: std::memcpy(raw, in, count * 4 * sizeof(float)); break;
        case Layout::Converted:
            m_work->convertPixelsTo(reinterpret_cast<const quint8 *>(in), raw, m_cs, int(count), KoColorConversionTransformation::internalRenderingIntent(),
                                    KoColorConversionTransformation::internalConversionFlags());
            break;
        }
    }

    /// \p color as the layer's pixels are decoded
    void decode(const QColor &color, float *out) const
    {
        const KoColor in(color, m_cs);
        decode(in.data(), 1, out);
        out[3] = float(color.alphaF());
    }

private:
    const KoColorSpace *m_cs;
    const KoColorSpace *m_work {nullptr};
    Layout m_layout;
};

/// How colorful \p v is, as a direction: two opponent axes without lightness, linear in the values
float chromaOf(const float *v)
{
    return std::hypot(v[0] - v[1], 0.5f * (v[0] + v[1]) - v[2]);
}

bool withinRange(const float *p, const float *color, float range)
{
    return std::abs(p[0] - color[0]) <= range && std::abs(p[1] - color[1]) <= range && std::abs(p[2] - color[2]) <= range;
}

/**
 * How much of \p p is \p color lying under everything (0 to 1), and the color it is there (\p under): the pixel is
 * read as the color with the least paint over it that can make it, in each channel how far the pixel is from the
 * color of the room toward the end it lies at (GIMP's color to alpha). Within \p range counts as the color.
 */
float shareUnder(const float *p, const float *color, float range, float *under)
{
    if (withinRange(p, color, range)) {
        std::copy_n(p, 3, under);
        return 1;
    }
    float cover = 0;
    float distance = 0;
    for (int c = 0; c < 3; c++) {
        const float d = std::abs(p[c] - color[c]);
        const float room = p[c] < color[c] ? color[c] : 1 - color[c];
        const float a = d <= range ? 0 : room <= range ? 1 : std::min(1.0f, (d - range) / (room - range));
        if (a > cover) {
            cover = a;
            distance = d;
        }
    }
    if (cover >= 1) return 0;
    // at the range's edge on the way to the pixel, so the paint fades in from it
    const float ratio = range / distance;
    float paint[3];
    for (int c = 0; c < 3; c++) {
        under[c] = color[c] + (p[c] - color[c]) * ratio;
        paint[c] = under[c] + (p[c] - under[c]) / cover;
    }
    // colors that cancel (red and teal) mix to a gray, and a gray is far likelier gray than the color with its
    // opposite: the less the pixel keeps of the two's color, the less of it is read as the color
    const float share = 1 - cover;
    const float apart = share * chromaOf(under) + cover * chromaOf(paint);
    const float kept = apart > 1e-4f ? chromaOf(p) / apart : 1;
    const float t = std::clamp((kept - 0.15f) / 0.2f, 0.0f, 1.0f);
    return share * t * t * (3 - 2 * t);
}

/// The distances the color mode steps along a blend, out to its reach
std::vector<int> edgeSteps(int reach)
{
    std::vector<int> steps;
    const int full[] = {1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
    for (int step : full) {
        const int d = std::max(1, int(std::lround(step * reach / double(edgeReach))));
        if (steps.empty() || d > steps.back()) steps.push_back(d);
    }
    return steps;
}

/// A patch of decoded pixels with which of them are the color
struct Source {
    const float *pixels;
    std::vector<quint8> isColor;
    QRect rect;

    const float *at(int x, int y) const
    {
        return pixels + (size_t(y - rect.top()) * rect.width() + (x - rect.left())) * 4;
    }
    bool colorAt(int x, int y) const
    {
        return isColor[size_t(y - rect.top()) * rect.width() + (x - rect.left())];
    }
};

/// How far past the range a color still counts as partly the color (the ringing beside a sharpened or resampled edge)
constexpr float nearBand = 0.08f;

/**
 * How much of the pixel at \p x, \p y is \p color as a blend with what lies beside it (an edge, a soft stroke): the
 * picture runs from the color through the pixel to something else, along the line between the two colors. Downhill
 * it must come to the color without crossing an even patch wider than \p flatRun: a shadow tone between the base
 * color and a line could be read as a mix of them, but it is a color of its own, and so are its edges.
 */
float shareBeside(const Source &source, int x, int y, const float *color, float range, float tolerance, const std::vector<int> &steps, int flatRun)
{
    const float *p = source.at(x, y);
    // how much a color is the color: all of it within the range, fading out past it
    auto nearness = [&](const float *q) {
        float apart = 0;
        for (int c = 0; c < 3; c++) apart = std::max(apart, std::abs(q[c] - color[c]));
        return std::clamp((range + nearBand - apart) / nearBand, 0.0f, 1.0f);
    };
    const float near = nearness(p);

    const float pc[3] = {p[0] - color[0], p[1] - color[1], p[2] - color[2]};
    const float length = std::sqrt(pc[0] * pc[0] + pc[1] * pc[1] + pc[2] * pc[2]);
    if (length < 1e-4f) return 1;
    const float u[3] = {pc[0] / length, pc[1] / length, pc[2] / length};
    // how far a pixel lies from the color along the way to this one, and how far off that way
    auto along = [&](const float *q, float *off) {
        const float qc[3] = {q[0] - color[0], q[1] - color[1], q[2] - color[2]};
        const float f = qc[0] * u[0] + qc[1] * u[1] + qc[2] * u[2];
        float off2 = 0;
        for (int c = 0; c < 3; c++) off2 += (qc[c] - f * u[c]) * (qc[c] - f * u[c]);
        *off = std::sqrt(off2);
        return f;
    };
    auto valueAt = [&](int qx, int qy) {
        if (!source.rect.contains(qx, qy)) return length;
        const float *q = source.at(qx, qy);
        float off;
        return q[3] > 0 ? along(q, &off) : length;
    };
    // which way the picture leaves the color (Sobel)
    const float gx = valueAt(x + 1, y - 1) + 2 * valueAt(x + 1, y) + valueAt(x + 1, y + 1) - valueAt(x - 1, y - 1) - 2 * valueAt(x - 1, y)
        - valueAt(x - 1, y + 1);
    const float gy = valueAt(x - 1, y + 1) + 2 * valueAt(x, y + 1) + valueAt(x + 1, y + 1) - valueAt(x - 1, y - 1) - 2 * valueAt(x, y - 1)
        - valueAt(x + 1, y - 1);
    const float slope = std::hypot(gx, gy);
    if (slope < 8e-3f) return near;
    const float dx = gx / slope;
    const float dy = gy / slope;
    // a step of 8 bits, which a gentle blend stays on for a few pixels
    const float level = 2e-3f;

    // downhill toward the color, on the line, crossing no even patch; how much of the color it comes to
    float reached = 0;
    float last = length;
    int fell = 0;
    QPoint previous(x, y);
    for (int step : steps) {
        const QPoint at(x - int(std::lround(dx * step)), y - int(std::lround(dy * step)));
        if (at == previous) continue;
        previous = at;
        if (!source.rect.contains(at)) break;
        const float *q = source.at(at.x(), at.y());
        if (q[3] <= 0) break;
        if (source.colorAt(at.x(), at.y())) {
            reached = 1;
            break;
        }
        float off;
        const float f = along(q, &off);
        if (off > tolerance || f > last + level) break;
        if (f < last - level) {
            last = f;
            fell = step;
        } else if (step - fell > flatRun) {
            break;
        }
        reached = std::max(reached, nearness(q));
    }
    if (reached <= 0) return near;

    // uphill to where the blend ends: the other color
    float far = length;
    previous = QPoint(x, y);
    for (int step : steps) {
        const QPoint at(x + int(std::lround(dx * step)), y + int(std::lround(dy * step)));
        if (at == previous) continue;
        previous = at;
        if (!source.rect.contains(at)) break;
        const float *q = source.at(at.x(), at.y());
        if (q[3] <= 0) break;
        float off;
        const float f = along(q, &off);
        if (off > tolerance || f < far - tolerance) break;
        far = std::max(far, f);
    }
    return std::max(near, far > length ? (1 - length / far) * reached : 0.0f);
}

/// \p p with \p share of it, which is \p under, made \p with instead
void replaceShare(float *p, float share, const float *under, const float *with)
{
    const float alpha = 1 - share * (1 - with[3]);
    for (int c = 0; c < 3; c++) p[c] = alpha > 1e-6f ? (p[c] - share * under[c] + share * with[3] * with[c]) / alpha : 0;
    p[3] *= alpha;
}

} // namespace

SkissaReplaceFilter::SkissaReplaceFilter()
    : KisFilter(replaceId(), FiltersCategoryColorId, i18nc("@action effect", "&Replace Color..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaReplaceFilter::replaceId()
{
    return KoID(QStringLiteral("skissa_replace"), i18nc("effect", "Replace color"));
}

QRect SkissaReplaceFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    if (Settings::from(*config).background) return rect;
    KisLodTransformScalar t(lod);
    // the pixels beside the edge and theirs, for how much the picture changes there
    const int reach = reachAt(t.scale(1.0)) + 1;
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaReplaceFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaReplaceFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings s = Settings::from(*config);
    if (s.changesNothing() || applyRect.isEmpty()) return;
    const Coding coding(device->colorSpace());
    if (!coding.valid()) return;
    float color[4];
    float with[4];
    coding.decode(s.color, color);
    coding.decode(s.with, with);

    KisLodTransformScalar t(device);
    const int reach = s.background ? 0 : reachAt(t.scale(1.0));
    const QRect sourceRect = s.background ? applyRect : applyRect.adjusted(-reach - 1, -reach - 1, reach + 1, reach + 1);
    const int pixelSize = device->pixelSize();
    const size_t sourceCount = size_t(sourceRect.width()) * sourceRect.height();
    std::vector<quint8> raw(sourceCount * pixelSize);
    SkissaPixels::readOriginal(device, sourceRect, raw.data());
    std::vector<float> pixels(sourceCount * 4);
    coding.decode(raw.data(), sourceCount, pixels.data());

    Source source {pixels.data(), {}, sourceRect};
    std::vector<int> steps;
    int flatRun = 0;
    // what the blends are judged by: a little past the range, for the steps 8 bits make
    const float tolerance = s.range + 0.03f;
    if (!s.background) {
        source.isColor.resize(sourceCount);
        for (size_t i = 0; i < sourceCount; i++) source.isColor[i] = pixels[i * 4 + 3] > 0 && withinRange(&pixels[i * 4], color, s.range);
        steps = edgeSteps(reach);
        flatRun = std::max(2, int(std::lround(8 * t.scale(1.0))));
    }

    const size_t count = size_t(applyRect.width()) * applyRect.height();
    std::vector<float> out(count * 4);
    std::vector<quint8> changed(count);
    bool any = false;
    for (int y = 0; y < applyRect.height(); y++) {
        for (int x = 0; x < applyRect.width(); x++) {
            const size_t i = size_t(y) * applyRect.width() + x;
            const int ix = applyRect.left() + x;
            const int iy = applyRect.top() + y;
            float *p = out.data() + i * 4;
            std::copy_n(source.at(ix, iy), 4, p);
            if (p[3] <= 0) continue;
            float under[3];
            float share;
            if (s.background) {
                share = shareUnder(p, color, s.range, under);
            } else if (source.colorAt(ix, iy)) {
                share = 1;
                std::copy_n(p, 3, under);
            } else {
                share = shareBeside(source, ix, iy, color, s.range, tolerance, steps, flatRun);
                std::copy_n(color, 3, under);
            }
            if (share <= 0) continue;
            replaceShare(p, share, under, with);
            changed[i] = 1;
            any = true;
        }
    }
    if (!any) return;

    // pixels left alone keep their bytes: a conversion there and back may not
    std::vector<quint8> written(count * pixelSize);
    coding.encode(out.data(), count, written.data());
    for (int y = 0; y < applyRect.height(); y++) {
        for (int x = 0; x < applyRect.width(); x++) {
            const size_t i = size_t(y) * applyRect.width() + x;
            if (changed[i]) continue;
            const size_t from = (size_t(y + applyRect.top() - sourceRect.top()) * sourceRect.width() + (x + applyRect.left() - sourceRect.left())) * pixelSize;
            std::memcpy(written.data() + i * pixelSize, raw.data() + from, pixelSize);
        }
    }
    device->writeBytes(written.data(), applyRect);
}

KisConfigWidget *SkissaReplaceFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return KisEffectPanels::create(replaceId().id(), parent, dev);
}

KisFilterConfigurationSP SkissaReplaceFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    Settings().writeTo(*config);
    return config;
}
