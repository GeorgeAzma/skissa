/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaRadialFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaPixels.h"
#include "SkissaRadial.h"

using namespace SkissaRadial;

namespace
{

/// A streak reads from anywhere toward the middle, which may be anywhere in the image
constexpr int anywhere = 16384;

} // namespace

SkissaRadialFilter::SkissaRadialFilter()
    : KisFilter(radialId(), FiltersCategoryBlurId, i18nc("@action effect", "&Radial Blur..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaRadialFilter::radialId()
{
    return KoID(QStringLiteral("skissa_radial"), i18nc("effect", "Radial blur"));
}

QRect SkissaRadialFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    if (Settings::from(*config).changesNothing()) return rect;
    return rect.adjusted(-anywhere, -anywhere, anywhere, anywhere);
}

QRect SkissaRadialFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaRadialFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    const QRect bounds = device->defaultBounds()->bounds();
    const Radial radial(settings, bounds, !SkissaPixels::holdsPastWhite(device->colorSpace()));

    // beyond the image its edge repeats
    const QRect source = (radial.sourceOf(applyRect) & (bounds | applyRect)) | applyRect;
    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    radial.apply(in.data(), source, applyRect, out.data());
    SkissaPixels::writeLinear(device, applyRect, out.data());
}

KisConfigWidget *SkissaRadialFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaRadialWidget(parent, dev);
}

KisFilterConfigurationSP SkissaRadialFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    zoomed().writeTo(*config);
    return config;
}
