/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSADETAILFILTER_H
#define SKISSADETAILFILTER_H

#include <QObject>
#include <QVariant>

#include <vector>

#include <filter/kis_filter.h>

#include "SkissaDetail.h"

class SkissaDetailPlugin : public QObject
{
    Q_OBJECT
public:
    SkissaDetailPlugin(QObject *parent, const QVariantList &);
};

/**
 * Detail, or one of the effects made of it with a slider or two (Blur, Sharpen, High pass, Smooth). Blur, Sharpen
 * and High pass are Krita's Gaussian blur, unsharp mask and Gaussian high pass, by their ids and settings, so files
 * made here open in Krita; Skissa makes their blurs. Their ids from before (\p earlier) still open.
 */
class SkissaDetailFilter : public KisFilter
{
public:
    explicit SkissaDetailFilter(SkissaDetail::Kind kind, bool earlier = false);

    static KoID idOf(SkissaDetail::Kind kind, bool earlier = false);

    void processImpl(KisPaintDeviceSP device, const QRect &applyRect, const KisFilterConfigurationSP config, KoUpdater *progressUpdater) const override;
    QRect neededRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;
    QRect changedRect(const QRect &rect, const KisFilterConfigurationSP config, int lod) const override;

    KisConfigWidget *createConfigurationWidget(QWidget *parent, const KisPaintDeviceSP dev, bool useForMasks) const override;
    KisFilterConfigurationSP defaultConfiguration(KisResourcesInterfaceSP resourcesInterface) const override;

private:
    SkissaDetail::Plan planAt(const KisFilterConfigurationSP config, int lod) const;
    /// \p plan applied to \p device over \p applyRect, as its pixels' bytes
    std::vector<quint8> rendered(const SkissaDetail::Plan &plan, KisPaintDeviceSP device, const QRect &applyRect) const;

    SkissaDetail::Kind m_kind;
    bool m_earlier;
};

#endif
