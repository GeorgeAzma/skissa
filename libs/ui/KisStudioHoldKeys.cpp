/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioHoldKeys.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QPointer>
#include <QWidgetAction>

#include <KoToolManager.h>
#include <kactioncollection.h>
#include <kxmlguiclient.h>
#include <kxmlguifactory.h>

#include "KisMainWindow.h"

namespace
{

bool isModifierKey(int key)
{
    return key == Qt::Key_Control || key == Qt::Key_Shift || key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}

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
        const QEvent::Type type = event->type();
        if (type != QEvent::ShortcutOverride && type != QEvent::KeyRelease) {
            return false;
        }
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->isAutoRepeat() || isModifierKey(key->key())) {
            return false;
        }

        if (type == QEvent::ShortcutOverride) {
            // the same press goes to the focused widget and on to its parents: seen once
            if (m_pressedKey == key->key() && m_since.isValid() && m_since.elapsed() < 50) return false;
            auto *widget = qobject_cast<QWidget *>(watched);
            if (!widget || widget->window() != m_mainWindow) return false;
            QAction *action = actionFor(QKeySequence(int(key->modifiers() & ~Qt::KeypadModifier) | key->key()));
            m_pressedKey = 0;
            if (!action) return false;
            // a toggle held is not flipped over and over by the key's repeats
            if (action->isCheckable()) action->setAutoRepeat(false);
            m_pressedKey = key->key();
            m_since.restart();
            m_action = action;
            m_wasChecked = action->isChecked();
            m_toolBefore = KoToolManager::instance()->activeToolId();
            return false;
        }

        // let go: held, what it turned on lasts no longer
        if (key->key() != m_pressedKey) return false;
        m_pressedKey = 0;
        if (!m_action || m_since.elapsed() < KisStudioHoldKeys::holdTime) return false;
        const QString tool = KoToolManager::instance()->activeToolId();
        if (!m_toolBefore.isEmpty() && tool != m_toolBefore) {
            // the tool it gave, given back
            KoToolManager::instance()->switchToolRequested(m_toolBefore);
        } else if (m_action->isCheckable() && m_action->isChecked() != m_wasChecked) {
            m_action->trigger();
        }
        return false;
    }

private:
    /// The action of the window whose shortcut \p sequence is (of one key and its modifiers)
    QAction *actionFor(const QKeySequence &sequence) const
    {
        QList<KisKActionCollection *> collections {m_mainWindow->actionCollection()};
        if (m_mainWindow->guiFactory()) {
            Q_FOREACH (KisKXMLGUIClient *client, m_mainWindow->guiFactory()->clients()) {
                if (client && client->actionCollection() && !collections.contains(client->actionCollection())) {
                    collections << client->actionCollection();
                }
            }
        }
        QList<QAction *> actions = m_mainWindow->actions();
        Q_FOREACH (KisKActionCollection *collection, collections) actions += collection->actions();
        Q_FOREACH (QAction *action, actions) {
            if (!action || !action->isEnabled() || action->menu()) continue;
            Q_FOREACH (const QKeySequence &shortcut, action->shortcuts()) {
                if (shortcut == sequence) return action;
            }
        }
        return nullptr;
    }

    KisMainWindow *m_mainWindow {nullptr};
    int m_pressedKey {0};
    QElapsedTimer m_since;
    QPointer<QAction> m_action;
    bool m_wasChecked {false};
    QString m_toolBefore;
};

} // namespace

void KisStudioHoldKeys::install(KisMainWindow *mainWindow)
{
    qApp->installEventFilter(new HoldKeys(mainWindow));
}
