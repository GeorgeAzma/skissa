/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioChips.h"

#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <KisModernStyle.h>

namespace {
constexpr int gap = KisModernStyle::space1;
constexpr int padding = 10;
constexpr int dotSize = 5;
}

KisStudioChips::KisStudioChips(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}

void KisStudioChips::setChips(const QVector<Chip> &chips)
{
    m_chips = chips;
    m_hovered = -1;
    updateGeometry();
    update();
}

void KisStudioChips::setTabs(bool tabs)
{
    m_tabs = tabs;
    updateGeometry();
    update();
}

void KisStudioChips::setTileSize(int side)
{
    m_tile = side;
    updateGeometry();
    update();
}

void KisStudioChips::setTileColumns(int columns, int height)
{
    m_columns = columns;
    m_tile = height;
    updateGeometry();
    update();
}

void KisStudioChips::setCurrent(int index)
{
    if (index == m_current) return;
    m_current = index;
    update();
}

int KisStudioChips::chipHeight() const
{
    if (m_tile) return m_tile;
    return m_tabs ? 32 : 28;
}

QVector<QRectF> KisStudioChips::layoutFor(int width) const
{
    QVector<QRectF> rects;
    const int height = chipHeight();
    if (m_tabs && !m_chips.isEmpty()) {
        const qreal each = (width - gap * (m_chips.size() - 1)) / qreal(m_chips.size());
        for (int i = 0; i < m_chips.size(); i++) {
            rects << QRectF(i * (each + gap), 0, each, height);
        }
        return rects;
    }
    qreal x = 0;
    qreal y = 0;
    const qreal column = m_columns > 0 ? (width - gap * (m_columns - 1)) / qreal(m_columns) : 0;
    Q_FOREACH (const Chip &chip, m_chips) {
        qreal w = m_columns > 0 ? column : m_tile;
        if (!m_tile) {
            w = fontMetrics().horizontalAdvance(chip.text) + 2 * padding + (chip.dot ? dotSize + 6 : 0);
        }
        if (x > 0 && x + w > width + 0.5) {
            x = 0;
            y += height + gap;
        }
        rects << QRectF(x, y, w, height);
        x += w + gap;
    }
    return rects;
}

int KisStudioChips::heightForWidth(int width) const
{
    const QVector<QRectF> rects = layoutFor(width);
    return rects.isEmpty() ? chipHeight() : int(rects.last().bottom());
}

QSize KisStudioChips::sizeHint() const
{
    qreal width = 0;
    Q_FOREACH (const QRectF &rect, layoutFor(10000)) {
        width = qMax(width, rect.right());
    }
    if (m_columns > 0) width = 360;
    return QSize(qMin<int>(width, 360), heightForWidth(qMin<int>(width, 360)));
}

QSize KisStudioChips::minimumSizeHint() const
{
    return QSize(m_tile && !m_columns ? m_tile : 60, chipHeight());
}

int KisStudioChips::at(const QPoint &pos) const
{
    const QVector<QRectF> rects = layoutFor(width());
    for (int i = 0; i < rects.size(); i++) {
        if (rects[i].contains(pos)) return i;
    }
    return -1;
}

void KisStudioChips::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = palette().color(QPalette::WindowText);
    const QColor accent = palette().color(QPalette::Highlight);
    const QVector<QRectF> rects = layoutFor(width());
    for (int i = 0; i < m_chips.size(); i++) {
        const Chip &chip = m_chips[i];
        const QRectF box = rects[i].adjusted(0.5, 0.5, -0.5, -0.5);
        const QPainterPath shape = KisModernStyle::squircle(box, KisModernStyle::tileRadius);
        QColor ground = ink;
        ground.setAlphaF(chip.on ? 0.14 : i == m_hovered ? 0.08 : 0.05);
        painter.fillPath(shape, ground);
        if (i == m_current) {
            painter.setPen(QPen(accent, 1.5));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(shape);
        }
        if (chip.picture) {
            QRectF picture = box.adjusted(4, 4, -4, -4);
            if (m_tile && !chip.text.isEmpty()) {
                // a picture with its name under it
                // a long name a little smaller rather than cut short
                QFont small = KisModernStyle::textFont(font(), 0.9);
                if (QFontMetrics(small).horizontalAdvance(chip.text) > box.width() - 6) small = KisModernStyle::textFont(font(), 0.8);
                const int line = QFontMetrics(small).height();
                picture.setBottom(box.bottom() - line - 4);
                painter.save();
                painter.setFont(small);
                painter.setPen(chip.on ? ink : KisModernStyle::secondaryInk(ink));
                const QRectF caption(box.left() + 2, picture.bottom(), box.width() - 4, box.bottom() - picture.bottom() - 1);
                painter.drawText(caption, Qt::AlignCenter, QFontMetrics(small).elidedText(chip.text, Qt::ElideRight, int(caption.width())));
                painter.restore();
            }
            painter.save();
            painter.setClipPath(shape);
            chip.picture(painter, picture);
            painter.restore();
            if (chip.on && i != m_current) {
                // a picture may fill its tile: the choice that is on gets a ring
                QColor ring = ink;
                ring.setAlphaF(0.7);
                painter.setPen(QPen(ring, 1.5));
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(KisModernStyle::squircle(box.adjusted(0.75, 0.75, -0.75, -0.75), KisModernStyle::tileRadius));
            }
            continue;
        }
        QRectF text = box.adjusted(padding, 0, -padding, 0);
        if (chip.dot) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            painter.drawEllipse(QRectF(text.left(), box.center().y() - dotSize / 2.0, dotSize, dotSize));
            text.setLeft(text.left() + dotSize + 6);
        }
        painter.setPen(chip.on || (m_tabs && i == m_current) ? ink : KisModernStyle::secondaryInk(ink));
        painter.drawText(text, (m_tabs ? Qt::AlignHCenter : Qt::AlignLeft) | Qt::AlignVCenter, chip.text);
    }
}

void KisStudioChips::mousePressEvent(QMouseEvent *event)
{
    // taken here, so the release comes here too (not to a window being dragged by its background)
    event->setAccepted(at(event->pos()) >= 0);
}

void KisStudioChips::mouseMoveEvent(QMouseEvent *event)
{
    const int hovered = at(event->pos());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        setCursor(hovered >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void KisStudioChips::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const int index = at(event->pos());
        if (index >= 0) Q_EMIT clicked(index);
    }
    QWidget::mouseReleaseEvent(event);
}

void KisStudioChips::leaveEvent(QEvent *event)
{
    m_hovered = -1;
    update();
    QWidget::leaveEvent(event);
}

bool KisStudioChips::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int index = at(help->pos());
        if (index >= 0 && !m_chips[index].toolTip.isEmpty()) {
            QToolTip::showText(help->globalPos(), m_chips[index].toolTip, this);
        } else {
            QToolTip::hideText();
        }
        return true;
    }
    return QWidget::event(event);
}
