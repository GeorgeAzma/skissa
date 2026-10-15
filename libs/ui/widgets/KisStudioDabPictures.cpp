/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioDabPictures.h"

#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QtMath>

namespace {

constexpr int count = 5;

/// Where dab \p i of a straight row sits, and its share of the way along
QPointF along(const QRectF &rect, qreal radius, int i, int n = count)
{
    const qreal t = n > 1 ? i / qreal(n - 1) : 0.5;
    return QPointF(rect.left() + radius + t * (rect.width() - 2 * radius), rect.center().y());
}

qreal share(int i, int n = count)
{
    return n > 1 ? i / qreal(n - 1) : 0.5;
}

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(qBound(0.0, alpha, 1.0));
    return color;
}

/// A fixed scatter of small numbers in -1..1, the same every time
qreal jitter(int i)
{
    static const qreal values[] = {-0.7, 0.55, -0.15, 0.8, -0.5, 0.3, -0.9, 0.65, -0.3, 0.1, 0.9, -0.6, 0.45, -0.8, 0.2, -0.4};
    return values[i % 16];
}

void dab(QPainter &painter, const QPointF &center, qreal radius, const QColor &color)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(center, radius, radius);
}

} // namespace

QColor KisStudioDabPictures::paintColor(const QColor &color, const QColor &fallback)
{
    auto colorful = [](const QColor &c) {
        const QColor hsv = c.toHsv();
        return c.isValid() && hsv.hsvSaturationF() >= 0.2 && hsv.valueF() >= 0.3;
    };
    if (colorful(color)) return color;
    // a theme without an accent has a gray highlight: a blue then, so hue and saturation still show
    return colorful(fallback) ? fallback : QColor::fromHsvF(0.6, 0.6, 0.85);
}

QVector<QPointF> KisStudioDabPictures::strokeDabs(const QRectF &rect, int n, qreal radius)
{
    QVector<QPointF> dabs;
    for (int i = 0; i < n; i++) {
        const qreal t = share(i, n);
        // a gentle wave, as a hand draws
        const qreal lift = std::sin(t * 2 * M_PI) * (rect.height() / 2 - radius) * 0.5;
        dabs << QPointF(rect.left() + radius + t * (rect.width() - 2 * radius), rect.center().y() - lift);
    }
    return dabs;
}

