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
 * Fingers, as opposed to a mouse or a pen.
 *
 * A finger does not hover, so what the UI shows only under the pointer has
 * to be there while a finger is in use: widgets ask isActive() when they
 * paint and listen to changed(). A finger on the screen turns it on; the
 * mouse moving, or a pen coming near the tablet, turns it off again.
 *
 * And a finger or a pen held still on a widget for half a second is a right
 * click there: the widget gets a context menu event (a card, a menu, a
 * rename...), and the press that started it does not become a click. A
 * widget that does something else on a long press (moving the item held,
 * say) sets the property ownLongPress to true, and is left alone.
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

    QTimer m_holdTimer;
    QPointer<QWidget> m_holdWidget;
    QPoint m_holdPos;
    /// The hold became a right click: the release that follows is not a click
    bool m_swallowRelease {false};
};

#endif // KISTOUCHMODE_H
