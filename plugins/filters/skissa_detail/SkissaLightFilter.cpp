/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaLightFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaPixels.h"
#include "SkissaLight.h"

using namespace SkissaLight;

SkissaLightFilter::SkissaLightFilter()
    : KisFilter(lightId(), FiltersCategoryArtisticId, i18nc("@action effect", "&Light..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaLightFilter::lightId()
{
    return KoID(QStringLiteral("skissa_light"), i18nc("effect", "Light"));
}

QRect SkissaLightFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    KisLodTransformScalar t(lod);
    const int reach = Light(Settings::from(*config), t.scale(1.0)).reach();
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaLightFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaLightFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    KisLodTransformScalar t(device);
    const Light light(settings, t.scale(1.0), !SkissaPixels::holdsPastWhite(device->colorSpace()));
    const int reach = light.reach();
    // long spikes and wide glows read far around every patch, each patch making most of the image's shine again:
    // made once over the whole image instead, a band a core; not at a level of detail, whose device is smaller
    const QRect bounds = device->defaultBounds()->bounds();
    if (reach >= 128 && bounds.contains(applyRect) && device->defaultBounds()->currentLevelOfDetail() == 0) {
        const quint64 settingsKey = qHash(config->toXML()) ^ (quint64(bounds.width()) << 32 | quint32(bounds.height()));
        const auto whole = SkissaPixels::wholeResult(device, bounds, settingsKey, [&light, &bounds, reach](const float *in, const QRect &part, float *out) {
            const int top = std::max(bounds.top(), part.top() - reach), bottom = std::min(bounds.bottom(), part.bottom() + reach);
            const QRect band(bounds.left(), top, bounds.width(), bottom - top + 1);
            light.apply(in + size_t(top - bounds.top()) * bounds.width() * 4, band, part, out);
        });
        if (whole) {
            SkissaPixels::writeLinear(device, applyRect, SkissaPixels::partOf(*whole, bounds, applyRect).data());
            return;
        }
    }
    // light from past the image shines into it too: a layer can reach beyond it
    const QRect source = applyRect.adjusted(-reach, -reach, reach, reach);
    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    light.apply(in.data(), source, applyRect, out.data());
    SkissaPixels::writeLinear(device, applyRect, out.data());
}

KisConfigWidget *SkissaLightFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaLightWidget(parent, dev);
}

KisFilterConfigurationSP SkissaLightFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    Settings().writeTo(*config);
    return config;
}
