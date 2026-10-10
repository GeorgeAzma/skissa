/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasRing.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QTabletEvent>
#include <QTimer>
#include <QVariantAnimation>
#include <QtMath>

#include <KoColor.h>
#include <KoToolManager.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisStrokeLog.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioMotion.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_canvas_resource_provider.h"
#include "kis_image.h"
#include "kis_node.h"
#include "kis_paintop_preset.h"
#include <KisModernStyle.h>

namespace {

constexpr int ringBrushes = 8;
constexpr int ringColors = 12;
constexpr qreal plateRadius = 172;
constexpr qreal slotDistance = 100;
constexpr qreal slotRadius = 31;
constexpr qreal colorDistance = 152;
constexpr qreal colorRadius = 9;
constexpr qreal middleRadius = 26;
/// Moved this far from where it opened: a flick towards a brush, not a click
constexpr qreal flick = 24;

/// The tools that paint with the brush: the ring is theirs; the others keep their own right click
bool paintToolActive()
{
    static const QStringList tools = {
        QStringLiteral("KritaShape/KisToolBrush"), QStringLiteral("KritaShape/KisToolDyna"),
        QStringLiteral("KritaShape/KisToolMultiBrush"), QStringLiteral("KritaShape/KisToolLine"),
    };
    return tools.contains(KoToolManager::instance()->activeToolId());
}

} // namespace

KisStudioCanvasRing::KisStudioCanvasRing(KisMainWindow *mainWindow)
    : QWidget(nullptr)
    , m_mainWindow(mainWindow)
{
    setAttribute(Qt::WA_NoSystemBackground);
    setMouseTracking(true);
    setFixedSize(int(2 * plateRadius + 8), int(2 * plateRadius + 8));
    hide();
    qApp->installEventFilter(this);
}

KisStudioCanvasRing::~KisStudioCanvasRing()
{
    qApp->removeEventFilter(this);
}

QPointF KisStudioCanvasRing::slotCenter(int index) const
{
    // the first at the top, then clockwise
    const qreal angle = qDegreesToRadians(-90.0 + index * 360.0 / ringBrushes);
    return m_center + slotDistance * QPointF(qCos(angle), qSin(angle));
}

QPointF KisStudioCanvasRing::colorCenter(int index) const
{
    // between the brushes' directions, from the top right
    const qreal angle = qDegreesToRadians(-75.0 + index * 360.0 / ringColors);
    return m_center + colorDistance * QPointF(qCos(angle), qSin(angle));
}

int KisStudioCanvasRing::targetAt(const QPointF &pos) const
{
    const QPointF delta = pos - m_center;
    const qreal distance = qSqrt(delta.x() * delta.x() + delta.y() * delta.y());
    if (distance < middleRadius + 2) {
        return -2;
    }
    for (int i = 0; i < m_colors.size(); i++) {
        if (QLineF(colorCenter(i), pos).length() <= colorRadius + 5) return 100 + i;
    }
    // a brush by its direction: anywhere in its eighth, also past the ring when flicking
    if (distance > plateRadius && !m_held) {
        return -1;
    }
    qreal angle = qRadiansToDegrees(qAtan2(delta.y(), delta.x())) + 90.0;
    while (angle < 0) angle += 360.0;
    const int index = int(qRound(angle / (360.0 / ringBrushes))) % ringBrushes;
    return index < m_slots.size() ? index : -1;
}

void KisStudioCanvasRing::open(QWidget *canvas, const QPointF &pos)
{
    KisViewManager *view = m_mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    if (!view || !resources) {
        return;
    }
    if (parentWidget() != canvas) {
        setParent(canvas);
    }

    // your first brushes, in your order, with what they paint
    m_slots.clear();
    static const QRegularExpression sortPrefix(QStringLiteral("^[a-z]\\)\\s+"));
    Q_FOREACH (const QString &name, KisStudioBrushPicker::yourBrushes().mid(0, ringBrushes)) {
        Slot slot;
        slot.brush = name;
        slot.title = QString(name).remove(sortPrefix);
        slot.picture = KisStudioBrushPicker::strokePicture(name);
        if (slot.picture.isNull()) {
            if (KisPaintOpPresetSP preset = KisResourceServerProvider::instance()->paintOpPresetServer()->resource(QString(), QString(), name)) {
                slot.picture = preset->image();
            }
        }
        m_slots << slot;
    }
    // the colors painted on the layer, around them
    m_colors.clear();
    if (view->activeNode()) {
        m_colors = view->activeNode()->strokeLog().colors(ringColors);
    }
    m_current = resources->fgColor().toQColor();
    m_currentBrush = resources->currentPreset() ? resources->currentPreset()->name() : QString();

    // all of it on the canvas: the ring moves in from the edges, the pointer stays where it is
    const QSizeF half = QSizeF(size()) / 2;
    QPointF at = pos;
    if (canvas->width() > width()) at.setX(qBound(half.width(), at.x(), canvas->width() - half.width()));
    if (canvas->height() > height()) at.setY(qBound(half.height(), at.y(), canvas->height() - half.height()));
    move((at - QPointF(half.width(), half.height())).toPoint());
    m_center = QPointF(half.width(), half.height());

    m_held = true;
    m_moved = false;
    m_hover = -1;
    show();
    raise();
    KisStudioMotion::showFaded(this);
    // it grows out of the pointer, quickly
    auto *grow = new QVariantAnimation(this);
    grow->setStartValue(0.86);
    grow->setEndValue(1.0);
    grow->setDuration(120);
    grow->setEasingCurve(QEasingCurve::OutCubic);
    connect(grow, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_scale = value.toReal();
        update();
    });
    m_scale = 0.86;
    grow->start(QAbstractAnimation::DeleteWhenStopped);
    update();
}

