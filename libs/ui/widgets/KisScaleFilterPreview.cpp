/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisScaleFilterPreview.h"

#include <cmath>

#include <QMouseEvent>
#include <QPainter>

#include <KisModernStyle.h>
#include <KoUpdater.h>
#include <klocalizedstring.h>
#include <kis_filter_strategy.h>
#include <kis_paint_device.h>
#include <kis_painter.h>
#include <kis_transform_worker.h>

namespace {

const int tileSide = 120;

int labelHeight(const QWidget *widget)
{
    return widget->fontMetrics().height() + KisModernStyle::space1;
}

}

KisScaleFilterPreview::KisScaleFilterPreview(QWidget *parent)
    : QWidget(parent)
{
    m_tiles = {
        {QStringLiteral("Smooth"), i18nc("resampling choice: sharp, without halos", "Smooth"), QImage()},
        {QStringLiteral("Pixel"), i18nc("resampling choice for pixel art", "Pixel"), QImage()},
        {QStringLiteral("Bicubic"), i18nc("resampling choice: Krita's bicubic", "Soft"), QImage()},
    };
    setMouseTracking(true);
    m_renderLater.setSingleShot(true);
    m_renderLater.setInterval(120);
    connect(&m_renderLater, &QTimer::timeout, this, [this]() {
        render();
        update();
    });
}

void KisScaleFilterPreview::setContent(KisPaintDeviceSP content, const QRect &bounds)
{
    m_content = content;
    m_bounds = bounds;
    findDetail();
    m_renderLater.start();
}

void KisScaleFilterPreview::setTargetSize(const QSize &size)
{
    if (size == m_target) return;
    m_target = size;
    m_renderLater.start();
}

void KisScaleFilterPreview::setCurrent(const QString &filterId)
{
    m_current = filterId;
    update();
}

QSize KisScaleFilterPreview::sizeHint() const
{
    const int count = m_tiles.size();
    return QSize(count * tileSide + (count - 1) * KisModernStyle::space2, tileSide + labelHeight(this));
}

void KisScaleFilterPreview::findDetail()
{
    // the busiest part of the picture shows the filters apart: the block with the most contrast in a small copy
    m_detail = m_bounds.center();
    if (!m_content || m_bounds.isEmpty()) return;

    const QImage thumb = m_content->createThumbnailUncached(256, 256, m_bounds).convertToFormat(QImage::Format_ARGB32);
    if (thumb.isNull()) return;
    const int blocks = 8;
    const int bw = qMax(1, thumb.width() / blocks);
    const int bh = qMax(1, thumb.height() / blocks);
    qreal best = -1;
    for (int by = 0; by + bh <= thumb.height(); by += bh) {
        for (int bx = 0; bx + bw <= thumb.width(); bx += bw) {
            qreal sum = 0;
            qreal squares = 0;
            int n = 0;
            for (int y = by; y < by + bh; y++) {
                const QRgb *line = reinterpret_cast<const QRgb *>(thumb.constScanLine(y));
                for (int x = bx; x < bx + bw; x++) {
                    const qreal value = qGray(line[x]) * qAlpha(line[x]) / 255.0;
                    sum += value;
                    squares += value * value;
                    n++;
                }
            }
            const qreal variance = squares / n - (sum / n) * (sum / n);
            if (variance > best) {
                best = variance;
                m_detail = m_bounds.topLeft() + QPoint(qRound((bx + bw / 2.0) * m_bounds.width() / thumb.width()),
                                                       qRound((by + bh / 2.0) * m_bounds.height() / thumb.height()));
            }
        }
    }
}

