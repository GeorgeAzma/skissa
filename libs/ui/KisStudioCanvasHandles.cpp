/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasHandles.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QTabletEvent>
#include <QTimer>

#include <kundo2stack.h>

#include "KisDocument.h"
#include "KisView.h"
#include "canvas/kis_canvas2.h"
#include "canvas/kis_canvas_decoration.h"
#include "canvas/kis_coordinates_converter.h"

namespace
{
const QString decorationId = QStringLiteral("studioCanvasHandles");
/// How near a press must be to take a handle, in screen pixels: hands aim roughly
constexpr qreal reach = 14;
constexpr qreal radius = 7;
constexpr qreal smallRadius = 5;

QPointer<KisStudioCanvasHandles> &current()
{
    static QPointer<KisStudioCanvasHandles> shown;
    return shown;
}

class HandlesDecoration : public KisCanvasDecoration
{
public:
    explicit HandlesDecoration(QPointer<KisView> view)
        : KisCanvasDecoration(decorationId, view)
    {
        setPriority(95);
        setVisible(true);
    }

protected:
    void drawDecoration(QPainter &gc, const QRectF &, const KisCoordinatesConverter *converter, KisCanvas2 *canvas) override
    {
        KisStudioCanvasHandles *handles = current();
        if (!handles || handles->canvas() != canvas) return;
        const QTransform toWidget = converter->imageToWidgetTransform();
        gc.save();
        gc.setRenderHint(QPainter::Antialiasing, true);
        if (handles->drawUnder) handles->drawUnder(gc, toWidget);
        for (const KisStudioCanvasHandles::Handle &handle : handles->handles()) {
            const QPointF at = toWidget.map(handle.at);
            const qreal r = handle.fill.isValid() ? radius : smallRadius;
            // a dark ring under a light one: seen on any picture
            gc.setPen(QPen(QColor(0, 0, 0, 120), 3.5));
            gc.setBrush(Qt::NoBrush);
            gc.drawEllipse(at, r, r);
            gc.setPen(QPen(Qt::white, 2));
            if (handle.soft) {
                // a dot that fades out to its rim
                QRadialGradient fade(at, r);
                fade.setColorAt(0, Qt::white);
                fade.setColorAt(1, QColor(255, 255, 255, 0));
                gc.setPen(Qt::NoPen);
                gc.setBrush(QColor(0, 0, 0, 90));
                gc.drawEllipse(at, r, r);
                gc.setBrush(fade);
            } else {
                gc.setBrush(handle.fill.isValid() ? QBrush(handle.fill) : QBrush(Qt::white));
            }
            gc.drawEllipse(at, r, r);
        }
        gc.restore();
    }
};

QPointF positionOf(QEvent *event)
{
    if (auto *tablet = dynamic_cast<QTabletEvent *>(event)) return tablet->posF();
    return static_cast<QMouseEvent *>(event)->localPos();
}

Qt::MouseButton buttonOf(QEvent *event)
{
    if (auto *tablet = dynamic_cast<QTabletEvent *>(event)) return tablet->button();
    return static_cast<QMouseEvent *>(event)->button();
}
}

struct KisStudioCanvasHandles::Private {
    QPointer<KisCanvas2> canvas;
    QPointer<KUndo2Stack> stack;
    int dragged {-1};
    /// Where in the handle the press took it, so it doesn't jump to the pointer
    QPointF grab;
    bool ending {false};
    bool penReleased {false};
};

KisStudioCanvasHandles::KisStudioCanvasHandles(KisCanvas2 *canvas)
    : QObject(qApp)
    , d(new Private)
{
    d->canvas = canvas;
    KisView *view = canvas->imageView();
    if (view && !canvas->decoration(decorationId)) canvas->addDecoration(new HandlesDecoration(view));
    if (view && view->document()) {
        d->stack = view->document()->undoStack();
        // after the step that changed the history has settled (a redo of the handles' own step arrives in two parts)
        connect(d->stack, &KUndo2Stack::indexChanged, this, [this]() {
            QTimer::singleShot(0, this, [this]() {
                if (alive && !alive()) end();
            });
        });
    }
    qApp->installEventFilter(this);
}

