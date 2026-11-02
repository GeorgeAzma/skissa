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
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>
#include <kpluginfactory.h>

#include "SkissaDetailWidget.h"

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
}

SkissaDetailFilter::SkissaDetailFilter(Kind kind)
    : KisFilter(idOf(kind), categoryOf(kind), menuText(kind))
    , m_kind(kind)
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaDetailFilter::idOf(Kind kind)
{
    switch (kind) {
    case Kind::Blur: return KoID(QStringLiteral("skissa_blur"), i18nc("effect", "Blur"));
    case Kind::Sharpen: return KoID(QStringLiteral("skissa_sharpen"), i18nc("effect", "Sharpen"));
    case Kind::HighPass: return KoID(QStringLiteral("skissa_high_pass"), i18nc("effect", "High pass"));
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
    if (layout == Layout::Converted && !work) return;
    const KoChannelInfo::enumChannelValueType type = cs->channels().first()->channelValueType();
    // integers hold 0 to 1; floats keep what goes past
    const bool fit = type != KoChannelInfo::FLOAT16 && type != KoChannelInfo::FLOAT32 && type != KoChannelInfo::FLOAT64;

    const int pixelSize = cs->pixelSize();
    std::vector<quint8> raw(size_t(source.width()) * source.height() * pixelSize);
    device->readBytes(raw.data(), source);
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
    device->writeBytes(raw.data(), applyRect);
}

KisConfigWidget *SkissaDetailFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    if (m_kind == Kind::Detail) return new SkissaDetailWidget(parent, dev);
    return new SkissaDetailSliders(parent, m_kind);
}

KisFilterConfigurationSP SkissaDetailFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    switch (m_kind) {
    case Kind::Blur: config->setProperty("radius", 5.0); break;
    case Kind::Sharpen:
        config->setProperty("amount", 1.0);
        config->setProperty("radius", 2.0);
        break;
    case Kind::HighPass: config->setProperty("radius", 4.0); break;
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
