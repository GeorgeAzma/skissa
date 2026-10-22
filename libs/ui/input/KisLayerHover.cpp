/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisLayerHover.h"

#include <QCursor>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTabletEvent>

#include <KisModernStyle.h>
#include <kis_canvas2.h>
#include <kis_coordinates_converter.h>
#include <kis_image.h>
#include <kis_node.h>
#include <kis_paint_device.h>

KisLayerHover::KisLayerHover(KisCanvas2 *canvas, std::function<KisNodeSP(const QPointF &)> picks)
    : QWidget(canvas->canvasWidget())
    , m_picks(std::move(picks))
    , m_canvas(canvas)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setGeometry(parentWidget()->rect());
    parentWidget()->installEventFilter(this);
    show();
    raise();
    const QPoint cursor = parentWidget()->mapFromGlobal(QCursor::pos());
    if (rect().contains(cursor)) pointAt(cursor);
}

void KisLayerHover::picksChanged(const QPointF &widgetPos)
{
    m_picked = false;
    pointAt(widgetPos);
}

void KisLayerHover::pointAt(const QPointF &widgetPos)
{
    const QRect before = pillRect();
    m_cursor = widgetPos;
    if (!m_canvas || !m_canvas->image()) {
        return;
    }
    // picking reads every layer's pixel there: not again for a pointer that barely moved
    KisNodeSP node = m_node;
    if (!m_picked || QLineF(widgetPos, m_pickedAt).length() >= 2) {
        node = m_picks(widgetPos);
        m_pickedAt = widgetPos;
        m_picked = true;
    }
    if (node == m_node) {
        // only the name moved with the pointer: not the whole canvas's overlay
        update(before.united(pillRect()));
        return;
    }
    m_node = node;
    m_tint = QImage();
    m_bounds = QRect();
    if (node && node->projection()) {
        m_bounds = node->exactBounds();
        if (!m_bounds.isEmpty()) {
            // in the selection color, at a size good enough over the canvas
            const QSize size = m_bounds.size().scaled(QSize(480, 480), Qt::KeepAspectRatio).expandedTo(QSize(1, 1));
            QImage alpha = node->projection()->createThumbnailUncached(size.width(), size.height(), m_bounds)
                               .convertToFormat(QImage::Format_ARGB32);
            const QColor ink = palette().color(QPalette::Highlight);
            for (int y = 0; y < alpha.height(); y++) {
                QRgb *line = reinterpret_cast<QRgb *>(alpha.scanLine(y));
                for (int x = 0; x < alpha.width(); x++) {
                    line[x] = qRgba(ink.red(), ink.green(), ink.blue(), qAlpha(line[x]) * 110 / 255);
                }
            }
            m_tint = alpha;
        }
    }
    update();
}

bool KisLayerHover::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget()) {
        switch (event->type()) {
        case QEvent::MouseMove:
            pointAt(static_cast<QMouseEvent *>(event)->localPos());
            break;
        case QEvent::TabletMove:
            pointAt(static_cast<QTabletEvent *>(event)->posF());
            break;
        case QEvent::Resize:
            setGeometry(parentWidget()->rect());
            break;
        default:
            break;
        }
    }
    return false;
}

void KisLayerHover::paintEvent(QPaintEvent *)
{
    if (!m_node || !m_canvas) {
        return;
    }
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QColor ink = palette().color(QPalette::Highlight);
    if (!m_tint.isNull()) {
        // in image coordinates: rotated and mirrored with the canvas
        painter.save();
        painter.setTransform(m_canvas->coordinatesConverter()->imageToWidgetTransform());
        painter.drawImage(QRectF(m_bounds), m_tint);
        QPen pen(ink, 1.5);
        pen.setCosmetic(true);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRectF(m_bounds));
        painter.restore();
    }
    // the name, by the pointer
    painter.setFont(nameFont());
    const QString name = m_node->name();
    const QRectF pill = pillAt();
    painter.setPen(Qt::NoPen);
    painter.setBrush(ink);
    painter.drawPath(KisModernStyle::squircle(pill, pill.height() / 2));
    painter.setPen(palette().color(QPalette::HighlightedText));
    painter.drawText(pill, Qt::AlignCenter, name);
}

QFont KisLayerHover::nameFont() const
{
    QFont font = this->font();
    font.setBold(true);
    return font;
}

QRectF KisLayerHover::pillAt() const
{
    if (!m_node) return QRectF();
    const QFontMetrics metrics(nameFont());
    QRectF pill(m_cursor + QPointF(18, 14), QSizeF(metrics.horizontalAdvance(m_node->name()) + 20, metrics.height() + 10));
    if (pill.right() > width() - 4) pill.moveRight(m_cursor.x() - 10);
    if (pill.bottom() > height() - 4) pill.moveBottom(m_cursor.y() - 10);
    return pill;
}

QRect KisLayerHover::pillRect() const
{
    return pillAt().toAlignedRect().adjusted(-2, -2, 2, 2);
}
