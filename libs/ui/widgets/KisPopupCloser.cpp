/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPopupCloser.h"

#include <QAbstractButton>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QTabletEvent>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <KisModernStyle.h>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{

/**
 * How far from the press that opened a popup from \p anchor a press is still on what opened it: a button is one thing
 * wherever it is pressed (-1), a surface (a list's viewport, a panel) opens popups for a place on it, so a press
 * elsewhere on it is about something else
 */
int reachOn(const QWidget *anchor, const QInputEvent *opening)
{
    if (!anchor || qobject_cast<const QAbstractButton *>(anchor)) return -1;
    const auto *area = qobject_cast<const QAbstractScrollArea *>(anchor->parentWidget());
    const bool surface = (area && area->viewport() == anchor) || !anchor->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly).isEmpty();
    return surface ? KisModernStyle::dragDistance(opening) : -1;
}

bool closeTo(const QPoint &global, const QPoint &at, int reach)
{
    return reach < 0 || (global - at).manhattanLength() <= reach;
}

QPoint globalPosOf(const QEvent *event)
{
    if (event->type() == QEvent::TabletPress) return static_cast<const QTabletEvent *>(event)->globalPos();
    return static_cast<const QMouseEvent *>(event)->globalPos();
}

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

    /// Only a press within \p reach of \p at counts, unless \p reach is -1
    void arm(QWidget *anchor, const QPoint &at = QPoint(), int reach = -1)
    {
        m_anchor = anchor;
        m_at = at;
        m_reach = reach;
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
        if (!closeTo(globalPosOf(event), m_at, m_reach)) return false;
        // the mouse press made from a tablet press comes next
        if (event->type() == QEvent::MouseButtonPress) m_anchor = nullptr;
        return true;
    }

