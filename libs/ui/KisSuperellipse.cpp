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
#include <QTimer>
#include <QTransform>
#include <QtMath>

#include <limits>

#include <klocalizedstring.h>

#include "KisSettingsRegistry.h"
#include "KisStudioTools.h"

namespace KisSuperellipse
{

namespace
{

const QString keyPrefix = QStringLiteral("shape:");
const QString toolId = QStringLiteral("KritaShape/KisToolSuperellipse");

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

qreal cross(const QPointF &a, const QPointF &b)
{
    return a.x() * b.y() - a.y() * b.x();
}

/// The look's polygon stretched to \p frame, its corners rounded
QPainterPath roundedIn(const QRectF &frame, const Look &look)
{
    QPainterPath path;
    const int n = look.sides;

    // the polygon: a side flat at the bottom, clockwise on screen, turned, then stretched to its frame
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
        p = QPointF(frame.left() + (p.x() - left) / std::max(1e-9, right - left) * frame.width(),
                    frame.top() + (p.y() - top) / std::max(1e-9, bottom - top) * frame.height());
    }

    // each corner reaches as far along its sides as roundness asks of the shortest half side (stretched, of the
    // longest), at most half of each
    QVector<qreal> halfSide(n);
    qreal longest = 0, shortest = std::numeric_limits<qreal>::max();
    for (int k = 0; k < n; k++) {
        const QPointF side = corners[(k + 1) % n] - corners[k];
        halfSide[k] = std::hypot(side.x(), side.y()) / 2;
        longest = std::max(longest, halfSide[k]);
        shortest = std::min(shortest, halfSide[k]);
    }
    const qreal reach = look.roundness * (look.stretched ? longest : shortest);
    if (reach < 1e-6) {
        path.addPolygon(QPolygonF(corners));
        path.closeSubpath();
        return path;
    }

    // Each corner keeps to its own wedge, between the lines from the polygon's middle to the middles of its two
    // sides: the wedges never overlap, so neither do the corners (pinched ones reach far inward at sharp corners)
    QPointF center;
    for (const QPointF &p : corners) center += p / n;
    QVector<QPointF> sideMiddles(n);
    for (int k = 0; k < n; k++) sideMiddles[k] = (corners[k] + corners[(k + 1) % n]) / 2;

    const qreal power = 2 / look.curve;
    bool first = true;
    QVector<QPointF> offsets;
    for (int k = 0; k < n; k++) {
        const QPointF corner = corners[k];
        const QPointF before = corners[(k + n - 1) % n], after = corners[(k + 1) % n];
        const qreal back = std::min(reach, halfSide[(k + n - 1) % n]), on = std::min(reach, halfSide[k]);
        const QPointF toBefore = unit(before - corner), toAfter = unit(after - corner);
        // the Lamé curve in the parallelogram of the two sides, from the side before to the side after
        const int steps = std::clamp(int(std::max(back, on) / 2), 24, 256);
        offsets.resize(steps + 1);
        for (int i = 0; i <= steps; i++) {
            const qreal t = qreal(i) / steps * M_PI / 2;
            const qreal c = std::pow(std::max(0.0, std::cos(t)), power), s = std::pow(std::max(0.0, std::sin(t)), power);
            offsets[i] = back * (1 - s) * toBefore + on * (1 - c) * toAfter;
        }
        // the share of its reach that stays in the wedge
        qreal share = 1;
        for (const int side : {(k + n - 1) % n, k}) {
            const QPointF across = sideMiddles[side] - center;
            const qreal room = cross(across, corner - center);
            if (qAbs(room) < 1e-12) continue;
            const qreal sign = room > 0 ? 1 : -1;
            for (const QPointF &offset : offsets) {
                const qreal toward = sign * cross(across, offset);
                if (toward < 0) share = std::min(share, qAbs(room) / -toward);
            }
        }
        for (const QPointF &offset : offsets) {
            const QPointF p = corner + share * offset;
            if (first) {
                path.moveTo(p);
                first = false;
            } else {
                path.lineTo(p);
            }
        }
    }
    path.closeSubpath();
    return path;
}

/// The scaling and moving that takes \p from onto \p to
QTransform fitting(const QRectF &from, const QRectF &to)
{
    QTransform fit;
    fit.translate(to.left(), to.top());
    fit.scale(to.width() / from.width(), to.height() / from.height());
    fit.translate(-from.left(), -from.top());
    return fit;
}

} // namespace

QPainterPath pathOf(const QRectF &box, const Look &wanted)
{
    const QRectF r = box.normalized();
    if (r.isEmpty()) return QPainterPath();
    const Look look = bounded(wanted);
    // stretched, made in a square and fitted to the box at the end, corners and all
    const QRectF target = look.stretched ? QRectF(0, 0, 1024, 1024) : r;

    // rounding pulls the shape in from its polygon's box: the polygon is stretched until the rounded shape fills
    // the box, so the corners stay round (stretching the rounded shape would make them oval)
    QRectF frame = target;
    QPainterPath path = roundedIn(frame, look);
    for (int i = 0; i < 4; i++) {
        const QRectF drawn = path.boundingRect();
        if (drawn.width() < 1e-6 || drawn.height() < 1e-6) break;
        const qreal off = std::max({qAbs(drawn.left() - target.left()), qAbs(drawn.top() - target.top()),
                                    qAbs(drawn.right() - target.right()), qAbs(drawn.bottom() - target.bottom())});
        if (off < 1e-3 * std::max(target.width(), target.height())) break;
        frame = fitting(drawn, target).mapRect(frame);
        path = roundedIn(frame, look);
    }

    // the last hair of it, and a stretched one is in its square: it fills the box
    const QRectF drawn = path.boundingRect();
    if (drawn.width() > 1e-6 && drawn.height() > 1e-6) path = fitting(drawn, r).map(path);
    return path;
}

