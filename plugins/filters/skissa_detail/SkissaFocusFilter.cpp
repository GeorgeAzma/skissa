/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaFocusFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaFocus.h"
#include "SkissaPixels.h"

using namespace SkissaFocus;

SkissaFocusFilter::SkissaFocusFilter()
    : KisFilter(focusId(), FiltersCategoryBlurId, i18nc("@action effect", "&Lens blur..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaFocusFilter::focusId()
{
    return KoID(QStringLiteral("skissa_focus"), i18nc("effect", "Lens blur"));
}

QRect SkissaFocusFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    KisLodTransformScalar t(lod);
    const int reach = Focus(Settings::from(*config), QRect(), t.scale(1.0)).reach();
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaFocusFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaFocusFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    KisLodTransformScalar t(device);
    const QRect bounds = device->defaultBounds()->bounds();
    const Focus focus(settings, bounds, t.scale(1.0), !SkissaPixels::holdsPastWhite(device->colorSpace()));
    const int reach = focus.reach();
    // beyond the image its edge repeats
    const QRect source = applyRect.adjusted(-reach, -reach, reach, reach) & (bounds | applyRect);
    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    focus.apply(in.data(), source, applyRect, out.data());
    SkissaPixels::writeLinear(device, applyRect, out.data());
}

KisConfigWidget *SkissaFocusFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaFocusWidget(parent, dev);
}

KisFilterConfigurationSP SkissaFocusFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    Settings().writeTo(*config);
    return config;
}
