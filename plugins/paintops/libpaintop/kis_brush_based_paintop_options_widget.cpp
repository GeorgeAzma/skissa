/*
 *  SPDX-FileCopyrightText: 2010 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_brush_based_paintop_options_widget.h"

#include "kis_brush_option_widget.h"
#include <klocalizedstring.h>

#include <kis_image_config.h>
#include <KisSizeOptionWidget.h>

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
    auto *size = dynamic_cast<KisSizeOptionWidget *>(option);
    // the masked brush's Size stays its share of this diameter
    if (!size || size->category() != KisPaintOpOption::GENERAL || m_sizeOption) return;
    m_sizeOption = size;
    lager::cursor<qreal> diameter = m_brushOption->brushSize();
    size->setRowValueSource(
        [diameter]() -> std::optional<KisPaintOpOption::RowValue> {
            KisPaintOpOption::RowValue value;
            value.value = diameter.get();
            value.minimum = 1.0;
            value.maximum = KisImageConfig(true).maxBrushSize();
            value.decimals = value.value < 10.0 ? 1 : 0;
            value.suffix = i18nc("brush size in pixels, after the number", " px");
            value.size = true;
            value.appliesUnchecked = true;
            return value;
        },
        [diameter](qreal value) mutable { diameter.set(value); });
    connect(m_brushOption, &KisPaintOpOption::sigSettingChanged, size, &KisPaintOpOption::notifyRowValueChanged);
}

void KisBrushBasedPaintopOptionWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    KisPaintOpSettingsWidget::setConfiguration(config);

    // the Size row is the diameter: a Size strength of an older preset becomes part of it
    if (!m_sizeOption) return;
    lager::cursor<qreal> strength = m_sizeOption->strength();
    if (strength.get() > 0.0 && strength.get() < 1.0 - 1e-6) {
        lager::cursor<qreal> diameter = m_brushOption->brushSize();
        diameter.set(diameter.get() * strength.get());
        strength.set(1.0);
    }
}

KisBrushOptionWidget *KisBrushBasedPaintopOptionWidget::brushOptionWidget() const
{
    return m_brushOption;
}
