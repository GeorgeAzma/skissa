/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasRing.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTabletEvent>
#include <QVariantAnimation>
#include <QtMath>

#include <KoColor.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisStrokeLog.h"
#include "KisStudioBrushPicker.h"
#include "KisStudioFind.h"
#include "KisStudioGlass.h"
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
/// A brush pressed and moved this far: dragged to another slot
constexpr qreal dragStart = 6;

/// A brush's stroke filling a circle, its middle in the middle
void paintBrush(QPainter &painter, const QPointF &center, qreal radius, const QImage &picture, qreal opacity)
{
    QPainterPath circle;
    circle.addEllipse(center, radius, radius);
    painter.save();
    painter.setOpacity(opacity);
    painter.setClipPath(circle);
    painter.fillPath(circle, QColor(244, 241, 234));
    if (!picture.isNull()) {
        const qreal scale = (2 * radius) / qMin(picture.width(), picture.height()) * 1.1;
        const QSizeF drawn = QSizeF(picture.size()) * scale;
        painter.drawImage(QRectF(center - QPointF(drawn.width(), drawn.height()) / 2, drawn), picture);
    }
    painter.restore();
}

} // namespace

KisStudioCanvasRing::KisStudioCanvasRing(KisMainWindow *mainWindow)
    // in the window, not the canvas: over the panels and bars that lie over the canvas
    : QWidget(mainWindow)
    , m_mainWindow(mainWindow)
{
    KisStudioGlass::setOverOverlays(this);
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
    // a brush by its direction: anywhere in its eighth, also past the plate when flicking
    if (distance > plateRadius && !m_held) {
        return -1;
    }
    qreal angle = qRadiansToDegrees(qAtan2(delta.y(), delta.x())) + 90.0;
    while (angle < 0) angle += 360.0;
    const int index = int(qRound(angle / (360.0 / ringBrushes))) % ringBrushes;
    if (index < m_slots.size()) return index;
    // an empty slot is for adding one, by clicking it (a flick picks nothing there)
    return !m_held && QLineF(slotCenter(index), pos).length() <= slotRadius + 4 ? 200 + index : -1;
}

