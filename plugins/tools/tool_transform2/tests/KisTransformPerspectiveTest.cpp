/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisTransformPerspectiveTest.h"

#include <QImage>
#include <QPainter>
#include <QPen>
#include <QRandomGenerator>

#include "KisTransformPerspective.h"
#include "kis_transform_utils.h"
#include "tool_transform_args.h"

namespace
{
QPolygonF corners(const QRectF &rect)
{
    return QPolygonF({rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()});
}

ToolTransformArgs freeArgs(const QRectF &rect)
{
    ToolTransformArgs args;
    args.setMode(ToolTransformArgs::FREE_TRANSFORM);
    args.setOriginalCenter(rect.center());
    args.setTransformedCenter(rect.center());
    return args;
}

void checkLands(const ToolTransformArgs &args, const QPolygonF &src, const QPolygonF &dst)
{
    const QTransform t = KisTransformUtils::MatricesPack(args).finalTransform();
    for (int i = 0; i < 4; i++) {
        const QPointF p = t.map(src[i]);
        QVERIFY2(QLineF(p, dst[i]).length() < 1e-3 * QLineF(dst[0], dst[2]).length(),
                 qPrintable(QString("corner %1: (%2, %3) instead of (%4, %5)").arg(i).arg(p.x()).arg(p.y()).arg(dst[i].x()).arg(dst[i].y())));
    }
}
}

void KisTransformPerspectiveTest::testFit()
{
    const QRectF rect(100, 50, 400, 300);
    const QPolygonF src = corners(rect);
    QRandomGenerator random(7);
    int fitted = 0;
    for (int n = 0; n < 500; n++) {
        QPolygonF dst;
        for (int i = 0; i < 4; i++) {
            dst << src[i] + QPointF((random.generateDouble() * 300 - 150), (random.generateDouble() * 300 - 150));
        }
        ToolTransformArgs args = freeArgs(rect);
        if (!KisTransformPerspective::fit(args, src, dst)) continue;
        fitted++;
        checkLands(args, src, dst);
    }
    QVERIFY(fitted > 300);

    // a mirrored, turned box
    ToolTransformArgs args = freeArgs(rect);
    const QPolygonF turned({QPointF(600, 100), QPointF(620, 500), QPointF(300, 450), QPointF(320, 140)});
    QVERIFY(KisTransformPerspective::fit(args, src, turned));
    checkLands(args, src, turned);
}

void KisTransformPerspectiveTest::testFitRotatedBounds()
{
    const QRectF rect(0, 0, 200, 100);
    ToolTransformArgs args = freeArgs(rect);
    args.setBoundsRotation(0.4);
    QTransform br;
    br.rotateRadians(0.4, Qt::ZAxis);
    const QTransform around = QTransform::fromTranslate(-rect.center().x(), -rect.center().y()) * br
                            * QTransform::fromTranslate(rect.center().x(), rect.center().y());
    const QPolygonF src = around.map(corners(rect.adjusted(-30, -30, 30, 30)));
    QPolygonF dst = src;
    dst[0] += QPointF(40, 30);
    dst[2] += QPointF(-10, 50);
    QVERIFY(KisTransformPerspective::fit(args, src, dst));
    checkLands(args, src, dst);
}

void KisTransformPerspectiveTest::testFlattening()
{
    // a 2:1 board turned in space and photographed
    const QPointF principal(960, 540);
    const qreal focal = 1400;
    const qreal angles[][2] = {{0.5, 0.2}, {-0.3, 0.6}, {0.0, 0.9}, {0.7, -0.4}};
    for (const auto &a : angles) {
        QPolygonF quad;
        for (const QPointF &p : {QPointF(-1, -0.5), QPointF(1, -0.5), QPointF(1, 0.5), QPointF(-1, 0.5)}) {
            const qreal x1 = p.x() * std::cos(a[1]);
            const qreal z1 = -p.x() * std::sin(a[1]);
            const qreal y2 = p.y() * std::cos(a[0]) - z1 * std::sin(a[0]);
            const qreal z2 = p.y() * std::sin(a[0]) + z1 * std::cos(a[0]) + 4.0;
            // off center, as the photo's middle rarely is the board's
            quad << principal + focal * QPointF((x1 + 0.6) / z2, (y2 - 0.4) / z2);
        }
        QTransform t;
        // with one vanishing point the lens is a guess: give the right one
        QVERIFY(KisTransformPerspective::flattening(quad, principal, focal, &t));
        const QPolygonF flat = t.map(quad);
        const qreal w = QLineF(flat[0], flat[1]).length();
        const qreal h = QLineF(flat[1], flat[2]).length();
        QVERIFY2(std::abs(w / h - 2.0) < 0.01, qPrintable(QString("aspect %1").arg(w / h)));
        QVERIFY(qFuzzyCompare(flat[0].y(), flat[1].y()));
    }
}

void KisTransformPerspectiveTest::testVanishingPoint()
{
    // top and bottom edges meet at (1000, 100)
    const QPolygonF quad({QPointF(100, 40), QPointF(400, 60), QPointF(400, 200), QPointF(100, 250)});
    QPointF vp;
    QVERIFY(KisTransformPerspective::vanishingPoint(quad, true, &vp));
    QVERIFY(QLineF(vp, QPointF(1000, 100)).length() < 1e-6);
    QVERIFY(!KisTransformPerspective::vanishingPoint(quad, false, &vp));

    const QPolygonF moved = KisTransformPerspective::withVanishingPoint(quad, true, QPointF(700, 100));
    QVERIFY(KisTransformPerspective::vanishingPoint(moved, true, &vp));
    QVERIFY(QLineF(vp, QPointF(700, 100)).length() < 1e-6);
    // the far edge stays
    QCOMPARE(moved[0], quad[0]);
    QCOMPARE(moved[3], quad[3]);
}

void KisTransformPerspectiveTest::testVanishingLines()
{
    const QPolygonF quad({QPointF(100, 40), QPointF(400, 60), QPointF(400, 200), QPointF(100, 250)});
    QImage image(1100, 300, QImage::Format_ARGB32);
    image.fill(Qt::white);
    QPainter gc(&image);
    QPen pen(Qt::black);
    pen.setWidth(2);
    KisTransformPerspective::paintVanishingLines(gc, quad, QTransform(), pen);
    gc.end();

    auto inked = [&image](int x, int y) { return qGray(image.pixel(x, y)) < 128; };
    // out from the near corners to the meeting point, not back over the box
    QVERIFY(inked(700, 80));
    QVERIFY(inked(700, 150));
    QVERIFY(!inked(700, 115));
    QVERIFY(!inked(250, 112));
}

SIMPLE_TEST_MAIN(KisTransformPerspectiveTest)
