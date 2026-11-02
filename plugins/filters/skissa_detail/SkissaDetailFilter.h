/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSADETAILFILTER_H
#define SKISSADETAILFILTER_H

#include <QObject>
#include <QVariant>

#include <filter/kis_filter.h>

#include "SkissaDetail.h"

class SkissaDetailPlugin : public QObject
{
    Q_OBJECT
public:
    SkissaDetailPlugin(QObject *parent, const QVariantList &);
};

/// Detail, or one of the effects made of it with a slider or two (Blur, Sharpen, High pass, Smooth)
class SkissaDetailFilter : public KisFilter
{
public:
    explicit SkissaDetailFilter(SkissaDetail::Kind kind);

    static KoID idOf(SkissaDetail::Kind kind);

    void processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *progressUpdater) const override;
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;

    KisConfigWidget *createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;

private:
    SkissaDetail::Plan planAt(const KisFilterConfigurationSP config, int lod) const;

    SkissaDetail::Kind m_kind;
};

#endif