void KisStudioCanvasRing::open(QWidget *canvas, const QPointF &pos)
{
    KisViewManager *view = m_mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    if (!view || !resources) {
        return;
    }
    loadSlots();
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
    move(m_mainWindow->mapFromGlobal(canvas->mapToGlobal((at - QPointF(half.width(), half.height())).toPoint())));
    m_center = QPointF(half.width(), half.height());
    m_openedAt = m_center + (pos - at);

    m_canvas = canvas;
    m_eatRelease = Qt::NoButton;
    m_held = true;
    m_moved = false;
    m_hover = -1;
    m_pressed = -1;
    m_dragging = false;
    show();
    raise();
    KisStudioMotion::showFaded(this);
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

void KisStudioCanvasRing::loadSlots()
{
    m_slots.clear();
    Q_FOREACH (const QString &name, KisStudioBrushPicker::yourBrushes().mid(0, ringBrushes)) {
        Slot slot;
        slot.brush = name;
        slot.title = KisStudioBrushPicker::title(name);
        slot.picture = KisStudioBrushPicker::strokePicture(name);
        if (slot.picture.isNull()) {
            if (KisPaintOpPresetSP preset = KisResourceServerProvider::instance()->paintOpPresetServer()->resource(QString(), QString(), name)) {
                slot.picture = preset->image();
            }
        }
        m_slots << slot;
    }
}

void KisStudioCanvasRing::changeBrushes(const std::function<void(QStringList &brushes)> &change)
{
    QStringList brushes = KisStudioBrushPicker::yourBrushes();
    change(brushes);
    brushes.removeDuplicates();
    KisStudioBrushPicker::setYourBrushes(brushes);
    loadSlots();
    update();
}

void KisStudioCanvasRing::pickBrush(int index)
{
    KisStudioFind *find = m_mainWindow->studioFind();
    if (!find) return;
    close();
    QPointer<KisStudioCanvasRing> self(this);
    find->pick(i18nc("@info:placeholder", "Add to ring"), [self, index](const QString &key) {
        if (!self || !key.startsWith(QLatin1String("brush:"))) return;
        const QString name = key.mid(6);
        self->changeBrushes([index, &name](QStringList &brushes) {
            const int already = brushes.indexOf(name);
            if (index < brushes.size()) {
                // the one there before goes to where this one was, if it was yours
                if (already >= 0) brushes.swapItemsAt(index, already);
                else brushes[index] = name;
            } else if (already < 0) {
                brushes.append(name);
            }
        });
    }, QStringLiteral("brush:"));
}

void KisStudioCanvasRing::slotMenu(int index, const QPoint &global)
{
    QMenu menu(this);
    QAction *replace = menu.addAction(i18nc("@action:inmenu a brush of the canvas ring", "Replace…"));
    QAction *remove = menu.addAction(i18nc("@action:inmenu a brush of the canvas ring", "Remove"));
    QAction *chosen = menu.exec(global);
    if (chosen == replace) {
        pickBrush(index);
    } else if (chosen == remove) {
        changeBrushes([index](QStringList &brushes) {
            if (index < brushes.size()) brushes.removeAt(index);
        });
    }
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
    if (target >= 200) {
        pickBrush(target - 200);
    } else if (target >= 0 && target < m_slots.size()) {
        KisStudioBrushPicker::use(view, m_slots[target].brush);
    } else if (target >= 100 && target - 100 < m_colors.size()) {
        const KoColorSpace *space = view->image() ? view->image()->colorSpace() : resources->fgColor().colorSpace();
        resources->setFGColor(KoColor(m_colors[target - 100], space));
    } else if (target == -2) {
        auto *picker = new KisStudioBrushPicker(m_mainWindow);
        picker->popupNear(QCursor::pos());
    }
}

bool KisStudioCanvasRing::eventFilter(QObject *watched, QEvent *event)
{
    // every event of the application comes here: the rest goes on before anything is looked up (also while the
    // window is being destroyed, its view manager half gone)
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
    case QEvent::WindowDeactivate:
        if (watched == m_mainWindow && KisStudioMotion::isShown(this)) close();
        return false;
    default:
        return false;
    }
    const bool shown = KisStudioMotion::isShown(this);
    if (type == QEvent::KeyPress && shown && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        close();
        return true;
    }

    KisViewManager *view = m_mainWindow->viewManager();
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    QWidget *canvasWidget = canvas ? canvas->canvasWidget() : nullptr;
    if (shown && canvasWidget != m_canvas) {
        close();
        return false;
    }
    if (!canvasWidget) {
        return false;
    }

    const bool onCanvas = watched == canvasWidget;
    const bool onRing = watched == this;
    if (!onCanvas && !onRing) {
        // a press elsewhere in the window (a panel, a bar) closes it, and is that place's
        const bool anyPress = type == QEvent::MouseButtonPress || type == QEvent::MouseButtonDblClick || type == QEvent::TabletPress;
        if (shown && anyPress && watched->isWidgetType() && static_cast<QWidget *>(watched)->window() == m_mainWindow) close();
        return false;
    }

    QPointF global;
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
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
        buttons = mouse->buttons();
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
        buttons = tablet->buttons();
        modifiers = tablet->modifiers();
        press = type == QEvent::TabletPress;
        move = type == QEvent::TabletMove;
        release = type == QEvent::TabletRelease;
        break;
    }
    default:
        return false;
    }

    // the release of the press that closed it: not the canvas's either, as the press was not
    if (release && m_eatRelease != Qt::NoButton && button == m_eatRelease) {
        m_eatRelease = Qt::NoButton;
        event->accept();
        return true;
    }
    if (!shown) {
        // the second click of a double click on a brush (chosen by the first) is not a dab
        if (onCanvas && press && m_closedByClick.isValid() && m_closedByClick.elapsed() < QApplication::doubleClickInterval()) {
            m_closedByClick.invalidate();
            m_eatRelease = button;
            event->accept();
            return true;
        }
        // a right press with a painting tool opens it, not one while painting (another button down)
        if (onCanvas && press && button == Qt::RightButton && buttons == Qt::RightButton && modifiers == Qt::NoModifier
            && KisStudioBrushPicker::paintToolActive()) {
            open(canvasWidget, canvasWidget->mapFromGlobal(global.toPoint()));
            event->accept();
            return true;
        }
        return false;
    }

    const QPointF local = mapFromGlobal(global.toPoint());
    if (m_pressed >= 0 && (move || release)) {
        if (move) {
            if (!m_dragging && QLineF(local, m_pressedAt).length() > dragStart) m_dragging = true;
            m_dragAt = local;
            m_hover = targetAt(local);
            update();
        } else {
            const int from = m_pressed;
            const int to = targetAt(local);
            m_pressed = -1;
            if (!m_dragging) {
                m_closedByClick.start();
                choose(from);
            } else {
                m_dragging = false;
                // let go on another of yours: the two change places
                if (to >= 0 && to < m_slots.size() && to != from) {
                    changeBrushes([from, to](QStringList &brushes) {
                        if (qMax(from, to) < brushes.size()) brushes.swapItemsAt(from, to);
                    });
                }
                update();
            }
        }
        event->accept();
        return true;
    }
    if (move) {
        if (m_held && QLineF(local, m_openedAt).length() > flick) m_moved = true;
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
            // let go after moving towards a brush: taken, also past the plate (while held); without moving: open
            const int target = m_moved ? targetAt(local) : -1;
            m_held = false;
            if (m_moved) {
                if (target != -1) choose(target);
                else close();
            }
        }
        event->accept();
        return true;
    }
    if (press) {
        const int target = QLineF(local, m_center).length() <= plateRadius ? targetAt(local) : -1;
        if (button == Qt::LeftButton && target >= 0 && target < m_slots.size()) {
            // taken when let go, unless dragged to another slot
            m_pressed = target;
            m_pressedAt = local;
            m_dragAt = local;
        } else if (button == Qt::LeftButton && target != -1) {
            m_closedByClick.start();
            m_eatRelease = button;
            choose(target);
        } else if (button == Qt::RightButton && target >= 0 && target < m_slots.size()) {
            slotMenu(target, global.toPoint());
        } else {
            // elsewhere, or the right button again: closed, and that press (and its release) is not a stroke
            close();
            m_eatRelease = button;
        }
        event->accept();
        return true;
    }
    return false;
}

