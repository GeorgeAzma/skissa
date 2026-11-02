/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaColorWidget.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <functional>

#include <KisGlobalResourcesInterface.h>
#include <KisModernStyle.h>
#include <KoColor.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kis_icon_utils.h>
#include <kis_paint_device.h>
#include <klocalizedstring.h>
#include <widgets/KisStudioChips.h>

#include "KoCompositeOpOklch.h"
#include "SkissaColor.h"

using namespace SkissaColor;

namespace
{

/// The tab of the red, green and blue curves, after lightness, chroma and hue
constexpr int rgbTab = 3;
constexpr int bins = 64;
/// The drag on the image across which a curve goes from bottom to top
constexpr qreal dragForWholeCurve = 300;

/// What the panel showed last, kept for the next time it opens
int lastChange = Lightness;
int lastBy[4] = {Lightness, Chroma, Hue, 0};

QString axisName(int axis)
{
    switch (axis) {
    case Lightness: return i18nc("color effect axis", "Lightness");
    case Chroma: return i18nc("color effect axis", "Chroma");
    default: return i18nc("color effect axis", "Hue");
    }
}

QString channelName(int channel)
{
    switch (channel) {
    case 0: return i18nc("color channel", "Red");
    case 1: return i18nc("color channel", "Green");
    default: return i18nc("color channel", "Blue");
    }
}

float linearOf(qreal c)
{
    return float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
}

QColor colorOfLab(const float lab[3])
{
    float rgb[3];
    KoOklch::inGamut(lab, rgb);
    auto encode = [](float c) {
        c = std::clamp(c, 0.0f, 1.0f);
        return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f;
    };
    return QColor::fromRgbF(encode(rgb[0]), encode(rgb[1]), encode(rgb[2]));
}

/// A pixel's place on each axis: lightness, chroma (of the axis' range) and hue (on the axis), and its sRGB values
struct Place {
    float axis[3] {0, 0, 0};
    float rgb[3] {0, 0, 0};
};

Place placeOf(const QColor &color)
{
    Place place;
    place.rgb[0] = color.redF();
    place.rgb[1] = color.greenF();
    place.rgb[2] = color.blueF();
    const float linear[3] = {linearOf(color.redF()), linearOf(color.greenF()), linearOf(color.blueF())};
    float lab[3];
    KoMixing::linearToOklab(linear, lab);
    place.axis[Lightness] = lab[0];
    place.axis[Chroma] = std::hypot(lab[1], lab[2]) / chromaRange;
    place.axis[Hue] = axisOfHue(std::atan2(lab[2], lab[1]) * float(180 / M_PI));
    return place;
}

/// \p image (8-bit, as Krita keeps it) with the settings applied
QImage applied(const QImage &image, const Settings &settings)
{
    QImage result = image.convertToFormat(QImage::Format_ARGB32);
    const Recipe recipe(settings);
    for (int y = 0; y < result.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(result.scanLine(y));
        for (int x = 0; x < result.width(); x++) {
            const QRgb pixel = line[x];
            float rgb[3] = {linearOf(qRed(pixel) / 255.0), linearOf(qGreen(pixel) / 255.0), linearOf(qBlue(pixel) / 255.0)};
            recipe.apply(rgb, true);
            auto out = [](float c) {
                c = std::clamp(c, 0.0f, 1.0f);
                return qRound((c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1 / 2.4f) - 0.055f) * 255);
            };
            line[x] = qRgba(out(rgb[0]), out(rgb[1]), out(rgb[2]), qAlpha(pixel));
        }
    }
    return result;
}

/**
 * One curve, drawn over how much of the image is at each place along it. A press on the line adds a point and
 * drags it; a point pulled far off the box, double-clicked or right-clicked goes; a right click elsewhere resets.
 */
class CurveView : public QWidget
{
    Q_OBJECT
public:
    std::function<void(const Curve &)> changed;
    /// The values at a place, as shown while a point is dragged
    std::function<QString(qreal x, qreal y)> describe;
    std::function<QColor(qreal)> xColor;
    std::function<QColor(qreal)> yColor;

    explicit CurveView(QWidget *parent)
        : QWidget(parent)
    {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }

    void setCurve(const Curve &curve, const Curve &fallback, bool wraps, bool mapping)
    {
        m_curve = curve;
        m_fallback = fallback;
        m_wraps = wraps;
        m_mapping = mapping;
        m_values = sampled(curve, wraps, mapping, 256);
        update();
    }

    void setHistogram(const QVector<float> &histogram)
    {
        m_histogram = histogram;
        update();
    }

    /// Marks the place of the pixel under the pointer; negative: none
    void setMarker(qreal x)
    {
        if (x == m_marker) return;
        m_marker = x;
        update();
    }

    qreal valueAt(qreal x) const
    {
        return m_values.isEmpty() ? x : m_values[qBound(0, qRound(x * (m_values.size() - 1)), m_values.size() - 1)];
    }

