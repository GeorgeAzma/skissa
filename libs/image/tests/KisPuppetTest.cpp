/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisPuppetTest.h"

#include <simpletest.h>

#include <QElapsedTimer>
#include <QPainter>

#include <KoColorSpaceRegistry.h>

#include <KisGridRenderer.h>
#include <KisPuppet.h>
#include <kis_paint_device.h>

namespace {

const QRect bounds(0, 0, 1200, 700);
// a long bar, and a blob far from it
const QRect bar(100, 300, 800, 100);
const QRect blob(1000, 80, 120, 120);

bool painted(const QRect &cell)
{
    return cell.intersects(bar) || cell.intersects(blob);
}

qreal length(const KisPuppet &puppet, qreal y)
{
    qreal total = 0;
    QPointF last = puppet.map(QPointF(bar.left(), y));
    for (int x = bar.left() + 10; x <= bar.right(); x += 10) {
        const QPointF point = puppet.map(QPointF(x, y));
        total += std::hypot(point.x() - last.x(), point.y() - last.y());
        last = point;
    }
    return total;
}

// the bent picture of stripes over the bar, to look at
void save(const KisPuppet &puppet, const QString &name)
{
    QImage image(bounds.size(), QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter p(&image);
    for (int x = bar.left(); x < bar.right(); x += 40) {
        p.fillRect(QRect(x, bar.top(), 20, bar.height()), Qt::darkBlue);
        p.fillRect(QRect(x + 20, bar.top(), 20, bar.height()), Qt::yellow);
    }
    p.fillRect(blob, Qt::red);
    p.end();

    KisPaintDeviceSP src = new KisPaintDevice(KoColorSpaceRegistry::instance()->rgb8());
    src->convertFromQImage(image, nullptr);
    KisPaintDeviceSP dst = new KisPaintDevice(src->colorSpace());
    const QSize grid(puppet.columns(), puppet.rows());
    KisGridRenderer(grid, puppet.restPoints(), puppet.bentPoints(), QRect(QPoint(), grid), KisGridRenderer::Exact).render(src, dst);
    dst->convertToQImage(nullptr, bounds).save(QDir::temp().filePath(name));
}

}

void KisPuppetTest::testOnePinMoves()
{
    KisPuppet puppet(bounds, 25, painted);
    const int pin = puppet.addPin(QPointF(200, 350));
    puppet.movePin(pin, QPointF(260, 330));

    // one pin moves its shape without turning it; the blob, without pins, stays
    const QPointF end = puppet.map(QPointF(800, 350));
    const QPointF other = puppet.map(QPointF(1050, 100));
    qDebug() << "bar end" << end << "blob" << other;
    QVERIFY(std::hypot(end.x() - 860, end.y() - 330) < 0.5);
    QVERIFY(std::hypot(other.x() - 1050, other.y() - 100) < 0.5);
}

void KisPuppetTest::testBendKeepsLength()
{
    KisPuppet puppet(bounds, 25, painted);
    puppet.addPin(QPointF(150, 350));
    puppet.addPin(QPointF(350, 350));
    const int tip = puppet.addPin(QPointF(850, 350));

    QElapsedTimer timer;
    timer.start();
    // as far from the middle pin as it was, half way down
    puppet.movePin(tip, QPointF(350 + 500 * std::cos(M_PI / 6), 350 + 500 * std::sin(M_PI / 6)));
    qDebug() << "pose" << timer.elapsed() << "ms," << puppet.columns() * puppet.rows() << "corners";
    save(puppet, "puppet-bend.png");

    // bent down a long way, the bar keeps its length and its thickness
    const qreal before = length(KisPuppet(bounds, 25, painted), 350);
    const qreal after = length(puppet, 350);
    qDebug() << "length" << before << "->" << after;
    QVERIFY(qAbs(after - before) < 0.05 * before);

    const QPointF top = puppet.map(QPointF(700, bar.top()));
    const QPointF bottom = puppet.map(QPointF(700, bar.bottom()));
    const qreal thickness = std::hypot(top.x() - bottom.x(), top.y() - bottom.y());
    qDebug() << "thickness" << bar.height() << "->" << thickness;
    QVERIFY(qAbs(thickness - bar.height()) < 0.1 * bar.height());
}

void KisPuppetTest::testAirLetsGo()
{
    KisPuppet puppet(bounds, 25, painted);
    puppet.addPin(QPointF(150, 350));
    puppet.addPin(QPointF(1060, 140));
    const int tip = puppet.addPin(QPointF(850, 350));
    puppet.movePin(tip, QPointF(700, 600));

    // the blob, held by its own pin across the air, neither moves nor turns
    const QPointF corner = puppet.map(QPointF(blob.left(), blob.top()));
    qDebug() << "blob corner moved" << corner - QPointF(blob.left(), blob.top());
    QVERIFY(std::hypot(corner.x() - blob.left(), corner.y() - blob.top()) < 2);
}

void KisPuppetTest::testSamePinsSamePose()
{
    KisPuppet a(bounds, 25, painted);
    KisPuppet b(bounds, 25, painted);
    for (KisPuppet *puppet : {&a, &b}) {
        puppet->addPin(QPointF(150, 350));
        const int tip = puppet->addPin(QPointF(850, 350));
        puppet->movePin(tip, QPointF(500, 100));
        puppet->movePin(tip, QPointF(800, 600));
    }
    // b reaches the same place by another way
    QCOMPARE(a.bentPoints(), b.bentPoints());
}

void KisPuppetTest::testPinStaysWhereAdded()
{
    KisPuppet puppet(bounds, 25, painted);
    puppet.addPin(QPointF(150, 350));
    const int tip = puppet.addPin(QPointF(850, 350));
    puppet.movePin(tip, QPointF(700, 600));

    // a pin added on the bent bar holds the point under it, and the pose barely moves
    const QVector<QPointF> before = puppet.bentPoints();
    const QPointF at = puppet.map(QPointF(500, 350));
    const int middle = puppet.addPin(at);
    const QPointF held = puppet.map(puppet.pinRest(middle));
    QVERIFY(std::hypot(held.x() - at.x(), held.y() - at.y()) < 0.5);

    // what shows, the bar, stays where it was
    qreal moved = 0;
    for (int i = 0; i < before.size(); i++) {
        const QPointF rest = puppet.restPoints()[i];
        if (!bar.adjusted(-25, -25, 25, 25).contains(rest.toPoint())) continue;
        const QPointF diff = puppet.bentPoints()[i] - before[i];
        moved = qMax(moved, std::hypot(diff.x(), diff.y()));
    }
    qDebug() << "a new pin moved the bar by" << moved;
    QVERIFY(moved < 1);
}

SIMPLE_TEST_MAIN(KisPuppetTest)
