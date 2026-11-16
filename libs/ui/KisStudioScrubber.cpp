/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioScrubber.h"

#include "KisStudioTips.h"

#include <QLineEdit>
#include <QContextMenuEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QtMath>
#include <QWheelEvent>

#include <cmath>
#include <KisModernStyle.h>
#include <KisValueSteps.h>

#include "widgets/KisStudioInlineEdit.h"

namespace {

constexpr int iconSize = 20;
/// The usual Grow or Soft, in pixels: a drag across the law's whole range goes this far
constexpr qreal usualAmount = 64;

int eventX(QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return qRound(event->position().x());
#else
    return event->x();
#endif
}

/// The largest dab shown at its own size; a larger one is shown this large, its ring dashed
constexpr qreal largestDab = 200;

/**
 * A brush's dab floating under its scrubber while its size or opacity changes: as large as the canvas will show
 * it, in its color and opacity, with a ring like the brush outline so a color like the panel's still shows.
 */
class DabView : public QWidget
{
public:
    explicit DabView(QWidget *owner)
        : QWidget(owner, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput | Qt::NoDropShadowWindowHint)
        , m_fade(this, "windowOpacity")
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        m_fade.setEasingCurve(KisModernStyle::motionCurve());
        QObject::connect(&m_fade, &QPropertyAnimation::finished, this, [this]() {
            if (m_leaving) hide();
        });
        m_linger.setSingleShot(true);
        QObject::connect(&m_linger, &QTimer::timeout, this, [this]() { fadeTo(0.0, KisModernStyle::exitDuration); });
    }

    void present(const KisStudioScrubber::Look &look, QWidget *under, int lingerMs)
    {
        m_look = look;
        const qreal diameter = qMax<qreal>(1.0, look.size * look.zoom);
        const int side = qCeil(qMax<qreal>(56, qMin(diameter, largestDab) + 2 * KisModernStyle::space4));
        resize(side, side);
        const QRect screen = under->screen() ? under->screen()->availableGeometry() : QRect();
        const QPoint below = under->mapToGlobal(QPoint(under->width() / 2, under->height())) + QPoint(-side / 2, KisModernStyle::space1);
        QPoint at = below;
        if (screen.isValid() && at.y() + side > screen.bottom()) at.setY(under->mapToGlobal(QPoint(0, 0)).y() - side - KisModernStyle::space1);
        if (screen.isValid()) at.setX(qBound(screen.left(), at.x(), screen.right() - side));
        move(at);
        if (!isVisible() || m_leaving) {
            if (!isVisible()) setWindowOpacity(0.0);
            show();
            fadeTo(1.0, KisModernStyle::enterDuration);
        }
        update();
        if (lingerMs >= 0) {
            m_linger.start(lingerMs);
        } else {
            m_linger.stop();
        }
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QColor window = palette().color(QPalette::Window);
        const QColor ink = palette().color(QPalette::WindowText);
        const QRectF panel = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QColor edge = ink;
        edge.setAlphaF(0.12);
        painter.setPen(QPen(edge, 1));
        painter.setBrush(window);
        painter.drawPath(KisModernStyle::squircle(panel, KisModernStyle::panelRadius));

        const qreal diameter = qMax<qreal>(1.0, m_look.size * m_look.zoom);
        const bool scaled = diameter > largestDab;
        const qreal shown = qMin(diameter, largestDab);
        const QPointF middle = panel.center();
        const QRectF dab(middle - QPointF(shown / 2, shown / 2), QSizeF(shown, shown));
        QPainterPath round;
        round.addEllipse(dab);
        painter.save();
        painter.setClipPath(round);
        // checks under a see-through dab, so its opacity reads as on the canvas
        if (m_look.opacity < 0.999 && shown > 6) {
            const qreal cell = qBound<qreal>(4, shown / 6, 10);
            const QColor dark = KisModernStyle::mix(window, ink, 0.14);
            for (qreal y = dab.top(); y < dab.bottom(); y += cell) {
                for (qreal x = dab.left(); x < dab.right(); x += cell) {
                    if ((qRound((x - dab.left()) / cell) + qRound((y - dab.top()) / cell)) % 2) painter.fillRect(QRectF(x, y, cell, cell), dark);
                }
            }
        }
        QColor paint = m_look.color;
        paint.setAlphaF(qBound(0.0, m_look.opacity, 1.0));
        painter.fillRect(dab, paint);
        painter.restore();
        QColor ring = ink;
        ring.setAlphaF(0.45);
        QPen pen(ring, 1);
        if (scaled) pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(dab.adjusted(-0.5, -0.5, 0.5, 0.5));
    }

private:
    void fadeTo(qreal opacity, int ms)
    {
        m_leaving = opacity == 0.0;
        m_fade.stop();
        m_fade.setDuration(ms);
        m_fade.setStartValue(windowOpacity());
        m_fade.setEndValue(opacity);
        m_fade.start();
    }