void KisStudioCanvasRing::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const QPalette pal = palette();
    QColor line = pal.color(QPalette::WindowText);
    line.setAlphaF(0.1);
    const QColor accent = pal.color(QPalette::Highlight);

    // glass with a soft shadow, as large as it has grown (a square with corners as round as half its side is a circle)
    const qreal plate = (plateRadius - 6) * m_scale;
    const QRectF plateRect(m_center - QPointF(plate, plate), QSizeF(2 * plate, 2 * plate));
    KisStudioGlass::paintShadow(painter, plateRect, plate, 8, 26, 3);
    painter.save();
    const QPainterPath plateShape = KisStudioGlass::paint(painter, this, plateRect, plate);
    painter.restore();
    KisStudioGlass::paintBorder(painter, this, plateShape, line);

    if (m_scale < 1.0) {
        painter.translate(m_center);
        painter.scale(m_scale, m_scale);
        painter.translate(-m_center);
    }

    // the direction being flicked towards
    if (m_hover >= 0 && m_hover < m_slots.size() && !m_dragging) {
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
        paintBrush(painter, c, r, m_slots[i].picture, m_dragging && i == m_pressed ? 0.35 : 1.0);
        const bool current = m_slots[i].brush == m_currentBrush;
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(hovered ? accent : current ? pal.color(QPalette::WindowText) : line, hovered ? 2.5 : current ? 1.5 : 1));
        painter.drawEllipse(c, r, r);
    }

    // the empty slots, to add a brush in
    for (int i = m_slots.size(); i < ringBrushes; i++) {
        const QPointF c = slotCenter(i);
        const bool hovered = 200 + i == m_hover;
        QColor faint = pal.color(QPalette::WindowText);
        faint.setAlphaF(hovered ? 0.6 : 0.25);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(faint, 1.2, Qt::DashLine));
        painter.drawEllipse(c, slotRadius - 4, slotRadius - 4);
        painter.setPen(QPen(faint, 1.6, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(c - QPointF(6, 0), c + QPointF(6, 0));
        painter.drawLine(c - QPointF(0, 6), c + QPointF(0, 6));
    }

    // a brush being dragged to another slot, under the pointer
    if (m_dragging && m_pressed >= 0 && m_pressed < m_slots.size()) {
        const qreal r = slotRadius - 4;
        paintBrush(painter, m_dragAt, r, m_slots[m_pressed].picture, 0.9);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(accent, 2));
        painter.drawEllipse(m_dragAt, r, r);
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
    else if (m_hover >= 200) name = i18nc("@info an empty slot of the canvas ring", "Add a brush");
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
