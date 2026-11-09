/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAHAZEFILTER_H
#define SKISSAHAZEFILTER_H

#include <filter/kis_filter.h>

/// Haze: the layer seen through air, fading toward the air's color as it does with distance
class SkissaHazeFilter : public KisFilter
{
public:
    SkissaHazeFilter();

    static KoID hazeId();

    void processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *progressUpdater) const override;

    KisConfigWidget *createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
};

#endif
