/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPopupCloser.h"

#include <QApplication>
#include <QEvent>
#include <QTimer>
#include <QWidget>

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
                if (!active || !isInside(active)) m_popup->close();
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
        m_popup->close();
        return onAnchor;
    }
    default:
        break;
    }
    return false;
}
