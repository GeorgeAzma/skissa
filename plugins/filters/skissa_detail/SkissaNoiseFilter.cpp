/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaNoiseFilter.h"

#include <vector>

#include <filter/kis_filter_category_ids.h>
#include <filter/kis_filter_configuration.h>
#include <kis_default_bounds_base.h>
#include <kis_lod_transform.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>

#include "SkissaDetailWidget.h"
#include "SkissaPixels.h"

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

    const std::vector<float> in = SkissaPixels::readLinear(device, source);
    std::vector<float> out(size_t(applyRect.width()) * applyRect.height() * 4);
    // floats hold colors past sRGB, which are kept
    apply(settings, scale, in.data(), source, applyRect, out.data(), !SkissaPixels::holdsPastWhite(device->colorSpace()));
    SkissaPixels::writeLinear(device, applyRect, out.data());
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
