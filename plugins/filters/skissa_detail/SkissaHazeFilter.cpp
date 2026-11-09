/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaHazeFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaHaze.h"
#include "SkissaPixels.h"

using namespace SkissaHaze;

SkissaHazeFilter::SkissaHazeFilter()
    : KisFilter(hazeId(), FiltersCategoryColorId, i18nc("@action effect", "&Haze..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaHazeFilter::hazeId()
{
    return KoID(QStringLiteral("skissa_haze"), i18nc("effect", "Haze"));
}

void SkissaHazeFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    std::vector<float> pixels = SkissaPixels::readLinear(device, applyRect);
    apply(settings, device->defaultBounds()->bounds(), applyRect, pixels.data());
    SkissaPixels::writeLinear(device, applyRect, pixels.data());
}

KisConfigWidget *SkissaHazeFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaHazeWidget(parent, dev);
}

KisFilterConfigurationSP SkissaHazeFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    Settings().writeTo(*config);
    return config;
}
