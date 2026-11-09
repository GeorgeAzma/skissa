/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaLensFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaLens.h"
#include "SkissaPixels.h"

using namespace SkissaLens;

namespace
{

/// A lens reads from anywhere in the image, and its sizes are shares of the image, which a filter isn't told
constexpr int anywhere = 16384;

} // namespace

SkissaLensFilter::SkissaLensFilter()
    : KisFilter(lensId(), FiltersCategoryArtisticId, i18nc("@action effect", "&Lens..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaLensFilter::lensId()
{
    return KoID(QStringLiteral("skissa_lens"), i18nc("effect", "Lens"));
}

QRect SkissaLensFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int) const
{
    if (Settings::from(*config).changesNothing()) return rect;
    return rect.adjusted(-anywhere, -anywhere, anywhere, anywhere);
}

QRect SkissaLensFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaLensFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    KisLodTransformScalar t(device);
    const QRect bounds = device->defaultBounds()->bounds();
    const Lens lens(settings, bounds, t.scale(1.0), !SkissaPixels::holdsPastWhite(device->colorSpace()));

    // beyond the image its edge repeats
    const QRect source = lens.sourceOf(applyRect) & (bounds | applyRect);
    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    lens.apply(in.data(), source, applyRect, out.data());
    SkissaPixels::writeLinear(device, applyRect, out.data());
}

KisConfigWidget *SkissaLensFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaLensWidget(parent, dev);
}

KisFilterConfigurationSP SkissaLensFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    vintage().writeTo(*config);
    return config;
}
