/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisTouchMode.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QTouchDevice>
#include <QTouchEvent>
#include <QWidget>

namespace {

/// How far a finger may drift and still be holding; less than a drag, so that a drag never turns into a hold
constexpr int holdSlack = 8;

/// The canvas has long presses of its own: painting a dot, say
bool isCanvas(const QWidget *widget)
{
    return widget->inherits("KisOpenGLCanvas2") || widget->inherits("KisQPainterCanvas");
}

} // namespace

KisTouchMode::KisTouchMode()
{
    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(longPressDelay);
    connect(&m_holdTimer, &QTimer::timeout, this, &KisTouchMode::hold);
}

KisTouchMode *KisTouchMode::instance()
{
    static KisTouchMode *mode = new KisTouchMode();
    return mode;
}

void KisTouchMode::install()
{
    static bool installed = false;
    if (!installed && qApp) {
        qApp->installEventFilter(instance());
        installed = true;
    }
}

bool KisTouchMode::isActive()
{
    return instance()->m_active;
}

void KisTouchMode::setActive(bool active)
{
    if (m_active != active) {
        m_active = active;
        Q_EMIT changed(active);
    }
}

bool KisTouchMode::eventFilter(QObject *watched, QEvent *event)
{
    switch (event->type()) {
    case QEvent::TouchBegin: {
        auto *touch = static_cast<QTouchEvent *>(event);
        if (touch->device() && touch->device()->type() == QTouchDevice::TouchScreen) {
            setActive(true);
        }
        break;
    }
    case QEvent::TabletEnterProximity:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
        // a pen hovers like a mouse
        setActive(false);
        break;
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->source() == Qt::MouseEventNotSynthesized) {
            // a mouse that moves is being used; the same position again is not a move
            if (mouse->globalPos() != m_lastMousePos && !m_lastMousePos.isNull()) {
                setActive(false);
            }
            m_lastMousePos = mouse->globalPos();
        }
        if (m_holdTimer.isActive() && (mouse->globalPos() - m_holdPos).manhattanLength() > holdSlack) {
            m_holdTimer.stop();
        }
        break;
    }
    case QEvent::MouseButtonPress: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        QWidget *widget = qobject_cast<QWidget *>(watched);
        m_swallowRelease = false;
        // a finger or a pen; a mouse has a right button
        if (widget && mouse->button() == Qt::LeftButton && mouse->source() != Qt::MouseEventNotSynthesized
            && !m_holdTimer.isActive()) {
            startHold(widget, mouse->globalPos());
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        m_holdTimer.stop();
        // the widget's, not its window's (which keeps track of what is pressed)
        if (m_swallowRelease && watched->isWidgetType() && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            m_swallowRelease = false;
            return true;
        }
        break;
    }
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

void KisTouchMode::startHold(QWidget *widget, const QPoint &globalPos)
{
    for (const QWidget *w = widget; w; w = w->parentWidget()) {
        // held still there means held: a value stepping on, a color being picked
        if (w->property("ownLongPress").toBool() || isCanvas(w) || qobject_cast<const QAbstractSpinBox *>(w)
            || qobject_cast<const QAbstractSlider *>(w) || w->inherits("KisColorSelectorBase")) {
            return;
        }
        if (w->isWindow()) break;
    }
    m_holdWidget = widget;
    m_holdPos = globalPos;
    m_holdTimer.start();
}

void KisTouchMode::hold()
{
    QWidget *widget = m_holdWidget;
    if (!widget || !widget->isVisible()) {
        return;
    }
    // the press is not a click: let it go far away from everything, then ignore the real release
    const QPointF away(-100000, -100000);
    QMouseEvent release(QEvent::MouseButtonRelease, away, away, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &release);
    m_swallowRelease = true;

    if (!m_holdWidget) {
        return;
    }
    QContextMenuEvent menu(QContextMenuEvent::Mouse, m_holdWidget->mapFromGlobal(m_holdPos), m_holdPos);
    QCoreApplication::sendEvent(m_holdWidget, &menu);
}
