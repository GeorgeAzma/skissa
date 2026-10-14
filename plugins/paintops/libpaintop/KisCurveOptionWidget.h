/*
 *  SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KisCurveOptionWidget_H
#define KisCurveOptionWidget_H

#include <kis_paintop_option.h>

class Ui_WdgCurveOption2;
class KisCurveOption;
class QComboBox;

#include <KisCurveOptionData.h>
#include <KisCurveRangeModelInterface.h>
#include <KisCurveOptionInputControlsStrategyInterface.h>
#include <KisCurveOptionRangeControlsStrategyInterface.h>
#include <lager/cursor.hpp>
#include <lager/constant.hpp>


class PAINTOP_EXPORT KisCurveOptionWidget : public KisPaintOpOption
{
    Q_OBJECT
public:
    enum Flag {
        None = 0x0,
        SupportsCommonCurve = 0x1,
        UseFloatingPointStrength = 0x4,
        /// One always on keeps no checkbox on its row for the pen (MyPaint: its pen off drops the input curves when saved)
        NoPenCheckbox = 0x8,
    };

    Q_DECLARE_FLAGS(Flags, Flag)

public:
    using data_type = KisCurveOptionDataCommon;

    KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                          KisPaintOpOption::PaintopCategory category,
                          lager::reader<bool> enabledLink = lager::make_constant(true),
                          std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader = std::nullopt);

    KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                          KisPaintOpOption::PaintopCategory category,
                          const QString &curveMinLabel, const QString &curveMaxLabel,
                          lager::reader<bool> enabledLink = lager::make_constant(true),
                          std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader = std::nullopt);

    KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                          KisPaintOpOption::PaintopCategory category,
                          const QString &curveMinLabel, const QString &curveMaxLabel,
                          int curveMinValue, int curveMaxValue, const QString &curveValueSuffix,
                          lager::reader<bool> enabledLink = lager::make_constant(true),
                          std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader = std::nullopt);

    KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                          PaintopCategory category,
                          const QString &curveMinLabel, const QString &curveMaxLabel,
                          int curveMinValue, int curveMaxValue, const QString &curveValueSuffix,
                          const QString &strengthPrefix, const QString &strengthSuffix,
                          qreal strengthDisplayMultiplier,
                          lager::reader<bool> enabledLink = lager::make_constant(true),
                          std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader = std::nullopt);

protected:
    KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                          KisPaintOpOption::PaintopCategory category,
                          const QString &strengthPrefix, const QString &strengthSuffix,
                          qreal strengthDisplayMultiplier,
                          lager::reader<bool> enabledLink,
                          std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader,
                          KisCurveRangeModelFactory curveRangeModelFactory,
                          KisCurveOptionInputControlsStrategyFactory inputControlsFactory,
                          KisCurveOptionRangeControlsStrategyFactory rangeControlsFactory,
                          Flags flags);
public:

    ~KisCurveOptionWidget() override;

    void writeOptionSetting(KisPropertiesConfigurationSP setting) const override;
    void readOptionSetting(const KisPropertiesConfigurationSP setting) override;

    bool isCheckable() const override;
    void show();

    /// Its data's id, after the masked brush's prefix when it is the mask's
    QString id() const override;

    /// The strength as stored (most options: 0 to 1)
    qreal strengthValue() const;

    QVector<PenInput> penInputs() const override;
    void setPenInputActive(const QString &id, bool active) override;
    void setPenCurve(const QString &id, const QString &curve) override;
    int penMode() const override;
    void setPenMode(int mode) override;
    QWidget *createPenInputPage(const QString &id, QWidget *parent) override;

protected:
    std::optional<RowValue> ownRowValue() const override;
    void setOwnRowValue(qreal value) override;

    lager::cursor<qreal> strengthValueDenorm();

    void setCurveWidgetsEnabled(bool value);
    QWidget* curveWidget();

protected Q_SLOTS:
    void updateThemedIcons();



protected:
    QWidget* m_widget {nullptr};
    Ui_WdgCurveOption2* m_curveOptionWidget {nullptr};
    QScopedPointer<KisCurveOptionInputControlsStrategyInterface> m_curveInputControlsStrategy;
    QScopedPointer<KisCurveOptionRangeControlsStrategyInterface> m_curveRangeControlsStrategy;
    QComboBox* m_curveMode {nullptr};
    struct Private;
    const QScopedPointer<Private> m_d;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(KisCurveOptionWidget::Flags)

#endif // KisCurveOptionWidget_H
