/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioScrubber.h"

#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>
#include <KisModernStyle.h>

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
    setToolTip(toolTip);
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
    if (m_dragging) {
        return; // the drag owns the value until it is committed
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
    return QString::number(m_value, 'f', m_value < 10.0 ? 1 : 0);
}

QSize KisStudioScrubber::sizeHint() const
{
    const int textWidth = fontMetrics().horizontalAdvance(m_kind == Kind::Opacity ? QStringLiteral("100%") : QStringLiteral("1000"));
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
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 2, -1, -2), 7));
    }

    const QRectF icon(8, (height() - iconSize) / 2.0, iconSize, iconSize);
    painter.setPen(Qt::NoPen);
    if (m_kind == Kind::Size) {
        // the dot grows the way the value is perceived, by ratios
        const qreal low = qMax(m_minimum, 1.0);
        const qreal position = qBound(0.0, std::log(qMax(m_value, low) / low) / std::log(m_maximum / low), 1.0);
        const qreal diameter = 3.0 + (iconSize - 3.0) * position;
        painter.setBrush(mix(window, text, 0.85));
        painter.drawEllipse(icon.center(), diameter / 2, diameter / 2);
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
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_moved = false;
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
    const int dx = eventX(event) - m_pressX;
    if (qAbs(dx) > 2) {
        m_moved = true;
    }
    if (m_kind == Kind::Size) {
        // 100 px of drag doubles or halves the size
        m_value = bounded(m_pressValue * std::pow(2.0, dx / 100.0));
    } else {
        // 2 px of drag per percent
        m_value = bounded(m_pressValue + dx * (m_maximum - m_minimum) / 200.0);
    }
    update();
}

void KisStudioScrubber::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    if (m_moved) {
        commit(m_value);
    } else {
        update();
    }
}

void KisStudioScrubber::mouseDoubleClickEvent(QMouseEvent *)
{
    if (!m_editor) {
        m_editor = new QLineEdit(this);
        m_editor->setFrame(false);
        m_editor->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        connect(m_editor, &QLineEdit::editingFinished, this, [this]() {
            bool ok = false;
            const qreal typed = QString(m_editor->text()).remove(QLatin1Char('%')).trimmed().toDouble(&ok);
            m_editor->hide();
            if (ok) {
                commit(typed);
            } else {
                update();
            }
        });
    }
    m_editor->setGeometry(8 + iconSize + 4, 3, width() - iconSize - 14, height() - 6);
    m_editor->setText(text().remove(QLatin1Char('%')));
    m_editor->show();
    m_editor->selectAll();
    m_editor->setFocus();
    update();
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
    return QWidget::event(event);
}
