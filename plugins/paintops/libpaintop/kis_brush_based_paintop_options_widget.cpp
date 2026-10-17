/*
 *  SPDX-FileCopyrightText: 2010 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_brush_based_paintop_options_widget.h"

#include <QLocale>

#include "kis_brush_option_widget.h"
#include <klocalizedstring.h>

#include <kis_image_config.h>
#include <KisSizeOptionWidget.h>
#include <KisSpacingOptionWidget.h>

KisBrushBasedPaintopOptionWidget::KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlags flags, QWidget* parent)
    : KisPaintOpSettingsWidget(parent)
{
    m_brushOption = new KisBrushOptionWidget(flags);
    addPaintOpOption(m_brushOption);
}

KisBrushBasedPaintopOptionWidget::~KisBrushBasedPaintopOptionWidget()
{
}

KisBrushSP KisBrushBasedPaintopOptionWidget::brush()
{
    return m_brushOption->brush();
}

lager::reader<qreal> KisBrushBasedPaintopOptionWidget::effectiveBrushSize() const
{
    return m_brushOption->effectiveBrushSize();
}

void KisBrushBasedPaintopOptionWidget::paintOpOptionAdded(KisPaintOpOption *option)
{
    if (auto *spacing = dynamic_cast<KisSpacingOptionWidget *>(option)) {
        if (spacing->category() == KisPaintOpOption::GENERAL && !m_spacingOption) {
            addSpacingRow(spacing);
        }
        return;
    }
    auto *size = dynamic_cast<KisSizeOptionWidget *>(option);
    // the masked brush's Size stays its share of this diameter
    if (!size || size->category() != KisPaintOpOption::GENERAL || m_sizeOption) return;
    m_sizeOption = size;
    lager::cursor<qreal> diameter = m_brushOption->brushSize();
    // the largest size it paints: the diameter, times the Size strength while Size is on (an older
    // preset's; the strength has no slider of its own any more). Nothing in the preset is changed for it.
    auto share = [size]() {
        const qreal strength = size->strength().get();
        return size->isChecked() && strength > 0.0 ? strength : 1.0;
    };
    size->setRowValueSource(
        [diameter, share]() -> std::optional<KisPaintOpOption::RowValue> {
            KisPaintOpOption::RowValue value;
            value.value = diameter.get() * share();
            value.minimum = 1.0;
            value.maximum = KisImageConfig(true).maxBrushSize();
            value.decimals = value.value < 10.0 ? 1 : 0;
            value.suffix = i18nc("brush size in pixels, after the number", " px");
            value.size = true;
            value.appliesUnchecked = true;
            return value;
        },
        [diameter, share](qreal value) mutable { diameter.set(value / share()); });
    connect(m_brushOption, &KisPaintOpOption::sigSettingChanged, size, &KisPaintOpOption::notifyRowValueChanged);
    connect(size, &KisPaintOpOption::sigSettingChanged, size, &KisPaintOpOption::notifyRowValueChanged);
}

void KisBrushBasedPaintopOptionWidget::addSpacingRow(KisSpacingOptionWidget *spacing)
{
    m_spacingOption = spacing;
    lager::cursor<KisBrushModel::CommonData> common = m_brushOption->commonData();
    // the widest spacing it lays dabs at: the tip's, times the Spacing strength while the pen drives it
    auto share = [spacing]() {
        const qreal strength = spacing->strengthValue();
        return spacing->isChecked() && strength > 0.0 ? strength : 1.0;
    };
    spacing->setRowValueSource(
        [common, share]() -> std::optional<KisPaintOpOption::RowValue> {
            const KisBrushModel::CommonData data = common.get();
            KisPaintOpOption::RowValue value;
            value.size = true;
            value.appliesUnchecked = true;
            if (data.useAutoSpacing) {
                // chosen from the brush size, times this factor: Krita's slider is the factor then
                value.value = data.autoSpacingCoeff * share();
                value.minimum = 0.02;
                value.maximum = 10.0;
                value.decimals = 1;
                value.suffix = QStringLiteral("×");
                value.text = i18nc("spacing chosen from the brush size, %1 is how much wider, as 1.0×", "Auto %1",
                                   QLocale().toString(value.value, 'f', 1) + QStringLiteral("×"));
                return value;
            }
            value.value = data.spacing * 100.0 * share();
            value.minimum = 2.0;
            value.maximum = 1000.0;
            value.suffix = i18nc("percent, after the number", "%");
            return value;
        },
        [common, share](qreal value) mutable {
            KisBrushModel::CommonData data = common.get();
            // within the range Krita's own spacing slider has
            if (data.useAutoSpacing) data.autoSpacingCoeff = qBound(0.02, value / share(), 10.0);
            else data.spacing = qBound(0.02, value / 100.0 / share(), 10.0);
            common.set(data);
        });
    connect(m_brushOption, &KisPaintOpOption::sigSettingChanged, spacing, &KisPaintOpOption::notifyRowValueChanged);
    connect(spacing, &KisPaintOpOption::sigSettingChanged, spacing, &KisPaintOpOption::notifyRowValueChanged);
}

KisBrushOptionWidget *KisBrushBasedPaintopOptionWidget::brushOptionWidget() const
{
    return m_brushOption;
}