    /// Gives the point nearest \p x (within a little), or a new one on the curve there; its index
    int pointAt(qreal x)
    {
        int nearest = -1;
        for (int i = 0; i < m_curve.points.size(); i++) {
            const qreal distance = std::abs(m_curve.points[i].x() - x);
            if (distance < 0.025 && (nearest < 0 || distance < std::abs(m_curve.points[nearest].x() - x))) nearest = i;
        }
        if (nearest >= 0) return nearest;
        if (m_wraps) {
            // a hue changed alone: the curve held where it is a little either way, unless a point is near already
            const qreal reach = 1.0 / 12;
            for (qreal side : {-reach, reach}) {
                const qreal at = x + side;
                bool held = false;
                Q_FOREACH (const QPointF &point, m_curve.points) {
                    if (std::abs(point.x() - at) < reach * 0.8 || (side < 0 ? point.x() > at && point.x() < x : point.x() < at && point.x() > x)) {
                        held = true;
                    }
                }
                if (!held && at > 0.01 && at < 0.99) insert(QPointF(at, qBound(0.0, valueAt(at), 1.0)));
            }
        }
        return insert(QPointF(x, qBound(0.0, valueAt(x), 1.0)));
    }

    /// Moves point \p index to height \p y and reports the change
    void setPointY(int index, qreal y)
    {
        if (index < 0 || index >= m_curve.points.size()) return;
        m_curve.points[index].setY(qBound(0.0, y, 1.0));
        tieEnds(index);
        commit();
    }

    const Curve &curve() const { return m_curve; }
    void setShownPoint(int index) { m_shown = index; update(); }

    QSize sizeHint() const override { return QSize(360, 170); }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return qBound(130, int(width * 0.4), 170); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor ink = palette().color(QPalette::WindowText);
        auto inked = [&ink](qreal alpha) {
            QColor color = ink;
            color.setAlphaF(alpha);
            return color;
        };
        const QRectF box = boxRect();

        painter.setPen(Qt::NoPen);
        painter.setBrush(inked(0.05));
        painter.drawPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius));

        painter.save();
        painter.setClipPath(KisModernStyle::squircle(box, KisModernStyle::cornerRadius));
        if (!m_histogram.isEmpty()) {
            QPainterPath shape;
            shape.moveTo(box.bottomLeft());
            for (int i = 0; i < m_histogram.size(); i++) {
                const qreal x = box.left() + box.width() * (i + 0.5) / m_histogram.size();
                shape.lineTo(x, box.bottom() - m_histogram[i] * box.height() * 0.85);
            }
            shape.lineTo(box.bottomRight());
            shape.closeSubpath();
            painter.setBrush(inked(0.09));
            painter.drawPath(shape);
        }
        painter.setPen(QPen(inked(0.07), 1));
        for (int i = 1; i < 4; i++) {
            const qreal x = box.left() + box.width() * i / 4;
            const qreal y = box.top() + box.height() * i / 4;
            painter.drawLine(QPointF(x, box.top()), QPointF(x, box.bottom()));
            painter.drawLine(QPointF(box.left(), y), QPointF(box.right(), y));
        }
        const QVector<float> fallback = sampled(m_fallback, m_wraps, m_mapping, 2);
        painter.setPen(QPen(inked(0.2), 1));
        painter.drawLine(toScreen(QPointF(0, fallback.first())), toScreen(QPointF(1, fallback.last())));

        QPainterPath line;
        for (int i = 0; i < m_values.size(); i++) {
            const QPointF at = toScreen(QPointF(qreal(i) / (m_values.size() - 1), m_values[i]));
            if (i == 0) line.moveTo(at);
            else line.lineTo(at);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(inked(0.9), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(line);

        if (m_marker >= 0) {
            const QPointF on = toScreen(QPointF(m_marker, valueAt(m_marker)));
            painter.setPen(QPen(inked(0.4), 1));
            painter.drawLine(QPointF(on.x(), box.top()), QPointF(on.x(), box.bottom()));
            painter.setPen(Qt::NoPen);
            painter.setBrush(inked(0.9));
            painter.drawEllipse(on, 3, 3);
        }
        painter.restore();

        const int active = m_dragged >= 0 ? m_dragged : m_shown;
        for (int i = 0; i < m_curve.points.size(); i++) {
            const QPointF at = toScreen(m_curve.points[i]);
            const qreal radius = i == active || i == m_hovered ? 5.5 : 4.5;
            painter.setOpacity(i == m_dragged && m_leaving ? 0.35 : 1.0);
            // white with a dark ring, seen on any background
            painter.setPen(QPen(QColor(0, 0, 0, 170), 1.5));
            painter.setBrush(Qt::white);
            painter.drawEllipse(at, radius, radius);
        }
        painter.setOpacity(1);

        if (active >= 0 && active < m_curve.points.size() && describe) {
            const QPointF point = m_curve.points[active];
            painter.setPen(KisModernStyle::secondaryInk(ink));
            painter.drawText(box.adjusted(KisModernStyle::space2, KisModernStyle::space1, -KisModernStyle::space2, 0),
                             Qt::AlignLeft | Qt::AlignTop, describe(point.x(), point.y()));
        }

        // what each axis is, as a strip of its colors
        auto strip = [&painter](const QRectF &rect, bool across, const std::function<QColor(qreal)> &color) {
            if (!color) return;
            QLinearGradient gradient(rect.topLeft(), across ? rect.topRight() : rect.bottomLeft());
            for (int i = 0; i <= 24; i++) {
                const qreal t = i / 24.0;
                gradient.setColorAt(t, color(across ? t : 1 - t));
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(gradient);
            painter.drawPath(KisModernStyle::squircle(rect, rect.height() < rect.width() ? rect.height() / 2 : rect.width() / 2));
        };
        strip(QRectF(box.left(), box.bottom() + KisModernStyle::space1, box.width(), stripSize), true, xColor);
        strip(QRectF(0, box.top(), stripSize, box.height()), false, yColor);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        const int near = pointNear(event->pos());
        if (event->button() == Qt::RightButton) {
            // a point goes; elsewhere, the curve resets
            if (near >= 0 && removable(near)) {
                m_curve.points.removeAt(near);
            } else {
                m_curve = m_fallback;
            }
            m_hovered = -1;
            commit();
            return;
        }
        if (event->button() != Qt::LeftButton) return;
        m_dragged = near >= 0 ? near : insert(QPointF(qBound(0.0, fromScreen(event->pos()).x(), 1.0),
                                                       qBound(0.0, valueAt(fromScreen(event->pos()).x()), 1.0)));
        m_grab = event->pos();
        m_start = m_curve.points[m_dragged];
        m_fine = event->modifiers() & Qt::ShiftModifier;
        m_leaving = false;
        commit();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragged < 0 || !(event->buttons() & Qt::LeftButton)) {
            if (m_dragged >= 0) finishDrag();
            const int near = pointNear(event->pos());
            if (near != m_hovered) {
                m_hovered = near;
                update();
            }
            return;
        }
        const bool fine = event->modifiers() & Qt::ShiftModifier;
        if (fine != m_fine) {
            // on from here at the new speed
            m_fine = fine;
            m_grab = event->pos();
            m_start = m_curve.points[m_dragged];
        }
        const QRectF box = boxRect();
        const QPointF moved = QPointF(event->pos() - m_grab) * (fine ? 0.25 : 1.0);
        QPointF point(m_start.x() + moved.x() / box.width(), m_start.y() - moved.y() / box.height());
        const int last = m_curve.points.size() - 1;
        const bool end = m_dragged == 0 || m_dragged == last;
        if (m_wraps && end) {
            point.setX(m_dragged == 0 ? 0.0 : 1.0);
        } else {
            const qreal low = m_dragged > 0 ? m_curve.points[m_dragged - 1].x() + 0.01 : 0.0;
            const qreal high = m_dragged < last ? m_curve.points[m_dragged + 1].x() - 0.01 : 1.0;
            point.setX(qBound(low, point.x(), qMax(low, high)));
        }
        point.setY(qBound(0.0, point.y(), 1.0));
        m_curve.points[m_dragged] = point;
        tieEnds(m_dragged);
        // pulled far off the box, the point will go
        const qreal off = qMax(box.top() - event->pos().y(), event->pos().y() - box.bottom());
        m_leaving = removable(m_dragged) && off > 32;
        commit();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dragged >= 0) finishDrag();
    }

    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        const int near = pointNear(event->pos());
        if (event->button() == Qt::LeftButton && near >= 0 && removable(near)) {
            m_curve.points.removeAt(near);
            m_dragged = -1;
            m_hovered = -1;
            commit();
        }
    }

    void leaveEvent(QEvent *) override
    {
        m_hovered = -1;
        update();
    }

