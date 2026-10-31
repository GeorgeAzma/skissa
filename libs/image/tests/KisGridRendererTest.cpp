/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisGridRendererTest.h"

#include <simpletest.h>

#include <QDomDocument>
#include <QElapsedTimer>
#include <QRandomGenerator>

#include <KoColorSpaceRegistry.h>
#include <quazip.h>
#include <quazipfile.h>

#include <testutil.h>
#include <kis_grid_interpolation_tools.h>
#include <kis_liquify_transform_worker.h>
#include <kis_warptransform_worker.h>
#include <kis_paint_device.h>
#include <KisGridRenderer.h>

namespace {

KisPaintDeviceSP noise(const KoColorSpace *cs, const QRect &rect, quint32 seed)
{
    KisPaintDeviceSP dev = new KisPaintDevice(cs);
    QRandomGenerator random(seed);
    QVector<quint8> bytes(rect.width() * rect.height() * cs->pixelSize());
    for (quint8 &byte : bytes) byte = quint8(random.bounded(256));
    dev->writeBytes(bytes.constData(), rect);
    return dev;
}

// Krita's own drawing of the grid, which the renderer must match
void drawAsKrita(KisLiquifyTransformWorker &worker, const QRect &subGrid, KisPaintDeviceSP src, KisPaintDeviceSP dst)
{
    using namespace GridIterationTools;
    PaintDevicePolygonOp polygonOp(src, dst);
    RegularGridIndexesOp indexesOp(worker.gridSize());
    polygonOp.setCanMergeRects(canProcessRectsInRandomOrder(indexesOp, worker.transformedPoints(), subGrid));
    iterateThroughGrid<AlwaysCompletePolygonPolicy>(polygonOp, indexesOp, worker.gridSize(),
                                                    worker.originalPoints(), worker.transformedPoints(), subGrid);
}

QByteArray bytesOf(KisPaintDeviceSP dev, const QRect &rect)
{
    QByteArray bytes(rect.width() * rect.height() * dev->pixelSize(), 0);
    dev->readBytes(reinterpret_cast<quint8 *>(bytes.data()), rect);
    return bytes;
}

void compare(KisLiquifyTransformWorker &worker, KisPaintDeviceSP src, const QString &name)
{
    const QRect subGrid(QPoint(0, 0), worker.gridSize());

    KisPaintDeviceSP expected = new KisPaintDevice(src->colorSpace());
    QElapsedTimer timer;
    timer.start();
    drawAsKrita(worker, subGrid, src, expected);
    const qint64 kritaMs = timer.elapsed();

    KisPaintDeviceSP result = new KisPaintDevice(src->colorSpace());
    timer.restart();
    KisGridRenderer(worker.gridSize(), worker.originalPoints(), worker.transformedPoints(), subGrid).render(src, result);
    const qint64 rendererMs = timer.elapsed();

    qDebug().noquote() << name << "Krita" << kritaMs << "ms, renderer" << rendererMs << "ms";

    const QRect bounds = expected->exactBounds() | result->exactBounds();
    QCOMPARE(result->exactBounds(), expected->exactBounds());
    QVERIFY2(bytesOf(result, bounds) == bytesOf(expected, bounds), qPrintable(name));

    // redrawing only some areas over other content gives the same pixels there
    KisPaintDeviceSP partial = noise(src->colorSpace(), bounds, 7);
    QRandomGenerator random(11);
    QVector<QRect> areas;
    for (int i = 0; i < 6; i++) {
        const int w = 20 + random.bounded(300);
        const int h = 20 + random.bounded(300);
        areas << QRect(bounds.left() + random.bounded(qMax(1, bounds.width() - w)),
                       bounds.top() + random.bounded(qMax(1, bounds.height() - h)), w, h);
    }
    KisGridRenderer(worker.gridSize(), worker.originalPoints(), worker.transformedPoints(), subGrid).render(src, partial, areas);
    for (const QRect &area : areas) {
        QVERIFY2(bytesOf(partial, area) == bytesOf(expected, area), qPrintable(name + " areas"));
    }
}

void liquify(KisLiquifyTransformWorker &worker, const QRect &bounds, quint32 seed, bool wash)
{
    QRandomGenerator random(seed);
    auto anywhere = [&]() {
        return QPointF(bounds.left() + random.bounded(qreal(bounds.width())), bounds.top() + random.bounded(qreal(bounds.height())));
    };
    for (int i = 0; i < 40; i++) {
        const qreal sigma = 10 + random.bounded(120.0);
        const qreal flow = random.bounded(1.0);
        switch (i % 3) {
        case 0:
            worker.translatePoints(anywhere(), QPointF(random.bounded(400.0) - 200, random.bounded(400.0) - 200), sigma, wash, flow);
            break;
        case 1:
            worker.rotatePoints(anywhere(), random.bounded(8.0) - 4, sigma, wash, flow);
            break;
        default:
            worker.scalePoints(anywhere(), random.bounded(4.0) - 2, sigma, wash, flow);
            break;
        }
    }
}

KisLiquifyTransformWorker *fromZip(const QString &filename)
{
    QuaZip zip(TestUtil::fetchDataFileLazy(filename));
    if (!zip.open(QuaZip::mdUnzip) || !zip.goToFirstFile()) return nullptr;
    QuaZipFile file(&zip);
    if (!file.open(QIODevice::ReadOnly)) return nullptr;
    QDomDocument doc;
    doc.setContent(&file);
    return KisLiquifyTransformWorker::fromXML(doc.documentElement().firstChildElement("data"));
}

}

