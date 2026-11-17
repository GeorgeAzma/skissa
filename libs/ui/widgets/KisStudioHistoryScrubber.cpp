/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioHistoryScrubber.h"

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QToolTip>
#include <QWheelEvent>

#include <KisModernStyle.h>

namespace
{
constexpr int trackY = 11;
constexpr int markY = 22;
constexpr qreal markRadius = 3.0;
} // namespace

KisStudioHistoryScrubber::KisStudioHistoryScrubber(QAbstractItemView *steps, QWidget *parent)
    : QWidget(parent)
    , m_steps(steps)
{
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    auto repaint = [this]() { update(); };
    if (QAbstractItemModel *model = steps->model()) {
        connect(model, &QAbstractItemModel::rowsInserted, this, repaint);
        connect(model, &QAbstractItemModel::rowsRemoved, this, repaint);
        connect(model, &QAbstractItemModel::modelReset, this, repaint);
        connect(model, &QAbstractItemModel::dataChanged, this, repaint);
    }
    if (steps->selectionModel()) connect(steps->selectionModel(), &QItemSelectionModel::currentChanged, this, repaint);
}

void KisStudioHistoryScrubber::setMarks(const QVector<Mark> &marks, const std::function<void(int, bool)> &open)
{
    m_marks = marks;
    m_open = open;
    update();
}

QSize KisStudioHistoryScrubber::sizeHint() const
{
    return QSize(160, 28);
}

QSize KisStudioHistoryScrubber::minimumSizeHint() const
{
    return QSize(60, 28);
}

int KisStudioHistoryScrubber::stepCount() const
{
    return m_steps && m_steps->model() ? m_steps->model()->rowCount() : 0;
}

int KisStudioHistoryScrubber::currentStep() const
{
    if (!m_steps) return 0;
    const int row = m_steps->currentIndex().row();
    return row < 0 ? stepCount() - 1 : row;
}

void KisStudioHistoryScrubber::goTo(int step)
{
    if (!m_steps || !m_steps->model()) return;
    step = qBound(0, step, stepCount() - 1);
    if (step == currentStep()) return;
    // the list's own choosing undoes and redoes to that step
    const QModelIndex index = m_steps->model()->index(step, 0);
    m_steps->setCurrentIndex(index);
    m_steps->scrollTo(index);
    update();
}

qreal KisStudioHistoryScrubber::lineLeft() const
{
    int before = 0;
    for (const Mark &mark : m_marks) before += mark.step < 0;
    return KisModernStyle::space2 + before * (2 * markRadius + KisModernStyle::space1);
}

QPointF KisStudioHistoryScrubber::markAtPoint(int mark) const
{
    const Mark &at = m_marks[mark];
    if (at.step >= 0) return QPointF(xOf(qMin(at.step, stepCount() - 1)), markY);
    // the gone ones in a row before the line, at its height
    int before = 0;
    for (int i = 0; i < mark; i++) before += m_marks[i].step < 0;
    return QPointF(KisModernStyle::space2 + markRadius + before * (2 * markRadius + KisModernStyle::space1), trackY);
}

qreal KisStudioHistoryScrubber::xOf(int step) const
{
    const qreal left = lineLeft();
    const qreal right = width() - KisModernStyle::space2;
    const int count = stepCount();
    return count < 2 ? right : left + (right - left) * step / (count - 1);
}

int KisStudioHistoryScrubber::stepAt(qreal x) const
{
    const qreal left = lineLeft();
    const qreal right = width() - KisModernStyle::space2;
    const int count = stepCount();
    if (count < 2 || right <= left) return count - 1;
    return qBound(0, qRound((x - left) / (right - left) * (count - 1)), count - 1);
}

int KisStudioHistoryScrubber::markAt(const QPoint &pos) const
{
    int best = -1;
    qreal nearest = 7;
    for (int i = 0; i < m_marks.size(); i++) {
        const QPointF at = markAtPoint(i);
        if (qAbs(pos.y() - at.y()) > 6) continue;
        const qreal distance = qAbs(at.x() - pos.x());
        if (distance < nearest) {
            nearest = distance;
            best = i;
        }
    }
    return best;
}