void KisStudioCanvasRing::close()
{
    m_held = false;
    m_hover = -1;
    if (isVisible()) {
        KisStudioMotion::hideFaded(this);
    }
}

void KisStudioCanvasRing::choose(int target)
{
    KisViewManager *view = m_mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    close();
    if (!resources) {
        return;
    }
    if (target >= 0 && target < m_slots.size()) {
        KisStudioBrushPicker::use(view, m_slots[target].brush);
    } else if (target >= 100 && target - 100 < m_colors.size()) {
        const KoColorSpace *space = view->image() ? view->image()->colorSpace() : resources->fgColor().colorSpace();
        resources->setFGColor(KoColor(m_colors[target - 100], space));
    } else if (target == -2) {
        // all the brushes
        auto *picker = new KisStudioBrushPicker(m_mainWindow);
        picker->popupNear(QCursor::pos());
    }
}

bool KisStudioCanvasRing::eventFilter(QObject *watched, QEvent *event)
{
    /**
     * Every event of the application comes here: only the pointer's and
     * Escape are of the ring's concern, the rest goes on before anything is
     * looked up (also while the window is being destroyed, its view
     * manager half gone, when its actions are taken away).
     */
    const QEvent::Type type = event->type();
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease:
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease:
    case QEvent::KeyPress:
        break;
    default:
        return false;
    }
    KisViewManager *view = m_mainWindow->viewManager();
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    QWidget *canvasWidget = canvas ? canvas->canvasWidget() : nullptr;
    if (!canvasWidget) {
        return false;
    }

    if (type == QEvent::KeyPress && isVisible() && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        close();
        return true;
    }

    const bool onCanvas = watched == canvasWidget;
    const bool onRing = watched == this;
    if (!onCanvas && !onRing) {
        return false;
    }

    // where, which button, held where
    QPointF global;
    Qt::MouseButton button = Qt::NoButton;
    Qt::KeyboardModifiers modifiers;
    bool press = false, move = false, release = false;
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        global = mouse->globalPos();
        button = mouse->button();
        modifiers = mouse->modifiers();
        press = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick;
        move = type == QEvent::MouseMove;
        release = type == QEvent::MouseButtonRelease;
        break;
    }
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease: {
        auto *tablet = static_cast<QTabletEvent *>(event);
        global = tablet->globalPosF();
        button = tablet->button();
        modifiers = tablet->modifiers();
        press = type == QEvent::TabletPress;
        move = type == QEvent::TabletMove;
        release = type == QEvent::TabletRelease;
        break;
    }
    default:
        return false;
    }

    if (!isVisible()) {
        // a right press on the canvas with a painting tool opens it there
        if (onCanvas && press && button == Qt::RightButton && modifiers == Qt::NoModifier && paintToolActive()) {
            open(canvasWidget, canvasWidget->mapFromGlobal(global.toPoint()));
            event->accept();
            return true;
        }
        return false;
    }

    const QPointF local = mapFromGlobal(global.toPoint());
    if (move) {
        if (m_held && QLineF(local, m_center).length() > flick) m_moved = true;
        const int hover = (m_held && !m_moved) ? -1 : targetAt(local);
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        event->accept();
        return true;
    }
    if (release) {
        if (m_held) {
            m_held = false;
            // let go on a brush after moving towards it: taken; without moving: open to click
            if (m_moved) {
                const int target = targetAt(local);
                if (target != -1) choose(target);
                else close();
            }
        }
        event->accept();
        return true;
    }
    if (press) {
        if (button == Qt::LeftButton && QLineF(local, m_center).length() <= plateRadius) {
            const int target = targetAt(local);
            if (target != -1) choose(target);
        } else {
            // elsewhere, or the right button again: closed, and that press is not a stroke
            close();
        }
        event->accept();
        return true;
    }
    return false;
}