void KisGridRendererTest::testSameAsKrita_data()
{
    QTest::addColumn<bool>("deep");
    QTest::addColumn<bool>("wash");
    QTest::addColumn<quint32>("seed");

    QTest::addRow("8 bit") << false << false << 1u;
    QTest::addRow("8 bit wash") << false << true << 2u;
    QTest::addRow("16 bit") << true << false << 3u;
    QTest::addRow("16 bit wash") << true << true << 4u;
}

void KisGridRendererTest::testSameAsKrita()
{
    QFETCH(bool, deep);
    QFETCH(bool, wash);
    QFETCH(quint32, seed);

    const KoColorSpace *cs = deep ? KoColorSpaceRegistry::instance()->rgb16() : KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 1600, 1200);
    KisPaintDeviceSP src = noise(cs, bounds, seed);

    KisLiquifyTransformWorker worker(bounds, nullptr, 8);
    liquify(worker, bounds, seed, wash);
    compare(worker, src, QTest::currentDataTag());
}

void KisGridRendererTest::testChanges_data()
{
    QTest::addColumn<bool>("wash");
    QTest::addColumn<quint32>("seed");

    QTest::addRow("build up") << false << 21u;
    QTest::addRow("wash") << true << 22u;
}

void KisGridRendererTest::testChanges()
{
    QFETCH(bool, wash);
    QFETCH(quint32, seed);

    const QRect bounds(0, 0, 1600, 1200);
    KisPaintDeviceSP src = noise(KoColorSpaceRegistry::instance()->rgb8(), bounds, seed);

    KisLiquifyTransformWorker worker(bounds, nullptr, 8);
    KisPaintDeviceSP kept = new KisPaintDevice(src->colorSpace());
    worker.run(src, kept);

    QRandomGenerator random(seed);
    for (int step = 0; step < 30; step++) {
        KisLiquifyTransformWorker previous(worker);
        const QPointF base(random.bounded(qreal(bounds.width())), random.bounded(qreal(bounds.height())));
        const qreal sigma = 5 + random.bounded(60.0);
        switch (step % 3) {
        case 0:
            worker.translatePoints(base, QPointF(random.bounded(200.0) - 100, random.bounded(200.0) - 100), sigma, wash, 0.5);
            break;
        case 1:
            worker.rotatePoints(base, random.bounded(6.0) - 3, sigma, wash, 0.5);
            break;
        default:
            worker.scalePoints(base, random.bounded(2.0) - 1, sigma, wash, 0.5);
            break;
        }

        QRect changed;
        QVERIFY(worker.runChanges(previous, src, kept, &changed));

        KisPaintDeviceSP expected = new KisPaintDevice(src->colorSpace());
        worker.run(src, expected);

        const QRect all = expected->exactBounds() | kept->exactBounds();
        QVERIFY2(bytesOf(kept, all) == bytesOf(expected, all), qPrintable(QString("step %1").arg(step)));
    }
}