KisStudioCanvasHandles::~KisStudioCanvasHandles()
{
    delete d;
}

KisStudioCanvasHandles *KisStudioCanvasHandles::start(KisCanvas2 *canvas)
{
    endAll();
    if (!canvas) return nullptr;
    current() = new KisStudioCanvasHandles(canvas);
    return current();
}

void KisStudioCanvasHandles::endAll()
{
    if (current()) current()->end();
}

KisCanvas2 *KisStudioCanvasHandles::canvas() const
{
    return d->canvas;
}

void KisStudioCanvasHandles::setHandles(const QVector<Handle> &handles)
{
    m_handles = handles;
    repaint();
}

void KisStudioCanvasHandles::end()
{
    if (d->ending) return;
    d->ending = true;
    qApp->removeEventFilter(this);
    if (current() == this) current() = nullptr;
    repaint();
    if (ended) ended();
    deleteLater();
}

void KisStudioCanvasHandles::repaint()
{
    if (d->canvas) d->canvas->updateCanvas();
}

int KisStudioCanvasHandles::handleAt(const QPointF &widgetPos) const
{
    if (!d->canvas) return -1;
    const QTransform toWidget = d->canvas->coordinatesConverter()->imageToWidgetTransform();
    int best = -1;
    qreal nearest = reach;
    // the last ones are drawn on top: they win a tie
    for (int i = 0; i < m_handles.size(); i++) {
        const qreal distance = QLineF(toWidget.map(m_handles[i].at), widgetPos).length();
        if (distance <= nearest) {
            nearest = distance;
            best = i;
        }
    }
    return best;
}

bool KisStudioCanvasHandles::eventFilter(QObject *watched, QEvent *event)
{
    if (!d->canvas) {
        end();
        return false;
    }
    QWidget *widget = d->canvas->canvasWidget();
    const QEvent::Type type = event->type();
    if (type == QEvent::KeyPress && widget && QApplication::activeWindow() == widget->window()) {
        const int key = static_cast<QKeyEvent *>(event)->key();
        // done: what was made stays as it is
        if (key == Qt::Key_Escape || key == Qt::Key_Return || key == Qt::Key_Enter) {
            QWidget *focus = QApplication::focusWidget();
            if (!focus || focus->window() == widget->window()) {
                // the key goes on: a fill's own panel takes Esc as taking the fill back
                end();
                return false;
            }
        }
        return false;
    }
    if (watched != widget) return false;
    // a pen's press arrives as a tablet event and again as a mouse event made from it
    auto *mouse = dynamic_cast<QMouseEvent *>(event);
    const bool synthesized = mouse && mouse->source() != Qt::MouseEventNotSynthesized;
    switch (type) {
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress: {
        if (buttonOf(event) != Qt::LeftButton) return false;
        if (synthesized && d->dragged >= 0) return true;
        const int index = handleAt(positionOf(event));
        if (index < 0) {
            // something else is being done: the handles go, the press goes on to the tool
            if (!synthesized) end();
            return false;
        }
        d->dragged = index;
        const QTransform toImage = d->canvas->coordinatesConverter()->imageToWidgetTransform().inverted();
        d->grab = m_handles[index].at - toImage.map(positionOf(event));
        event->accept();
        return true;
    }
    case QEvent::MouseMove:
    case QEvent::TabletMove: {
        if (d->dragged < 0) return false;
        if (synthesized) return true;
        const QTransform toImage = d->canvas->coordinatesConverter()->imageToWidgetTransform().inverted();
        const QPointF at = toImage.map(positionOf(event)) + d->grab;
        if (moved) moved(d->dragged, at, static_cast<QInputEvent *>(event)->modifiers());
        repaint();
        event->accept();
        return true;
    }
    case QEvent::MouseButtonRelease:
    case QEvent::TabletRelease: {
        if (d->dragged < 0) {
            // the mouse release made from the pen's, whose drag already ended
            const bool echo = synthesized && d->penReleased;
            d->penReleased = false;
            return echo;
        }
        d->penReleased = type == QEvent::TabletRelease;
        const int index = d->dragged;
        d->dragged = -1;
        if (released) released(index);
        event->accept();
        return true;
    }
    default:
        return false;
    }
}
