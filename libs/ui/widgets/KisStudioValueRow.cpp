/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioValueRow.h"

#include "KisStudioTips.h"

#include <KisTouchMode.h>

#include <QContextMenuEvent>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QHelpEvent>
#include <QPainter>
#include <QToolTip>

#include <klocalizedstring.h>
#include <QPainterPath>

#include <cmath>

#include <KisModernStyle.h>
#include <kis_icon_utils.h>

#include "KisStudioInlineEdit.h"
#include <kis_cubic_curve.h>

namespace {

constexpr int glyphWidth = 18;
constexpr int glyphHeight = 12;

int eventX(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return qRound(event->position().x());
#else
    return event->x();
#endif
}

int eventY(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return qRound(event->position().y());
#else
    return event->y();
#endif
}

} // namespace

KisStudioValueRow::KisStudioValueRow(const QString &name, QWidget *parent)
    : QWidget(parent)
    , m_name(name)
{
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::SizeHorCursor);
    setAccessibleName(name);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void KisStudioValueRow::setName(const QString &name)
{
    m_name = name;
    setAccessibleName(name);
    update();
}

void KisStudioValueRow::setRange(const Range &range)
{
    m_range = range;
    m_range.maximum = qMax(range.minimum + 1e-9, range.maximum);
    update();
}

void KisStudioValueRow::setValue(qreal value)
{
    // an outside update doesn't fight a drag
    if (m_dragging) return;
    m_value = value;
    update();
}

void KisStudioValueRow::setValueText(const QString &text)
{
    m_valueText = text;
    update();
}

void KisStudioValueRow::setPenCurve(const QString &curve)
{
    if (curve == m_penCurve) return;
    m_penCurve = curve;
    update();
}

void KisStudioValueRow::setSelected(bool selected)
{
    if (selected == m_selected) return;
    m_selected = selected;
    update();
}

void KisStudioValueRow::setCompact(bool compact)
{
    m_compact = compact;
    updateGeometry();
    update();
}

void KisStudioValueRow::setPinnable(bool pinnable, bool pinned)
{
    m_pinnable = pinnable;
    m_pinned = pinned;
    update();
}

QRectF KisStudioValueRow::pinRect() const
{
    return QRectF(width() - KisModernStyle::space2 - 16, (height() - 16) / 2.0, 16, 16);
}

void KisStudioValueRow::setRemovable(bool removable)
{
    m_removable = removable;
    update();
}

bool KisStudioValueRow::revealed() const
{
    // fingers can't hover: what shows on hover always shows
    return underMouse() || KisTouchMode::isActive();
}

bool KisStudioValueRow::showsPin() const
{
    return m_pinnable && ((m_pinned && !m_pinOnHoverOnly) || revealed());
}

QRectF KisStudioValueRow::removeRect() const
{
    const qreal right = m_pinnable ? pinRect().left() - KisModernStyle::space1 : width() - KisModernStyle::space2;
    return QRectF(right - 16, (height() - 16) / 2.0, 16, 16);
}

QRectF KisStudioValueRow::textArea() const
{
    QRectF content = QRectF(rect()).adjusted(KisModernStyle::space3, 0, -KisModernStyle::space3, 0);
    if (showsPin()) content.setRight(pinRect().left() - KisModernStyle::space2);
    if (m_removable && revealed()) content.setRight(removeRect().left() - KisModernStyle::space2);
    if (!m_penCurve.isEmpty()) content.setRight(content.right() - glyphWidth - KisModernStyle::space2);
    return content;
}

void KisStudioValueRow::setInactive(bool inactive)
{
    if (inactive == m_inactive) return;
    m_inactive = inactive;
    update();
}

void KisStudioValueRow::setDraggable(bool draggable)
{
    m_draggable = draggable;
    setCursor(draggable ? Qt::SizeHorCursor : Qt::PointingHandCursor);
    update();
}

QSize KisStudioValueRow::sizeHint() const
{
    const int text = fontMetrics().horizontalAdvance(m_name) + fontMetrics().horizontalAdvance(QStringLiteral("1000 px"));
    return QSize(text + 3 * KisModernStyle::space3 + glyphWidth, m_compact ? 28 : 34);
}

QSize KisStudioValueRow::minimumSizeHint() const
{
    return QSize(120, m_compact ? 28 : 34);
}

