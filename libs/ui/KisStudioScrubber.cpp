/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioScrubber.h"

#include "KisStudioTips.h"

#include <QLineEdit>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>
#include <KisModernStyle.h>

#include "widgets/KisStudioInlineEdit.h"

namespace {

constexpr int iconSize = 20;

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

int eventX(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return qRound(event->position().x());
#else
    return event->x();
#endif
}

} // namespace

KisStudioScrubber::KisStudioScrubber(Kind kind, const QString &toolTip, QWidget *parent)
    : QWidget(parent)
    , m_kind(kind)
{
    // an amount to apply has nothing to reset to
    const bool amount = kind == Kind::Grow || kind == Kind::Soft;
    setToolTip(KisStudioTips::tip(toolTip, KisStudioTips::valueGestures(!amount) << KisStudioTips::scrollStep()));
    setAccessibleName(toolTip);
    setCursor(Qt::SizeHorCursor);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
}

void KisStudioScrubber::setRange(qreal minimum, qreal maximum)
{
    m_minimum = minimum;
    m_maximum = qMax(minimum + 1e-6, maximum);
    m_value = bounded(m_value);
    update();
}

void KisStudioScrubber::setValue(qreal value)
{
    // Don't let outside updates fight an ongoing drag
    if (m_dragging) {
        return;
    }
    m_value = bounded(value);
    update();
}

qreal KisStudioScrubber::bounded(qreal value) const
{
    return qBound(m_minimum, value, m_maximum);
}

QString KisStudioScrubber::text() const
{
    if (m_kind == Kind::Opacity) {
        return QString::number(qRound(m_value)) + QLatin1Char('%');
    }
    // an amount to apply: named until dragged
    if (m_kind == Kind::Grow || m_kind == Kind::Soft) {
        const int amount = qRound(m_value);
        if (amount == 0) return accessibleName();
        return (m_kind == Kind::Grow ? (amount > 0 ? QStringLiteral("+") : QStringLiteral("−")) : QString())
            + QString::number(qAbs(amount));
    }
    return QString::number(m_value, 'f', m_value < 10.0 ? 1 : 0);
}

QSize KisStudioScrubber::sizeHint() const
{
    const int textWidth = qMax(fontMetrics().horizontalAdvance(m_kind == Kind::Opacity ? QStringLiteral("100%") : QStringLiteral("1000")),
                               m_kind == Kind::Grow || m_kind == Kind::Soft ? fontMetrics().horizontalAdvance(accessibleName()) : 0);
    return QSize(8 + iconSize + 6 + textWidth + 8, 30);
}

void KisStudioScrubber::commit(qreal value)
{
    m_value = bounded(value);
    update();
    Q_EMIT valueCommitted(m_value);
}

void KisStudioScrubber::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPalette &pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::ButtonText);

    if (underMouse() || m_dragging) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(mix(window, text, m_dragging ? 0.16 : 0.09));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 2, -1, -2), KisModernStyle::tileRadius));
    }

    const QRectF icon(8, (height() - iconSize) / 2.0, iconSize, iconSize);
    painter.setPen(Qt::NoPen);
    if (m_kind == Kind::Size) {
        // Logarithmic, to match how sizes are perceived
        const qreal low = qMax(m_minimum, 1.0);
        const qreal position = qBound(0.0, std::log(qMax(m_value, low) / low) / std::log(m_maximum / low), 1.0);
        const qreal diameter = 3.0 + (iconSize - 3.0) * position;
        painter.setBrush(mix(window, text, 0.85));
        painter.drawEllipse(icon.center(), diameter / 2, diameter / 2);
    } else if (m_kind == Kind::Grow) {
        // a shape and the outline it grows (or shrinks) to
        const QColor ink = mix(window, text, 0.85);
        painter.setBrush(ink);
        painter.drawPath(KisModernStyle::squircle(QRectF(icon.center() - QPointF(4, 4), QSizeF(8, 8)), 2));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ink, 1.3, Qt::DashLine));
        painter.drawPath(KisModernStyle::squircle(icon.adjusted(2.5, 2.5, -2.5, -2.5), 4));
    } else if (m_kind == Kind::Soft) {
        // a dot whose edge fades
        QRadialGradient fade(icon.center(), iconSize / 2.0);
        QColor ink = mix(window, text, 0.85);
        fade.setColorAt(0.3, ink);
        ink.setAlphaF(0);
        fade.setColorAt(1, ink);
        painter.setBrush(fade);
        painter.drawEllipse(icon.center(), iconSize / 2.0, iconSize / 2.0);
    } else {
        QPainterPath tile;
        tile.addPath(KisModernStyle::squircle(icon.adjusted(1, 1, -1, -1), 4));
        painter.save();
        painter.setClipPath(tile);
        const qreal cell = (iconSize - 2) / 3.0;
        for (int y = 0; y < 3; y++) {
            for (int x = 0; x < 3; x++) {
                painter.fillRect(QRectF(icon.left() + 1 + x * cell, icon.top() + 1 + y * cell, cell, cell),
                                 (x + y) % 2 ? mix(window, text, 0.18) : mix(window, text, 0.36));
            }
        }
        QColor fill = mix(window, text, 0.85);
        fill.setAlphaF(qBound(0.0, (m_value - m_minimum) / (m_maximum - m_minimum), 1.0));
        painter.fillRect(icon, fill);
        painter.restore();
    }

    if (!m_editor || !m_editor->isVisible()) {
        painter.setPen(text);
        painter.drawText(QRectF(icon.right() + 6, 0, width() - icon.right() - 6, height()), Qt::AlignVCenter | Qt::AlignLeft, this->text());
    }
}