void KisStudioCanvasRing::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // off the canvas while hidden: a closed view must not take the ring with it
    QTimer::singleShot(0, this, [this]() {
        if (!isVisible() && parentWidget()) setParent(nullptr);
    });
}

void KisStudioCanvasRing::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    if (m_scale < 1.0) {
        painter.translate(m_center);
        painter.scale(m_scale, m_scale);
        painter.translate(-m_center);
    }
    const QPalette pal = palette();
    QColor plate = pal.color(QPalette::Window);
    plate.setAlphaF(0.92);
    QColor line = pal.color(QPalette::WindowText);
    line.setAlphaF(0.1);
    const QColor accent = pal.color(QPalette::Highlight);

    // the plate, with a soft shadow
    for (int i = 6; i > 0; i -= 2) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 14));
        painter.drawEllipse(m_center + QPointF(0, 3), plateRadius - 6 + i, plateRadius - 6 + i);
    }
    painter.setBrush(plate);
    painter.setPen(QPen(line, 1));
    painter.drawEllipse(m_center, plateRadius - 6, plateRadius - 6);

    // the direction being flicked towards
    if (m_hover >= 0 && m_hover < m_slots.size()) {
        QColor wedge = accent;
        wedge.setAlphaF(0.16);
        const qreal start = 90.0 - m_hover * 360.0 / ringBrushes - 180.0 / ringBrushes;
        QPainterPath path;
        path.moveTo(m_center);
        path.arcTo(QRectF(m_center - QPointF(plateRadius - 6, plateRadius - 6), QSizeF(2 * (plateRadius - 6), 2 * (plateRadius - 6))),
                   start, 360.0 / ringBrushes);
        path.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(wedge);
        painter.drawPath(path);
    }

    // the brushes
    for (int i = 0; i < m_slots.size(); i++) {
        const QPointF c = slotCenter(i);
        const bool hovered = i == m_hover;
        const qreal r = hovered ? slotRadius + 3 : slotRadius;
        QPainterPath circle;
        circle.addEllipse(c, r, r);
        painter.save();
        painter.setClipPath(circle);
        painter.fillPath(circle, QColor(244, 241, 234));
        const QImage &picture = m_slots[i].picture;
        if (!picture.isNull()) {
            // the middle of the stroke, filling the circle
            const qreal scale = (2 * r) / qMin(picture.width(), picture.height()) * 1.1;
            const QSizeF drawn = QSizeF(picture.size()) * scale;
            painter.drawImage(QRectF(c - QPointF(drawn.width(), drawn.height()) / 2, drawn), picture);
        }
        painter.restore();
        const bool current = m_slots[i].brush == m_currentBrush;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(hovered ? accent : current ? pal.color(QPalette::WindowText) : line, hovered ? 2.5 : current ? 1.5 : 1));
        painter.drawEllipse(c, r, r);
    }

    // the colors painted on the layer
    for (int i = 0; i < m_colors.size(); i++) {
        const QPointF c = colorCenter(i);
        const qreal r = 100 + i == m_hover ? colorRadius + 2 : colorRadius;
        painter.setPen(QPen(QColor(0, 0, 0, 90), 1));
        painter.setBrush(m_colors[i]);
        painter.drawEllipse(c, r, r);
        if (100 + i == m_hover) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(accent, 2));
            painter.drawEllipse(c, r + 2.5, r + 2.5);
        }
    }

    // the current color in the middle
    painter.setPen(QPen(QColor(0, 0, 0, 120), 1));
    painter.setBrush(m_current);
    painter.drawEllipse(m_center, middleRadius - 4, middleRadius - 4);
    if (m_hover == -2) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(accent, 2));
        painter.drawEllipse(m_center, middleRadius, middleRadius);
    }

    // the name of what is pointed at, under the middle
    QString name;
    if (m_hover >= 0 && m_hover < m_slots.size()) name = m_slots[m_hover].title;
    else if (m_hover == -2) name = i18nc("@info the middle of the canvas ring", "All brushes");
    if (!name.isEmpty()) {
        QFont font = this->font();
        font.setBold(true);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        const QRectF pill(m_center.x() - metrics.horizontalAdvance(name) / 2.0 - 10, m_center.y() + middleRadius + 10,
                          metrics.horizontalAdvance(name) + 20, metrics.height() + 8);
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        painter.drawPath(KisModernStyle::squircle(pill, pill.height() / 2));
        painter.setPen(pal.color(QPalette::HighlightedText));
        painter.drawText(pill, Qt::AlignCenter, name);
    }
}