void KisStudioHistoryScrubber::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor text = palette().color(QPalette::Text);
    auto ink = [&text](qreal alpha) {
        QColor color = text;
        color.setAlphaF(alpha);
        return color;
    };
    const int count = stepCount();
    const int current = currentStep();
    const qreal left = xOf(0);
    const qreal right = width() - KisModernStyle::space2;
    const qreal at = xOf(current);

    // what is done, and fainter what was undone and can still be redone
    painter.setPen(QPen(ink(0.16), 2, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(left, trackY), QPointF(right, trackY));
    painter.setPen(QPen(ink(0.55), 2, Qt::SolidLine, Qt::RoundCap));
    if (at > left) painter.drawLine(QPointF(left, trackY), QPointF(at, trackY));

    // each step a dot while there is room for them
    if (count > 1 && (right - left) / (count - 1) >= 5) {
        painter.setPen(Qt::NoPen);
        for (int step = 0; step < count; step++) {
            painter.setBrush(ink(step <= current ? 0.55 : 0.22));
            painter.drawEllipse(QPointF(xOf(step), trackY), 1.6, 1.6);
        }
    }

    // the snapshots, where they were kept; rings for those of a history that is gone
    const QColor markInk = KisModernStyle::hasAccent(palette()) ? palette().color(QPalette::Highlight) : ink(0.8);
    for (int i = 0; i < m_marks.size(); i++) {
        const bool gone = m_marks[i].step < 0;
        painter.setPen(gone ? QPen(markInk, 1.3) : Qt::NoPen);
        painter.setBrush(gone ? Qt::NoBrush : QBrush(markInk));
        painter.drawEllipse(markAtPoint(i), markRadius, markRadius);
    }

    // where the image is now
    const QRectF knob(at - 4, trackY - 7, 8, 14);
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink(m_dragging ? 1.0 : 0.9));
    painter.drawPath(KisModernStyle::squircle(knob, 4));
}

void KisStudioHistoryScrubber::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    m_pressedMark = markAt(event->pos());
    if (m_pressedMark >= 0) return;
    m_dragging = true;
    goTo(stepAt(event->pos().x()));
    update();
}

void KisStudioHistoryScrubber::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        goTo(stepAt(event->pos().x()));
        // the step reached, named where the pointer is
        const QModelIndex index = m_steps ? m_steps->model()->index(currentStep(), 0) : QModelIndex();
        QToolTip::showText(event->globalPos(), index.data(Qt::DisplayRole).toString(), this);
    }
}

void KisStudioHistoryScrubber::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_pressedMark >= 0 && m_pressedMark == markAt(event->pos())) openLater(m_pressedMark, false);
    m_pressedMark = -1;
    m_dragging = false;
    QToolTip::hideText();
    update();
}

void KisStudioHistoryScrubber::openLater(int mark, bool menu)
{
    // going back replaces the image and its history: not while this event is still being handled
    QTimer::singleShot(0, this, [this, mark, menu]() {
        // a copy: what it does sets the marks again, replacing m_open
        const std::function<void(int, bool)> open = m_open;
        if (open && mark < m_marks.size()) open(mark, menu);
    });
}

void KisStudioHistoryScrubber::wheelEvent(QWheelEvent *event)
{
    const int notches = event->angleDelta().y() / 120;
    if (notches == 0) {
        event->ignore();
        return;
    }
    goTo(currentStep() + notches);
    event->accept();
}

void KisStudioHistoryScrubber::contextMenuEvent(QContextMenuEvent *event)
{
    // a snapshot's choices, or back to the newest step
    const int mark = markAt(event->pos());
    if (mark >= 0) openLater(mark, true);
    else goTo(stepCount() - 1);
    event->accept();
}

bool KisStudioHistoryScrubber::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int mark = markAt(help->pos());
        QString name;
        if (mark >= 0) {
            name = m_marks[mark].name;
        } else if (m_steps && stepCount() > 0) {
            name = m_steps->model()->index(stepAt(help->pos().x()), 0).data(Qt::DisplayRole).toString();
        }
        if (name.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(help->globalPos(), name, this);
        return true;
    }
    return QWidget::event(event);
}
