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

/**
 * Any popup or menu opened by a press on something is closed by a press on it again, which then does nothing:
 * the press isn't replayed to it (as Qt does for menus) to open the popup again. What opened a popup is what
 * was pressed just before it showed.
 */
class PopupToggle : public QObject
{
public:
    static bool isPopup(const QWidget *widget)
    {
        if (!widget || !widget->isWindow()) return false;
        const Qt::WindowType type = widget->windowType();
        return type == Qt::Popup || (type == Qt::Tool && widget->windowFlags().testFlag(Qt::FramelessWindowHint));
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonDblClick:
        case QEvent::TabletPress:
            return pressed(qobject_cast<QWidget *>(watched), event->type());
        case QEvent::MouseButtonRelease:
            if (m_swallowRelease && watched == m_swallowRelease) {
                m_swallowRelease = nullptr;
                return true;
            }
            break;
        case QEvent::Show: {
            auto *popup = qobject_cast<QWidget *>(watched);
            if (!isPopup(popup) || !m_pressed || m_pressedAt.elapsed() > 800 || isPopup(m_pressed->window())) break;
            forget(popup);
            m_open.append({popup, m_pressed});
            break;
        }
        case QEvent::Hide: {
            auto *popup = qobject_cast<QWidget *>(watched);
            if (!isPopup(popup)) break;
            for (const Opened &opened : qAsConst(m_open)) {
                if (opened.popup != popup || !opened.anchor) continue;
                m_closedAnchor = opened.anchor;
                m_closedAt.start();
            }
            forget(popup);
            break;
        }
        default:
            break;
        }
        return false;
    }

private:
    struct Opened {
        QPointer<QWidget> popup;
        QPointer<QWidget> anchor;
    };

    static bool within(const QWidget *anchor, const QWidget *widget) { return anchor && widget && (widget == anchor || anchor->isAncestorOf(widget)); }

    bool pressed(QWidget *widget, QEvent::Type type)
    {
        if (!widget) return false;
        // on what opened a popup still open: it closes
        for (const Opened &opened : qAsConst(m_open)) {
            if (opened.popup && opened.popup->isVisible() && within(opened.anchor, widget)) {
                m_closedAnchor = opened.anchor;
                m_closedAt.start();
                opened.popup->close();
                break;
            }
        }
        // on what opened a popup this press (or one just before) closed: nothing more
        if (m_closedAnchor && m_closedAt.elapsed() < 400 && within(m_closedAnchor, widget)) {
            // the mouse press made from a tablet press comes next
            if (type != QEvent::TabletPress) {
                m_swallowRelease = widget;
                m_closedAnchor = nullptr;
            }
            return true;
        }
        if (type != QEvent::MouseButtonDblClick && !isPopup(widget->window())) {
            m_pressed = widget;
            m_pressedAt.start();
        }
        return false;
    }

    void forget(QWidget *popup)
    {
        for (int i = m_open.size() - 1; i >= 0; i--) {
            if (!m_open[i].popup || m_open[i].popup == popup) m_open.removeAt(i);
        }
    }

    QVector<Opened> m_open;
    QPointer<QWidget> m_pressed;
    QElapsedTimer m_pressedAt;
    QPointer<QWidget> m_closedAnchor;
    QElapsedTimer m_closedAt;
    QPointer<QWidget> m_swallowRelease;
};

} // namespace

void KisPopupCloser::installToggle()
{
    static PopupToggle *toggle = nullptr;
    if (toggle) return;
    toggle = new PopupToggle();
    toggle->setParent(qApp);
    qApp->installEventFilter(toggle);
}

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
