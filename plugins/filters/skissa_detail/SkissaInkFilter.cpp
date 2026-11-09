/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaInkFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaInk.h"
#include "SkissaPixels.h"

using namespace SkissaInk;

SkissaInkFilter::SkissaInkFilter()
    : KisFilter(inkId(), FiltersCategoryArtisticId, i18nc("@action effect", "&Drawing..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaInkFilter::inkId()
{
    return KoID(QStringLiteral("skissa_ink"), i18nc("effect", "Drawing"));
}

QRect SkissaInkFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    KisLodTransformScalar t(lod);
    const int reach = Ink(Settings::from(*config), t.scale(1.0)).reach();
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaInkFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaInkFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    KisLodTransformScalar t(device);
    const Ink ink(settings, t.scale(1.0));
    const int reach = ink.reach();
    // beyond the image the lines stop
    const QRect bounds = device->defaultBounds()->bounds();
    const QRect source = (applyRect.adjusted(-reach, -reach, reach, reach) & (bounds | applyRect)) | applyRect;
    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    ink.apply(in.data(), source, applyRect, out.data());
    SkissaPixels::writeLinear(device, applyRect, out.data());
}

KisConfigWidget *SkissaInkFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaInkWidget(parent, dev);
}

KisFilterConfigurationSP SkissaInkFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    inked().writeTo(*config);
    return config;
}
