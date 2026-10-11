/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisDragValueAction.h"

#include <QApplication>
#include <QEvent>

#include <klocalizedstring.h>
#include <kis_slider_spin_box.h>

#include "KisViewManager.h"
#include "kis_canvas2.h"
#include "kis_input_manager.h"
#include "kis_paintop_box.h"

namespace
{
/// The pen dragged this far (widget pixels) sideways goes through the whole range of the value
constexpr qreal fullRange = 320;
}

KisDragValueAction::KisDragValueAction()
    : KisAbstractInputAction("Drag Value")
{
    setName(i18nc("canvas input action", "Drag to Change"));
    setDescription(i18n("Hold and drag to change opacity or flow"));
    QHash<QString, int> shortcuts;
    shortcuts.insert(i18nc("canvas input action mode", "Opacity"), OpacityShortcut);
    shortcuts.insert(i18nc("canvas input action mode", "Flow"), FlowShortcut);
    setShortcutIndexes(shortcuts);
}

KisDragValueAction::~KisDragValueAction() = default;

int KisDragValueAction::priority() const
{
    // as the brush size's: over painting, under panning
    return 8;
}

KisDoubleSliderSpinBox *KisDragValueAction::slider(int shortcut) const
{
    KisCanvas2 *canvas = inputManager()->canvas();
    KisPaintopBox *box = canvas && canvas->viewManager() ? canvas->viewManager()->paintOpBox() : nullptr;
    if (!box) return nullptr;
    return box->brushSlider(shortcut == FlowShortcut ? QStringLiteral("flow") : QStringLiteral("opacity"));
}

void KisDragValueAction::activate(int shortcut)
{
    Q_UNUSED(shortcut);
    QApplication::setOverrideCursor(Qt::SizeHorCursor);
}

void KisDragValueAction::deactivate(int shortcut)
{
    Q_UNUSED(shortcut);
    QApplication::restoreOverrideCursor();
}

void KisDragValueAction::begin(int shortcut, QEvent *event)
{
    KisAbstractInputAction::begin(shortcut, event);
    m_shortcut = shortcut;
    m_start = event ? eventPosF(event) : QPointF();
    if (KisDoubleSliderSpinBox *s = slider(shortcut)) m_startValue = s->value();
}

void KisDragValueAction::inputEvent(QEvent *event)
{
    if (!event || (event->type() != QEvent::MouseMove && event->type() != QEvent::TabletMove && event->type() != QEvent::TouchUpdate)) {
        return;
    }
    KisDoubleSliderSpinBox *s = slider(m_shortcut);
    if (!s) return;
    const qreal dx = eventPosF(event).x() - m_start.x();
    const qreal value = qBound(s->minimum(), m_startValue + dx / fullRange * (s->maximum() - s->minimum()), s->maximum());
    s->setValue(value);
    if (KisCanvas2 *canvas = inputManager()->canvas()) {
        const QString what = m_shortcut == FlowShortcut ? i18nc("brush value", "Flow") : i18nc("brush value", "Opacity");
        canvas->viewManager()->showFloatingMessage(i18nc("a brush value being dragged, e.g. Opacity: 45%", "%1: %2%", what, qRound(value)),
                                                   QIcon(), 700, KisFloatingMessage::Low);
    }
}