private:
    static constexpr int stripSize = 6;

    QRectF boxRect() const
    {
        const qreal left = stripSize + KisModernStyle::space1;
        return QRectF(left, 0, width() - left, height() - stripSize - KisModernStyle::space1);
    }
    QPointF toScreen(const QPointF &value) const
    {
        const QRectF box = boxRect();
        return QPointF(box.left() + value.x() * box.width(), box.bottom() - value.y() * box.height());
    }
    QPointF fromScreen(const QPointF &pos) const
    {
        const QRectF box = boxRect();
        return QPointF((pos.x() - box.left()) / box.width(), (box.bottom() - pos.y()) / box.height());
    }
    int pointNear(const QPoint &pos) const
    {
        int best = -1;
        qreal bestDistance = 10;
        for (int i = 0; i < m_curve.points.size(); i++) {
            const qreal distance = QLineF(toScreen(m_curve.points[i]), pos).length();
            if (distance < bestDistance) {
                bestDistance = distance;
                best = i;
            }
        }
        return best;
    }
    bool removable(int index) const
    {
        // a curve keeps two points; a curve by hue keeps its ends, which are one point
        if (m_curve.points.size() <= 2) return false;
        return !(m_wraps && (index == 0 || index == m_curve.points.size() - 1));
    }
    int insert(const QPointF &point)
    {
        int at = 0;
        while (at < m_curve.points.size() && m_curve.points[at].x() < point.x()) at++;
        m_curve.points.insert(at, point);
        return at;
    }
    /// The two ends of a curve by hue are the same place
    void tieEnds(int index)
    {
        if (!m_wraps || m_curve.points.size() < 2) return;
        const int last = m_curve.points.size() - 1;
        if (index == 0 && m_curve.points[last].x() >= 0.999) m_curve.points[last].setY(m_curve.points[0].y());
        if (index == last && m_curve.points[0].x() <= 0.001) m_curve.points[0].setY(m_curve.points[last].y());
    }
    void finishDrag()
    {
        if (m_leaving && m_dragged >= 0) m_curve.points.removeAt(m_dragged);
        m_dragged = -1;
        m_leaving = false;
        commit();
    }
    void commit()
    {
        m_values = sampled(m_curve, m_wraps, m_mapping, 256);
        update();
        if (changed) changed(m_curve);
    }

    Curve m_curve;
    Curve m_fallback;
    bool m_wraps {false};
    bool m_mapping {true};
    QVector<float> m_values;
    QVector<float> m_histogram;
    qreal m_marker {-1};
    int m_hovered {-1};
    int m_dragged {-1};
    int m_shown {-1};
    bool m_leaving {false};
    bool m_fine {false};
    QPoint m_grab;
    QPointF m_start;
};

