/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaDetailFilter.h"

#include <algorithm>
#include <cmath>

#include <KoChannelInfo.h>
#include <KoColorConversionTransformation.h>
#include <KoColorModelStandardIds.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <KoCompositeOpRegistry.h>
#include <KoConvolutionOp.h>
#include <cstring>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>
#include <kpluginfactory.h>

#include "SkissaDetailWidget.h"
#include "SkissaPixels.h"
#include "SkissaFewerFilter.h"
#include "SkissaLensFilter.h"
#include "SkissaInkFilter.h"
#include "SkissaRadialFilter.h"
#include "SkissaNoiseFilter.h"

K_PLUGIN_FACTORY_WITH_JSON(SkissaDetailFactory, "kritaskissadetail.json", registerPlugin<SkissaDetailPlugin>();)

using namespace SkissaDetail;

namespace
{

QString menuText(Kind kind)
{
    switch (kind) {
    case Kind::Blur: return i18nc("@action effect", "&Blur...");
    case Kind::Sharpen: return i18nc("@action effect", "&Sharpen...");
    case Kind::HighPass: return i18nc("@action effect", "&High Pass...");
    case Kind::Smooth: return i18nc("@action effect", "S&mooth...");
    default: return i18nc("@action effect", "&Detail...");
    }
}

KoID categoryOf(Kind kind)
{
    switch (kind) {
    case Kind::Blur: return FiltersCategoryBlurId;
    case Kind::HighPass: return FiltersCategoryEdgeDetectionId;
    default: return FiltersCategoryEnhanceId;
    }
}

/// How the pixels of a color space are read as floats: Krita's own 8 and 16-bit (blue first) and 32-bit float
/// layouts directly, anything else through a conversion
enum class Layout { U8, U16, F32, Converted };

} // namespace

SkissaDetailPlugin::SkissaDetailPlugin(QObject *parent, const QVariantList &)
    : QObject(parent)
{
    for (Kind kind : {Kind::Blur, Kind::Sharpen, Kind::HighPass, Kind::Smooth, Kind::Detail}) {
        KisFilterRegistry::instance()->add(KisFilterSP(new SkissaDetailFilter(kind)));
    }
    for (Kind kind : {Kind::Blur, Kind::Sharpen, Kind::HighPass}) {
        KisFilterRegistry::instance()->add(KisFilterSP(new SkissaDetailFilter(kind, true)));
    }
    for (SkissaNoise::Kind kind : {SkissaNoise::Kind::Grain, SkissaNoise::Kind::Noise}) {
        KisFilterRegistry::instance()->add(KisFilterSP(new SkissaNoiseFilter(kind)));
    }
    KisFilterRegistry::instance()->add(KisFilterSP(new SkissaLensFilter()));
    KisFilterRegistry::instance()->add(KisFilterSP(new SkissaRadialFilter()));
    KisFilterRegistry::instance()->add(KisFilterSP(new SkissaInkFilter()));
    KisFilterRegistry::instance()->add(KisFilterSP(new SkissaFewerFilter()));
}

SkissaDetailFilter::SkissaDetailFilter(Kind kind, bool earlier)
    : KisFilter(idOf(kind, earlier), categoryOf(kind), menuText(kind))
    , m_kind(kind)
    , m_earlier(earlier)
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaDetailFilter::idOf(Kind kind, bool earlier)
{
    switch (kind) {
    case Kind::Blur: return KoID(earlier ? QStringLiteral("skissa_blur") : QStringLiteral("gaussian blur"), i18nc("effect", "Blur"));
    case Kind::Sharpen: return KoID(earlier ? QStringLiteral("skissa_sharpen") : QStringLiteral("unsharp"), i18nc("effect", "Sharpen"));
    case Kind::HighPass: return KoID(earlier ? QStringLiteral("skissa_high_pass") : QStringLiteral("gaussianhighpass"), i18nc("effect", "High pass"));
    case Kind::Smooth: return KoID(QStringLiteral("skissa_smooth"), i18nc("effect", "Smooth"));
    default: return KoID(QStringLiteral("skissa_detail"), i18nc("effect", "Detail"));
    }
}

Plan SkissaDetailFilter::planAt(const KisFilterConfigurationSP config, int lod) const
{
    KisLodTransformScalar t(lod);
    return planOf(m_kind, *config).scaled(t.scale(1.0));
}