void KisScaleFilterPreview::render()
{
    if (!m_content || m_bounds.isEmpty() || m_target.isEmpty()) return;

    const qreal dpr = devicePixelRatioF();
    const int side = qRound(tileSide * dpr);
    const qreal sx = qreal(m_target.width()) / m_bounds.width();
    const qreal sy = qreal(m_target.height()) / m_bounds.height();

    // as much of the picture as fills a tile at its new size, around its detail, and a margin for the filters' reach
    const QSize cropSize(qBound(2, int(std::ceil(side / sx)), m_bounds.width()), qBound(2, int(std::ceil(side / sy)), m_bounds.height()));
    QRect crop(m_detail - QPoint(cropSize.width() / 2, cropSize.height() / 2), cropSize);
    crop.moveLeft(qBound(m_bounds.left(), crop.left(), m_bounds.right() + 1 - crop.width()));
    crop.moveTop(qBound(m_bounds.top(), crop.top(), m_bounds.bottom() + 1 - crop.height()));
    const int margin = 4;
    const QRect source = crop.adjusted(-margin, -margin, margin, margin) & m_bounds;

    for (Tile &tile : m_tiles) {
        KisFilterStrategy *filter = KisFilterStrategyRegistry::instance()->value(tile.filterId);
        if (!filter) continue;
        KisPaintDeviceSP piece = new KisPaintDevice(m_content->colorSpace());
        // the crop's corner goes to the origin, which the scaling keeps in place
        KisPainter::copyAreaOptimized(source.topLeft() - crop.topLeft(), m_content, piece, source);
        KisTransformWorker(piece, sx, sy, 0, 0, 0, 0, 0, KoUpdaterPtr(), filter).run();
        const QSize shown(qMin(side, qRound(crop.width() * sx)), qMin(side, qRound(crop.height() * sy)));
        tile.picture = piece->convertToQImage(nullptr, 0, 0, shown.width(), shown.height());
        tile.picture.setDevicePixelRatio(dpr);
    }
}

QRect KisScaleFilterPreview::tileRect(int index) const
{
    const int count = m_tiles.size();
    const int total = count * tileSide + (count - 1) * KisModernStyle::space2;
    const int left = (width() - total) / 2;
    return QRect(left + index * (tileSide + KisModernStyle::space2), 0, tileSide, tileSide);
}

int KisScaleFilterPreview::tileAt(const QPoint &point) const
{
    for (int i = 0; i < m_tiles.size(); i++) {
        if (tileRect(i).adjusted(0, 0, 0, labelHeight(this)).contains(point)) return i;
    }
    return -1;
}

void KisScaleFilterPreview::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = palette().color(QPalette::Text);

    for (int i = 0; i < m_tiles.size(); i++) {
        const Tile &tile = m_tiles[i];
        const QRect rect = tileRect(i);
        const QPainterPath shape = KisModernStyle::squircle(rect, KisModernStyle::tileRadius);

        p.save();
        p.setClipPath(shape);
        QColor paper = ink;
        paper.setAlphaF(0.06);
        p.fillRect(rect, paper);
        if (!tile.picture.isNull()) {
            const QSizeF size = QSizeF(tile.picture.size()) / tile.picture.devicePixelRatio();
            const QPointF at = QRectF(rect).center() - QPointF(size.width() / 2, size.height() / 2);
            p.drawImage(at, tile.picture);
        }
        p.restore();

        const bool current = tile.filterId == m_current;
        if (current || i == m_hovered) {
            QColor edge = ink;
            edge.setAlphaF(current ? 0.85 : 0.3);
            p.setPen(QPen(edge, current ? 2.0 : 1.0));
            p.setBrush(Qt::NoBrush);
            p.drawPath(KisModernStyle::squircle(QRectF(rect).adjusted(1, 1, -1, -1), KisModernStyle::tileRadius - 1));
        }

        p.setPen(current ? ink : KisModernStyle::secondaryInk(ink));
        p.drawText(QRect(rect.left(), rect.bottom() + 1, rect.width(), labelHeight(this)), Qt::AlignCenter, tile.name);
    }
}

void KisScaleFilterPreview::mousePressEvent(QMouseEvent *event)
{
    const int index = tileAt(event->pos());
    if (index < 0 || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_current = m_tiles[index].filterId;
    update();
    Q_EMIT picked(m_current);
}

void KisScaleFilterPreview::mouseMoveEvent(QMouseEvent *event)
{
    const int hovered = tileAt(event->pos());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void KisScaleFilterPreview::leaveEvent(QEvent *event)
{
    m_hovered = -1;
    update();
    QWidget::leaveEvent(event);
}