/**
 * A color shift as a point on a disc of OKLab's colors: dragged toward orange, the image (or its shadows or
 * highlights) warms. A click puts it there, right click takes it back to the middle.
 */
class BalanceDisc : public QWidget
{
    Q_OBJECT
public:
    std::function<void(const QPointF &)> changed;

    BalanceDisc(const QString &name, qreal range, QWidget *parent)
        : QWidget(parent)
        , m_name(name)
        , m_range(range)
    {
        setToolTip(name);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setValue(const QPointF &value)
    {
        m_value = value;
        update();
    }

    QSize sizeHint() const override
    {
        return QSize(side() + KisModernStyle::space2 + fontMetrics().horizontalAdvance(m_name) + KisModernStyle::space1, side());
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRectF disc = discRect();
        const qreal dpr = devicePixelRatioF();
        if (m_picture.isNull() || m_picture.width() != qRound(disc.width() * dpr)) makePicture(qRound(disc.width() * dpr));
        painter.drawImage(disc, m_picture);
        const QColor ink = palette().color(QPalette::WindowText);
        QColor faint = ink;
        faint.setAlphaF(0.25);
        painter.setPen(QPen(faint, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(disc.adjusted(0.5, 0.5, -0.5, -0.5));
        painter.drawLine(QPointF(disc.center().x() - 3, disc.center().y()), QPointF(disc.center().x() + 3, disc.center().y()));
        painter.drawLine(QPointF(disc.center().x(), disc.center().y() - 3), QPointF(disc.center().x(), disc.center().y() + 3));

        painter.setPen(QPen(QColor(0, 0, 0, 170), 1.5));
        painter.setBrush(Qt::white);
        painter.drawEllipse(spotOf(m_value), 5, 5);

        painter.setPen(m_value.isNull() ? KisModernStyle::secondaryInk(ink) : ink);
        painter.drawText(QRectF(disc.right() + KisModernStyle::space2, 0, width() - disc.right() - KisModernStyle::space2, height()),
                         Qt::AlignLeft | Qt::AlignVCenter, m_name);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::RightButton) {
            set(QPointF());
            return;
        }
        if (event->button() != Qt::LeftButton) return;
        m_grab = event->pos();
        m_start = m_value;
        m_moved = false;
        m_fine = event->modifiers() & Qt::ShiftModifier;
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (!(event->buttons() & Qt::LeftButton)) return;
        if (!m_moved && (event->pos() - m_grab).manhattanLength() < 3) return;
        m_moved = true;
        const bool fine = event->modifiers() & Qt::ShiftModifier;
        if (fine != m_fine) {
            m_fine = fine;
            m_grab = event->pos();
            m_start = m_value;
        }
        // the disc is small: a drag across the whole range is longer than it
        const QPointF moved = QPointF(event->pos() - m_grab) * (fine ? 0.25 : 1.0) / dragForWholeRange * m_range;
        set(m_start + QPointF(moved.x(), -moved.y()));
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        // a click without a drag puts the point where it was
        if (event->button() == Qt::LeftButton && !m_moved && discRect().contains(event->pos())) set(valueAt(event->pos()));
    }

private:
    static constexpr qreal dragForWholeRange = 150;

    int side() const { return qMax(40, int(fontMetrics().height() * 2.2)); }
    QRectF discRect() const { return QRectF(0, (height() - side()) / 2.0, side(), side()); }
    QPointF spotOf(const QPointF &value) const
    {
        const QRectF disc = discRect();
        const qreal radius = disc.width() / 2;
        return disc.center() + QPointF(value.x(), -value.y()) / m_range * radius;
    }
    QPointF valueAt(const QPointF &pos) const
    {
        const QRectF disc = discRect();
        const QPointF offset = (pos - disc.center()) / (disc.width() / 2) * m_range;
        return QPointF(offset.x(), -offset.y());
    }
    void set(QPointF value)
    {
        const qreal length = std::hypot(value.x(), value.y());
        if (length > m_range) value *= m_range / length;
        // near the middle is none
        if (length < m_range * 0.03) value = QPointF();
        m_value = value;
        update();
        if (changed) changed(value);
    }
    void makePicture(int size)
    {
        m_picture = QImage(size, size, QImage::Format_ARGB32_Premultiplied);
        m_picture.fill(Qt::transparent);
        const qreal radius = size / 2.0;
        // the colors shown stronger than the shift itself, which is small
        const qreal shown = qMax(m_range, 0.2);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                const qreal dx = (x + 0.5 - radius) / radius;
                const qreal dy = (radius - y - 0.5) / radius;
                const qreal distance = std::hypot(dx, dy);
                if (distance > 1) continue;
                const float lab[3] = {0.74f, float(dx * shown), float(dy * shown)};
                QColor color = colorOfLab(lab);
                // a soft edge
                const qreal alpha = qBound(0.0, (1 - distance) * radius, 1.0);
                m_picture.setPixel(x, y, qPremultiply(qRgba(color.red(), color.green(), color.blue(), qRound(alpha * 255))));
            }
        }
        m_picture.setDevicePixelRatio(1);
    }