void KisGridRendererTest::testWarpSameAsKrita_data()
{
    QTest::addColumn<int>("type");
    QTest::addColumn<bool>("deep");

    QTest::addRow("affine") << int(KisWarpTransformWorker::AFFINE_TRANSFORM) << false;
    QTest::addRow("similitude") << int(KisWarpTransformWorker::SIMILITUDE_TRANSFORM) << false;
    QTest::addRow("rigid") << int(KisWarpTransformWorker::RIGID_TRANSFORM) << false;
    QTest::addRow("rigid 16 bit") << int(KisWarpTransformWorker::RIGID_TRANSFORM) << true;
}

void KisGridRendererTest::testWarpSameAsKrita()
{
    QFETCH(int, type);
    QFETCH(bool, deep);

    const KoColorSpace *cs = deep ? KoColorSpaceRegistry::instance()->rgb16() : KoColorSpaceRegistry::instance()->rgb8();
    const QRect bounds(0, 0, 1600, 1200);
    KisPaintDeviceSP src = noise(cs, bounds, 31 + type);

    // a 4 by 4 grid of handles, pulled around hard enough to fold
    QRandomGenerator random(41 + type);
    QVector<QPointF> from;
    QVector<QPointF> to;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            const QPointF point(bounds.width() * (x + 0.5) / 4, bounds.height() * (y + 0.5) / 4);
            from << point;
            to << point + QPointF(random.bounded(500.0) - 250, random.bounded(500.0) - 250);
        }
    }

    const KisWarpTransformWorker::WarpType warpType = KisWarpTransformWorker::WarpType(type);
    QPointF (*function)(QPointF, QVector<QPointF>, QVector<QPointF>, qreal) =
        warpType == KisWarpTransformWorker::AFFINE_TRANSFORM ? &KisWarpTransformWorker::affineTransformMath :
        warpType == KisWarpTransformWorker::SIMILITUDE_TRANSFORM ? &KisWarpTransformWorker::similitudeTransformMath :
        &KisWarpTransformWorker::rigidTransformMath;

    // Krita's own warp
    KisPaintDeviceSP expected = new KisPaintDevice(cs);
    QElapsedTimer timer;
    timer.start();
    {
        auto functionOp = [&](const QPointF &point) { return function(point, from, to, 1.0); };
        GridIterationTools::PaintDevicePolygonOp polygonOp(src, expected);
        polygonOp.setCanMergeRects(false);
        GridIterationTools::processGrid(polygonOp, functionOp, src->region().boundingRect(), 8);
        polygonOp.finalize();
    }
    const qint64 kritaMs = timer.elapsed();

    KisPaintDeviceSP result = new KisPaintDevice(cs);
    timer.restart();
    KisWarpTransformWorker(warpType, from, to, 1.0, nullptr).run(src, result);
    const qint64 workerMs = timer.elapsed();
    qDebug().noquote() << QTest::currentDataTag() << "Krita" << kritaMs << "ms, now" << workerMs << "ms";

    QCOMPARE(result->exactBounds(), expected->exactBounds());
    const QRect all = expected->exactBounds();
    QVERIFY(bytesOf(result, all) == bytesOf(expected, all));
}

void KisGridRendererTest::testSavedConfigs_data()
{
    QTest::addColumn<QString>("config");

    QTest::addRow("mask") << "liquify_mask_rendering/liquify_mask_rendering_mask_config.zip";
    QTest::addRow("small change") << "liquify_mask_rendering/liquify_mask_rendering_small_change_mask_config.zip";
    QTest::addRow("16 bit mask") << "liquify_mask_rendering/liquify_mask_rendering_16bit_mask_config.zip";
}

void KisGridRendererTest::testSavedConfigs()
{
    QFETCH(QString, config);

    QScopedPointer<KisLiquifyTransformWorker> worker(fromZip(config));
    QVERIFY(worker);

    QRectF bounds;
    for (const QPointF &point : worker->originalPoints()) KisAlgebra2D::accumulateBounds(point, &bounds);
    KisPaintDeviceSP src = noise(KoColorSpaceRegistry::instance()->rgb8(), bounds.toAlignedRect(), 5);
    compare(*worker, src, config);
}

SIMPLE_TEST_MAIN(KisGridRendererTest)
