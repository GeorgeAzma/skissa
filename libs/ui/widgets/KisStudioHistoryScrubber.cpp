/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioHistoryScrubber.h"
#include <KisLanczos.h>

#include <QAbstractItemView>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QMouseEvent>
#include <QPainter>
#include <QTimer>
#include <QLabel>
#include <QToolTip>
#include <QWheelEvent>

#include <KisModernStyle.h>

namespace
{
constexpr int trackY = 11;
constexpr int markY = 22;
constexpr qreal markRadius = 3.0;
constexpr qreal capsuleHeight = 14.0;
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

void KisStudioHistoryScrubber::setStepTo(const std::function<void(int)> &stepTo)
{
    m_stepTo = stepTo;
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
    if (step == currentStep() || !m_stepTo) return;
    m_stepTo(step);
    update();
}

qreal KisStudioHistoryScrubber::lineLeft() const
{
    int before = 0;
    for (const Mark &mark : m_marks) before += mark.step < 0;
    // the capsule's round end reaches back past the line's start
    return KisModernStyle::space2 + before * (2 * markRadius + KisModernStyle::space1) + (before ? capsuleHeight / 2 : 0);
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
    // with one step there is nothing to go along: only the snapshots kept from a history that is gone
    const bool line = count > 1;

    // a capsule filled as far as the image is now; what is past that can still be redone
    const qreal capsuleLeft = left - capsuleHeight / 2;
    const QRectF capsule(capsuleLeft, trackY - capsuleHeight / 2, right + capsuleHeight / 2 - capsuleLeft, capsuleHeight);
    const QPainterPath shape = KisModernStyle::squircle(capsule, capsuleHeight / 2);
    if (line) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink(m_dragging ? 0.14 : 0.1));
        painter.drawPath(shape);
        painter.save();
        painter.setClipPath(shape);
        painter.fillRect(QRectF(capsule.left(), capsule.top(), at + capsuleHeight / 2 - capsule.left(), capsule.height()), ink(m_dragging ? 0.62 : 0.5));
        // a faint line at each step while they stand apart; more would only be a gray haze
        if ((right - left) / (count - 1) >= 4) {
            for (int step = 1; step < count - 1; step++) {
                const qreal x = xOf(step);
                painter.fillRect(QRectF(x - 0.5, capsule.top() + 4, 1, capsule.height() - 8), ink(step <= current ? 0.18 : 0.12));
            }
        }
        painter.restore();
    }

    // the snapshots, where they were kept; rings for those of a history that is gone
    const QColor markInk = KisModernStyle::hasAccent(palette()) ? palette().color(QPalette::Highlight) : ink(0.8);
    for (int i = 0; i < m_marks.size(); i++) {
        const bool gone = m_marks[i].step < 0;
        painter.setPen(gone ? QPen(markInk, 1.3) : Qt::NoPen);
        painter.setBrush(gone ? Qt::NoBrush : QBrush(markInk));
        painter.drawEllipse(markAtPoint(i), markRadius, markRadius);
    }
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
        // the step reached, shown where the pointer is
        showStep(currentStep(), event->globalPos());
    } else if (markAt(event->pos()) < 0 && m_steps && stepCount() > 0) {
        showStep(stepAt(event->pos().x()), event->globalPos());
    } else {
        hideStep();
    }
}

void KisStudioHistoryScrubber::showStep(int step, const QPoint &globalPos)
{
    const QModelIndex index = m_steps ? m_steps->model()->index(step, 0) : QModelIndex();
    if (!index.isValid()) {
        hideStep();
        return;
    }
    if (!m_bubble) {
        m_bubble = new QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint);
        m_bubble->setAttribute(Qt::WA_TranslucentBackground);
        m_bubble->setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(this, &QObject::destroyed, m_bubble.data(), &QObject::deleteLater);
    }
    if (step != m_bubbleStep) {
        m_bubbleStep = step;
        // the image after that step, as the history kept it, with the step's name under it
        const QImage picture = index.data(Qt::DecorationRole).value<QImage>();
        const QString name = index.data(Qt::DisplayRole).toString();
        const qreal ratio = devicePixelRatioF();
        const int pad = KisModernStyle::space2;
        const QSize shown = picture.isNull() ? QSize() : (QSizeF(picture.size()) / picture.devicePixelRatio()).scaled(QSizeF(128, 128), Qt::KeepAspectRatio).toSize();
        const QFontMetrics metrics(font());
        const int width = qMax(shown.width(), metrics.horizontalAdvance(name)) + 2 * pad;
        const int height = shown.height() + (shown.isEmpty() ? 0 : pad) + metrics.height() + 2 * pad;
        QPixmap pixmap(QSize(width, height) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.setPen(Qt::NoPen);
        painter.setBrush(palette().color(QPalette::ToolTipBase));
        painter.drawPath(KisModernStyle::squircle(QRectF(0, 0, width, height), KisModernStyle::tileRadius));
        if (!shown.isEmpty()) {
            KisLanczos::draw(painter, QRectF(QPointF((width - shown.width()) / 2.0, pad), shown), picture);
        }
        painter.setPen(palette().color(QPalette::ToolTipText));
        painter.drawText(QRectF(pad, height - pad - metrics.height(), width - 2 * pad, metrics.height()), Qt::AlignCenter, name);
        painter.end();
        m_bubble->setPixmap(pixmap);
        m_bubble->resize(width, height);
    }
    // above the line, centered on the pointer
    m_bubble->move(globalPos.x() - m_bubble->width() / 2, mapToGlobal(QPoint(0, 0)).y() - m_bubble->height() - KisModernStyle::space1);
    m_bubble->show();
}

void KisStudioHistoryScrubber::hideStep()
{
    if (m_bubble) m_bubble->hide();
    m_bubbleStep = -1;
}

void KisStudioHistoryScrubber::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    if (m_pressedMark >= 0 && m_pressedMark == markAt(event->pos())) openLater(m_pressedMark, false);
    m_pressedMark = -1;
    m_dragging = false;
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
        // the steps show themselves as the pointer moves
        if (mark >= 0) name = m_marks[mark].name;
        if (name.isEmpty()) QToolTip::hideText();
        else QToolTip::showText(help->globalPos(), name, this);
        return true;
    }
    if (event->type() == QEvent::Leave || event->type() == QEvent::Hide) hideStep();
    return QWidget::event(event);
}
