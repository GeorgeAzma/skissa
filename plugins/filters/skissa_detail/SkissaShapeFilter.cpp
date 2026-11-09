/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaShapeFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaPixels.h"
#include "SkissaShape.h"

using namespace SkissaShape;

SkissaShapeFilter::SkissaShapeFilter()
    : KisFilter(shapeId(), FiltersCategoryArtisticId, i18nc("@action effect", "&Shape..."))
{
    setSupportsPainting(true);
    setSupportsAdjustmentLayers(true);
    setSupportsThreading(true);
    setSupportsLevelOfDetail(true);
    setColorSpaceIndependence(FULLY_INDEPENDENT);
}

KoID SkissaShapeFilter::shapeId()
{
    return KoID(QStringLiteral("skissa_shape"), i18nc("effect", "Shape"));
}

QRect SkissaShapeFilter::neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    KisLodTransformScalar t(lod);
    const int reach = Shape(Settings::from(*config), t.scale(1.0)).reach();
    return rect.adjusted(-reach, -reach, reach, reach);
}

QRect SkissaShapeFilter::changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const
{
    return neededRect(rect, config, lod);
}

void SkissaShapeFilter::processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *) const
{
    KIS_SAFE_ASSERT_RECOVER_RETURN(config);
    const Settings settings = Settings::from(*config);
    if (settings.changesNothing() || applyRect.isEmpty()) return;
    KisLodTransformScalar t(device);
    const Shape shape(settings, t.scale(1.0));
    const int reach = shape.reach();
    // what lies past the image counts too: a layer can reach beyond it
    const QRect source = applyRect.adjusted(-reach, -reach, reach, reach);
    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    shape.apply(in.data(), source, applyRect, out.data());
    SkissaPixels::writeLinear(device, applyRect, out.data());
}

KisConfigWidget *SkissaShapeFilter::createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool) const
{
    return new SkissaShapeWidget(parent, dev);
}

KisFilterConfigurationSP SkissaShapeFilter::defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const
{
    KisFilterConfigurationSP config = factoryConfiguration(resourcesInterface);
    Settings settings;
    settings.outline = 4;
    settings.writeTo(*config);
    return config;
}