    QString m_name;
    qreal m_range;
    QPointF m_value;
    QImage m_picture;
    QPoint m_grab;
    QPointF m_start;
    bool m_moved {false};
    bool m_fine {false};
};

struct Preset {
    QString name;
    Settings settings;
};

Curve curveOf(std::initializer_list<QPointF> points)
{
    return Curve{QVector<QPointF>(points)};
}

} // namespace

struct SkissaColorWidget::Private {
    Settings settings;
    KisPaintDeviceSP original;
    QImage sample;          ///< the image small, for the histograms
    QImage tileBase;        ///< the image as the preset tiles show it
    QVector<float> histograms[3];
    QVector<float> rgbHistograms[3];
    QVector<Preset> presets;
    QVector<QImage> presetPictures;

    KisStudioChips *presetChips {nullptr};
    KisStudioChips *changeTabs {nullptr};
    KisStudioChips *byChips {nullptr};
    CurveView *curve {nullptr};
    BalanceDisc *shadows {nullptr};
    BalanceDisc *balance {nullptr};
    BalanceDisc *highlights {nullptr};
    QToolButton *pickGray {nullptr};

    int change {lastChange};
    int by[4] {lastBy[0], lastBy[1], lastBy[2], lastBy[3]};

    /// The drag on the image in progress: the point it moves and where that started
    int dragPoint {-1};
    qreal dragStart {0};
    QString dragFrom;

    bool isRgb() const { return change == rgbTab; }
    int currentBy() const { return by[change]; }
    Curve &current() { return isRgb() ? settings.rgb[currentBy()] : settings.curves[currentBy()][change]; }
    Curve fallback() const { return isRgb() ? defaultRgbCurve() : defaultCurve(currentBy(), change); }
    bool wraps() const { return !isRgb() && currentBy() == Hue; }
    bool mapping() const { return isRgb() || isMapping(currentBy(), change); }

    /// The text of a value on the x axis (\p onX) or on the y axis of the curve shown
    QString valueText(qreal value, bool onX) const
    {
        if (isRgb()) return QString::number(qRound(value * 255));
        const int axis = onX ? currentBy() : change;
        if (!onX && !mapping()) {
            // a change by another axis: a shift or a gain
            if (change == Chroma) return QStringLiteral("×") + QString::number(2 * value, 'f', 2);
            const qreal shift = (value - 0.5) * 2 * (change == Hue ? hueShift : lightnessShift * 100);
            return (shift >= 0 ? QStringLiteral("+") : QString()) + QString::number(qRound(shift)) + (change == Hue ? QStringLiteral("°") : QString());
        }
        if (axis == Hue) return QString::number(qRound(value * 360)) + QStringLiteral("°");
        return QString::number(qRound(value * 100));
    }

    void makeHistograms()
    {
        for (int i = 0; i < 3; i++) {
            histograms[i] = QVector<float>(bins, 0);
            rgbHistograms[i] = QVector<float>(bins, 0);
        }
        if (sample.isNull()) return;
        const QImage image = sample.convertToFormat(QImage::Format_ARGB32);
        for (int y = 0; y < image.height(); y++) {
            const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
            for (int x = 0; x < image.width(); x++) {
                if (qAlpha(line[x]) < 8) continue;
                const Place place = placeOf(QColor(line[x]));
                for (int i = 0; i < 3; i++) {
                    // hue counts by how colored a pixel is: grays have none
                    const float weight = i == Hue ? std::min(1.0f, place.axis[Chroma] * chromaRange / 0.04f) : 1.0f;
                    histograms[i][qBound(0, int(place.axis[i] * bins), bins - 1)] += weight;
                    rgbHistograms[i][qBound(0, int(place.rgb[i] * bins), bins - 1)] += 1;
                }
            }
        }
        // the square root, so that a few pixels still show beside many
        auto settle = [](QVector<float> &values) {
            float most = 0;
            for (float &value : values) most = std::max(most, value = std::sqrt(value));
            if (most > 0) for (float &value : values) value /= most;
        };
        for (int i = 0; i < 3; i++) {
            settle(histograms[i]);
            settle(rgbHistograms[i]);
        }
    }

