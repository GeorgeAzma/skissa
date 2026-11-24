/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioGestures.h"

#include <QAction>
#include <QElapsedTimer>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>

#include <KisMainWindow.h>
#include <KisModernStyle.h>
#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisStudioItemCard.h"
#include "widgets/KisStudioPopup.h"

#include <functional>

namespace
{
/// How long one round of a drawing takes
constexpr int periodMs = 3200;

struct Scene {
    QString name;
    /// Draws the moment \p t (0 to 1) of the round in \p area
    std::function<void(QPainter &, const QRectF &area, qreal t, const QPalette &palette)> draw;
};

qreal between(qreal t, qreal from, qreal to)
{
    return qBound(0.0, (t - from) / (to - from), 1.0);
}

qreal eased(qreal x)
{
    return 1.0 - qPow(1.0 - x, 3);
}

QPointF lerp(const QPointF &a, const QPointF &b, qreal x)
{
    return a + (b - a) * x;
}

QColor inkOf(const QPalette &palette, qreal alpha = 1.0)
{
    QColor ink = palette.color(QPalette::Text);
    ink.setAlphaF(alpha);
    return ink;
}

/// The pointer, its tip at \p at; pressed, a ring around the tip (a right press a dashed one)
void pointer(QPainter &p, const QPointF &at, const QPalette &palette, bool pressed, bool right = false)
{
    if (pressed) {
        QPen ring(inkOf(palette, 0.55), 1.5);
        if (right) ring.setDashPattern({2, 2});
        p.setPen(ring);
        p.setBrush(inkOf(palette, 0.12));
        p.drawEllipse(at, 7, 7);
    }
    QPainterPath arrow;
    arrow.moveTo(0, 0);
    arrow.lineTo(0, 13);
    arrow.lineTo(3.5, 9.8);
    arrow.lineTo(6, 14.5);
    arrow.lineTo(8, 13.6);
    arrow.lineTo(5.6, 9);
    arrow.lineTo(10, 9);
    arrow.closeSubpath();
    p.setPen(QPen(palette.color(QPalette::Window), 1.2));
    p.setBrush(palette.color(QPalette::Text));
    p.drawPath(arrow.translated(at));
}

/// A ring filling as a hold goes on
void holdRing(QPainter &p, const QPointF &at, qreal filled, const QPalette &palette)
{
    if (filled <= 0) return;
    p.setPen(QPen(inkOf(palette, 0.8), 2, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(at - QPointF(11, 11), QSizeF(22, 22)), 90 * 16, -int(filled * 360 * 16));
}

QPainterPath rounded(const QRectF &rect, qreal radius)
{
    return KisModernStyle::squircle(rect, radius);
}

QVector<Scene> scenes()
{
    const QColor red(226, 92, 76);
    const QColor blue(76, 132, 226);
    const QColor gold(236, 182, 64);
    return {
        {i18nc("gesture: a drag on a value changes it, a right click resets it", "Drag a value, right-click resets"),
         [](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QRectF row(a.left() + 10, a.center().y() - 12, a.width() - 20, 24);
             p.setPen(Qt::NoPen);
             p.setBrush(inkOf(pal, 0.08));
             p.drawPath(rounded(row, KisModernStyle::tileRadius));
             const qreal drag = eased(between(t, 0.1, 0.5));
             const bool reset = t > 0.72;
             const int value = reset ? 20 : 20 + qRound(60 * drag);
             p.setBrush(inkOf(pal, 0.18));
             p.drawPath(rounded(QRectF(row.topLeft(), QSizeF(row.width() * value / 100.0, row.height())), KisModernStyle::tileRadius));
             p.setPen(inkOf(pal));
             p.drawText(row.adjusted(8, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, i18nc("a brush's size", "Size"));
             p.drawText(row.adjusted(8, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, QString::number(value));
             const QPointF start(row.left() + row.width() * 0.3, row.center().y());
             const QPointF end(row.left() + row.width() * 0.75, row.center().y());
             const QPointF at = t < 0.6 ? lerp(start, end, drag) : end;
             pointer(p, at, pal, (t > 0.1 && t < 0.5) || (t > 0.66 && t < 0.74), t > 0.6);
         }},
        {i18nc("gesture: a rough lasso held still at its end becomes a clean ellipse", "Hold still to snap a shape"),
         [](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QPointF c = a.center();
             const qreal rx = a.width() * 0.28;
             const qreal ry = a.height() * 0.3;
             const qreal drawn = between(t, 0.05, 0.45);
             QPainterPath rough;
             QPointF tip = c + QPointF(rx, 0);
             const int steps = 60;
             for (int i = 0; i <= int(steps * drawn); i++) {
                 const qreal angle = 2 * M_PI * i / steps;
                 const qreal wobble = 1.0 + 0.08 * qSin(angle * 5) + 0.05 * qCos(angle * 3);
                 tip = c + QPointF(qCos(angle) * rx * wobble, qSin(angle) * ry * wobble);
                 if (i == 0) rough.moveTo(tip);
                 else rough.lineTo(tip);
             }
             p.setBrush(Qt::NoBrush);
             if (t > 0.7) {
                 p.setPen(QPen(inkOf(pal), 1.5, Qt::DashLine));
                 p.drawEllipse(c, rx, ry);
             } else {
                 p.setPen(QPen(inkOf(pal), 1.5));
                 p.drawPath(rough);
             }
             holdRing(p, tip, t > 0.45 && t < 0.7 ? between(t, 0.45, 0.7) : 0, pal);
             pointer(p, tip, pal, t > 0.05 && t < 0.7);
         }},
        {i18nc("gesture: clicks place a selection's corners, a drag on a line adds one", "Click corners, drag a line to bend"),
         [](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             QVector<QPointF> corners = {a.topLeft() + QPointF(a.width() * 0.2, a.height() * 0.3),
                                         a.topLeft() + QPointF(a.width() * 0.8, a.height() * 0.3),
                                         a.topLeft() + QPointF(a.width() * 0.65, a.height() * 0.8)};
             const int placed = t < 0.1 ? 0 : t < 0.22 ? 1 : t < 0.34 ? 2 : 3;
             const qreal bend = eased(between(t, 0.5, 0.7));
             QVector<QPointF> shape = corners.mid(0, placed);
             if (t > 0.5) shape.insert(1, lerp((corners[0] + corners[1]) / 2, (corners[0] + corners[1]) / 2 - QPointF(0, a.height() * 0.22), bend));
             QPainterPath path;
             for (int i = 0; i < shape.size(); i++) {
                 if (i == 0) path.moveTo(shape[i]);
                 else path.lineTo(shape[i]);
             }
             if (t > 0.8) path.closeSubpath();
             p.setPen(QPen(inkOf(pal), 1.5, t > 0.8 ? Qt::DashLine : Qt::SolidLine));
             p.setBrush(Qt::NoBrush);
             p.drawPath(path);
             p.setBrush(inkOf(pal));
             p.setPen(Qt::NoPen);
             for (const QPointF &corner : shape) p.drawEllipse(corner, 2.5, 2.5);
             QPointF at = corners[qBound(0, placed - 1, 2)];
             if (t > 0.4) at = shape.size() > 1 && t > 0.5 ? shape[1] : (corners[0] + corners[1]) / 2;
             if (t > 0.8) at = corners[0];
             const bool pressed = (t > 0.1 && t < 0.14) || (t > 0.22 && t < 0.26) || (t > 0.34 && t < 0.38) || (t > 0.5 && t < 0.7)
                 || (t > 0.8 && t < 0.84);
             pointer(p, at, pal, pressed);
         }},
        {i18nc("gesture: a layer row dragged sideways changes its opacity", "Drag a layer sideways for opacity"),
         [red](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QRectF row(a.left() + 8, a.center().y() - 14, a.width() - 16, 28);
             p.setPen(Qt::NoPen);
             p.setBrush(inkOf(pal, 0.1));
             p.drawPath(rounded(row, KisModernStyle::tileRadius));
             const qreal drag = t < 0.5 ? eased(between(t, 0.1, 0.45)) : 1.0 - eased(between(t, 0.6, 0.9));
             const qreal opacity = 1.0 - 0.75 * drag;
             const QRectF thumb(row.left() + 4, row.top() + 4, 28, 20);
             QColor paint = red;
             paint.setAlphaF(opacity);
             p.setBrush(paint);
             p.drawPath(rounded(thumb, 3));
             p.setPen(inkOf(pal));
             p.drawText(row.adjusted(40, 0, -8, 0), Qt::AlignLeft | Qt::AlignVCenter, i18nc("a layer's name", "Layer 1"));
             p.setPen(inkOf(pal, 0.6));
             p.drawText(row.adjusted(40, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("%1%").arg(qRound(opacity * 100)));
             const QPointF from(row.left() + row.width() * 0.6, row.center().y());
             const QPointF to(row.left() + row.width() * 0.25, row.center().y());
             pointer(p, lerp(from, to, drag), pal, (t > 0.1 && t < 0.45) || (t > 0.6 && t < 0.9));
         }},
        {i18nc("gesture: a layer's picture held shows that layer alone", "Hold a layer's picture to see it alone"),
         [red, blue, gold](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QColor colors[] = {gold, blue, red};
             const bool alone = t > 0.4 && t < 0.85;
             const QRectF canvas(a.left() + 8, a.top() + 10, a.width() * 0.45, a.height() - 20);
             p.setPen(Qt::NoPen);
             p.setBrush(QColor(250, 248, 244));
             p.drawRect(canvas);
             for (int i = 0; i < 3; i++) {
                 if (alone && i != 1) continue;
                 p.setBrush(colors[i]);
                 const QPointF c = canvas.topLeft() + QPointF(canvas.width() * (0.3 + 0.2 * i), canvas.height() * (0.35 + 0.15 * i));
                 p.drawEllipse(c, canvas.width() * 0.18, canvas.width() * 0.18);
             }
             const qreal left = canvas.right() + 10;
             for (int i = 0; i < 3; i++) {
                 const QRectF thumb(left, a.top() + 14 + i * 24, 26, 18);
                 p.setBrush(inkOf(pal, 0.1));
                 p.drawPath(rounded(thumb, 3));
                 p.setBrush(colors[2 - i]);
                 p.drawEllipse(thumb.center(), 5, 5);
                 p.setBrush(inkOf(pal, 0.25));
                 p.drawRoundedRect(QRectF(thumb.right() + 6, thumb.center().y() - 2, a.right() - thumb.right() - 18, 4), 2, 2);
             }
             const QPointF at(left + 13, a.top() + 14 + 24 + 9);
             holdRing(p, at, t > 0.15 && t < 0.4 ? between(t, 0.15, 0.4) : 0, pal);
             pointer(p, at, pal, t > 0.15 && t < 0.85);
         }},
        {i18nc("gesture: the pen held still on the picture picks its color", "Hold the pen still to pick a color"),
         [red, blue](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QRectF canvas(a.left() + 8, a.top() + 10, a.width() - 50, a.height() - 20);
             p.setPen(Qt::NoPen);
             p.setBrush(blue);
             p.drawRect(canvas);
             p.setBrush(red);
             p.drawEllipse(canvas.center() + QPointF(canvas.width() * 0.15, 0), canvas.height() * 0.3, canvas.height() * 0.3);
             const bool picked = t > 0.62 && t < 0.95;
             p.setBrush(picked ? red : blue);
             p.setPen(QPen(inkOf(pal, 0.4), 1));
             p.drawEllipse(QPointF(a.right() - 21, a.center().y()), 11, 11);
             const QPointF from = canvas.topLeft() + QPointF(canvas.width() * 0.15, canvas.height() * 0.7);
             const QPointF to = canvas.center() + QPointF(canvas.width() * 0.15, 0);
             const QPointF at = lerp(from, to, eased(between(t, 0.05, 0.3)));
             holdRing(p, at, t > 0.32 && t < 0.62 ? between(t, 0.32, 0.62) : 0, pal);
             pointer(p, at, pal, t > 0.05 && t < 0.62);
         }},
        {i18nc("gesture: a color dropped on a palette swatch replaces it", "Drop a color on a swatch"),
         [red, blue, gold](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QColor swatches[] = {blue, gold, QColor(120, 190, 110), QColor(170, 110, 200)};
             const QPointF chip(a.left() + 24, a.center().y());
             const bool dropped = t > 0.6;
             p.setPen(QPen(inkOf(pal, 0.4), 1));
             p.setBrush(red);
             p.drawEllipse(chip, 11, 11);
             for (int i = 0; i < 4; i++) {
                 const QRectF swatch(a.right() - 110 + i * 26, a.center().y() - 11, 22, 22);
                 p.setBrush(i == 2 && dropped ? red : swatches[i]);
                 p.drawPath(rounded(swatch, KisModernStyle::cornerRadius));
             }
             const QPointF target(a.right() - 110 + 2 * 26 + 11, a.center().y());
             const qreal moved = eased(between(t, 0.15, 0.55));
             const QPointF at = lerp(chip, target, moved);
             if (t > 0.15 && t < 0.6) {
                 p.setBrush(red);
                 p.setPen(Qt::NoPen);
                 p.drawEllipse(at, 8, 8);
             }
             pointer(p, at, pal, t > 0.1 && t < 0.6);
         }},
        {i18nc("gesture: a right click on any button opens its card, with its key", "Right-click a button for its key"),
         [](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QRectF button(a.left() + 16, a.center().y() - 14, 28, 28);
             p.setPen(Qt::NoPen);
             p.setBrush(inkOf(pal, 0.12));
             p.drawPath(rounded(button, KisModernStyle::tileRadius));
             p.setPen(QPen(inkOf(pal), 1.8, Qt::SolidLine, Qt::RoundCap));
             p.drawLine(button.center() + QPointF(-5, 6), button.center() + QPointF(5, -6));
             if (t > 0.35) {
                 const QRectF card(button.right() + 10, a.top() + 12, a.width() - button.width() - 42, a.height() - 24);
                 p.setPen(QPen(inkOf(pal, 0.2), 1));
                 p.setBrush(pal.color(QPalette::Window));
                 p.drawPath(rounded(card, KisModernStyle::cardRadius));
                 p.setPen(inkOf(pal));
                 p.drawText(card.adjusted(10, 6, -10, 0), Qt::AlignLeft | Qt::AlignTop, i18nc("a brush", "Brush"));
                 const QRectF key(card.left() + 10, card.bottom() - 30, 24, 22);
                 p.setPen(QPen(inkOf(pal, 0.5), 1));
                 p.setBrush(inkOf(pal, 0.06));
                 p.drawPath(rounded(key, KisModernStyle::cornerRadius));
                 p.setPen(inkOf(pal));
                 p.drawText(key, Qt::AlignCenter, QStringLiteral("B"));
             }
             const QPointF at = lerp(a.center() + QPointF(20, 30), button.center(), eased(between(t, 0.0, 0.25)));
             pointer(p, at, pal, t > 0.27 && t < 0.35, true);
         }},
        {i18nc("gesture: a moved picture lines up with the canvas's middle and sides", "Moving lines up by itself"),
         [blue](QPainter &p, const QRectF &a, qreal t, const QPalette &pal) {
             const QRectF canvas(a.left() + 10, a.top() + 8, a.width() - 20, a.height() - 16);
             p.setPen(Qt::NoPen);
             p.setBrush(QColor(250, 248, 244));
             p.drawRect(canvas);
             const QSizeF size(canvas.width() * 0.3, canvas.height() * 0.4);
             const QPointF start = canvas.topLeft() + QPointF(8, 8);
             const QPointF middle = canvas.center() - QPointF(size.width() / 2, size.height() / 2);
             const qreal moved = eased(between(t, 0.1, 0.6));
             QPointF top = lerp(start, middle + QPointF(6, 4), moved);
             const bool caught = moved > 0.85 && t < 0.9;
             if (caught) top = middle;
             const QRectF box(top, size);
             p.setBrush(blue);
             p.drawRect(box);
             p.setPen(QPen(pal.color(QPalette::Highlight), 1));
             p.setBrush(Qt::NoBrush);
             p.drawRect(box);
             if (caught) {
                 p.drawLine(QPointF(canvas.center().x(), canvas.top()), QPointF(canvas.center().x(), canvas.bottom()));
                 p.drawLine(QPointF(canvas.left(), canvas.center().y()), QPointF(canvas.right(), canvas.center().y()));
             }
             pointer(p, box.center(), pal, t > 0.1 && t < 0.9);
         }},
    };
}

/// One drawing, going round while it is shown
class Clip : public QWidget
{
public:
    Clip(const Scene &scene, QWidget *parent)
        : QWidget(parent)
        , m_scene(scene)
    {
        setFixedSize(200, 110);
        setToolTip(scene.name);
        m_frames.setInterval(33);
        connect(&m_frames, &QTimer::timeout, this, QOverload<>::of(&QWidget::update));
        m_clock.start();
    }

protected:
    void showEvent(QShowEvent *) override { m_frames.start(); }
    void hideEvent(QHideEvent *) override { m_frames.stop(); }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(inkOf(palette(), 0.05));
        p.drawPath(rounded(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), KisModernStyle::tileRadius));
        const qreal t = (m_clock.elapsed() % periodMs) / qreal(periodMs);
        m_scene.draw(p, QRectF(rect()), t, palette());
    }

private:
    Scene m_scene;
    QTimer m_frames;
    QElapsedTimer m_clock;
};

void open(KisMainWindow *window)
{
    auto *popup = new KisStudioPopup(window);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    auto *layout = new QGridLayout(popup);
    popup->fitLayout(layout, KisModernStyle::space3);
    layout->setHorizontalSpacing(KisModernStyle::space3);
    layout->setVerticalSpacing(KisModernStyle::space2);
    const QVector<Scene> all = scenes();
    for (int i = 0; i < all.size(); i++) {
        auto *cell = new QWidget(popup);
        auto *column = new QVBoxLayout(cell);
        column->setContentsMargins(0, 0, 0, 0);
        column->setSpacing(KisModernStyle::space1);
        column->addWidget(new Clip(all[i], cell));
        auto *name = new QLabel(all[i].name, cell);
        name->setWordWrap(true);
        name->setFixedWidth(200);
        column->addWidget(name);
        layout->addWidget(cell, i / 3, i % 3, Qt::AlignTop);
    }
    popup->adjustSize();
    popup->popupAt(window->mapToGlobal(window->rect().center() - QPoint(popup->width() / 2, popup->height() / 2)), window);
}
} // namespace

void KisStudioGestures::install(KisMainWindow *window)
{
    auto *action = new QAction(i18nc("@action the gestures, drawn", "Gestures"), window);
    action->setObjectName(QStringLiteral("studio_gestures"));
    action->setShortcutContext(Qt::WindowShortcut);
    KisStudioShortcuts::restore(action);
    window->actionCollection()->addAction(action->objectName(), action);
    QPointer<KisMainWindow> owner = window;
    QObject::connect(action, &QAction::triggered, window, [owner]() {
        if (owner) open(owner);
    });
}
