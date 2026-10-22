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
#include <QDateTime>
#include <QMouseEvent>
#include <QTouchDevice>
#include <QTouchEvent>
#include <QWidget>

#include <kconfiggroup.h>
#include <ksharedconfig.h>

#include "kis_config_notifier.h"

namespace {

/// How far a finger may drift during a long press; below the drag distance so a drag never becomes one.
constexpr int holdSlack = 8;

/// The canvas handles long presses itself, e.g. painting a dot.
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

    // When KisLongPressEventFilter is enabled it opens the menu, so don't do it twice.
#ifdef Q_OS_ANDROID
    const bool longPressDefault = true;
#else
    const bool longPressDefault = false;
#endif
    m_kritaLongPress = KSharedConfig::openConfig()->group("").readEntry("longPressEnabled", longPressDefault);
    connect(KisConfigNotifier::instance(), &KisConfigNotifier::sigLongPressChanged, this, [this](bool enabled) {
        m_kritaLongPress = enabled;
    });
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
        // fingers on the canvas pan and zoom there, also between pen strokes on a pen display: only a touch on
        // the interface makes it touch-friendly (each switch restyles every widget)
        bool onCanvas = false;
        for (QObject *object = watched; object && !onCanvas; object = object->parent()) {
            onCanvas = object->inherits("KisOpenGLCanvas2") || object->inherits("KisQPainterCanvas");
        }
        if (!onCanvas && touch->device() && touch->device()->type() == QTouchDevice::TouchScreen) {
            setActive(true);
        }
        break;
    }
    case QEvent::TabletEnterProximity:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
        // A pen hovers like a mouse.
        setActive(false);
        break;
    case QEvent::MouseMove: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->source() == Qt::MouseEventNotSynthesized) {
            // Ignore move events that repeat the last position.
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
        // Only synthesized presses come from fingers; pens synthesize too but have a side button.
        if (widget && m_active && !m_kritaLongPress && mouse->button() == Qt::LeftButton
            && mouse->source() != Qt::MouseEventNotSynthesized && !m_holdTimer.isActive()) {
            startHold(widget, mouse->globalPos());
        }
        break;
    }
    case QEvent::MouseButtonRelease: {
        m_holdTimer.stop();
        // Swallow it at the widget, not the window, which tracks the pressed buttons.
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
        // Widgets where holding has its own meaning, like auto-repeat or color picking.
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
    // Release far away so the press doesn't click, then swallow the real release.
    const QPointF away(-100000, -100000);
    QMouseEvent release(QEvent::MouseButtonRelease, away, away, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &release);
    m_swallowRelease = true;

    if (!m_holdWidget) {
        return;
    }
    // as Krita's own long press marks it, so the menu can tell a held finger from a right click
    qApp->setProperty("longPressAt", QDateTime::currentMSecsSinceEpoch());
    QContextMenuEvent menu(QContextMenuEvent::Mouse, m_holdWidget->mapFromGlobal(m_holdPos), m_holdPos);
    QCoreApplication::sendEvent(m_holdWidget, &menu);
}
