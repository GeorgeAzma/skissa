/*
 * SPDX-FileCopyrightText: 2008 Boudewijn Rempt <boud@valdyas.org>
 * SPDX-FileCopyrightText: 2009 Sven Langkamp <sven.langkamp@gmail.com>
 * SPDX-FileCopyrightText: 2011 Silvio Heinrich <plassy@web.de>
 * SPDX-FileCopyrightText: 2022 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisCurveOptionWidget.h"

#include "ui_wdgcurveoption2.h"
#include "widgets/kis_curve_widget.h"
#include "kis_global.h"
#include "kis_icon_utils.h"

#include <kis_signals_blocker.h>
#include <KisCurveOptionModel.h>
#include <KisWidgetConnectionUtils.h>

#include <kis_algebra_2d.h>

#include <KisMultiSensorsSelector.h>
#include <KisDynamicSensorFactoryRegistry.h>
#include <KisCurveWidgetConnectionHelper.h>
#include <KisCurveRangeModel.h>
#include <KisCurveOptionInputControlsStrategy.h>
#include <KisCurvePresetStrip.h>

#include <QCheckBox>
#include <QVBoxLayout>

#include <kis_config.h>
#include <kis_config_notifier.h>

namespace
{
/// The row's checkbox: the option on or off, or for one that is always on (Opacity, Flow), whether the pen changes it
auto rowCheckedLens = lager::lenses::getset(
    [](const KisCurveOptionDataCommon &data) -> bool {
        return data.isCheckable ? data.isChecked : data.useCurve;
    },
    [](KisCurveOptionDataCommon data, bool checked) -> KisCurveOptionDataCommon {
        if (data.isCheckable) {
            data.isChecked = checked;
        } else {
            data.useCurve = checked;
        }
        return data;
    });
} // namespace

struct KisCurveOptionWidget::Private
{
    Private(lager::cursor<KisCurveOptionDataCommon> optionData,
            lager::reader<bool> enabledLink,
            std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader,
            qreal strengthDisplayMultiplier,
            KisCurveRangeModelFactory curveRangeFactory)
        : model(optionData, enabledLink, strengthRangeReader, strengthDisplayMultiplier, curveRangeFactory)
    {}

    KisCurveOptionModel model;
};

KisCurveOptionWidget::KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                                             PaintopCategory category,
                                             lager::reader<bool> enabledLink,
                                             std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader)
    : KisCurveOptionWidget(optionData,
                            category,
                            i18n("0%"), i18n("100%"),
                            enabledLink,
                            strengthRangeReader)
{
}

KisCurveOptionWidget::KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                                             PaintopCategory category,
                                             const QString &curveMinLabel, const QString &curveMaxLabel,
                                             lager::reader<bool> enabledLink,
                                             std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader)
    : KisCurveOptionWidget(optionData,
                            category,
                            curveMinLabel, curveMaxLabel,
                            0, 100, i18n("%"),
                            enabledLink,
                            strengthRangeReader)
{

}

KisCurveOptionWidget::KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                                             PaintopCategory category,
                                             const QString &curveMinLabel, const QString &curveMaxLabel,
                                             int curveMinValue, int curveMaxValue, const QString &curveValueSuffix,
                                             lager::reader<bool> enabledLink,
                                             std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader)
    : KisCurveOptionWidget(optionData,
                            category,
                            curveMinLabel, curveMaxLabel,
                            curveMinValue, curveMaxValue, curveValueSuffix,
                            i18n("Strength: "), i18n("%"), 100.0,
                            enabledLink,
                            strengthRangeReader)
{
}

KisCurveOptionWidget::KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                                             PaintopCategory category,
                                             const QString &curveMinLabel, const QString &curveMaxLabel,
                                             int curveMinValue, int curveMaxValue, const QString &curveValueSuffix,
                                             const QString &strengthPrefix, const QString &strengthSuffix,
                                             qreal strengthDisplayMultiplier,
                                             lager::reader<bool> enabledLink,
                                             std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader)
    : KisCurveOptionWidget(optionData,
                            category,
                            strengthPrefix, strengthSuffix, strengthDisplayMultiplier,
                            enabledLink,
                            strengthRangeReader,
                            KisCurveRangeModel::factory(curveMinLabel,
                                                        curveMaxLabel,
                                                        curveMinValue,
                                                        curveMaxValue,
                                                        curveValueSuffix),
                            KisCurveOptionInputControlsStrategyInt::factory(),
                            {},
                            SupportsCommonCurve | SupportsCurveMode)
{
}

KisCurveOptionWidget::KisCurveOptionWidget(lager::cursor<KisCurveOptionDataCommon> optionData,
                                             PaintopCategory category,
                                             const QString &strengthPrefix, const QString &strengthSuffix,
                                             qreal strengthDisplayMultiplier,
                                             lager::reader<bool> enabledLink,
                                             std::optional<lager::reader<std::tuple<qreal, qreal>>> strengthRangeReader,
                                             KisCurveRangeModelFactory curveRangeModelFactory,
                                             KisCurveOptionInputControlsStrategyFactory inputControlsFactory,
                                             KisCurveOptionRangeControlsStrategyFactory rangeControlsFactory,
                                             Flags flags)
    : KisPaintOpOption(optionData->id.name(), category, optionData.zoom(rowCheckedLens), enabledLink)
    , m_widget(new QWidget)
    , m_curveOptionWidget(new Ui_WdgCurveOption2())
    , m_d(new Private(optionData, enabledLink, strengthRangeReader,
                      strengthDisplayMultiplier,
                      curveRangeModelFactory))
{
    using namespace KisWidgetConnectionUtils;

    setObjectName("KisCurveOptionWidget");

    m_curveOptionWidget->setupUi(m_widget);
    m_curveOptionWidget->sensorSelector->setOptionDataCursor(m_d->model.optionData);

    KIS_SAFE_ASSERT_RECOVER (inputControlsFactory) {
        inputControlsFactory = KisCurveOptionInputControlsStrategyInt::factory();
    }

    m_curveInputControlsStrategy.reset(
        inputControlsFactory(m_d->model.rangeModel.get(),
                             m_curveOptionWidget->curveWidget,
                             m_curveOptionWidget->intInPlaceholder,
                             m_curveOptionWidget->intOutPlaceholder));

    if (rangeControlsFactory) {
        m_curveRangeControlsStrategy.reset(
            rangeControlsFactory(m_d->model.rangeModel.get(),
                                 m_curveOptionWidget->wdgRangeControlsPlaceholder));
    } else {
        m_curveOptionWidget->wdgRangeControlsPlaceholder->setVisible(false);
    }

    setConfigurationPage(m_widget);

    // the eraser's own size or opacity, where size and opacity are set (not the masked brush's)
    const QString optionId = optionData->id.id();
    if (optionData->prefix.isEmpty() && category == KisPaintOpOption::GENERAL
        && (optionId == QLatin1String("Size") || optionId == QLatin1String("Opacity"))) {
        const bool size = optionId == QLatin1String("Size");
        auto *eraser = new QCheckBox(size ? i18nc("@option:check", "Eraser keeps its own size")
                                          : i18nc("@option:check", "Eraser keeps its own opacity"), m_widget);
        eraser->setToolTip(size ? i18nc("@info:tooltip", "Switching to the eraser and back swaps between the two sizes")
                                : i18nc("@info:tooltip", "Switching to the eraser and back swaps between the two opacities"));
        auto show = [eraser, size]() {
            KisConfig cfg(true);
            const QSignalBlocker blocker(eraser);
            eraser->setChecked(size ? cfg.useEraserBrushSize() : cfg.useEraserBrushOpacity());
        };
        show();
        connect(KisConfigNotifier::instance(), &KisConfigNotifier::configChanged, eraser, show);
        connect(eraser, &QCheckBox::toggled, eraser, [size](bool on) {
            KisConfig cfg(false);
            if (size) cfg.setUseEraserBrushSize(on);
            else cfg.setUseEraserBrushOpacity(on);
            KisConfigNotifier::instance()->notifyConfigChanged();
        });
        // usable with the option off too: only the pen's part follows its checkbox
        auto *page = new QWidget();
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 0, 0);
        column->addWidget(m_widget, 1);
        column->addWidget(eraser);
        setCheckedPart(m_widget);
        setConfigurationPage(page);
    }

    connect(&m_d->model, &KisCurveOptionModel::curveXMinLabelChanged,
            m_curveOptionWidget->label_xmin, &QLabel::setText);
    m_curveOptionWidget->label_xmin->setText(m_d->model.curveXMinLabel());

    connect(&m_d->model, &KisCurveOptionModel::curveXMaxLabelChanged,
            m_curveOptionWidget->label_xmax, &QLabel::setText);
    m_curveOptionWidget->label_xmax->setText(m_d->model.curveXMaxLabel());

    connect(&m_d->model, &KisCurveOptionModel::curveYMinLabelChanged,
            m_curveOptionWidget->label_ymin, &QLabel::setText);
    m_curveOptionWidget->label_ymin->setText(m_d->model.curveYMinLabel());

    connect(&m_d->model, &KisCurveOptionModel::curveYMaxLabelChanged,
            m_curveOptionWidget->label_ymax, &QLabel::setText);
    m_curveOptionWidget->label_ymax->setText(m_d->model.curveYMaxLabel());

    m_d->model.activeSensorIdData.bind(
        std::bind(qOverload<const QString&>(&KisMultiSensorsSelector::setCurrent),
                  m_curveOptionWidget->sensorSelector,
                  std::placeholders::_1));
    connect(m_curveOptionWidget->sensorSelector,
            &KisMultiSensorsSelector::highlightedSensorChanged,
            std::bind(&KisCurveOptionModel::setactiveSensorId, &m_d->model, std::placeholders::_1));

    updateThemedIcons();

    // curves picked at a click: Linear, Soft, Firm and your own, each drawn as it is
    for (QWidget *old : {static_cast<QWidget *>(m_curveOptionWidget->linearCurveButton), static_cast<QWidget *>(m_curveOptionWidget->revLinearButton),
                         static_cast<QWidget *>(m_curveOptionWidget->jCurveButton), static_cast<QWidget *>(m_curveOptionWidget->lCurveButton),
                         static_cast<QWidget *>(m_curveOptionWidget->sCurveButton), static_cast<QWidget *>(m_curveOptionWidget->reverseSCurveButton),
                         static_cast<QWidget *>(m_curveOptionWidget->uCurveButton), static_cast<QWidget *>(m_curveOptionWidget->revUCurveButton)}) {
        old->setVisible(false);
    }
    auto *presets = new KisCurvePresetStrip(m_widget);
    m_curveOptionWidget->horizontalLayout_2->insertWidget(0, presets);
    connect(presets, &KisCurvePresetStrip::picked, &m_d->model, &KisCurveOptionModel::setdisplayedCurve);
    connect(&m_d->model, &KisCurveOptionModel::displayedCurveChanged, presets, &KisCurvePresetStrip::setCurrent);
    presets->setCurrent(m_d->model.displayedCurve());

    m_curveOptionWidget->strengthSlider->setPrefix(strengthPrefix);
    m_curveOptionWidget->strengthSlider->setSuffix(strengthSuffix);

    connect(m_curveOptionWidget->strengthSlider, qOverload<qreal>(&KisDoubleSliderSpinBox::valueChanged),
            &m_d->model, &KisCurveOptionModel::setstrengthValueDenorm);

    const bool useFloatingPointStrength = flags & UseFloatingPointStrength;

    if (useFloatingPointStrength) {
        m_curveOptionWidget->strengthSlider->setSingleStep(0.01);
    }

    m_d->model.LAGER_QT(effectiveStrengthStateDenorm).bind(
                kismpl::unzip_wrapper([this, useFloatingPointStrength] (qreal value, qreal min, qreal max) {
                    KisSignalsBlocker b(m_curveOptionWidget->strengthSlider);
                    m_curveOptionWidget->strengthSlider->setRange(min, max, useFloatingPointStrength ? 2 : 0);
                    m_curveOptionWidget->strengthSlider->setValue(value);
                    notifyRowValueChanged();
                }));
    // the strength is the option's row in the list, dragged there
    m_curveOptionWidget->strengthSlider->setVisible(false);

    connectControl(m_curveOptionWidget->curveWidget, &m_d->model, "displayedCurve");

    // no sensor ticked is the pen off: no switch of its own
    m_d->model.LAGER_QT(useCurve).bind(std::bind(&KisCurveOptionWidget::setCurveWidgetsEnabled, this, std::placeholders::_1));
    m_curveOptionWidget->checkBoxUseCurve->setVisible(false);

    if (flags & SupportsCommonCurve) {
        connectControl(m_curveOptionWidget->checkBoxUseSameCurve, &m_d->model, "useSameCurve");
    } else {
        m_curveOptionWidget->checkBoxUseSameCurve->setVisible(false);
    }

    // the sensors' curves multiply: a preset combining them otherwise keeps its way, with no choice shown
    m_curveOptionWidget->curveMode->setVisible(false);
    m_curveOptionWidget->lblCurveMode->setVisible(false);

    m_d->model.optionData.bind(std::bind(&KisCurveOptionWidget::emitSettingChanged, this));
}

KisCurveOptionWidget::~KisCurveOptionWidget()
{
    delete m_curveOptionWidget;
}

void KisCurveOptionWidget::writeOptionSetting(KisPropertiesConfigurationSP setting) const
{
    m_d->model.bakedOptionData().write(setting.data());
}

void KisCurveOptionWidget::readOptionSetting(const KisPropertiesConfigurationSP setting)
{
    KisCurveOptionDataCommon data(*m_d->model.optionData);
    data.read(setting.data());
    m_d->model.optionData.set(data);
}

bool KisCurveOptionWidget::isCheckable() const
{
    // one that is always on is checked when the pen changes it
    return true;
}

std::optional<KisPaintOpOption::RowValue> KisCurveOptionWidget::ownRowValue() const
{
    const KisDoubleSliderSpinBox *slider = m_curveOptionWidget->strengthSlider;
    RowValue value;
    value.value = slider->value();
    value.minimum = slider->minimum();
    value.maximum = slider->maximum();
    value.decimals = slider->decimals();
    value.suffix = slider->suffix();
    value.appliesUnchecked = !m_d->model.isCheckable();
    return value;
}

void KisCurveOptionWidget::setOwnRowValue(qreal value)
{
    m_d->model.setstrengthValueDenorm(value);
}

lager::cursor<qreal> KisCurveOptionWidget::strengthValueDenorm()
{
    return m_d->model.LAGER_QT(strengthValueDenorm);
}

void KisCurveOptionWidget::setCurveWidgetsEnabled(bool value)
{
    m_curveOptionWidget->checkBoxUseSameCurve->setEnabled(value);
    m_curveOptionWidget->curveWidget->setEnabled(value);
    m_curveOptionWidget->sensorSelector->setEnabled(value);
    m_curveOptionWidget->label_xmax->setEnabled(value);
    m_curveOptionWidget->label_xmin->setEnabled(value);
    m_curveOptionWidget->label_ymax->setEnabled(value);
    m_curveOptionWidget->label_ymin->setEnabled(value);
    m_curveOptionWidget->intInPlaceholder->setEnabled(value);
    m_curveOptionWidget->intOutPlaceholder->setEnabled(value);

    m_curveOptionWidget->linearCurveButton->setEnabled(value);
    m_curveOptionWidget->revLinearButton->setEnabled(value);
    m_curveOptionWidget->jCurveButton->setEnabled(value);
    m_curveOptionWidget->lCurveButton->setEnabled(value);
    m_curveOptionWidget->revUCurveButton->setEnabled(value);
    m_curveOptionWidget->reverseSCurveButton->setEnabled(value);
    m_curveOptionWidget->uCurveButton->setEnabled(value);
    m_curveOptionWidget->sCurveButton->setEnabled(value);

    m_curveOptionWidget->curveMode->setEnabled(value);
    m_curveOptionWidget->lblCurveMode->setEnabled(value);

}

QWidget* KisCurveOptionWidget::curveWidget()
{
    return m_widget;
}










void KisCurveOptionWidget::updateThemedIcons()
{
    // this helps make the checkboxes show themselves on the dark color themes
    QPalette pal = m_curveOptionWidget->sensorSelector->palette();
    QPalette newPalette = pal;
    newPalette.setColor(QPalette::Active, QPalette::Window, pal.text().color() );
    m_curveOptionWidget->sensorSelector->setPalette(newPalette);

}
