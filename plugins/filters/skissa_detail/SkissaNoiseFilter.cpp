/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaNoiseFilter.h"

#include <algorithm>
#include <vector>

#include <KoChannelInfo.h>
#include <KoColorConversionTransformation.h>
#include <KoColorSpace.h>
#include <KoColorSpaceRegistry.h>
#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "KoCompositeOpInLinear.h"
#include "SkissaDetailWidget.h"

using namespace SkissaNoise;

SkissaNoiseFilter::SkissaNoiseFilter(Kind kind)
    : KisFilter(idOf(kind), FiltersCategoryOtherId,
                kind == Kind::Grain ? i18nc("@action effect", "&Grain...") : i18nc("@action effect", "&Noise..."))
    , m_kind(kind)
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaNoiseFilter::idOf(Kind kind)
{
    return kind == Kind::Grain ? KoID(QStringLiteral("skissa_grain"), i18nc("effect", "Grain"))
                               : KoID(QStringLiteral("skissa_noise"), i18nc("effect", "Noise"));
}

QRect SkissaNoiseFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    KisLodTransformScalar t(lod);
    const int reach = Settings::from(m_kind, *config).reach(t.scale(1.0));
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaNoiseFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaNoiseFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(m_kind, *config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    KisLodTransformScalar t(device);
    const qreal scale = t.scale(1.0);

    // beyond the image its edge repeats, for pixels taken from beside it
    const int reach = settings.reach(scale);
    QRect source = applyRect.adjusted(-reach, -reach, reach, reach);
    if (!device->defaultBounds()->wrapAroundMode()) source &= applyRect | device->defaultBounds()->bounds();

    const KoColorSpace *cs = device->colorSpace();
    const KoColorSpace *linear = KoMixing::linearSpace();
    const bool srgb8 = *cs == *KoColorSpaceRegistry::instance()->rgb8();
    const bool isLinear = *cs == *linear;
    const KoChannelInfo::enumChannelValueType type = cs->channels().first()->channelValueType();
    // floats hold colors past sRGB, which are kept
    const bool fit = type != KoChannelInfo::FLOAT16 && type != KoChannelInfo::FLOAT32 && type != KoChannelInfo::FLOAT64;

    const size_t count = size_t(source.width()) * source.height();
    std::vector<quint8> raw(count * cs->pixelSize());
    device->readBytes(raw.data(), source);
    std::vector<float> in(count * 4);
    if (srgb8) {
        // blue, green, red, alpha, as Krita keeps 8-bit RGB
        const float *decode = KoMixing::srgbToLinearTable();
        for (size_t i = 0; i < count; i++) {
            const quint8 *pixel = raw.data() + i * 4;
            float *to = in.data() + i * 4;
            to[0] = decode[pixel[2]];
            to[1] = decode[pixel[1]];
            to[2] = decode[pixel[0]];
            to[3] = pixel[3] / 255.0f;
        }
    } else if (isLinear) {
        std::copy(raw.begin(), raw.end(), reinterpret_cast<quint8 *>(in.data()));
    } else {
        cs->convertPixelsTo(raw.data(), reinterpret_cast<quint8 *>(in.data()), linear, int(count),
                            KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    }

    const size_t written = size_t(applyRect.width()) * applyRect.height();
    std::vector<float> out(written * 4);
    apply(settings, scale, in.data(), source, applyRect, out.data(), fit);

    raw.resize(written * cs->pixelSize());
    if (srgb8) {
        const quint8 *encode = KoMixing::linearToSrgbTable();
        auto encoded = [encode](float c) { return encode[int(std::clamp(c, 0.0f, 1.0f) * 65535 + 0.5f)]; };
        for (size_t i = 0; i < written; i++) {
            const float *from = out.data() + i * 4;
            quint8 *pixel = raw.data() + i * 4;
            pixel[2] = encoded(from[0]);
            pixel[1] = encoded(from[1]);
            pixel[0] = encoded(from[2]);
            pixel[3] = quint8(std::clamp(from[3], 0.0f, 1.0f) * 255 + 0.5f);
        }
    } else if (isLinear) {
        std::copy_n(reinterpret_cast<const quint8 *>(out.data()), raw.size(), raw.data());
    } else {
        linear->convertPixelsTo(reinterpret_cast<const quint8 *>(out.data()), raw.data(), cs, int(written),
                                KoColorConversionTransformation::internalRenderingIntent(), KoColorConversionTransformation::internalConversionFlags());
    }
    device->writeBytes(raw.data(), applyRect);
}

KisConfigWidget *SkissaNoiseFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    if (m_kind == Kind::Noise) return new SkissaNoiseWidget(parent, dev);
    const QString percent = QStringLiteral("%");
    return new SkissaSliders(parent, id(),
                             {SkissaSlider{"amount", i18nc("effect setting", "Amount"), 400, 100, 0, percent, 100},
                              SkissaSlider{"size", i18nc("effect setting", "Size"), 64, 8, 1, i18nc("pixels, after a number", " px"), 1, QString(), 0.5},
                              SkissaSlider{"color", i18nc("noise effect setting", "Color"), 100, 100, 0, percent, 100}});
}

KisFilterConfigurationSP SkissaNoiseFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    if (m_kind == Kind::Grain) {
        config->setProperty("amount", 0.3);
        config->setProperty("size", 1.0);
        config->setProperty("color", 0.0);
    } else {
        Settings().writeTo(*config);
    }
    return config;
}