    /// The tone curve from the darkest to the lightest of the image (the darkest and lightest half percent left out)
    Curve autoTone() const
    {
        QVector<int> counts(256, 0);
        int total = 0;
        if (!sample.isNull()) {
            const QImage image = sample.convertToFormat(QImage::Format_ARGB32);
            for (int y = 0; y < image.height(); y++) {
                const QRgb *line = reinterpret_cast<const QRgb *>(image.constScanLine(y));
                for (int x = 0; x < image.width(); x++) {
                    if (qAlpha(line[x]) < 8) continue;
                    counts[qBound(0, int(placeOf(QColor(line[x])).axis[Lightness] * 255), 255)]++;
                    total++;
                }
            }
        }
        if (!total) return defaultRgbCurve();
        int low = 0, high = 255, seen = 0;
        while (low < 255 && (seen += counts[low]) < total * 0.005) low++;
        seen = 0;
        while (high > 0 && (seen += counts[high]) < total * 0.005) high--;
        if (high - low < 12) return defaultRgbCurve();
        return curveOf({QPointF(low / 255.0, 0), QPointF(high / 255.0, 1)});
    }

    void makePresets()
    {
        auto with = [](std::function<void(Settings &)> change) {
            Settings settings;
            change(settings);
            return settings;
        };
        const Curve tone = autoTone();
        presets = {
            {i18nc("color effect preset", "Original"), Settings()},
            {i18nc("color effect preset", "Auto"), with([tone](Settings &s) { s.curves[Lightness][Lightness] = tone; })},
            {i18nc("color effect preset", "Brighter"), with([](Settings &s) { s.curves[Lightness][Lightness] = curveOf({{0, 0}, {0.45, 0.58}, {1, 1}}); })},
            {i18nc("color effect preset", "Darker"), with([](Settings &s) { s.curves[Lightness][Lightness] = curveOf({{0, 0}, {0.55, 0.42}, {1, 1}}); })},
            {i18nc("color effect preset", "Punchy"), with([](Settings &s) {
                 s.curves[Lightness][Lightness] = curveOf({{0, 0}, {0.25, 0.19}, {0.75, 0.81}, {1, 1}});
                 s.curves[Chroma][Chroma] = curveOf({{0, 0}, {0.3, 0.36}, {1, 1}});
             })},
            {i18nc("color effect preset", "Fade"), with([](Settings &s) {
                 s.curves[Lightness][Lightness] = curveOf({{0, 0.12}, {1, 0.94}});
                 s.curves[Chroma][Chroma] = curveOf({{0, 0}, {1, 0.8}});
             })},
            {i18nc("color effect preset", "Vivid"), with([](Settings &s) { s.curves[Chroma][Chroma] = curveOf({{0, 0}, {0.25, 0.38}, {1, 1}}); })},
            {i18nc("color effect preset", "Muted"), with([](Settings &s) { s.curves[Chroma][Chroma] = curveOf({{0, 0}, {1, 0.55}}); })},
            {i18nc("color effect preset", "Warm"), with([](Settings &s) { s.balance = QPointF(0.012, 0.03); })},
            {i18nc("color effect preset", "Cool"), with([](Settings &s) { s.balance = QPointF(-0.012, -0.03); })},
            {i18nc("color effect preset", "Split"), with([](Settings &s) {
                 s.shadows = QPointF(-0.035, -0.035);
                 s.highlights = QPointF(0.02, 0.04);
             })},
            {i18nc("color effect preset", "Mono"), with([](Settings &s) { s.curves[Chroma][Chroma] = curveOf({{0, 0}, {1, 0}}); })},
        };
        presetPictures.clear();
        Q_FOREACH (const Preset &preset, presets) presetPictures << applied(tileBase, preset.settings);
    }
};

