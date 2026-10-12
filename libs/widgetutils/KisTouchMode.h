/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISTOUCHMODE_H
#define KISTOUCHMODE_H

#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QTimer>

#include "kritawidgetutils_export.h"

class QWidget;

/**
 * Fingers, as opposed to a mouse or a pen: a finger on the screen turns it on, the mouse moving or a pen near
 * turns it off. A finger does not hover, so widgets show what is otherwise shown under the pointer while it is on.
 *
 * A finger held still on a widget is a right click there, unless the widget sets the property ownLongPress.
 */
class KRITAWIDGETUTILS_EXPORT KisTouchMode : public QObject
{
    Q_OBJECT
public:
    static KisTouchMode *instance();
    /// Watches the application's input; once is enough, more does nothing
    static void install();
    static bool isActive();

    /// How long a hold is, in ms
    static constexpr int longPressDelay = 480;

Q_SIGNALS:
    void changed(bool active);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    KisTouchMode();
    void setActive(bool active);
    void startHold(QWidget *widget, const QPoint &globalPos);
    void hold();

    bool m_active {false};
    QPoint m_lastMousePos;
    /// Krita's own long press is on: it makes the context menu instead
    bool m_kritaLongPress {false};

    QTimer m_holdTimer;
    QPointer<QWidget> m_holdWidget;
    QPoint m_holdPos;
    /// The hold became a right click: the release that follows is not a click
    bool m_swallowRelease {false};
};

#endif // KISTOUCHMODE_H