private:
    using QObject::QObject;

    QPointer<QWidget> m_anchor;
    QPoint m_at;
    int m_reach {-1};
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
        // a message over the canvas (mirrored, rotated) is no popup: the next press on its button acts again
        if (widget->windowFlags().testFlag(Qt::WindowTransparentForInput)) return false;
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
            return pressed(qobject_cast<QWidget *>(watched), event);
        case QEvent::MouseButtonRelease:
            if (m_swallowRelease && watched == m_swallowRelease) {
                m_swallowRelease = nullptr;
                return true;
            }
            break;
        case QEvent::Show: {
            auto *popup = qobject_cast<QWidget *>(watched);
            if (isPopup(popup)) own(popup);
            if (!isPopup(popup) || !m_pressed || m_pressedAt.elapsed() > 800) break;
            forget(popup);
            m_open.append({popup, m_pressed, m_pressedPos, m_pressedReach});
            break;
        }
        case QEvent::Hide: {
            auto *popup = qobject_cast<QWidget *>(watched);
            if (!isPopup(popup)) break;
            disown(popup);
            for (const Opened &opened : qAsConst(m_open)) {
                if (opened.popup != popup || !closedFrom(opened)) continue;
                m_closed = opened;
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
    /**
     * A popup of the Studio's (a tool window: true popups are black on Windows) opened from another one is owned by
     * it, which Windows keeps it above: owned only by the main window like its opener, a click on the opener, or
     * its activation, put the opener over it.
     */
    void own(QWidget *popup)
    {
        if (popup->windowType() != Qt::Tool) return;
        QWidget *opener = m_pressed && m_pressedAt.elapsed() <= 800 ? m_pressed->window() : QApplication::activeWindow();
        if (!opener || opener == popup || !isPopup(opener) || opener->windowType() != Qt::Tool || popup->isAncestorOf(opener)) return;
        QWindow *handle = popup->windowHandle();
        QWindow *above = opener->windowHandle();
        if (!handle || !above || handle->transientParent() == above) return;
        m_owned.append({popup, opener, handle->transientParent()});
        handle->setTransientParent(above);
    }

    /// \p widget hid: what it owns is the original owner's again (Windows destroys what an owner owns with it), and
    /// it is too if it was owned
    void disown(QWidget *widget)
    {
        for (int i = m_owned.size() - 1; i >= 0; i--) {
            const Owned owned = m_owned[i];
            if (owned.popup && owned.popup != widget && owned.opener != widget) continue;
            if (owned.popup && owned.popup->windowHandle() && owned.original) owned.popup->windowHandle()->setTransientParent(owned.original);
            m_owned.removeAt(i);
        }
    }

    struct Owned {
        QPointer<QWidget> popup;
        QPointer<QWidget> opener;
        QPointer<QWindow> original;
    };
    QVector<Owned> m_owned;

    struct Opened {
        QPointer<QWidget> popup;
        QPointer<QWidget> anchor;
        /// Where the press that opened it was, and how near it a press is still on its anchor (reachOn)
        QPoint at;
        int reach {-1};
    };

    static bool within(const QWidget *anchor, const QWidget *widget) { return anchor && widget && (widget == anchor || anchor->isAncestorOf(widget)); }

    static bool over(const QWidget *anchor, const QPoint &global)
    {
        return anchor && anchor->isVisible() && anchor->rect().contains(anchor->mapFromGlobal(global));
    }

    /// Whether a popup closing now was closed by a press on \p anchor: the press just made (a menu closes on it, and
    /// Qt then gives it to what is under it), or one on its way (the window took the focus back first). A popup that
    /// closed otherwise (a choice made in it, a press elsewhere) leaves the next press on its anchor alone.
    bool closedFrom(const Opened &opened) const
    {
        const QWidget *anchor = opened.anchor;
        if (!anchor) return false;
        if (m_lastPressAt.isValid() && m_lastPressAt.elapsed() < 100 && over(anchor, m_lastPressPos)) {
            return closeTo(m_lastPressPos, opened.at, opened.reach);
        }
#ifdef Q_OS_WIN
        return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) && over(anchor, QCursor::pos()) && closeTo(QCursor::pos(), opened.at, opened.reach);
#else
        return false;
#endif
    }

    bool pressed(QWidget *widget, QEvent *event)
    {
        if (!widget) return false;
        const QEvent::Type type = event->type();
        m_lastPressPos = type == QEvent::TabletPress ? static_cast<QTabletEvent *>(event)->globalPos() : static_cast<QMouseEvent *>(event)->globalPos();
        m_lastPressAt.start();
        // a quick press is a press and then a double click, before its release: when the press was swallowed, so is that
        if (type == QEvent::MouseButtonDblClick && m_swallowRelease && within(m_swallowRelease, widget)) return true;
        // on what opened a popup still open: it closes
        for (const Opened &opened : qAsConst(m_open)) {
            if (opened.popup && opened.popup->isVisible() && within(opened.anchor, widget)) {
                // elsewhere on a surface it only closes, and the press goes on to what it is on
                if (closeTo(m_lastPressPos, opened.at, opened.reach)) {
                    m_closed = opened;
                    m_closedAt.start();
                }
                opened.popup->close();
                break;
            }
        }
        // on what opened a popup this press (or one just before) closed: nothing more
        if (m_closed.anchor && m_closedAt.elapsed() < 150 && within(m_closed.anchor, widget) && closeTo(m_lastPressPos, m_closed.at, m_closed.reach)) {
            // the mouse press made from a tablet press comes next
            if (type != QEvent::TabletPress) {
                m_swallowRelease = widget;
                m_closed = Opened();
            }
            return true;
        }
        // not a menu's item: a menu opened from it (a submenu) belongs to the menu
        if (!qobject_cast<QMenu *>(widget->window())) {
            m_pressed = widget;
            m_pressedPos = m_lastPressPos;
            m_pressedReach = reachOn(widget, static_cast<QInputEvent *>(event));
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
    QPoint m_pressedPos;
    int m_pressedReach {-1};
    QElapsedTimer m_pressedAt;
    /// What a press on its anchor closed last
    Opened m_closed;
    QElapsedTimer m_closedAt;
    QPointer<QWidget> m_swallowRelease;
    QPoint m_lastPressPos;
    QElapsedTimer m_lastPressAt;
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
    case QEvent::Show:
        if (watched == m_popup) {
            // the press that opened it was there, or close by
            m_shownAt = QCursor::pos();
            m_reach = reachOn(m_anchor, nullptr);
        }
        break;
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
                    AnchorGuard::instance()->arm(m_anchor, m_shownAt, m_reach);
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
        const bool onAnchor = m_anchor && (widget == m_anchor || m_anchor->isAncestorOf(widget)) && closeTo(globalPosOf(event), m_shownAt, m_reach);
        // the mouse press Windows makes from a pen's comes after the popup is gone: it must not open it again
        if (onAnchor && event->type() == QEvent::TabletPress) AnchorGuard::instance()->arm(m_anchor, m_shownAt, m_reach);
        m_popup->close();
        return onAnchor;
    }
    default:
        break;
    }
    return false;
}
