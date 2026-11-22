/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "OverviewLive.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <QThreadPool>

#include <KisDisplayConfig.h>
#include <kis_canvas2.h>
#include <kis_display_color_converter.h>
#include <kis_image.h>
#include <kis_paint_device.h>

#include "overviewwidget.h"

namespace {
/// How often a part is made while the image keeps changing
constexpr int interval = 50;
}

OverviewLive::OverviewLive(OverviewWidget *widget)
    : QObject(widget)
    , m_widget(widget)
    , m_compressor(interval, KisSignalCompressor::FIRST_ACTIVE, this)
{
    connect(&m_compressor, &KisSignalCompressor::timeout, this, [this]() { start(); });
}

void OverviewLive::setCanvas(KisCanvas2 *canvas)
{
    disconnect(m_updated);
    m_canvas = canvas;
    m_dirty = QRect();
    if (!canvas || !canvas->image()) return;
    // reported from the image's own threads: queued to this one
    m_updated = connect(canvas->image().data(), &KisImage::sigImageUpdated, this, [this](const QRect &rect) {
        m_dirty |= rect;
        m_compressor.start();
    });
}

void OverviewLive::start()
{
    if (m_busy || m_dirty.isEmpty() || !m_canvas || !m_canvas->image() || !m_widget->isVisible()) return;
    KisImageSP image = m_canvas->image();
    // the picture as Krita's update last made it, in its own pixels; until there is one, nothing to add to
    const QSize thumbnail = m_widget->m_pixmap.size();
    if (m_widget->m_pixmapImage.data() != image.data()) return;
    const QRect bounds = image->bounds();
    if (thumbnail.isEmpty() || bounds.isEmpty()) return;

    const qreal sx = qreal(thumbnail.width()) / bounds.width();
    const qreal sy = qreal(thumbnail.height()) / bounds.height();
    const QRectF changed = m_dirty.translated(-bounds.topLeft());
    const QRect whole(QPoint(), thumbnail);
    // a pixel around it, as the downscale spreads; made from a little more, so its edge pixels aren't half empty
    const QRect target = QRectF(changed.x() * sx, changed.y() * sy, changed.width() * sx, changed.height() * sy)
                             .toAlignedRect().adjusted(-1, -1, 1, 1) & whole;
    const QRect sampled = target.adjusted(-2, -2, 2, 2) & whole;
    m_dirty = QRect();
    if (target.isEmpty()) return;

    m_busy = true;
    const KisDisplayConfig config = m_canvas->displayColorConverter()->displayConfig();
    KisPaintDeviceSP projection = image->projection();
    QPointer<OverviewLive> self(this);
    KisImageWSP made(image);
    QThreadPool::globalInstance()->start([self, made, projection, bounds, thumbnail, target, sampled, config]() {
        KisPaintDeviceSP part = projection->createThumbnailDeviceOversampled(thumbnail.width(), thumbnail.height(), 2.0, bounds, sampled);
        const QImage picture = part->convertToQImage(config.profile, target, config.intent, config.conversionFlags);
        QMetaObject::invokeMethod(qApp, [self, made, picture, target, thumbnail]() {
            if (!self) return;
            self->m_busy = false;
            QPixmap &pixmap = self->m_widget->m_pixmap;
            // made again meanwhile at another size, or of another image: Krita's update has it
            if (pixmap.size() == thumbnail && self->m_widget->m_pixmapImage.data() == made.data() && !picture.isNull()) {
                QPainter painter(&pixmap);
                painter.setCompositionMode(QPainter::CompositionMode_Source);
                painter.drawImage(target.topLeft(), picture);
                painter.end();
                self->m_widget->update();
            }
            if (!self->m_dirty.isEmpty()) self->m_compressor.start();
        }, Qt::QueuedConnection);
    });
}