    KisStudioScrubber::Look m_look;
    QPropertyAnimation m_fade;
    QTimer m_linger;
    bool m_leaving {false};
};

} // namespace

KisStudioScrubber::KisStudioScrubber(Kind kind, const QString &toolTip, QWidget *parent)
    : QWidget(parent)
    , m_kind(kind)
{
    // an amount to apply has nothing to reset to
    const bool amount = kind == Kind::Grow || kind == Kind::Soft;
    setToolTip(KisStudioTips::tip(toolTip, KisStudioTips::valueGestures(!amount) << KisStudioTips::scrollStep()));
    setAccessibleName(toolTip);
    setCursor(Qt::SizeHorCursor);
    setAttribute(Qt::WA_Hover);
    setFocusPolicy(Qt::NoFocus);
}

void KisStudioScrubber::setRange(qreal minimum, qreal maximum)
{
    m_minimum = minimum;
    m_maximum = qMax(minimum + 1e-6, maximum);
    m_value = bounded(m_value);
    update();
}

void KisStudioScrubber::setValue(qreal value)
{
    // Don't let outside updates fight an ongoing drag
    if (m_dragging) {
        return;
    }
    m_value = bounded(value);
    update();
}

void KisStudioScrubber::setLook(const std::function<Look()> &look)
{
    m_look = look;
}

void KisStudioScrubber::showDab(int lingerMs)
{
    if (!m_look || (m_kind != Kind::Size && m_kind != Kind::Opacity)) return;
    Look look = m_look();
    if (m_kind == Kind::Size) {
        look.size = m_value;
    } else {
        look.opacity = (m_value - m_minimum) / (m_maximum - m_minimum);
    }
    if (!m_dab) m_dab = new DabView(this);
    static_cast<DabView *>(m_dab.data())->present(look, this, lingerMs);
}

qreal KisStudioScrubber::bounded(qreal value) const
{
    return qBound(m_minimum, value, m_maximum);
}

QString KisStudioScrubber::text() const
{
    if (m_kind == Kind::Opacity) {
        return QString::number(qRound(m_value)) + QLatin1Char('%');
    }
    // an amount to apply: named until dragged
    if (m_kind == Kind::Grow || m_kind == Kind::Soft) {
        const int amount = qRound(m_value);
        if (amount == 0) return accessibleName();
        return (m_kind == Kind::Grow ? (amount > 0 ? QStringLiteral("+") : QStringLiteral("−")) : QString())
            + QString::number(qAbs(amount));
    }
    if (m_kind == Kind::Threshold) return QString::number(qRound(m_value));
    // tenths below 10, and no trailing zeros (2, not 2.0)
    return QString::number(m_value < 10.0 ? qRound(m_value * 10) / 10.0 : qRound(m_value));
}

QSize KisStudioScrubber::sizeHint() const
{
    const int textWidth = qMax(fontMetrics().horizontalAdvance(m_kind == Kind::Opacity ? QStringLiteral("100%") : QStringLiteral("1000")),
                               m_kind == Kind::Grow || m_kind == Kind::Soft ? fontMetrics().horizontalAdvance(accessibleName()) : 0);
    return QSize(8 + iconSize + 6 + textWidth + 8, 30);
}

