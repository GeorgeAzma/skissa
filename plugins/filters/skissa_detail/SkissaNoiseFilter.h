/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSANOISEFILTER_H
#define SKISSANOISEFILTER_H

#include <filter/kis_filter.h>

#include "SkissaNoise.h"

/// Noise, or Grain: Noise with a few sliders
class SkissaNoiseFilter : public KisFilter
{
public:
    explicit SkissaNoiseFilter(SkissaNoise::Kind kind);

    static KoID idOf(SkissaNoise::Kind kind);

    void processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *progressUpdater) const override;
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;

    KisConfigWidget *createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;

private:
    SkissaNoise::Kind m_kind;
};

#endif
