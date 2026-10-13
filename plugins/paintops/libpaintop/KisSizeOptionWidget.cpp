/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSizeOptionWidget.h"

#include <KisLager.h>
#include <kis_paintop_lod_limitations.h>

struct KisSizeOptionWidget::Private
{
    Private(lager::cursor<KisSizeOptionData> optionData)
        : lodLimitations(optionData.map(std::mem_fn(&KisSizeOptionData::lodLimitations)))
        , strength(optionData.zoom(kislager::lenses::to_base<KisCurveOptionDataCommon>)[&KisCurveOptionDataCommon::strengthValue])
    {}

    lager::reader<KisPaintopLodLimitations> lodLimitations;
    lager::cursor<qreal> strength;
};

KisSizeOptionWidget::KisSizeOptionWidget(lager::cursor<KisSizeOptionData> optionData)
    : KisSizeOptionWidget(optionData, KisPaintOpOption::GENERAL)
{
}

KisSizeOptionWidget::KisSizeOptionWidget(lager::cursor<KisSizeOptionData> optionData, PaintopCategory categoryOverride)
    : KisCurveOptionWidget(optionData.zoom(kislager::lenses::to_base<KisCurveOptionDataCommon>), categoryOverride)
    , m_d(new Private(optionData))
{
}

KisSizeOptionWidget::~KisSizeOptionWidget()
{
}

lager::cursor<qreal> KisSizeOptionWidget::strength() const
{
    return m_d->strength;
}

KisPaintOpOption::OptionalLodLimitationsReader KisSizeOptionWidget::lodLimitationsReader() const
{
    return m_d->lodLimitations;
}