qreal KisStudioValueRow::fraction() const
{
    const qreal low = m_range.minimum;
    const qreal high = m_range.maximum;
    if (m_range.size && low > 0) {
        return qBound(0.0, std::log(qMax(m_value, low) / low) / std::log(high / low), 1.0);
    }
    return qBound(0.0, (m_value - low) / (high - low), 1.0);
}

QString KisStudioValueRow::text() const
{
    if (!m_valueText.isEmpty()) return m_valueText;
    const int decimals = m_range.size && m_value >= 10.0 ? 0 : m_range.decimals;
    return QLocale().toString(m_value, 'f', decimals) + m_range.suffix;
}

void KisStudioValueRow::change(qreal value)
{
    const qreal step = std::pow(10.0, -m_range.decimals);
    value = qBound(m_range.minimum, std::round(value / step) * step, m_range.maximum);
    if (qFuzzyCompare(value + 1.0, m_value + 1.0)) return;
    m_value = value;
    m_valueText.clear();
    update();
    Q_EMIT valueChanged(m_value);
}

void KisStudioValueRow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = palette().color(QPalette::WindowText);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QPainterPath shape = KisModernStyle::squircle(box, KisModernStyle::tileRadius);

    QColor ground = ink;
    ground.setAlphaF(m_dragging ? 0.10 : underMouse() ? 0.075 : m_inactive ? 0.025 : 0.05);
    if (m_selected) {
        ground = palette().color(QPalette::Highlight);
        ground.setAlphaF(0.16);
    }
    painter.fillPath(shape, ground);

    if (m_draggable && m_valueText.isEmpty() && !m_inactive) {
        QColor bar = ink;
        bar.setAlphaF(0.08);
        painter.save();
        painter.setClipPath(shape);
        painter.fillRect(QRectF(box.left(), box.top(), box.width() * fraction(), box.height()), bar);
        painter.restore();
    }
    if (m_selected) {
        painter.setPen(QPen(palette().color(QPalette::Highlight), 1.5));
        painter.drawPath(shape);
    }

    if (showsPin()) {
        const QIcon pin = KisIconUtils::loadIcon(QStringLiteral("studio_pin"));
        pin.paint(&painter, pinRect().toRect(), Qt::AlignCenter, QIcon::Normal, m_pinned ? QIcon::On : QIcon::Off);
    }
    if (m_removable && revealed()) {
        // a cross: off the list
        const QRectF cross = removeRect().adjusted(4, 4, -4, -4);
        painter.setPen(QPen(KisModernStyle::secondaryInk(ink), 1.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(cross.topLeft(), cross.bottomRight());
        painter.drawLine(cross.topRight(), cross.bottomLeft());
    }
    const QRectF content = textArea();
    if (!m_penCurve.isEmpty()) {
        const QRectF glyph(content.right() + KisModernStyle::space2, content.center().y() - glyphHeight / 2.0, glyphWidth, glyphHeight);
        const KisCubicCurve curve(m_penCurve);
        QPainterPath path;
        for (int i = 0; i <= 12; i++) {
            const qreal x = i / 12.0;
            const QPointF point(glyph.left() + x * glyph.width(), glyph.bottom() - qBound(0.0, curve.value(x), 1.0) * glyph.height());
            if (i) path.lineTo(point);
            else path.moveTo(point);
        }
        painter.setPen(QPen(ink, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    painter.setPen(m_inactive ? KisModernStyle::secondaryInk(ink) : ink);
    painter.drawText(content, Qt::AlignVCenter | Qt::AlignLeft, m_name);
    // while it is typed, the editor stands where the value is
    if (m_editing) return;
    painter.setPen(KisModernStyle::secondaryInk(ink));
    painter.drawText(content, Qt::AlignVCenter | Qt::AlignRight, text());
}

void KisStudioValueRow::mousePressEvent(QMouseEvent *event)
{
    m_draggedThisPress = false;
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    if (showsPin() && pinRect().adjusted(-4, -6, 4, 6).contains(event->pos())) {
        Q_EMIT pinClicked();
        event->accept();
        return;
    }
    if (m_removable && revealed() && removeRect().adjusted(-3, -6, 3, 6).contains(event->pos())) {
        Q_EMIT removeClicked();
        event->accept();
        return;
    }
    m_pressed = true;
    m_dragging = false;
    m_pressY = eventY(event);
    m_fine = event->modifiers() & Qt::ShiftModifier;
    m_pressX = eventX(event);
    m_pressValue = m_value;
    event->accept();
}

void KisStudioValueRow::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_pressed || !m_draggable) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const int x = eventX(event);
    if (!m_dragging) {
        // pulled out up or down: carried elsewhere, not scrubbed
        const int dy = eventY(event) - m_pressY;
        if (m_pinnable && qAbs(dy) > 10 && qAbs(dy) > 2 * qAbs(x - m_pressX)) {
            m_pressed = false;
            Q_EMIT pulledOut();
            return;
        }
        // a click with a little jitter isn't a drag
        if (qAbs(x - m_pressX) <= 3) return;
        m_dragging = true;
        m_draggedThisPress = true;
        m_pressX = x;
    }
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    if (fine != m_fine) {
        // on from where it is, at the other speed
        m_fine = fine;
        m_pressX = x;
        m_pressValue = m_value;
    }
    const qreal moved = (x - m_pressX) / qMax<qreal>(40, width()) * (fine ? 0.25 : 1.0);
    qreal next;
    if (m_range.size && m_range.minimum > 0) {
        next = qMax(m_pressValue, m_range.minimum) * std::exp(moved * std::log(m_range.maximum / m_range.minimum));
    } else {
        next = m_pressValue + moved * (m_range.maximum - m_range.minimum);
    }
    change(next);
    event->accept();
}

void KisStudioValueRow::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_pressed || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    m_pressed = false;
    const bool dragged = m_dragging;
    m_dragging = false;
    update();
    if (!dragged && rect().contains(event->pos())) Q_EMIT clicked();
    event->accept();
}

void KisStudioValueRow::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_draggable || event->button() != Qt::LeftButton) {
        QWidget::mouseDoubleClickEvent(event);
        return;
    }
    Q_EMIT typingStarted();
    // the number is typed where it is written, the unit left out
    const QRectF content = textArea();
    const QString shown = QLocale().toString(m_value, 'f', m_range.size && m_value >= 10.0 ? 0 : m_range.decimals);
    const int width = fontMetrics().horizontalAdvance(text());
    const QRect at(QPointF(content.right() - width, content.top()).toPoint(), QSize(width, int(content.height())));
    m_editing = true;
    QLineEdit *editor = KisStudioInlineEdit::open(this, at, shown, Qt::AlignRight, font(), [this](const QString &typed) {
        QString number = typed;
        if (!m_range.suffix.trimmed().isEmpty()) number.remove(m_range.suffix.trimmed());
        bool ok = false;
        const qreal value = QLocale().toDouble(number.trimmed(), &ok);
        if (ok) change(value);
    });
    connect(editor, &QObject::destroyed, this, [this]() {
        m_editing = false;
        update();
    });
    update();
    event->accept();
}