QRect SkissaDetailFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    const int reach = planAt(config, lod).reach();
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaDetailFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    const int reach = planAt(config, lod).reach();
    return rect.adjusted(-reach, -reach, reach, reach);
}

void SkissaDetailFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Plan plan = planAt(config, device->defaultBounds()->currentLevelOfDetail());
    if (plan.changesNothing() || applyRect.isEmpty()) return;
    if (m_earlier || (m_kind != Kind::Sharpen && m_kind != Kind::HighPass)) {
        const std::vector<quint8> bytes = rendered(plan, device, applyRect);
        if (!bytes.empty()) device->writeBytes(bytes.data(), applyRect);
        return;
    }

    // Krita's unsharp mask and high pass, made with this blur: the blur first, then put with the pixels as Krita does
    Plan blur;
    blur.fine = 0;
    blur.steps << Step{plan.steps.first().radius, 1};
    const std::vector<quint8> blurred = rendered(blur, device, applyRect);
    if (blurred.empty()) return;
    const KoColorSpace *cs = device->colorSpace();
    const int pixelSize = cs->pixelSize();
    const int count = applyRect.width() * applyRect.height();
    std::vector<quint8> pixels(size_t(count) * pixelSize);
    SkissaPixels::readOriginal(device, applyRect, pixels.data());
    if (m_kind == Kind::HighPass) {
        // the blur taken away by Grain extract, so a flat area is middle gray
        const KoCompositeOp *op = cs->compositeOp(COMPOSITE_GRAIN_EXTRACT);
        KoCompositeOp::ParameterInfo params;
        params.dstRowStart = pixels.data();
        params.dstRowStride = applyRect.width() * pixelSize;
        params.srcRowStart = blurred.data();
        params.srcRowStride = applyRect.width() * pixelSize;
        params.rows = applyRect.height();
        params.cols = applyRect.width();
        params.opacity = 1.0f;
        params.flow = 1.0f;
        params.channelFlags = config->channelFlags();
        op->composite(params);
        device->writeBytes(pixels.data(), applyRect);
        return;
    }
    const qreal amount = config->getDouble("amount", 1);
    const quint8 threshold = quint8(std::clamp(config->getInt("threshold", 0), 0, 255));
    const bool lightnessOnly = config->getBool("lightnessOnly", true);
    // as Krita weighs them: the pixel by 1 + amount, its blur by -amount, in 128ths
    const qreal factor = 128;
    qreal weights[2] = {factor * (1 + amount), -factor * amount};
    std::vector<quint8> out(pixels.size());
    // pixels too close to their blur stay as they were; with no threshold none do (each comparison converts both)
    std::vector<bool> kept(threshold ? size_t(count) : 0);
    for (int i = 0; threshold && i < count; i++) {
        const quint8 *pixel = pixels.data() + size_t(i) * pixelSize;
        const quint8 *soft = blurred.data() + size_t(i) * pixelSize;
        const quint8 diff = threshold == 1 ? quint8(std::memcmp(pixel, soft, pixelSize) != 0)
            : lightnessOnly ? cs->differenceA(pixel, soft) : cs->difference(pixel, soft);
        kept[size_t(i)] = diff < threshold;
    }
    if (lightnessOnly) {
        std::vector<quint16> lab(size_t(count) * 4), labBlurred(size_t(count) * 4);
        cs->toLabA16(pixels.data(), reinterpret_cast<quint8 *>(lab.data()), quint32(count));
        cs->toLabA16(blurred.data(), reinterpret_cast<quint8 *>(labBlurred.data()), quint32(count));
        for (int i = 0; i < count; i++) {
            if (threshold && kept[size_t(i)]) continue;
            // lightness and alpha sharpened, the color kept
            for (int channel : {0, 3}) {
                quint16 &value = lab[size_t(i) * 4 + channel];
                value = quint16(std::clamp((value * weights[0] + labBlurred[size_t(i) * 4 + channel] * weights[1]) / factor, 0.0, 65535.0));
            }
        }
        cs->fromLabA16(reinterpret_cast<const quint8 *>(lab.data()), out.data(), quint32(count));
        for (int i = 0; threshold && i < count; i++) {
            if (kept[size_t(i)]) std::memcpy(out.data() + size_t(i) * pixelSize, pixels.data() + size_t(i) * pixelSize, pixelSize);
        }
    } else {
        KoConvolutionOp *convolution = cs->convolutionOp();
        const QBitArray channelFlags = config->channelFlags();
        std::vector<quint8> pair(size_t(pixelSize) * 2);
        quint8 *colors[2] = {pair.data(), pair.data() + pixelSize};
        for (int i = 0; i < count; i++) {
            const quint8 *pixel = pixels.data() + size_t(i) * pixelSize;
            const quint8 *soft = blurred.data() + size_t(i) * pixelSize;
            quint8 *target = out.data() + size_t(i) * pixelSize;
            if (threshold && kept[size_t(i)]) {
                std::memcpy(target, pixel, pixelSize);
                continue;
            }
            std::memcpy(colors[0], pixel, pixelSize);
            std::memcpy(colors[1], soft, pixelSize);
            convolution->convolveColors(colors, weights, target, factor, 0, 2, channelFlags);
        }
    }
    device->writeBytes(out.data(), applyRect);
}

