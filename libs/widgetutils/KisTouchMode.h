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
 * Tracks whether the user is working with fingers rather than a mouse or pen. A touch turns it on; mouse
 * movement or a nearby pen turns it off. Fingers can't hover, so widgets show their hover-only controls
 * while it is on.
 *
 * A long press opens the widget's context menu, unless the widget sets the "ownLongPress" property.
 */
class KRITAWIDGETUTILS_EXPORT KisTouchMode : public QObject
{
    Q_OBJECT
public:
    static KisTouchMode *instance();
    /// Starts watching the application's input. Further calls do nothing.
    static void install();
    static bool isActive();

    /// Long-press duration in ms.
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
    /// Krita's own long-press setting is on, which opens the context menu instead.
    bool m_kritaLongPress {false};

    QTimer m_holdTimer;
    QPointer<QWidget> m_holdWidget;
    QPoint m_holdPos;
    /// A long press opened a menu, so the following release must not click.
    bool m_swallowRelease {false};
};

#endif // KISTOUCHMODE_H
