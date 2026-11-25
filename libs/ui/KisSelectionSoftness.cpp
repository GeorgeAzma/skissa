/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSelectionSoftness.h"

#include <QCoreApplication>
#include <QPointer>

#include <kis_image.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_sequential_iterator.h>
#include <kis_spontaneous_job.h>

namespace {

class SoftBandJob : public KisSpontaneousJob
{
public:
    SoftBandJob(KisSelectionSP selection, const QColor &color, QObject *receiver,
                const std::function<void(const QImage &, const QTransform &)> &done)
        : m_selection(selection), m_color(color), m_receiver(receiver), m_done(done)
    {
    }

    bool overrides(const KisSpontaneousJob *otherJob) override
    {
        return dynamic_cast<const SoftBandJob *>(otherJob);
    }

    void run() override
    {
        QImage band;
        QTransform at;
        KisPixelSelectionSP pixels = m_selection->pixelSelection();
        const QRect rc = pixels ? pixels->selectedExactRect() : QRect();
        if (!rc.isEmpty()) {
            // a big selection is read smaller: the veil is a hint, not the pixels
            const int largest = 1500;
            KisPaintDeviceSP source = pixels;
            QRect read = rc;
            at = QTransform::fromTranslate(rc.x(), rc.y());
            if (rc.width() > largest || rc.height() > largest) {
                const qreal factor = qreal(largest) / qMax(rc.width(), rc.height());
                read = QRect(0, 0, qMax(1, qRound(rc.width() * factor)), qMax(1, qRound(rc.height() * factor)));
                source = pixels->createThumbnailDevice(read.width(), read.height(), rc);
                at = QTransform::fromScale(qreal(rc.width()) / read.width(), qreal(rc.height()) / read.height()) * at;
            }
            QImage image(read.size(), QImage::Format_ARGB32_Premultiplied);
            image.fill(Qt::transparent);
            bool soft = false;
            KisSequentialConstIterator it(source, read);
            while (it.nextPixel()) {
                const quint8 selected = *it.rawDataConst();
                if (selected == 0 || selected == MAX_SELECTED) continue;
                soft = true;
                // clear where nearly selected, deepest where it is about to end
                const qreal veil = 0.55 * (MAX_SELECTED - selected) / qreal(MAX_SELECTED);
                image.setPixel(it.x() - read.x(), it.y() - read.y(),
                               qPremultiply(qRgba(m_color.red(), m_color.green(), m_color.blue(), qRound(255 * veil))));
            }
            if (soft) band = image;
        }
        // checked on the GUI thread, where the receiver may have gone
        const QPointer<QObject> receiver = m_receiver;
        const auto done = m_done;
        QMetaObject::invokeMethod(qApp, [receiver, done, band, at]() {
            if (receiver) done(band, at);
        }, Qt::QueuedConnection);
    }

    int levelOfDetail() const override
    {
        return 0;
    }

    QString debugName() const override
    {
        return QStringLiteral("SoftBandJob");
    }

private:
    KisSelectionSP m_selection;
    QColor m_color;
    QPointer<QObject> m_receiver;
    std::function<void(const QImage &, const QTransform &)> m_done;
};

}

void KisSelectionSoftness::start(KisImageSP image, KisSelectionSP selection, const QColor &color, QObject *receiver,
                                 const std::function<void(const QImage &, const QTransform &)> &done)
{
    if (!image || !selection) return;
    image->addSpontaneousJob(new SoftBandJob(selection, color, receiver, done));
}