bool KisStudioDabPictures::effect(QPainter &painter, const QRectF &rect, const QString &id, const QColor &ink, const QColor &paint)
{
    if (rect.width() < 4 || rect.height() < 4) return false;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal r = qMin(rect.height() * 0.32, rect.width() / (2.0 * count) * 1.25);
    const QColor shape = withAlpha(ink, 0.85);
    bool known = true;

    if (id == QLatin1String("Size")) {
        for (int i = 0; i < count; i++) dab(painter, along(rect, r, i), r * (0.35 + 0.65 * share(i)), shape);
    } else if (id == QLatin1String("Opacity") || id == QLatin1String("Flow")) {
        for (int i = 0; i < count; i++) dab(painter, along(rect, r, i), r, withAlpha(ink, 0.12 + 0.78 * share(i)));
    } else if (id == QLatin1String("Ratio")) {
        for (int i = 0; i < count; i++) {
            const QPointF c = along(rect, r, i);
            painter.setPen(Qt::NoPen);
            painter.setBrush(shape);
            painter.drawEllipse(c, r * 0.8, r * (1.0 - 0.75 * share(i)));
        }
    } else if (id == QLatin1String("Rotation")) {
        for (int i = 0; i < count; i++) {
            painter.save();
            painter.translate(along(rect, r, i));
            painter.rotate(share(i) * 135);
            painter.setPen(Qt::NoPen);
            painter.setBrush(shape);
            painter.drawEllipse(QPointF(), r * 0.95, r * 0.35);
            painter.restore();
        }
    } else if (id == QLatin1String("Mirror")) {
        // a lopsided dab, turned over every other time
        for (int i = 0; i < count; i++) {
            painter.save();
            painter.translate(along(rect, r, i));
            if (i % 2) painter.scale(-1, 1);
            QPainterPath path;
            path.moveTo(-r * 0.3, -r * 0.8);
            path.cubicTo(r * 0.9, -r * 0.8, r * 0.9, r * 0.8, -r * 0.3, r * 0.8);
            path.closeSubpath();
            painter.fillPath(path, shape);
            painter.restore();
        }
    } else if (id == QLatin1String("Softness")) {
        for (int i = 0; i < count; i++) {
            const QPointF c = along(rect, r, i);
            QRadialGradient soft(c, r);
            soft.setColorAt(0, shape);
            soft.setColorAt(qMax(0.05, 0.95 - 0.9 * share(i)), shape);
            soft.setColorAt(1, withAlpha(ink, 0));
            painter.setPen(Qt::NoPen);
            painter.setBrush(soft);
            painter.drawEllipse(c, r, r);
        }
    } else if (id == QLatin1String("Sharpness")) {
        // soft edges becoming whole pixels
        for (int i = 0; i < count; i++) {
            const QPointF c = along(rect, r, i);
            const qreal t = share(i);
            const qreal cell = qMax(1.5, r / 3.5);
            painter.setPen(Qt::NoPen);
            for (qreal y = -r; y < r; y += cell) {
                for (qreal x = -r; x < r; x += cell) {
                    const qreal d = std::hypot(x + cell / 2, y + cell / 2) / r;
                    const qreal soft = qBound(0.0, 1.0 - d, 1.0) * 1.6;
                    const qreal alpha = t * (d < 1.0 ? 1.0 : 0.0) + (1 - t) * qMin(1.0, soft);
                    if (alpha <= 0.02) continue;
                    painter.fillRect(QRectF(c.x() + x, c.y() + y, cell, cell), withAlpha(ink, 0.85 * alpha));
                }
            }
        }
    } else if (id == QLatin1String("Scatter")) {
        const int n = 8;
        const qreal small = r * 0.6;
        for (int i = 0; i < n; i++) {
            const QPointF c = along(rect, small, i, n);
            const qreal spread = share(i, n) * (rect.height() / 2 - small);
            dab(painter, c + QPointF(jitter(i + 3) * small * 0.4, jitter(i) * spread), small, shape);
        }
    } else if (id == QLatin1String("Spacing")) {
        qreal x = rect.left() + r * 0.6;
        qreal step = r * 0.35;
        while (x < rect.right() - r * 0.6) {
            dab(painter, QPointF(x, rect.center().y()), r * 0.6, withAlpha(ink, 0.6));
            x += step;
            step *= 1.6;
        }
    } else if (id == QLatin1String("Rate")) {
        // spray piling up faster
        int k = 0;
        for (int i = 0; i < count; i++) {
            const QPointF c = along(rect, r, i);
            const int dots = 2 + i * 4;
            for (int j = 0; j < dots; j++, k++) {
                const QPointF at = c + QPointF(jitter(k) * r * 0.9, jitter(k * 7 + 3) * r * 0.9);
                dab(painter, at, 1.1, shape);
            }
        }
    } else if (id == QLatin1String("Darken")) {
        for (int i = 0; i < count; i++) dab(painter, along(rect, r, i), r, paint.darker(100 + int(250 * share(i))));
    } else if (id == QLatin1String("Mix")) {
        // the paint taking in the color under it
        const QColor under = QColor::fromHsvF(fmod(qMax(0.0, paint.toHsv().hsvHueF()) + 0.5, 1.0), 0.45, 0.75);
        for (int i = 0; i < count; i++) {
            const qreal t = share(i) * 0.8;
            dab(painter, along(rect, r, i),
                r, QColor::fromRgbF(paint.redF() * (1 - t) + under.redF() * t, paint.greenF() * (1 - t) + under.greenF() * t, paint.blueF() * (1 - t) + under.blueF() * t));
        }
    } else if (id == QLatin1String("h")) {
        const QColor hsv = paint.toHsv();
        for (int i = 0; i < count; i++) {
            dab(painter, along(rect, r, i), r,
                QColor::fromHsvF(fmod(qMax(0.0, hsv.hsvHueF()) + share(i) * 0.75, 1.0), hsv.hsvSaturationF(), hsv.valueF()));
        }
    } else if (id == QLatin1String("s")) {
        const QColor hsv = paint.toHsv();
        for (int i = 0; i < count; i++) {
            dab(painter, along(rect, r, i), r, QColor::fromHsvF(qMax(0.0, hsv.hsvHueF()), hsv.hsvSaturationF() * (1 - share(i)), hsv.valueF()));
        }
    } else if (id == QLatin1String("v") || id == QLatin1String("LightnessStrength")) {
        const QColor hsv = paint.toHsv();
        for (int i = 0; i < count; i++) {
            dab(painter, along(rect, r, i), r, QColor::fromHsvF(qMax(0.0, hsv.hsvHueF()), hsv.hsvSaturationF(), 0.25 + 0.75 * share(i)));
        }
    } else {
        known = false;
    }
    painter.restore();
    return known;
}
