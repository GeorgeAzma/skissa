/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioPopup.h"

#include <QGuiApplication>
#include <QKeyEvent>
#include <QLayout>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

#include <KisModernStyle.h>

#include "KisPopupCloser.h"
#include "KisStudioGlass.h"
#include "KisStudioMotion.h"

namespace {
constexpr int glassShadowMargin = 16;
constexpr int gap = 8;
}

KisStudioPopup::KisStudioPopup(QWidget *parent)
    // not Qt::Popup on Windows, where translucent popups get a black background (see KisStudioGlass::popupWindowType)
    : QFrame(parent, KisStudioGlass::popupWindowType() | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground, KisStudioGlass::seeThroughWindows());
    setAttribute(Qt::WA_DeleteOnClose);
}

KisStudioPopup::~KisStudioPopup()
{
}

int KisStudioPopup::shadowMargin() const
{
    return testAttribute(Qt::WA_TranslucentBackground) ? glassShadowMargin : 0;
}

void KisStudioPopup::fitLayout(QLayout *layout, int padding)
{
    const int margin = shadowMargin() + padding;
    layout->setContentsMargins(margin, margin, margin, margin);
}

int KisStudioPopup::heightRoom() const
{
    QScreen *screen = m_anchor ? m_anchor->screen() : (this->screen() ? this->screen() : QGuiApplication::primaryScreen());
    if (!screen) return QWIDGETSIZE_MAX;
    QRect available = screen->availableGeometry();
    if (!m_anchor || (m_edge != Qt::TopEdge && m_edge != Qt::BottomEdge)) return available.height() + 2 * shadowMargin();
    // within the window it belongs to, when the window is on this screen
    const QWidget *window = m_anchor->window();
    const QRect windowRect(window->mapToGlobal(QPoint(0, 0)), window->size());
    if (available.intersects(windowRect)) available &= windowRect;
    const QRect around(m_anchor->mapToGlobal(QPoint(0, 0)), m_anchor->size());
    const int below = available.bottom() - around.bottom() - gap;
    const int above = around.top() - gap - available.top();
    return qMax(below, above) + 2 * shadowMargin();
}

void KisStudioPopup::popupBeside(QWidget *anchor, Qt::Edge edge)
{
    m_anchor = anchor;
    m_edge = edge;
    fitRoom();
    adjustSize();
    const QSize panel = size() - QSize(2 * shadowMargin(), 2 * shadowMargin());
    const QRect around(anchor->mapToGlobal(QPoint(0, 0)), anchor->size());
    QScreen *screen = anchor->screen();
    const QRect available = screen ? screen->availableGeometry() : QRect(around.topLeft() - QPoint(5000, 5000), QSize(10000, 10000));

    QPoint at;
    switch (edge) {
    case Qt::RightEdge:
    case Qt::LeftEdge: {
        const bool right = edge == Qt::RightEdge ? around.right() + gap + panel.width() <= available.right()
                                                 : around.left() - gap - panel.width() < available.left();
        at = QPoint(right ? around.right() + 1 + gap : around.left() - gap - panel.width(), around.top());
        break;
    }
    case Qt::TopEdge:
    case Qt::BottomEdge: {
        const bool below = edge == Qt::BottomEdge ? around.bottom() + gap + panel.height() <= available.bottom()
                                                  : around.top() - gap - panel.height() < available.top();
        at = QPoint(around.left(), below ? around.bottom() + 1 + gap : around.top() - gap - panel.height());
        break;
    }
    }
    popupAt(at, anchor);
}

void KisStudioPopup::popupAt(const QPoint &globalPos, QWidget *anchor)
{
    adjustSize();
    move(globalPos - QPoint(shadowMargin(), shadowMargin()));
    keepOnScreen();
    if (!m_closer) {
        m_closer = new KisPopupCloser(this, anchor);
        m_closer->setEnabled(!m_pinned);
    }
    KisStudioGlass::setMovableByBackground(this);
    KisStudioMotion::showFaded(this);
    raise();
    activateWindow();
}

void KisStudioPopup::keepOnScreen()
{
    QScreen *screen = this->screen() ? this->screen() : QGuiApplication::screenAt(pos());
    if (!screen) return;
    const QRect available = screen->availableGeometry().adjusted(-shadowMargin(), -shadowMargin(), shadowMargin(), shadowMargin());
    QPoint at = pos();
    at.setX(qBound(available.left(), at.x(), available.right() - width()));
    at.setY(qBound(available.top(), at.y(), available.bottom() - height()));
    move(at);
}

void KisStudioPopup::setPinned(bool pinned)
{
    m_pinned = pinned;
    if (m_closer) m_closer->setEnabled(!pinned);
}

void KisStudioPopup::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const int margin = shadowMargin();
    const QRectF panel = QRectF(rect()).adjusted(margin, margin, -margin, -margin);
    const qreal radius = KisModernStyle::panelRadius;
    if (margin) KisStudioGlass::paintShadow(painter, panel, radius, margin, 34, 4);
    const QPainterPath shape = KisStudioGlass::paint(painter, this, panel.adjusted(0.5, 0.5, -0.5, -0.5), radius);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.08);
    KisStudioGlass::paintBorder(painter, this, shape, border);
}

void KisStudioPopup::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape) {
        close();
        return;
    }
    QFrame::keyPressEvent(event);
}
