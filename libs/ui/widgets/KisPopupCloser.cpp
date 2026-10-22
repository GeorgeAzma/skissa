/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPopupCloser.h"

#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEvent>
#include <QTimer>
#include <QWidget>

namespace
{

/**
 * When the window takes the focus back before the press that did it arrives, the popup has
 * already closed: the press on its anchor that follows is swallowed all the same.
 */
class AnchorGuard : public QObject
{
public:
    static AnchorGuard *instance()
    {
        static AnchorGuard *guard = []() {
            auto *made = new AnchorGuard(qApp);
            qApp->installEventFilter(made);
            return made;
        }();
        return guard;
    }

    void arm(QWidget *anchor)
    {
        m_anchor = anchor;
        m_since.start();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_anchor || (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::TabletPress)) return false;
        if (m_since.elapsed() > 400) {
            m_anchor = nullptr;
            return false;
        }
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget || (widget != m_anchor && !m_anchor->isAncestorOf(widget))) return false;
        // the mouse press made from a tablet press comes next
        if (event->type() == QEvent::MouseButtonPress) m_anchor = nullptr;
        return true;
    }

private:
    using QObject::QObject;

    QPointer<QWidget> m_anchor;
    QElapsedTimer m_since;
};

} // namespace

KisPopupCloser::KisPopupCloser(QWidget *popup, QWidget *anchor)
    : QObject(popup)
    , m_popup(popup)
    , m_anchor(anchor)
{
    qApp->installEventFilter(this);
}

bool KisPopupCloser::isInside(QWidget *widget) const
{
    // Menus opened from the popup are separate windows but still count as inside.
    for (QWidget *window = widget->window(); window; window = window->parentWidget() ? window->parentWidget()->window() : nullptr) {
        if (window == m_popup) {
            return true;
        }
    }
    return false;
}

bool KisPopupCloser::eventFilter(QObject *watched, QEvent *event)
{
    if (!m_popup->isVisible() || !m_enabled) {
        m_wasActive = false;
        return false;
    }

    switch (event->type()) {
    case QEvent::WindowActivate:
        if (watched == m_popup) {
            m_wasActive = true;
        }
        break;
    case QEvent::WindowDeactivate:
        // Ignore deactivation if the popup was never active, e.g. shown while another app had focus.
        if (watched == m_popup && m_wasActive) {
            // not when a popup opened from it took the focus: it counts as inside
            QTimer::singleShot(0, m_popup, [this]() {
                QWidget *active = QApplication::activeWindow();
                if (active && isInside(active)) return;
                if (m_anchor && m_anchor->isVisible() && m_anchor->rect().contains(m_anchor->mapFromGlobal(QCursor::pos()))) {
                    AnchorGuard::instance()->arm(m_anchor);
                }
                m_popup->close();
            });
        }
        break;
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress: {
        QWidget *widget = qobject_cast<QWidget *>(watched);
        if (!widget || isInside(widget)) {
            break;
        }
        const bool onAnchor = m_anchor && (widget == m_anchor || m_anchor->isAncestorOf(widget));
        // the mouse press Windows makes from a pen's comes after the popup is gone: it must not open it again
        if (onAnchor && event->type() == QEvent::TabletPress) AnchorGuard::instance()->arm(m_anchor);
        m_popup->close();
        return onAnchor;
    }
    default:
        break;
    }
    return false;
}
