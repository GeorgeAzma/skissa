/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisCurvePresetStrip.h"

#include <QContextMenuEvent>
#include <QHelpEvent>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QToolTip>

#include <KConfigGroup>
#include <KSharedConfig>
#include <klocalizedstring.h>

#include <KisModernStyle.h>
#include <kis_cubic_curve.h>

namespace
{

const int tileSize = 26;
const int gap = KisModernStyle::space1;

QString curveOf(const QList<QPointF> &points)
{
    return KisCubicCurve(points).toString();
}

/// The curves saved, in every brush setting; strips change together
class Saved : public QObject
{
public:
    static Saved &instance()
    {
        static Saved *saved = new Saved();
        return *saved;
    }

    QStringList names() const { return group().readEntry("names", QStringList()); }
    QStringList curves() const { return group().readEntry("curves", QStringList()); }

    void set(const QStringList &names, const QStringList &curves)
    {
        KConfigGroup g = group();
        g.writeEntry("names", names);
        g.writeEntry("curves", curves);
        g.sync();
        Q_FOREACH (const QPointer<QWidget> &strip, m_strips) {
            if (strip) {
                strip->updateGeometry();
                strip->update();
            }
        }
    }

    void add(QWidget *strip) { m_strips << strip; }

private:
    static KConfigGroup group() { return KSharedConfig::openConfig()->group("SavedCurves"); }
    QList<QPointer<QWidget>> m_strips;
};

} // namespace

KisCurvePresetStrip::KisCurvePresetStrip(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_Hover);
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    Saved::instance().add(this);
}

void KisCurvePresetStrip::setCurrent(const QString &curve)
{
    if (curve == m_current) return;
    m_current = curve;
    update();
}

QVector<KisCurvePresetStrip::Entry> KisCurvePresetStrip::entries() const
{
    QVector<Entry> list = {
        {i18nc("curve preset", "Linear"), curveOf({QPointF(0, 0), QPointF(1, 1)})},
        {i18nc("curve preset", "Soft: a light touch is enough"), curveOf({QPointF(0, 0), QPointF(0.25, 0.5), QPointF(1, 1)})},
        {i18nc("curve preset", "Firm: press harder"), curveOf({QPointF(0, 0), QPointF(0.5, 0.25), QPointF(1, 1)})},
    };
    const QStringList names = Saved::instance().names();
    const QStringList curves = Saved::instance().curves();
    for (int i = 0; i < curves.size(); i++) {
        list.append({i < names.size() ? names[i] : QString(), curves[i], true});
    }
    return list;
}

QRect KisCurvePresetStrip::tile(int i) const
{
    return QRect(i * (tileSize + gap), 0, tileSize, tileSize);
}

int KisCurvePresetStrip::at(const QPoint &pos) const
{
    const int count = entries().size();
    for (int i = 0; i <= count; i++) {
        if (tile(i).contains(pos)) return i;
    }
    return -1;
}

QSize KisCurvePresetStrip::sizeHint() const
{
    const int count = entries().size() + 1;
    return QSize(count * tileSize + (count - 1) * gap, tileSize);
}

void KisCurvePresetStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor ink = palette().color(QPalette::WindowText);
    auto faint = [ink](qreal alpha) {
        QColor c = ink;
        c.setAlphaF(alpha);
        return c;
    };

    const QVector<Entry> list = entries();
    for (int i = 0; i <= list.size(); i++) {
        const QRectF r = QRectF(tile(i)).adjusted(0.5, 0.5, -0.5, -0.5);
        const bool chosen = i < list.size() && !m_current.isEmpty() && KisCubicCurve(list[i].curve) == KisCubicCurve(m_current);
        painter.setPen(QPen(faint(chosen ? 0.0 : 0.14), 1));
        painter.setBrush(chosen ? faint(0.16) : (i == m_hovered ? faint(0.06) : QColor(Qt::transparent)));
        painter.drawRoundedRect(r, KisModernStyle::cornerRadius, KisModernStyle::cornerRadius);

        if (i == list.size()) {
            // +: the curve being edited, saved
            painter.setPen(QPen(KisModernStyle::secondaryInk(ink), 1.4, Qt::SolidLine, Qt::RoundCap));
            const QPointF c = r.center();
            painter.drawLine(c - QPointF(5, 0), c + QPointF(5, 0));
            painter.drawLine(c - QPointF(0, 5), c + QPointF(0, 5));
            continue;
        }
        // the curve itself, as it shapes the value
        const KisCubicCurve curve(list[i].curve);
        const QRectF plot = r.adjusted(5, 5, -5, -5);
        QPainterPath path;
        const int steps = 24;
        for (int s = 0; s <= steps; s++) {
            const qreal x = qreal(s) / steps;
            const QPointF p(plot.left() + x * plot.width(), plot.bottom() - qBound(0.0, curve.value(x), 1.0) * plot.height());
            if (s == 0) path.moveTo(p);
            else path.lineTo(p);
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(chosen || i == m_hovered ? ink : KisModernStyle::secondaryInk(ink), 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(path);
    }
}

void KisCurvePresetStrip::mouseMoveEvent(QMouseEvent *event)
{
    const int hovered = at(event->pos());
    if (hovered != m_hovered) {
        m_hovered = hovered;
        update();
    }
}

void KisCurvePresetStrip::leaveEvent(QEvent *)
{
    m_hovered = -1;
    update();
}

void KisCurvePresetStrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) return;
    const QVector<Entry> list = entries();
    const int i = at(event->pos());
    if (i < 0) return;
    if (i < list.size()) {
        Q_EMIT picked(list[i].curve);
        return;
    }
    if (m_current.isEmpty()) return;
    QStringList names = Saved::instance().names();
    QStringList curves = Saved::instance().curves();
    if (curves.contains(m_current)) return;
    while (names.size() < curves.size()) names << QString();
    names << i18nc("a saved curve's name", "Curve %1", curves.size() + 1);
    curves << m_current;
    Saved::instance().set(names, curves);
}

void KisCurvePresetStrip::contextMenuEvent(QContextMenuEvent *event)
{
    const QVector<Entry> list = entries();
    const int i = at(event->pos());
    if (i < 0 || i >= list.size() || !list[i].saved) return;
    const int index = i - (list.size() - Saved::instance().curves().size());

    QMenu menu(this);
    QAction *rename = menu.addAction(i18nc("@action a saved curve", "Rename"));
    QAction *remove = menu.addAction(i18nc("@action a saved curve", "Delete"));
    QAction *chosen = menu.exec(event->globalPos());
    QStringList names = Saved::instance().names();
    QStringList curves = Saved::instance().curves();
    while (names.size() < curves.size()) names << QString();
    if (chosen == rename) {
        bool ok = false;
        const QString name = QInputDialog::getText(this, i18nc("@title:window", "Rename curve"), QString(), QLineEdit::Normal, names[index], &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        names[index] = name.trimmed();
    } else if (chosen == remove) {
        names.removeAt(index);
        curves.removeAt(index);
    } else {
        return;
    }
    Saved::instance().set(names, curves);
}

bool KisCurvePresetStrip::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const QVector<Entry> list = entries();
        const int i = at(help->pos());
        if (i < 0) {
            QToolTip::hideText();
        } else {
            QToolTip::showText(help->globalPos(), i < list.size() ? list[i].name : i18nc("@info:tooltip", "Save this curve"), this, tile(i));
        }
        return true;
    }
    return QWidget::event(event);
}