void KisStudioScrubber::commit(qreal value)
{
    m_value = bounded(value);
    update();
    Q_EMIT valueCommitted(m_value);
}

void KisStudioScrubber::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QPalette &pal = palette();
    const QColor window = pal.color(QPalette::Window);
    const QColor text = pal.color(QPalette::ButtonText);

    if (underMouse() || m_dragging) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(KisModernStyle::mix(window, text, m_dragging ? 0.16 : 0.09));
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()).adjusted(1, 2, -1, -2), KisModernStyle::tileRadius));
    }

    const QRectF icon(8, (height() - iconSize) / 2.0, iconSize, iconSize);
    painter.setPen(Qt::NoPen);
    if (m_kind == Kind::Size) {
        // Logarithmic, to match how sizes are perceived
        const qreal low = qMax(m_minimum, 1.0);
        const qreal position = qBound(0.0, std::log(qMax(m_value, low) / low) / std::log(m_maximum / low), 1.0);
        const qreal diameter = 3.0 + (iconSize - 3.0) * position;
        painter.setBrush(KisModernStyle::mix(window, text, 0.85));
        painter.drawEllipse(icon.center(), diameter / 2, diameter / 2);
    } else if (m_kind == Kind::Grow) {
        // a shape and the outline it grows (or shrinks) to
        const QColor ink = KisModernStyle::mix(window, text, 0.85);
        painter.setBrush(ink);
        painter.drawPath(KisModernStyle::squircle(QRectF(icon.center() - QPointF(4, 4), QSizeF(8, 8)), 2));
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ink, 1.3, Qt::DashLine));
        painter.drawPath(KisModernStyle::squircle(icon.adjusted(2.5, 2.5, -2.5, -2.5), 4));
    } else if (m_kind == Kind::Threshold) {
        // two shades that count as one, more alike the lower it is
        const qreal t = qBound(0.0, (m_value - m_minimum) / (m_maximum - m_minimum), 1.0);
        painter.setBrush(KisModernStyle::mix(window, text, 0.85));
        painter.drawPie(icon.adjusted(2, 2, -2, -2), 90 * 16, 180 * 16);
        painter.setBrush(KisModernStyle::mix(window, text, 0.85 - 0.6 * t));
        painter.drawPie(icon.adjusted(2, 2, -2, -2), 270 * 16, 180 * 16);
    } else if (m_kind == Kind::Soft) {
        // a dot whose edge fades
        QRadialGradient fade(icon.center(), iconSize / 2.0);
        QColor ink = KisModernStyle::mix(window, text, 0.85);
        fade.setColorAt(0.3, ink);
        ink.setAlphaF(0);
        fade.setColorAt(1, ink);
        painter.setBrush(fade);
        painter.drawEllipse(icon.center(), iconSize / 2.0, iconSize / 2.0);
    } else {
        QPainterPath tile;
        tile.addPath(KisModernStyle::squircle(icon.adjusted(1, 1, -1, -1), 4));
        painter.save();
        painter.setClipPath(tile);
        const qreal cell = (iconSize - 2) / 3.0;
        for (int y = 0; y < 3; y++) {
            for (int x = 0; x < 3; x++) {
                painter.fillRect(QRectF(icon.left() + 1 + x * cell, icon.top() + 1 + y * cell, cell, cell),
                                 (x + y) % 2 ? KisModernStyle::mix(window, text, 0.18) : KisModernStyle::mix(window, text, 0.36));
            }
        }
        QColor fill = KisModernStyle::mix(window, text, 0.85);
        fill.setAlphaF(qBound(0.0, (m_value - m_minimum) / (m_maximum - m_minimum), 1.0));
        painter.fillRect(icon, fill);
        painter.restore();
    }

    if (!m_editor || !m_editor->isVisible()) {
        painter.setPen(text);
        painter.drawText(QRectF(icon.right() + 6, 0, width() - icon.right() - 6, height()), Qt::AlignVCenter | Qt::AlignLeft, this->text());
    }
}

