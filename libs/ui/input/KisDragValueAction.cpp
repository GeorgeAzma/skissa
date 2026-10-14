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
#include "KisStudioSmoothing.h"

namespace
{
/// Horizontal drag distance, in widget pixels, that covers the value's whole range
constexpr qreal fullRange = 320;
}

KisDragValueAction::KisDragValueAction()
    : KisAbstractInputAction("Drag Value")
{
    setName(i18nc("canvas input action", "Drag to Change"));
    setDescription(i18n("Hold and drag to change opacity, flow or smoothing"));
    QHash<QString, int> shortcuts;
    shortcuts.insert(i18nc("canvas input action mode", "Opacity"), OpacityShortcut);
    shortcuts.insert(i18nc("canvas input action mode", "Flow"), FlowShortcut);
    shortcuts.insert(i18nc("canvas input action mode", "Smoothing"), SmoothingShortcut);
    setShortcutIndexes(shortcuts);
}

KisDragValueAction::~KisDragValueAction() = default;

int KisDragValueAction::priority() const
{
    // Same as the brush size action: above painting, below panning
    return 8;
}

KisMainWindow *KisDragValueAction::mainWindow() const
{
    KisCanvas2 *canvas = inputManager()->canvas();
    return canvas && canvas->viewManager() ? canvas->viewManager()->mainWindow() : nullptr;
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
    if (shortcut == SmoothingShortcut) {
        m_startValue = KisStudioSmoothing::amount(mainWindow());
    } else if (KisDoubleSliderSpinBox *s = slider(shortcut)) {
        m_startValue = s->value();
    }
}

void KisDragValueAction::inputEvent(QEvent *event)
{
    if (!event || (event->type() != QEvent::MouseMove && event->type() != QEvent::TabletMove && event->type() != QEvent::TouchUpdate)) {
        return;
    }
    const qreal dx = eventPosF(event).x() - m_start.x();
    if (m_shortcut == SmoothingShortcut) {
        // the brush tool's smoothing, by the same amounts as its chip on the top bar; off at the left end
        if (m_startValue < 0) return;
        KisStudioSmoothing::setAmount(mainWindow(), qBound(0.0, m_startValue + dx / fullRange, 1.0));
        if (KisCanvas2 *canvas = inputManager()->canvas()) {
            canvas->viewManager()->showFloatingMessage(i18nc("the smoothing being dragged, e.g. Smoothing: 50", "Smoothing: %1", KisStudioSmoothing::text(mainWindow())),
                                                       QIcon(), 700, KisFloatingMessage::Low);
        }
        return;
    }
    KisDoubleSliderSpinBox *s = slider(m_shortcut);
    if (!s) return;
    const qreal value = qBound(s->minimum(), m_startValue + dx / fullRange * (s->maximum() - s->minimum()), s->maximum());
    s->setValue(value);
    if (KisCanvas2 *canvas = inputManager()->canvas()) {
        const QString what = m_shortcut == FlowShortcut ? i18nc("brush value", "Flow") : i18nc("brush value", "Opacity");
        canvas->viewManager()->showFloatingMessage(i18nc("a brush value being dragged, e.g. Opacity: 45%", "%1: %2%", what, qRound(value)),
                                                   QIcon(), 700, KisFloatingMessage::Low);
    }
}
