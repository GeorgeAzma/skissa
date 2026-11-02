/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSACOLORFILTER_H
#define SKISSACOLORFILTER_H

#include <QObject>
#include <QVariant>

#include <filter/kis_color_transformation_filter.h>

class SkissaColorPlugin : public QObject
{
    Q_OBJECT
public:
    SkissaColorPlugin(QObject *parent, const QVariantList &);
};

class SkissaColorFilter : public KisColorTransformationFilter
{
public:
    SkissaColorFilter();

    static KoID id();

    KoColorTransformation *createTransformation(const KoColorSpace *cs, const KisFilterConfigurationSP config) const override;
    KisConfigWidget *createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;
};

#endif
