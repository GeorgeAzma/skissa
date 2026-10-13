/*
 *  SPDX-FileCopyrightText: 2010 Sven Langkamp <sven.langkamp@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_BRUSH_BASED_PAINTOP_OPTIONS_WIDGET_H
#define KIS_BRUSH_BASED_PAINTOP_OPTIONS_WIDGET_H

#include "kis_paintop_settings_widget.h"
#include "kis_types.h"
#include "kis_brush.h"
#include <kritapaintop_export.h>
#include "KisBrushOptionWidgetFlags.h"

class KisBrushOptionWidget;
class KisSizeOptionWidget;

class PAINTOP_EXPORT KisBrushBasedPaintopOptionWidget : public KisPaintOpSettingsWidget
{
public:
    KisBrushBasedPaintopOptionWidget(KisBrushOptionWidgetFlags flags, QWidget* parent = 0);
    ~KisBrushBasedPaintopOptionWidget() override;

    KisBrushSP brush();

    lager::reader<qreal> effectiveBrushSize() const override;

protected:
    KisBrushOptionWidget *brushOptionWidget() const;
    /// The Size row is the tip's diameter
    void paintOpOptionAdded(KisPaintOpOption *option) override;

private:
    KisBrushOptionWidget *m_brushOption;
    KisSizeOptionWidget *m_sizeOption {nullptr};
};

#endif // KIS_BRUSH_BASED_PAINTOP_OPTIONS_WIDGET_H