std::vector<quint8> SkissaDetailFilter::rendered(const Plan &plan, KisPaintDeviceSP device, const QRect &applyRect) const
{

    // beyond the image its edge repeats, as Krita's blurs do; a wrapping image wraps by itself
    const int reach = plan.reach();
    QRect source = applyRect.adjusted(-reach, -reach, reach, reach);
    if (!device->defaultBounds()->wrapAroundMode()) source &= applyRect | device->defaultBounds()->bounds();

    const KoColorSpace *cs = device->colorSpace();
    const bool rgb = cs->colorModelId() == RGBAColorModelID;
    const QString depth = cs->colorDepthId().id();
    const Layout layout = !rgb                                   ? Layout::Converted
        : depth == Integer8BitsColorDepthID.id()                 ? Layout::U8
        : depth == Integer16BitsColorDepthID.id()                ? Layout::U16
        : depth == Float32BitsColorDepthID.id()                  ? Layout::F32
                                                                 : Layout::Converted;
    const KoColorSpace *work = layout != Layout::Converted ? nullptr
        : KoColorSpaceRegistry::instance()->colorSpace(RGBAColorModelID.id(), Float32BitsColorDepthID.id(),
                                                       rgb ? cs->profile() : KoColorSpaceRegistry::instance()->rgb8()->profile());
    if (layout == Layout::Converted && !work) return {};
    const KoChannelInfo::enumChannelValueType type = cs->channels().first()->channelValueType();
    // integers hold 0 to 1; floats keep what goes past
    const bool fit = type != KoChannelInfo::FLOAT16 && type != KoChannelInfo::FLOAT32 && type != KoChannelInfo::FLOAT64;

    const int pixelSize = cs->pixelSize();
    std::vector<quint8> raw(size_t(source.width()) * source.height() * pixelSize);
    SkissaPixels::readOriginal(device, source, raw.data());
    std::vector<float> converted;
    if (layout == Layout::Converted) {
        converted.resize(size_t(source.width()) * source.height() * 4);
        cs->convertPixelsTo(raw.data(), reinterpret_cast<quint8 *>(converted.data()), work, source.width() * source.height(),
                            KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    }

    Planes planes;
    planes.resize(source.width(), source.height());
    const size_t count = size_t(source.width()) * source.height();
    for (size_t i = 0; i < count; i++) {
        float value[4];
        switch (layout) {
        case Layout::U8: {
            const quint8 *pixel = raw.data() + i * 4;
            value[0] = pixel[2] / 255.0f;
            value[1] = pixel[1] / 255.0f;
            value[2] = pixel[0] / 255.0f;
            value[3] = pixel[3] / 255.0f;
            break;
        }
        case Layout::U16: {
            const quint16 *pixel = reinterpret_cast<const quint16 *>(raw.data()) + i * 4;
            value[0] = pixel[2] / 65535.0f;
            value[1] = pixel[1] / 65535.0f;
            value[2] = pixel[0] / 65535.0f;
            value[3] = pixel[3] / 65535.0f;
            break;
        }
        case Layout::F32: std::copy_n(reinterpret_cast<const float *>(raw.data()) + i * 4, 4, value); break;
        case Layout::Converted: std::copy_n(converted.data() + i * 4, 4, value); break;
        }
        const float alpha = std::clamp(value[3], 0.0f, 1.0f);
        for (int c = 0; c < 3; c++) planes.plane[c][i] = value[c] * alpha;
        planes.plane[3][i] = alpha;
    }

    Planes result;
    apply(plan, planes, source, applyRect, result);

    const size_t written = size_t(applyRect.width()) * applyRect.height();
    raw.resize(written * pixelSize);
    if (layout == Layout::Converted) converted.resize(written * 4);
    for (size_t i = 0; i < written; i++) {
        float value[4];
        value[3] = std::clamp(result.plane[3][i], 0.0f, 1.0f);
        for (int c = 0; c < 3; c++) {
            float v = value[3] > 1e-6f ? result.plane[c][i] / value[3] : 0.0f;
            if (!std::isfinite(v)) v = 0;
            value[c] = fit ? std::clamp(v, 0.0f, 1.0f) : v;
        }
        switch (layout) {
        case Layout::U8: {
            quint8 *pixel = raw.data() + i * 4;
            pixel[2] = quint8(value[0] * 255 + 0.5f);
            pixel[1] = quint8(value[1] * 255 + 0.5f);
            pixel[0] = quint8(value[2] * 255 + 0.5f);
            pixel[3] = quint8(value[3] * 255 + 0.5f);
            break;
        }
        case Layout::U16: {
            quint16 *pixel = reinterpret_cast<quint16 *>(raw.data()) + i * 4;
            pixel[2] = quint16(value[0] * 65535 + 0.5f);
            pixel[1] = quint16(value[1] * 65535 + 0.5f);
            pixel[0] = quint16(value[2] * 65535 + 0.5f);
            pixel[3] = quint16(value[3] * 65535 + 0.5f);
            break;
        }
        case Layout::F32: std::copy_n(value, 4, reinterpret_cast<float *>(raw.data()) + i * 4); break;
        case Layout::Converted: std::copy_n(value, 4, converted.data() + i * 4); break;
        }
    }
    if (layout == Layout::Converted) {
        work->convertPixelsTo(reinterpret_cast<const quint8 *>(converted.data()), raw.data(), cs, int(written),
                              KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    }
    return raw;
}

KisConfigWidget *SkissaDetailFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    if (m_kind == Kind::Detail) return new SkissaDetailWidget(parent, dev);
    const QString px = i18nc("pixels, after a number", " px");
    const QString percent = QStringLiteral("%");
    const bool krita = !m_earlier && m_kind != Kind::Smooth;
    QVector<SkissaSlider> sliders;
    if (m_kind == Kind::Sharpen) sliders << SkissaSlider{"amount", i18nc("effect setting", "Amount"), 1000, 0, percent, 100};
    const char *radius = !krita ? "radius" : m_kind == Kind::Blur ? "horizRadius" : m_kind == Kind::Sharpen ? "halfSize" : "blurAmount";
    SkissaSlider size{radius, i18nc("effect setting", "Radius"), 500, 1, px, 1, QString(), 0.1};
    // a blur stretched one way in Krita keeps its shape
    if (krita && m_kind == Kind::Blur) size.follower = "vertRadius";
    sliders << size;
    if (m_kind == Kind::Smooth) {
        sliders << SkissaSlider{"edges", i18nc("effect setting", "Edges"), 100, 0, percent, 100, i18nc("@info:tooltip", "Keeps edges sharp")};
    }
    return new SkissaSliders(parent, id(), sliders);
}

KisFilterConfigurationSP SkissaDetailFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    switch (m_kind) {
    case Kind::Blur:
        if (m_earlier) {
            config->setProperty("radius", 5.0);
            break;
        }
        config->setProperty("horizRadius", 5.0);
        config->setProperty("vertRadius", 5.0);
        config->setProperty("lockAspect", true);
        break;
    case Kind::Sharpen:
        config->setProperty("amount", 1.0);
        if (m_earlier) {
            config->setProperty("radius", 2.0);
            break;
        }
        config->setProperty("halfSize", 2.0);
        config->setProperty("threshold", 0);
        config->setProperty("lightnessOnly", true);
        break;
    case Kind::HighPass: config->setProperty(m_earlier ? "radius" : "blurAmount", 4.0); break;
    case Kind::Smooth:
        config->setProperty("radius", 4.0);
        config->setProperty("edges", 0.4);
        break;
    case Kind::Detail:
        config->setProperty("curve", flatCurve().toString());
        config->setProperty("edges", 0.0);
        break;
    }
    return config;
}

#include "SkissaDetailFilter.moc"
