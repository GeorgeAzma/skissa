/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioHoldKeys.h"
#include "KisStudioTools.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPointer>
#include <QTimer>

#include <KoToolManager.h>

#include "KisMainWindow.h"
#include "KisStudioItemCard.h"
#include <KisModernStyle.h>

namespace
{

class HoldKeys : public QObject
{
public:
    explicit HoldKeys(KisMainWindow *mainWindow)
        : QObject(mainWindow)
        , m_mainWindow(mainWindow)
    {
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::TabletPress:
            // a pointer press while the key is down makes it a hold, however short
            if (m_pressedKey) m_used = true;
            m_pointerDown = true;
            break;
        case QEvent::MouseButtonRelease:
        case QEvent::TabletRelease:
            m_pointerDown = false;
            // key released mid-stroke: revert once the stroke ends
            if (m_revertPending) {
                m_revertPending = false;
                revert();
            }
            break;
        case QEvent::WindowDeactivate:
            // another application will receive the key release
            if (watched == m_mainWindow && m_pressedKey) released(0);
            break;
        case QEvent::ShortcutOverride:
            pressed(watched, static_cast<QKeyEvent *>(event));
            break;
        case QEvent::Shortcut:
            // the action the press triggers, whichever form of the key matched ("!" or Shift+1)
            if (auto *action = qobject_cast<QAction *>(watched)) {
                if (!static_cast<QShortcutEvent *>(event)->isAmbiguous()) ran(action);
            }
            break;
        case QEvent::KeyRelease: {
            auto *key = static_cast<QKeyEvent *>(event);
            // match the scan code too: releasing Shift first turns "!" into "1"
            const bool same = key->key() == m_pressedKey || (m_scanCode && key->nativeScanCode() == m_scanCode);
            if (m_pressedKey && !key->isAutoRepeat() && same) released(key->timestamp());
            break;
        }
        default:
            break;
        }
        return false;
    }

private:
    void pressed(QObject *watched, const QKeyEvent *key)
    {
        if (key->isAutoRepeat() || KisStudioShortcuts::isModifierKey(key->key())) return;
        // the same press reaches the focused widget and then its parents; handle it once
        if (m_pressedKey == key->key() && m_since.isValid() && m_since.elapsed() < 50) return;
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || widget->window() != m_mainWindow) return;
        m_pressedKey = key->key();
        m_scanCode = key->nativeScanCode();
        m_since.restart();
        m_pressedAt = key->timestamp();
        m_used = false;
        m_revertPending = false;
        m_action = nullptr;
    }

    void ran(QAction *action)
    {
        if (!m_pressedKey || m_action || action->menu() || m_since.elapsed() > 50) return;
        // a held toggle must not flip on every key repeat
        if (action->isCheckable()) action->setAutoRepeat(false);
        m_action = action;
        m_wasChecked = action->isChecked();
        m_toolBefore = KisStudioTools::activeToolId();
        // the tool the action switches to, known once it has run
        m_toolGiven.clear();
        QTimer::singleShot(0, this, [this]() { m_toolGiven = KisStudioTools::activeToolId(); });
    }

    /// The key was released at \p timestamp (0 if unknown)
    void released(ulong timestamp)
    {
        m_pressedKey = 0;
        m_scanCode = 0;
        if (!m_action) return;
        // use the key events' own times so a slow action doesn't turn a tap into a hold
        const qint64 down = timestamp && m_pressedAt ? qint64(timestamp - m_pressedAt) : m_since.elapsed();
        if (!m_used && (down < KisStudioHoldKeys::holdTime || paintsOnly(m_action))) return;
        // a missed release (drag and drop can swallow it) is not a stroke in progress
        m_pointerDown = m_pointerDown && KisModernStyle::pointerHeld();
        if (m_pointerDown) {
            m_revertPending = true;
            return;
        }
        revert();
    }

    /// A switch that shows nothing until a stroke uses it (the eraser): held without painting, a slow press (a tablet
    /// pad's button) would otherwise be taken back as it is let go
    static bool paintsOnly(const QAction *action)
    {
        static const QStringList names = {QStringLiteral("erase_action"), QStringLiteral("preserve_alpha"),
                                          QStringLiteral("hmirror_action"), QStringLiteral("vmirror_action")};
        return names.contains(action->objectName());
    }

    void revert()
    {
        if (!m_action) return;
        const QString tool = KisStudioTools::activeToolId();
        if (!m_toolBefore.isEmpty() && tool != m_toolBefore) {
            // switch back only if the held tool was used; a tool picked some other way meanwhile stays
            const bool givenKnown = !m_toolGiven.isEmpty() && m_toolGiven != m_toolBefore;
            if (m_used && (!givenKnown || tool == m_toolGiven)) KisStudioTools::switchTo(m_toolBefore);
        } else if (m_action->isCheckable() && m_action->isChecked() != m_wasChecked) {
            m_action->trigger();
        }
    }

    KisMainWindow *m_mainWindow {nullptr};
    int m_pressedKey {0};
    quint32 m_scanCode {0};
    QElapsedTimer m_since;
    ulong m_pressedAt {0};
    /// The pointer was pressed while the key was down
    bool m_used {false};
    bool m_pointerDown {false};
    bool m_revertPending {false};
    QPointer<QAction> m_action;
    bool m_wasChecked {false};
    QString m_toolBefore;
    QString m_toolGiven;
};

} // namespace

void KisStudioHoldKeys::install(KisMainWindow *mainWindow)
{
    qApp->installEventFilter(new HoldKeys(mainWindow));
}
