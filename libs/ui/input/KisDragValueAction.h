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

/**
 * A key held and the pen dragged sideways changes one of the brush's values:
 * its opacity or its flow, as the brush size is changed by Shift and a drag
 * (KisChangePrimarySettingAction). Its keys are set on the Keys page.
 */
class KisDragValueAction : public KisAbstractInputAction
{
public:
    enum Shortcut {
        OpacityShortcut,
        FlowShortcut,
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

    int m_shortcut {OpacityShortcut};
    QPointF m_start;
    qreal m_startValue {0};
};

#endif // KISDRAGVALUEACTION_H
