/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisResamplingTest.h"

#include <simpletest.h>

#include <QRandomGenerator>

#include <KoColorSpaceRegistry.h>
#include <KoUpdater.h>

#include <KisResampling.h>
#include <kis_filter_strategy.h>
#include <kis_paint_device.h>
#include <kis_transform_worker.h>

namespace {

KisFilterStrategy *filter(const char *id)
{
    return KisFilterStrategyRegistry::instance()->value(QLatin1String(id));
}

KisPaintDeviceSP scaled(const QImage &image, qreal scale, const char *id)
{
    KisPaintDeviceSP dev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    dev->convertFromQImage(image, nullptr);
    KisTransformWorker(dev, scale, scale, 0, 0, 0, 0, 0, KoUpdaterPtr(), filter(id)).run();
    return dev;
}

// a little sprite: four colours in blocks of single pixels
QImage sprite()
{
    QImage image(16, 16, QImage::Format_ARGB32);
    QRandomGenerator random(5);
    const QRgb colors[4] = {qRgb(20, 30, 60), qRgb(230, 80, 40), qRgb(250, 220, 120), qRgb(60, 160, 90)};
    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) image.setPixel(x, y, colors[random.bounded(4)]);
    }
    return image;
}

}

void KisResamplingTest::testPixelCopiesWholeScales()
{
    const QImage source = sprite();
    for (int scale : {2, 3, 5}) {
        const QImage result = scaled(source, scale, "Pixel")->convertToQImage(nullptr, 0, 0, 16 * scale, 16 * scale);
        int wrong = 0;
        for (int y = 0; y < result.height(); y++) {
            for (int x = 0; x < result.width(); x++) {
                if (result.pixel(x, y) != source.pixel(x / scale, y / scale)) wrong++;
            }
        }
        QVERIFY2(wrong == 0, qPrintable(QString("%1x: %2 pixels differ").arg(scale).arg(wrong)));
    }
}

void KisResamplingTest::testPixelKeepsPixelsEven()
{
    // at 2.5x each pixel is 2 or 3 wide, never smeared over more: inside every block, one colour
    const QImage source = sprite();
    const QImage result = scaled(source, 2.5, "Pixel")->convertToQImage(nullptr, 0, 0, 40, 40);
    int smeared = 0;
    for (int y = 0; y < 40; y++) {
        for (int x = 0; x < 40; x++) {
            // a pixel well inside a source pixel's area
            const qreal sx = (x + 0.5) / 2.5;
            const qreal sy = (y + 0.5) / 2.5;
            const qreal fx = sx - std::floor(sx);
            const qreal fy = sy - std::floor(sy);
            if (fx < 0.25 || fx > 0.75 || fy < 0.25 || fy > 0.75) continue;
            if (result.pixel(x, y) != source.pixel(int(sx), int(sy))) smeared++;
        }
    }
    QCOMPARE(smeared, 0);
}

void KisResamplingTest::testSmoothHasNoHalos()
{
    // a hard edge between two greys, enlarged: Lanczos overshoots past them, Smooth stays between
    QImage edge(32, 8, QImage::Format_ARGB32);
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 32; x++) edge.setPixel(x, y, x < 16 ? qRgb(64, 64, 64) : qRgb(192, 192, 192));
    }
    auto range = [](const QImage &image) {
        int low = 255;
        int high = 0;
        for (int y = 2; y < image.height() - 2; y++) {
            for (int x = 8; x < image.width() - 8; x++) {
                low = qMin(low, qRed(image.pixel(x, y)));
                high = qMax(high, qRed(image.pixel(x, y)));
            }
        }
        return qMakePair(low, high);
    };
    const auto lanczos = range(scaled(edge, 3, "Lanczos3")->convertToQImage(nullptr, 0, 0, 96, 24));
    const auto smooth = range(scaled(edge, 3, "Smooth")->convertToQImage(nullptr, 0, 0, 96, 24));
    qDebug() << "Lanczos3" << lanczos << "Smooth" << smooth;
    QVERIFY(lanczos.first < 64 || lanczos.second > 192);
    QVERIFY(smooth.first >= 64 && smooth.second <= 192);
}

void KisResamplingTest::testShrinkingKeepsLight()
{
    // one white line a pixel wide on black, halved: in light it is half as bright, which encoded is far
    // brighter than half the number
    QImage line(64, 64, QImage::Format_ARGB32);
    line.fill(qRgb(0, 0, 0));
    for (int y = 0; y < 64; y++) line.setPixel(31, y, qRgb(255, 255, 255));
    auto brightest = [](const QImage &image) {
        int high = 0;
        for (int x = 0; x < image.width(); x++) high = qMax(high, qRed(image.pixel(x, 16)));
        return high;
    };
    const int bicubic = brightest(scaled(line, 0.5, "Bicubic")->convertToQImage(nullptr, 0, 0, 32, 32));
    const int smooth = brightest(scaled(line, 0.5, "Smooth")->convertToQImage(nullptr, 0, 0, 32, 32));
    qDebug() << "brightest after halving: Bicubic" << bicubic << "Smooth" << smooth;
    QVERIFY(smooth > bicubic + 30);
}

void KisResamplingTest::testTellsPixelArt()
{
    KisPaintDeviceSP spriteDev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    spriteDev->convertFromQImage(sprite(), nullptr);
    QVERIFY(KisResampling::looksLikePixelArt(spriteDev, QRect(0, 0, 16, 16)));

    QImage noise(200, 200, QImage::Format_ARGB32);
    QRandomGenerator random(9);
    for (int y = 0; y < 200; y++) {
        for (int x = 0; x < 200; x++) noise.setPixel(x, y, qRgb(random.bounded(256), random.bounded(256), random.bounded(256)));
    }
    KisPaintDeviceSP noiseDev = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    noiseDev->convertFromQImage(noise, nullptr);
    QVERIFY(!KisResampling::looksLikePixelArt(noiseDev, QRect(0, 0, 200, 200)));
}

SIMPLE_TEST_MAIN(KisResamplingTest)
