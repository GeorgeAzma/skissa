/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushSlider.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <cmath>

namespace {

constexpr int labelHeight = 16;
/// Room for a preview: a 24 px dot reads as a brush, a number does not
constexpr int previewHeight = 30;

qreal mouseY(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->position().y();
#else
    return event->localPos().y();
#endif
}

QColor mix(const QColor &a, const QColor &b, qreal t)
{
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

} // namespace

KisStudioBrushSlider::KisStudioBrushSlider(const QString &title, QWidget *parent)
    : QWidget(parent)
    , m_title(title)
    , m_formatter([](qreal value) { return QString::number(value, 'f', 0); })
{
    setCursor(Qt::SizeVerCursor);
    setAccessibleName(title);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Preferred);
    updateToolTip();
}

void KisStudioBrushSlider::setRange(qreal minimum, qreal maximum, qreal exponent)
{
    m_minimum = minimum;
    m_maximum = qMax(minimum, maximum);
    m_exponent = qMax(exponent, 0.01);
    setValue(m_value);
}

void KisStudioBrushSlider::setLogarithmic(bool logarithmic)
{
    m_logarithmic = logarithmic;
    update();
}

void KisStudioBrushSlider::setValue(qreal value)
{
    if (m_dragging) {
        // don't fight with the user while they drag
        return;
    }
    m_value = qBound(m_minimum, value, m_maximum);
    updateToolTip();
    update();
}

qreal KisStudioBrushSlider::value() const
{
    return m_value;
}

void KisStudioBrushSlider::setValueFormatter(std::function<QString(qreal)> formatter)
{
    m_formatter = std::move(formatter);
    updateToolTip();
    update();
}

void KisStudioBrushSlider::setPreview(Preview preview)
{
    m_preview = preview;
    updateGeometry();
    update();
}

QSize KisStudioBrushSlider::sizeHint() const
{
    return QSize(30, 180);
}

QSize KisStudioBrushSlider::minimumSizeHint() const
{
    return QSize(30, 80);
}

QRectF KisStudioBrushSlider::trackRect() const
{
    const qreal width = 14;
    const int bottom = m_preview == Preview::None ? labelHeight : previewHeight;
    return QRectF((this->width() - width) / 2.0, 2, width, height() - bottom - 4);
}

qreal KisStudioBrushSlider::position() const
{
    if (m_maximum <= m_minimum) {
        return 0.0;
    }
    if (m_logarithmic) {
        const qreal low = qMax(m_minimum, 1.0);
        return qBound(0.0, std::log(qMax(m_value, low) / low) / std::log(m_maximum / low), 1.0);
    }
    const qreal linear = (m_value - m_minimum) / (m_maximum - m_minimum);
    return std::pow(qBound(0.0, linear, 1.0), 1.0 / m_exponent);
}

qreal KisStudioBrushSlider::valueAtPosition(qreal position) const
{
    if (m_logarithmic) {
        const qreal low = qMax(m_minimum, 1.0);
        return low * std::pow(m_maximum / low, qBound(0.0, position, 1.0));
    }
    return m_minimum + (m_maximum - m_minimum) * std::pow(qBound(0.0, position, 1.0), m_exponent);
}

qreal KisStudioBrushSlider::valueAtY(qreal y) const
{
    const QRectF track = trackRect();
    return valueAtPosition((track.bottom() - y) / track.height());
}

void KisStudioBrushSlider::updateToolTip()
{
    setToolTip(QStringLiteral("%1: %2").arg(m_title, m_formatter(m_value)));
}

bool KisStudioBrushSlider::event(QEvent *event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        update(); // the track is highlighted while hovered
    }
    return QWidget::event(event);
}

void KisStudioBrushSlider::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette &pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::WindowText);
    const QRectF track = trackRect();
    const qreal radius = track.width() / 2.0;

    painter.setPen(Qt::NoPen);
    painter.setBrush(mix(window, text, underMouse() || m_dragging ? 0.16 : 0.10));
    painter.drawRoundedRect(track, radius, radius);

    // fill from the bottom, clipped to the rounded track
    const qreal fillHeight = track.height() * position();
    if (fillHeight > 0.5) {
        QPainterPath trackPath;
        trackPath.addRoundedRect(track, radius, radius);
        painter.save();
        painter.setClipPath(trackPath);
        // a value, not a selection: neutral, the accent stays for "active"
        painter.setBrush(mix(window, text, underMouse() || m_dragging ? 0.62 : 0.48));
        painter.drawRect(QRectF(track.left(), track.bottom() - fillHeight, track.width(), fillHeight));
        painter.restore();
    }

    const bool showNumber = m_preview == Preview::None || underMouse() || m_dragging;
    const QRectF bottom(0, track.bottom() + 2, width(), height() - track.bottom() - 2);
    if (showNumber) {
        QFont font = this->font();
        font.setPointSizeF(font.pointSizeF() * 0.8);
        painter.setFont(font);
        painter.setPen(text);
        painter.drawText(bottom, Qt::AlignCenter, m_formatter(m_value));
        return;
    }

    const QPointF center = bottom.center();
    const qreal room = qMin(bottom.width(), bottom.height()) - 4;
    if (m_preview == Preview::Dot) {
        // a dot that grows the way the slider moves, so 2 px and 200 px both read
        const qreal diameter = 3.0 + (room - 3.0) * position();
        painter.setBrush(mix(window, text, 0.85));
        painter.drawEllipse(center, diameter / 2, diameter / 2);
    } else {
        // a tile, not a disc: it must not look like the size dot
        const qreal side = room - 4;
        const QRectF disc(center.x() - side / 2, center.y() - side / 2, side, side);
        QPainterPath discPath;
        discPath.addRoundedRect(disc, 4, 4);
        painter.save();
        painter.setClipPath(discPath);
        const qreal cell = side / 4;
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                painter.fillRect(QRectF(disc.left() + x * cell, disc.top() + y * cell, cell, cell),
                                 (x + y) % 2 ? mix(window, text, 0.18) : mix(window, text, 0.34));
            }
        }
        QColor fill = mix(window, text, 0.85);
        fill.setAlphaF(qBound(0.0, (m_value - m_minimum) / qMax(1e-6, m_maximum - m_minimum), 1.0));
        painter.fillRect(disc, fill);
        painter.restore();
    }
}

void KisStudioBrushSlider::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_value = valueAtY(mouseY(event));
    update();
}

void KisStudioBrushSlider::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    m_value = valueAtY(mouseY(event));
    update();
}

void KisStudioBrushSlider::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_dragging = false;
    m_value = valueAtY(mouseY(event));
    updateToolTip();
    update();
    Q_EMIT valueCommitted(m_value);
}

void KisStudioBrushSlider::wheelEvent(QWheelEvent *event)
{
    const qreal notches = event->angleDelta().y() / 120.0;
    if (qFuzzyIsNull(notches) || m_dragging) {
        event->ignore();
        return;
    }
    m_value = valueAtPosition(position() + 0.02 * notches);
    updateToolTip();
    update();
    Q_EMIT valueCommitted(m_value);
    event->accept();
}