void KisStudioScrubber::mousePressEvent(QMouseEvent *event)
{
    m_moved = false;
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_dragging = true;
    m_fine = event->modifiers() & Qt::ShiftModifier;
    m_pressX = eventX(event);
    m_pressValue = m_value;
    update();
}

void KisStudioScrubber::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_dragging) {
        QWidget::mouseMoveEvent(event);
        return;
    }
    const bool fine = event->modifiers() & Qt::ShiftModifier;
    if (fine != m_fine) {
        // Continue from the current value at the new speed
        m_fine = fine;
        m_pressX = eventX(event);
        m_pressValue = m_value;
    }
    // Ignore small jitter so a plain click doesn't count as a drag
    if (!m_moved && qAbs(eventX(event) - m_pressX) <= KisModernStyle::scrubDistance(event)) {
        return;
    }
    if (!m_moved) Q_EMIT dragStarted();
    m_moved = true;
    const qreal dx = (eventX(event) - m_pressX) * (fine ? KisValueSteps::fine : 1.0);
    const qreal before = m_value;
    if (m_kind == Kind::Size) {
        m_value = bounded(KisValueSteps::draggedBy(m_pressValue, dx, 0, true, qMax<qreal>(m_minimum, 1)));
    } else if (m_kind == Kind::Grow || m_kind == Kind::Soft) {
        // whole pixels
        m_value = bounded(qRound(KisValueSteps::draggedBy(m_pressValue, dx, usualAmount, false)));
    } else {
        m_value = bounded(KisValueSteps::draggedBy(m_pressValue, dx, m_maximum - m_minimum, false));
    }
    if (m_value != before) Q_EMIT valueChanging(m_value);
    // stays while the drag does
    showDab(-1);
    update();
}

void KisStudioScrubber::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_dragging || event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    endDrag();
}

void KisStudioScrubber::endDrag()
{
    m_dragging = false;
    if (m_dab && m_dab->isVisible()) showDab(600);
    if (m_moved) {
        commit(m_value);
    } else {
        update();
    }
}

void KisStudioScrubber::mouseDoubleClickEvent(QMouseEvent *)
{
    // typed where the number is written
    const QString shown = text().remove(QLatin1Char('%'));
    const QRect at(8 + iconSize + 6, 0, fontMetrics().horizontalAdvance(text()), height());
    m_editor = KisStudioInlineEdit::open(this, at, shown, Qt::AlignLeft, font(), [this](const QString &typed) {
        bool ok = false;
        const qreal value = QString(typed).remove(QLatin1Char('%')).trimmed().toDouble(&ok);
        if (ok) commit(value);
    });
    update();
}

void KisStudioScrubber::contextMenuEvent(QContextMenuEvent *event)
{
    // a finger held still at the end of a drag isn't asking for a reset
    if (m_moved) {
        event->accept();
        return;
    }
    if (!qIsNaN(m_default)) {
        commit(m_default);
        event->accept();
        return;
    }
    QWidget::contextMenuEvent(event);
}

void KisStudioScrubber::wheelEvent(QWheelEvent *event)
{
    const int steps = KisModernStyle::wheelSteps(event, this);
    if (!steps) {
        event->accept();
        return;
    }
    if (m_kind == Kind::Size) {
        commit(KisValueSteps::turnedBy(m_value, steps, 0, true, qMax<qreal>(m_minimum, 1)));
    } else if (m_kind == Kind::Grow || m_kind == Kind::Soft) {
        // whole pixels, at least one a notch
        const qreal by = KisValueSteps::turnedBy(0, steps, usualAmount, false);
        commit(m_value + (steps > 0 ? qMax<qreal>(1, qRound(by)) : qMin<qreal>(-1, qRound(by))));
    } else {
        commit(KisValueSteps::turnedBy(m_value, steps, m_maximum - m_minimum, false));
    }
    showDab(700);
    event->accept();
}

bool KisStudioScrubber::event(QEvent *event)
{
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        update();
    }
    // The release never comes when the window loses focus or the bar hides mid-drag: end the drag here instead
    if (m_dragging && (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide
                       || event->type() == QEvent::UngrabMouse)) {
        endDrag();
    }
    return QWidget::event(event);
}
