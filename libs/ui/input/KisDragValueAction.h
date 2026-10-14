/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISDRAGVALUEACTION_H
#define KISDRAGVALUEACTION_H

#include "kis_abstract_input_action.h"

#include <QPointF>

class KisDoubleSliderSpinBox;
class KisMainWindow;

/**
 * Holding a key and dragging sideways changes the brush opacity, flow or line smoothing,
 * the same way Shift+drag changes the brush size (KisChangePrimarySettingAction).
 */
class KisDragValueAction : public KisAbstractInputAction
{
public:
    enum Shortcut {
        OpacityShortcut,
        FlowShortcut,
        SmoothingShortcut,
    };

    KisDragValueAction();
    ~KisDragValueAction() override;

    int priority() const override;
    void activate(int shortcut) override;
    void deactivate(int shortcut) override;
    void begin(int shortcut, QEvent *event) override;
    void inputEvent(QEvent *event) override;

private:
    KisDoubleSliderSpinBox *slider(int shortcut) const;
    KisMainWindow *mainWindow() const;

    int m_shortcut {OpacityShortcut};
    QPointF m_start;
    qreal m_startValue {0};
};

#endif // KISDRAGVALUEACTION_H
