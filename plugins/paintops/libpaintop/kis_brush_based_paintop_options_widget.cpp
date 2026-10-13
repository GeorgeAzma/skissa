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

KisBrushOptionWidget *KisBrushBasedPaintopOptionWidget::brushOptionWidget() const
{
    return m_brushOption;
}