QVector<Preset> presets()
{
    return {
        {QStringLiteral("rectangle"), i18nc("a shape", "Rectangle"), {4, 0, 2, 0, false}},
        {QStringLiteral("rounded"), i18nc("a shape: a rectangle with round corners", "Rounded"), {4, 0.5, 2, 0, false}},
        {QStringLiteral("squircle"), i18nc("a shape between a square and a circle", "Squircle"), {4, 1, 4.5, 0, false}},
        {QStringLiteral("ellipse"), i18nc("a shape", "Ellipse"), {4, 1, 2, 0, true}},
        {QStringLiteral("diamond"), i18nc("a shape", "Diamond"), {4, 1, 1, 0, true}},
        {QStringLiteral("star"), i18nc("a shape: four points curving inward", "Star"), {4, 1, 0.5, 0, true}},
        {QStringLiteral("triangle"), i18nc("a shape", "Triangle"), {3, 0, 2, 0, false}},
        {QStringLiteral("soft-triangle"), i18nc("a shape: a triangle with round corners", "Rounded triangle"), {3, 0.5, 2, 0, false}},
        {QStringLiteral("pentagon"), i18nc("a shape", "Pentagon"), {5, 0, 2, 0, false}},
        {QStringLiteral("hexagon"), i18nc("a shape", "Hexagon"), {6, 0, 2, 0, false}},
        {QStringLiteral("octagon"), i18nc("a shape", "Octagon"), {8, 0, 2, 0, false}},
        {QStringLiteral("five-star"), i18nc("a shape: five points curving inward", "Five-point star"), {5, 1, 0.5, 36, true}},
    };
}

QString keyOf(const Look &wanted)
{
    const Look look = bounded(wanted);
    Q_FOREACH (const Preset &preset, presets()) {
        if (preset.look == look) return keyPrefix + preset.id;
    }
    return keyPrefix
        + QStringLiteral("%1,%2,%3,%4").arg(look.sides).arg(look.roundness, 0, 'g', 4).arg(look.curve, 0, 'g', 4).arg(look.angle, 0, 'g', 5)
        + (look.stretched ? QStringLiteral(",s") : QString());
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
    QStringList parts = rest.split(QLatin1Char(','));
    // stretched when it ends in "s"
    const bool stretched = parts.size() == 5 && parts.last() == QLatin1String("s");
    if (stretched) parts.removeLast();
    if (parts.size() != 4) return false;
    bool ok[4];
    Look read;
    read.sides = parts[0].toInt(&ok[0]);
    read.roundness = parts[1].toDouble(&ok[1]);
    read.curve = parts[2].toDouble(&ok[2]);
    read.angle = parts[3].toDouble(&ok[3]);
    read.stretched = stretched;
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

QStringList settingIds()
{
    QStringList ids;
    for (const char *key : {"skissaSides", "skissaRoundness", "skissaCurve", "skissaAngle", "skissaStretched"}) {
        ids << QStringLiteral("[%1] %2").arg(toolId, QLatin1String(key));
    }
    return ids;
}

Look current()
{
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    const QStringList ids = settingIds();
    Look look;
    look.sides = settings->value(ids[0]).toInt();
    look.roundness = settings->value(ids[1]).toDouble();
    look.curve = settings->value(ids[2]).toDouble();
    look.angle = settings->value(ids[3]).toDouble();
    look.stretched = settings->value(ids[4]).toBool();
    return bounded(look);
}

void use(const Look &wanted)
{
    // through the settings, so Ctrl+Z takes a look back (and with it the reshaping of what was just drawn): one part
    // changed alone is written alone, so a drag of it is one step
    const Look look = bounded(wanted);
    const Look now = current();
    if (look == now) return;
    const QStringList ids = settingIds();
    const QVariant values[] = {look.sides, look.roundness, look.curve, look.angle, look.stretched};
    const QVariant before[] = {now.sides, now.roundness, now.curve, now.angle, now.stretched};
    QVector<QPair<QString, QVariant>> changed;
    for (int i = 0; i < ids.size(); i++) {
        if (values[i] != before[i]) changed.append({ids[i], values[i]});
    }
    if (changed.size() == 1) {
        KisSettingsRegistry::instance()->setValue(changed[0].first, changed[0].second);
    } else {
        KisSettingsRegistry::instance()->setValues(changed);
    }
}

void draw(const Look &look)
{
    use(look);
    KisStudioTools::switchTo(toolId);
}

void written()
{
    static bool pending = false;
    if (pending) return;
    pending = true;
    QTimer::singleShot(0, Watch::instance(), []() {
        pending = false;
        Q_EMIT Watch::instance()->changed();
    });
}

Watch *Watch::instance()
{
    static Watch *watch = new Watch();
    return watch;
}

} // namespace KisSuperellipse