SkissaColorWidget::SkissaColorWidget(QWidget *parent, KisPaintDeviceSP dev)
    : KisConfigWidget(parent)
    , d(new Private)
{
    // the pixels as they were before the preview changes them; their tiles are shared until then
    if (dev) {
        d->original = new KisPaintDevice(*dev);
        d->sample = d->original->createThumbnail(200, 200, Qt::KeepAspectRatio);
        const QSize tile(160, 96);
        const QImage whole = d->original->createThumbnail(tile.width() * 2, tile.height() * 2, Qt::KeepAspectRatio);
        if (!whole.isNull()) {
            const QImage covering = whole.scaled(tile, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
            d->tileBase = covering.copy((covering.width() - tile.width()) / 2, (covering.height() - tile.height()) / 2, tile.width(), tile.height());
        }
    }
    if (d->tileBase.isNull()) {
        // a blank layer: a ramp of colors and grays shows what each starting point does
        d->tileBase = QImage(160, 96, QImage::Format_ARGB32);
        for (int y = 0; y < 96; y++) {
            for (int x = 0; x < 160; x++) {
                d->tileBase.setPixelColor(x, y, y < 64 ? QColor::fromHsvF(x / 160.0, 0.6, 0.35 + 0.6 * y / 64.0) : QColor::fromHsvF(0, 0, x / 160.0));
            }
        }
    }
    d->makeHistograms();
    d->makePresets();

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);

    d->presetChips = new KisStudioChips(this);
    d->presetChips->setTileColumns(6, qMax(48, fontMetrics().height() + 30));
    connect(d->presetChips, &KisStudioChips::clicked, this, [this](int index) {
        if (index < 0 || index >= d->presets.size()) return;
        d->settings = d->presets[index].settings;
        refresh();
        Q_EMIT sigConfigurationItemChanged();
    });
    layout->addWidget(d->presetChips);

    d->changeTabs = new KisStudioChips(this);
    d->changeTabs->setTabs(true);
    connect(d->changeTabs, &KisStudioChips::clicked, this, [this](int index) {
        d->change = lastChange = index;
        refresh();
    });
    layout->addWidget(d->changeTabs);

    d->curve = new CurveView(this);
    d->curve->changed = [this](const Curve &curve) {
        d->current() = curve;
        refreshChips();
        Q_EMIT sigConfigurationItemChanged();
    };
    d->curve->describe = [this](qreal x, qreal y) { return d->valueText(x, true) + QStringLiteral("  →  ") + d->valueText(y, false); };

    // the curve with what it goes by under it, and the discs in a column beside it
    auto *middle = new QHBoxLayout();
    middle->setSpacing(KisModernStyle::space3);
    auto *curveColumn = new QVBoxLayout();
    curveColumn->setSpacing(KisModernStyle::space1 + KisModernStyle::hairline);
    curveColumn->addWidget(d->curve);
    auto *byRow = new QHBoxLayout();
    byRow->setSpacing(KisModernStyle::space2);
    auto *byLabel = new QLabel(i18nc("color effect: the curve goes by this", "By"), this);
    byRow->addWidget(byLabel);
    d->byChips = new KisStudioChips(this);
    connect(d->byChips, &KisStudioChips::clicked, this, [this](int index) {
        d->by[d->change] = lastBy[d->change] = index;
        refresh();
    });
    byRow->addWidget(d->byChips, 1);
    curveColumn->addLayout(byRow);
    middle->addLayout(curveColumn, 1);

    auto *discs = new QGridLayout();
    discs->setSpacing(KisModernStyle::space1);
    discs->setColumnStretch(0, 1);
    middle->addLayout(discs);
    layout->addLayout(middle);
    d->shadows = new BalanceDisc(i18nc("color effect tint", "Shadows"), 0.08, this);
    d->balance = new BalanceDisc(i18nc("color effect white balance", "Balance"), 0.1, this);
    d->highlights = new BalanceDisc(i18nc("color effect tint", "Highlights"), 0.08, this);
    d->shadows->changed = [this](const QPointF &value) {
        d->settings.shadows = value;
        Q_EMIT sigConfigurationItemChanged();
    };
    d->balance->changed = [this](const QPointF &value) {
        d->settings.balance = value;
        Q_EMIT sigConfigurationItemChanged();
    };
    d->highlights->changed = [this](const QPointF &value) {
        d->settings.highlights = value;
        Q_EMIT sigConfigurationItemChanged();
    };
    discs->addWidget(d->highlights, 0, 0);
    discs->addWidget(d->balance, 1, 0);
    discs->addWidget(d->shadows, 2, 0);
    discs->setRowStretch(3, 1);
    d->pickGray = new QToolButton(this);
    d->pickGray->setAutoRaise(true);
    d->pickGray->setCheckable(true);
    d->pickGray->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_sampler")));
    d->pickGray->setToolTip(i18nc("@info:tooltip", "Pick something gray"));
    discs->addWidget(d->pickGray, 1, 1, Qt::AlignVCenter);

    refresh();
}

SkissaColorWidget::~SkissaColorWidget()
{
}

void SkissaColorWidget::setConfiguration(const KisPropertiesConfigurationSP config)
{
    d->settings = config ? Settings::from(*config) : Settings();
    refresh();
}

KisPropertiesConfigurationSP SkissaColorWidget::configuration() const
{
    KisFilterSP filter = KisFilterRegistry::instance()->value(QStringLiteral("skissa_color"));
    KisFilterConfigurationSP config = filter->factoryConfiguration(KisGlobalResourcesInterface::instance());
    d->settings.writeTo(*config);
    return config;
}

QSize SkissaColorWidget::sizeHint() const
{
    const int width = 500;
    return QSize(width, layout()->heightForWidth(width));
}

void SkissaColorWidget::refresh()
{
    d->curve->setCurve(d->current(), d->fallback(), d->wraps(), d->mapping());
    d->curve->setHistogram(d->isRgb() ? d->rgbHistograms[d->currentBy()] : d->histograms[d->currentBy()]);

    // each axis as a strip of what it is
    auto axisColor = [](int axis) -> std::function<QColor(qreal)> {
        switch (axis) {
        case Lightness:
            return [](qreal t) {
                const float lab[3] = {float(t), 0, 0};
                return colorOfLab(lab);
            };
        case Chroma:
            return [](qreal t) {
                const float lab[3] = {0.7f, float(t * chromaRange * 0.5), float(t * chromaRange * 0.5)};
                return colorOfLab(lab);
            };
        default:
            return [](qreal t) { return QColor::fromHsvF(std::fmod(t, 1.0), 0.75, 0.95); };
        }
    };
    if (d->isRgb()) {
        const int channel = d->currentBy();
        auto ramp = [channel](qreal t) {
            QColor color(0, 0, 0);
            if (channel == 0) color.setRedF(t);
            else if (channel == 1) color.setGreenF(t);
            else color.setBlueF(t);
            return color;
        };
        d->curve->xColor = ramp;
        d->curve->yColor = ramp;
    } else {
        d->curve->xColor = axisColor(d->currentBy());
        if (d->mapping()) {
            d->curve->yColor = axisColor(d->change);
        } else if (d->change == Hue) {
            // a turn either way from the color as it is
            d->curve->yColor = [](qreal t) { return QColor::fromHsvF(std::fmod(0.9 + (t - 0.5) / 3 + 1.0, 1.0), 0.75, 0.95); };
        } else {
            d->curve->yColor = axisColor(d->change);
        }
    }
    d->curve->update();
    d->shadows->setValue(d->settings.shadows);
    d->balance->setValue(d->settings.balance);
    d->highlights->setValue(d->settings.highlights);
    refreshChips();
}

