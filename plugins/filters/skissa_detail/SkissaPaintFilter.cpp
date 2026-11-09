/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaPaintFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaPixels.h"
#include "SkissaPaint.h"

using namespace SkissaPaint;

SkissaPaintFilter::SkissaPaintFilter()
    : KisFilter(paintId(), FiltersCategoryArtisticId, i18nc("@action effect", "&Painting..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaPaintFilter::paintId()
{
    return KoID(QStringLiteral("skissa_paint"), i18nc("effect", "Painting"));
}

QRect SkissaPaintFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    KisLodTransformScalar t(lod);
    const int reach = Paint(Settings::from(*config), t.scale(1.0)).reach();
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaPaintFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaPaintFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (applyRect.isEmpty()) return;
    KisLodTransformScalar t(device);
    const Paint paint(settings, t.scale(1.0));
    const int reach = paint.reach();
    // beyond the image its edge repeats
    const QRect bounds = device->defaultBounds()->bounds();
    const QRect source = (applyRect.adjusted(-reach, -reach, reach, reach) & (bounds | applyRect)) | applyRect;
    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    paint.apply(in.data(), source, applyRect, out.data());
    SkissaPixels::writeLinear(device, applyRect, out.data());
}

KisConfigWidget *SkissaPaintFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaPaintWidget(parent, dev);
}

KisFilterConfigurationSP SkissaPaintFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    Settings().writeTo(*config);
    return config;
}
