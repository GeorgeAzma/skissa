/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisSuperellipse.h"

#include <algorithm>
#include <cmath>

#include <QPainter>
#include <QPixmap>
#include <QTransform>
#include <QtMath>

#include <KSharedConfig>
#include <kconfiggroup.h>
#include <klocalizedstring.h>

#include "KisSettingsRegistry.h"
#include "KisStudioTools.h"

namespace KisSuperellipse
{

namespace
{

const QString keyPrefix = QStringLiteral("shape:");
const QString toolId = QStringLiteral("KritaShape/KisToolSuperellipse");

KConfigGroup toolConfig()
{
    return KSharedConfig::openConfig()->group(toolId);
}

Look bounded(Look look)
{
    look.sides = qBound(leastSides, look.sides, mostSides);
    look.roundness = qBound(0.0, look.roundness, 1.0);
    look.curve = qBound(leastCurve, look.curve, mostCurve);
    look.angle = std::fmod(look.angle, 360.0);
    return look;
}

QPointF unit(const QPointF &v)
{
    const qreal length = std::hypot(v.x(), v.y());
    return length > 0 ? v / length : QPointF();
}

} // namespace

QPainterPath pathOf(const QRectF &box, const Look &wanted)
{
    QPainterPath path;
    const QRectF r = box.normalized();
    if (r.isEmpty()) return path;
    const Look look = bounded(wanted);
    const int n = look.sides;

    // the polygon: a side flat at the bottom, clockwise on screen, turned, then stretched to the box
    QVector<QPointF> corners(n);
    for (int k = 0; k < n; k++) {
        const qreal phi = qDegreesToRadians(90.0 + 180.0 / n + 360.0 * k / n + look.angle);
        corners[k] = QPointF(std::cos(phi), std::sin(phi));
    }
    qreal left = corners[0].x(), right = left, top = corners[0].y(), bottom = top;
    Q_FOREACH (const QPointF &p, corners) {
        left = std::min(left, p.x());
        right = std::max(right, p.x());
        top = std::min(top, p.y());
        bottom = std::max(bottom, p.y());
    }
    for (QPointF &p : corners) {
        p = QPointF(r.left() + (p.x() - left) / std::max(1e-9, right - left) * r.width(),
                    r.top() + (p.y() - top) / std::max(1e-9, bottom - top) * r.height());
    }

    // each corner reaches as far along its sides as roundness asks of the longest half side, at most half of each
    QVector<qreal> halfSide(n);
    qreal longest = 0;
    for (int k = 0; k < n; k++) {
        const QPointF side = corners[(k + 1) % n] - corners[k];
        halfSide[k] = std::hypot(side.x(), side.y()) / 2;
        longest = std::max(longest, halfSide[k]);
    }
    const qreal reach = look.roundness * longest;
    if (reach < 1e-6) {
        path.addPolygon(QPolygonF(corners));
        path.closeSubpath();
        return path;
    }
    const qreal power = 2 / look.curve;
    bool first = true;
    for (int k = 0; k < n; k++) {
        const QPointF corner = corners[k];
        const QPointF before = corners[(k + n - 1) % n], after = corners[(k + 1) % n];
        const qreal back = std::min(reach, halfSide[(k + n - 1) % n]), on = std::min(reach, halfSide[k]);
        const QPointF toBefore = unit(before - corner), toAfter = unit(after - corner);
        // the Lamé curve in the parallelogram of the two sides, from the side before to the side after
        const QPointF middle = corner + back * toBefore + on * toAfter;
        const int steps = std::clamp(int(std::max(back, on) / 2), 24, 256);
        for (int i = 0; i <= steps; i++) {
            const qreal t = qreal(i) / steps * M_PI / 2;
            const qreal c = std::pow(std::max(0.0, std::cos(t)), power), s = std::pow(std::max(0.0, std::sin(t)), power);
            const QPointF p = middle - on * c * toAfter - back * s * toBefore;
            if (first) {
                path.moveTo(p);
                first = false;
            } else {
                path.lineTo(p);
            }
        }
    }
    path.closeSubpath();

    // rounding can pull the shape in from its box: it still fills the box
    const QRectF drawn = path.boundingRect();
    if (drawn.width() > 1e-6 && drawn.height() > 1e-6) {
        QTransform fit;
        fit.translate(r.left(), r.top());
        fit.scale(r.width() / drawn.width(), r.height() / drawn.height());
        fit.translate(-drawn.left(), -drawn.top());
        path = fit.map(path);
    }
    return path;
}

QVector<Preset> presets()
{
    return {
        {QStringLiteral("rectangle"), i18nc("a shape", "Rectangle"), {4, 0, 2, 0}},
        {QStringLiteral("rounded"), i18nc("a shape: a rectangle with round corners", "Rounded"), {4, 0.25, 2, 0}},
        {QStringLiteral("squircle"), i18nc("a shape between a square and a circle", "Squircle"), {4, 1, 4.5, 0}},
        {QStringLiteral("ellipse"), i18nc("a shape", "Ellipse"), {4, 1, 2, 0}},
        {QStringLiteral("diamond"), i18nc("a shape", "Diamond"), {4, 1, 1, 0}},
        {QStringLiteral("star"), i18nc("a shape: four points curving inward", "Star"), {4, 1, 0.5, 0}},
        {QStringLiteral("triangle"), i18nc("a shape", "Triangle"), {3, 0, 2, 0}},
        {QStringLiteral("soft-triangle"), i18nc("a shape: a triangle with round corners", "Rounded triangle"), {3, 0.3, 2, 0}},
        {QStringLiteral("pentagon"), i18nc("a shape", "Pentagon"), {5, 0, 2, 0}},
        {QStringLiteral("hexagon"), i18nc("a shape", "Hexagon"), {6, 0, 2, 0}},
        {QStringLiteral("octagon"), i18nc("a shape", "Octagon"), {8, 0, 2, 0}},
        {QStringLiteral("five-star"), i18nc("a shape: five points curving inward", "Five-point star"), {5, 1, 0.5, 36}},
    };
}

QString keyOf(const Look &wanted)
{
    const Look look = bounded(wanted);
    Q_FOREACH (const Preset &preset, presets()) {
        if (preset.look == look) return keyPrefix + preset.id;
    }
    return keyPrefix
        + QStringLiteral("%1,%2,%3,%4").arg(look.sides).arg(look.roundness, 0, 'g', 4).arg(look.curve, 0, 'g', 4).arg(look.angle, 0, 'g', 5);
}

bool lookOf(const QString &key, Look *look)
{
    if (!key.startsWith(keyPrefix)) return false;
    const QString rest = key.mid(keyPrefix.size());
    Q_FOREACH (const Preset &preset, presets()) {
        if (preset.id == rest) {
            if (look) *look = preset.look;
            return true;
        }
    }
    const QStringList parts = rest.split(QLatin1Char(','));
    if (parts.size() != 4) return false;
    bool ok[4];
    Look read;
    read.sides = parts[0].toInt(&ok[0]);
    read.roundness = parts[1].toDouble(&ok[1]);
    read.curve = parts[2].toDouble(&ok[2]);
    read.angle = parts[3].toDouble(&ok[3]);
    if (!ok[0] || !ok[1] || !ok[2] || !ok[3]) return false;
    if (look) *look = bounded(read);
    return true;
}

QString nameOf(const Look &wanted)
{
    const Look look = bounded(wanted);
    Q_FOREACH (const Preset &preset, presets()) {
        if (preset.look == look) return preset.name;
    }
    return i18nc("a shape of the user's own, by its number of sides", "%1-sided shape", look.sides);
}

QIcon iconOf(const Look &look, const QColor &ink, int size)
{
    QIcon icon;
    const qreal margin = size / 6.0;
    for (qreal ratio : {1.0, 1.5, 2.0}) {
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setPen(QPen(ink, 1.75, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(pathOf(QRectF(margin, margin, size - 2 * margin, size - 2 * margin), look));
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

Look current()
{
    const KConfigGroup config = toolConfig();
    const Look rounded = presets()[1].look;
    Look look;
    look.sides = config.readEntry("skissaSides", rounded.sides);
    look.roundness = config.readEntry("skissaRoundness", rounded.roundness);
    look.curve = config.readEntry("skissaCurve", rounded.curve);
    look.angle = config.readEntry("skissaAngle", rounded.angle);
    return bounded(look);
}

void use(const Look &wanted)
{
    const Look look = bounded(wanted);
    if (look == current()) return;
    KConfigGroup config = toolConfig();
    config.writeEntry("skissaSides", look.sides);
    config.writeEntry("skissaRoundness", look.roundness);
    config.writeEntry("skissaCurve", look.curve);
    config.writeEntry("skissaAngle", look.angle);
    KisSettingsRegistry::instance()->scheduleSync();
    Q_EMIT Watch::instance()->changed();
}

void draw(const Look &look)
{
    use(look);
    KisStudioTools::switchTo(toolId);
}

Watch *Watch::instance()
{
    static Watch *watch = new Watch();
    return watch;
}

} // namespace KisSuperellipse