void KisStudioScrubber::mousePressEvent(QMouseEvent *event)
{
    m_moved = false;
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_fine = event->modifiers() & Qt::ShiftModifier;
    m_pressX = eventX(event);
    m_pressValue = m_value;
    update();
}

void KisStudioScrubber::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    if (fine != m_fine) {
        // Continue from the current value at the new speed
        m_fine = fine;
        m_pressX = eventX(event);
        m_pressValue = m_value;
    }
    // Ignore small jitter so a plain click doesn't count as a drag
    if (!m_moved && qAbs(eventX(event) - m_pressX) <= 2) {
        return;
    }
    if (!m_moved) Q_EMIT dragStarted();
    m_moved = true;
    const qreal dx = (eventX(event) - m_pressX) * (fine ? 0.25 : 1.0);
    const qreal before = m_value;
    if (m_kind == Kind::Size) {
        // 100 px of drag doubles or halves the size
        m_value = bounded(m_pressValue * std::pow(2.0, dx / 100.0));
    } else if (m_kind == Kind::Grow || m_kind == Kind::Soft) {
        // whole pixels, 3 px of drag each
        m_value = bounded(qRound(m_pressValue + dx / 3.0));
    } else {
        // 2 px of drag per percent
        m_value = bounded(m_pressValue + dx * (m_maximum - m_minimum) / 200.0);
    }
    if (m_value != before) Q_EMIT valueChanging(m_value);
    update();
}

void KisStudioScrubber::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    endDrag();
}

void KisStudioScrubber::endDrag()
{
    m_dragging = false;
    if (m_moved) {
        commit(m_value);
    } else {
        update();
    }
}

void KisStudioScrubber::mouseDoubleClickEvent(QMouseEvent *)
{
    // typed where the number is written
    const QString shown = text().remove(QLatin1Char('%'));
    const QRect at(8 + iconSize + 6, 0, fontMetrics().horizontalAdvance(text()), height());
    m_editor = KisStudioInlineEdit::open(this, at, shown, Qt::AlignLeft, font(), [this](const QString &typed) {
        bool ok = false;
        const qreal value = QString(typed).remove(QLatin1Char('%')).trimmed().toDouble(&ok);
        if (ok) commit(value);
    });
    update();
}

void KisStudioScrubber::contextMenuEvent(QContextMenuEvent *event)
{
    // a finger held still at the end of a drag isn't asking for a reset
    if (m_moved) {
        event->accept();
        return;
    }
    if (!qIsNaN(m_default)) {
        commit(m_default);
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

void KisStudioScrubber::wheelEvent(QWheelEvent *event)
{
    const int steps = KisModernStyle::wheelSteps(event, this);
    if (!steps) {
        event->accept();
        return;
    }
    if (m_kind == Kind::Size) {
        commit(m_value * std::pow(1.15, steps));
    } else if (m_kind == Kind::Grow || m_kind == Kind::Soft) {
        commit(m_value + steps);
    } else {
        commit(m_value + steps * (m_maximum - m_minimum) / 20.0);
    }
    event->accept();
}

bool KisStudioScrubber::event(QEvent *event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        update();
    }
    // The release never comes when the window loses focus or the bar hides mid-drag: end the drag here instead
    if (m_dragging && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide
                       || event->type() == QEvent::UngrabMouse)) {
        endDrag();
    }
    return QWidget::event(event);
}
