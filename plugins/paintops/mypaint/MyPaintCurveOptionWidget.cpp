/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "MyPaintCurveOptionWidget.h"
#include "KisLager.h"
#include "kis_paintop_option.h"
#include "MyPaintCurveRangeModel.h"
#include "MyPaintCurveOptionRangeControlsStrategy.h"
#include "KisCurveOptionInputControlsStrategy.h"
#include "MyPaintSensorPack.h"
#include "kis_paintop_lod_limitations.h"
#include "ui_wdgcurveoption2.h"

#include <libmypaint/mypaint-brush-settings.h>

namespace {
KisPaintopLodLimitations calcGeneralMyPaintLodLimitations(const MyPaintCurveOptionData &data)
{
    KisPaintopLodLimitations l;

    if (data.sensorStruct().sensorRandom.isActive) {
        l.limitations << KoID(QString("Random Sensor Active, %1").arg(data.id.id()), i18nc("PaintOp instant preview limitation", "Random Sensor Active in %1 option, consider disabling Instant Preview", data.id.name()));
    }

    return l;
}
} // namespace

struct MyPaintCurveOptionWidget::Private
{
    Private(lager::cursor<MyPaintCurveOptionData> optionData)
        : lodLimitations(optionData.map(&calcGeneralMyPaintLodLimitations))
        , setting(optionData->id.id())
    {}

    lager::reader<KisPaintopLodLimitations> lodLimitations;
    /// libmypaint's name of the setting
    QString setting;
};

MyPaintCurveOptionWidget::MyPaintCurveOptionWidget(lager::cursor<MyPaintCurveOptionData> optionData,
                                                     qreal maxYRange,
                                                     const QString &yValueSuffix)
    : KisCurveOptionWidget(optionData.zoom(kislager::lenses::to_base<KisCurveOptionDataCommon>),
                            KisPaintOpOption::GENERAL,
                            i18n("Base Value: "), yValueSuffix, 1.0,
                            lager::make_constant(true),
                            std::nullopt,
                            MyPaintCurveRangeModel::factory(maxYRange, yValueSuffix),
                            KisCurveOptionInputControlsStrategyDouble::factory(),
                            MyPaintCurveOptionRangeControlsStrategy::factory(),
                            UseFloatingPointStrength | NoPenCheckbox)
    , m_d(new Private(optionData))
{
    m_curveOptionWidget->curveWidget->setGlobalPointConstrain(KisCurveWidget::PointConstrain_AlwaysCorner);
}

MyPaintCurveOptionWidget::~MyPaintCurveOptionWidget()
{
}

std::optional<KisPaintOpOption::RowValue> MyPaintCurveOptionWidget::ownRowValue() const
{
    std::optional<RowValue> value = KisCurveOptionWidget::ownRowValue();
    // libmypaint's own default, where the setting changes nothing: a brush's settings at it aren't its character
    // (the size, hardness and opacity always are)
    static const QStringList always = {QStringLiteral("radius_logarithmic"), QStringLiteral("hardness"), QStringLiteral("opaque")};
    if (value && !always.contains(m_d->setting)) {
        const MyPaintBrushSetting setting = mypaint_brush_setting_from_cname(m_d->setting.toLatin1().constData());
        if (int(setting) >= 0) value->neutral = mypaint_brush_setting_info(setting)->def;
    }
    return value;
}

KisPaintOpOption::OptionalLodLimitationsReader MyPaintCurveOptionWidget::lodLimitationsReader() const
{
    return m_d->lodLimitations;
}