void KisStudioValueRow::contextMenuEvent(QContextMenuEvent *event)
{
    // a finger held still at the end of a drag isn't asking for a reset
    if (m_draggedThisPress) {
        event->accept();
        return;
    }
    if (!qIsNaN(m_default)) {
        change(m_default);
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

bool KisStudioValueRow::event(QEvent *event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        update();
    }
    if (event->type() == QEvent::ToolTip) {
        // the pin and the cross say what they do; the rest of the row has its own tooltip
        auto *help = static_cast<QHelpEvent *>(event);
        QString tip;
        if (showsPin() && pinRect().adjusted(-4, -6, 4, 6).contains(help->pos())) {
            tip = m_pinned ? i18nc("@info:tooltip", "Unpin from the top bar") : i18nc("@info:tooltip", "Pin to the top bar");
        } else if (m_removable && removeRect().adjusted(-3, -6, 3, 6).contains(help->pos())) {
            tip = i18nc("@info:tooltip", "Take off the brush");
        }
        // what the row is, then its gestures
        if (tip.isEmpty() && m_draggable) tip = KisStudioTips::tip(toolTip(), KisStudioTips::valueGestures(!qIsNaN(m_default)));
        if (!tip.isEmpty()) {
            QToolTip::showText(help->globalPos(), tip, this);
            return true;
        }
    }
    // the release never comes when the window loses focus mid-drag
    if (m_pressed && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide
                      || event->type() == QEvent::UngrabMouse)) {
        m_pressed = false;
        m_dragging = false;
        update();
    }
    return QWidget::event(event);
}
