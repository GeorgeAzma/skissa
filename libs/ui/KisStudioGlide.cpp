/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioGlide.h"

#include <QEvent>
#include <QRect>
#include <QWidget>

#include <KisModernStyle.h>

KisStudioGlide::KisStudioGlide(QWidget *widget)
    : QObject(widget)
    , m_widget(widget)
{
    m_glide.setDuration(KisModernStyle::changeDuration);
    m_glide.setEasingCurve(KisModernStyle::motionCurve());
    connect(&m_glide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_widget->move(m_target + value.toPoint());
    });
    widget->installEventFilter(this);
}

QPoint KisStudioGlide::offsetNow() const
{
    return m_glide.state() == QAbstractAnimation::Running ? m_glide.currentValue().toPoint() : QPoint();
}

QPoint KisStudioGlide::target() const
{
    return m_placed ? m_target : m_widget->pos();
}

void KisStudioGlide::markTarget()
{
    m_widget->setProperty("studioTarget", QRect(m_target, m_widget->size()));
}

void KisStudioGlide::moveTo(const QPoint &target, const std::optional<QPoint> &byPointer)
{
    const QWidget *area = m_widget->parentWidget();
    const QSize areaSize = area ? area->size() : QSize();
    // nothing to glide from: hidden, just shown (and settling in), or the window changed size
    const bool fresh = !m_placed || !m_widget->isVisible() || areaSize != m_areaSize || !m_shown.isValid() || m_shown.elapsed() < 500;
    const QPoint now = offsetNow();
    const QPoint extra = byPointer ? target - *byPointer : QPoint();
    QPoint from = fresh ? QPoint() : m_widget->pos() - target;
    if (byPointer && !fresh) {
        // dragged, it follows the pointer at once, each way apart; only a jump away from it (a catch) glides
        const QPoint moved = target - m_target;
        const QPoint pointerPart = moved - (extra - m_extra);
        auto jump = [](int moved, int pointer) { return qAbs(moved) > 3 && qAbs(moved - pointer) > 3 ? moved : 0; };
        from = now - QPoint(jump(moved.x(), pointerPart.x()), jump(moved.y(), pointerPart.y()));
    }
    m_areaSize = areaSize;
    m_target = target;
    m_extra = extra;
    m_placed = true;
    markTarget();
    if (from.manhattanLength() < 2) {
        m_glide.stop();
        if (m_widget->pos() != target) m_widget->move(target);
        return;
    }
    // a new jump starts the glide again; else it keeps going
    if (m_glide.state() != QAbstractAnimation::Running || from != now) {
        m_glide.stop();
        m_glide.setStartValue(from);
        m_glide.setEndValue(QPoint());
        m_glide.start();
    }
    m_widget->move(target + offsetNow());
}

bool KisStudioGlide::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_widget) {
        if (event->type() == QEvent::Show) {
            m_shown.restart();
        } else if (event->type() == QEvent::Resize && m_placed) {
            markTarget();
        }
    }
    return QObject::eventFilter(watched, event);
}