void SkissaColorWidget::refreshChips()
{
    QVector<KisStudioChips::Chip> presets;
    for (int i = 0; i < d->presets.size(); i++) {
        KisStudioChips::Chip chip;
        chip.text = d->presets[i].name;
        chip.on = d->settings == d->presets[i].settings;
        const QImage picture = d->presetPictures.value(i);
        chip.picture = [picture](QPainter &painter, const QRectF &rect) {
            painter.save();
            painter.setClipPath(KisModernStyle::squircle(rect, KisModernStyle::cornerRadius));
            const QSizeF size = QSizeF(picture.size()).scaled(rect.size(), Qt::KeepAspectRatioByExpanding);
            painter.drawImage(QRectF(rect.center() - QPointF(size.width(), size.height()) / 2, size), picture);
            painter.restore();
        };
        presets << chip;
    }
    d->presetChips->setChips(presets);

    QVector<KisStudioChips::Chip> tabs;
    for (int change = 0; change <= rgbTab; change++) {
        KisStudioChips::Chip chip;
        chip.text = change == rgbTab ? i18nc("color effect tab", "RGB") : axisName(change);
        chip.on = change == d->change;
        for (int by = 0; by < 3; by++) {
            if (change == rgbTab ? d->settings.rgb[by] != defaultRgbCurve() : d->settings.curves[by][change] != defaultCurve(by, change)) {
                chip.dot = true;
            }
        }
        tabs << chip;
    }
    d->changeTabs->setChips(tabs);

    QVector<KisStudioChips::Chip> bys;
    for (int by = 0; by < 3; by++) {
        KisStudioChips::Chip chip;
        chip.text = d->isRgb() ? channelName(by) : axisName(by);
        chip.on = by == d->currentBy();
        chip.dot = d->isRgb() ? d->settings.rgb[by] != defaultRgbCurve() : d->settings.curves[by][d->change] != defaultCurve(by, d->change);
        bys << chip;
    }
    d->byChips->setChips(bys);
}

void SkissaColorWidget::canvasHovered(const QPoint &pixel)
{
    if (!d->original || pixel.x() == INT_MIN) {
        d->curve->setMarker(-1);
        return;
    }
    KoColor color;
    d->original->pixel(pixel.x(), pixel.y(), &color);
    if (color.opacityU8() < 8) {
        d->curve->setMarker(-1);
        return;
    }
    QColor rgb;
    color.toQColor(&rgb);
    const Place place = placeOf(rgb);
    d->curve->setMarker(qBound(0.0f, d->isRgb() ? place.rgb[d->currentBy()] : place.axis[d->currentBy()], 1.0f));
}

bool SkissaColorWidget::canvasPressed(const QPoint &pixel)
{
    if (!d->original) return false;
    KoColor color;
    d->original->pixel(pixel.x(), pixel.y(), &color);
    if (color.opacityU8() < 8) return false;
    QColor rgb;
    color.toQColor(&rgb);

    if (d->pickGray->isChecked()) {
        // the color of this pixel, as a cast, is taken away: it turns gray
        const float linear[3] = {linearOf(rgb.redF()), linearOf(rgb.greenF()), linearOf(rgb.blueF())};
        float lab[3];
        KoMixing::linearToOklab(linear, lab);
        if (lab[0] > 0.05f) {
            d->settings.balance = QPointF(-lab[1] / lab[0], -lab[2] / lab[0]);
            d->balance->setValue(d->settings.balance);
            Q_EMIT sigConfigurationItemChanged();
        }
        d->pickGray->setChecked(false);
        return false;
    }

    const Place place = placeOf(rgb);
    const qreal x = qBound(0.0f, d->isRgb() ? place.rgb[d->currentBy()] : place.axis[d->currentBy()], 1.0f);
    d->dragPoint = d->curve->pointAt(x);
    d->dragStart = d->curve->curve().points.value(d->dragPoint).y();
    d->dragFrom = d->valueText(d->dragStart, false);
    d->curve->setShownPoint(d->dragPoint);
    d->current() = d->curve->curve();
    refreshChips();
    return true;
}

QString SkissaColorWidget::canvasDragged(const QPointF &moved, bool fine)
{
    if (d->dragPoint < 0) return QString();
    const qreal y = d->dragStart - moved.y() * (fine ? 0.25 : 1.0) / dragForWholeCurve;
    d->curve->setPointY(d->dragPoint, y);
    const QString name = d->isRgb() ? channelName(d->currentBy()) : axisName(d->change);
    return name + QStringLiteral("  ") + d->valueText(d->curve->curve().points.value(d->dragPoint).y(), false);
}

void SkissaColorWidget::canvasReleased()
{
    d->dragPoint = -1;
    d->curve->setShownPoint(-1);
}

#include "SkissaColorWidget.moc"
