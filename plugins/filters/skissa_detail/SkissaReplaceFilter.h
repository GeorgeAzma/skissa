/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAREPLACEFILTER_H
#define SKISSAREPLACEFILTER_H

#include <filter/kis_filter.h>

/**
 * Replace color: one color made another, or see-through. A pixel that holds some of the color is read as the color
 * with other paint over it, and that paint is laid over the new color instead, so soft edges keep their blend: white
 * paper made cream leaves no white fringe around the lines, black lines made blue take their gray edges with them.
 * As a background the color lies under everything, and every pixel is read as the least paint over it that can make
 * it (GIMP's color to alpha); otherwise only the color and its blends with what lies beside it hold any of it. It
 * works on the values as the image keeps them, the values Krita blended them in.
 */
class SkissaReplaceFilter : public KisFilter
{
public:
    SkissaReplaceFilter();

    static KoID replaceId();

    void processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *progressUpdater) const override;
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;

    KisConfigWidget *createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
};

#endif
