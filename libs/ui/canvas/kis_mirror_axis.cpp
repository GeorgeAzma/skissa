/*
 * SPDX-FileCopyrightText: 2014 Arjen Hiemstra <ahiemstra@heimr.nl>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 */

#include "kis_mirror_axis.h"

#include "KoConfig.h"

#include <QPainter>
#include <QApplication>
#include <QPaintEngine>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QAction>

#include <kis_icon.h>

#include "kis_canvas2.h"
#include "kis_canvas_resource_provider.h"
#include "KisViewManager.h"
#include "KisView.h"
#include "kis_image.h"
#include "canvas/kis_canvas_controller.h"
#include "input/kis_input_manager.h"
#include "kis_algebra_2d.h"

#include <KisMirrorAxisConfig.h>
#include <kis_signals_blocker.h>
#include <kactioncollection.h>
#include "kis_floating_message.h"
#include <KoColorDisplayRendererInterface.h>

#ifdef Q_OS_MACOS
    // HACK alert
    // macOS.SDK openGL does not define GL_MULTISAMPLE_EXT
    #define GL_MULTISAMPLE_EXT GL_MULTISAMPLE
#endif

class KisMirrorAxis::Private
{
public:
    Private(KisMirrorAxis* qq)
        : q(qq)
        , resourceProvider(0)
        , xActive(false)
        , yActive(false)
        , sideMargin(10.f)
        , minHandlePosition(10.f + 32.f)
        , horizontalContainsCursor(false)
        , verticalContainsCursor(false)
        , horizontalAxis(QLineF())
        , verticalAxis(QLineF())
        , config(KisMirrorAxisConfig())
    { }

    void setAxisPosition(float x, float y);
    void recomputeVisibleAxes(QRect viewport);

    KisMirrorAxis* q;

    KisCanvasResourceProvider* resourceProvider;
    KisImageWSP image;


    QRectF horizontalHandle;
    QRectF verticalHandle;
    bool xActive;
    bool yActive;

    float sideMargin;
    float minHandlePosition;
    bool horizontalContainsCursor;
    bool verticalContainsCursor;

    QLineF horizontalAxis;
    QLineF verticalAxis;

    KisMirrorAxisConfig config;
};

KisMirrorAxis::KisMirrorAxis(KisCanvasResourceProvider* provider, QPointer<KisView>parent)
    : KisCanvasDecoration("mirror_axis", parent)
    , d(new Private(this))
{
    d->resourceProvider = provider;
    connect(d->resourceProvider, SIGNAL(mirrorModeChanged()), SLOT(mirrorModeChanged()));
    connect(d->resourceProvider, SIGNAL(moveMirrorVerticalCenter()), SLOT(moveVerticalAxisToCenter()));
    connect(d->resourceProvider, SIGNAL(moveMirrorHorizontalCenter()), SLOT(moveHorizontalAxisToCenter()));

    d->config.setMirrorHorizontal(d->resourceProvider->mirrorHorizontal());
    d->config.setMirrorVertical(d->resourceProvider->mirrorVertical());
    setVisible(d->config.mirrorHorizontal() || d->config.mirrorVertical());

    d->image = parent->canvasBase()->image();
}

KisMirrorAxis::~KisMirrorAxis()
{
}

float KisMirrorAxis::handleSize() const
{
    return d->config.handleSize();
}

void KisMirrorAxis::setHandleSize(float newSize)
{
    if(d->config.handleSize() != newSize) {
        d->config.setHandleSize(newSize);
        d->minHandlePosition = d->sideMargin + newSize;
        Q_EMIT handleSizeChanged();
        Q_EMIT sigConfigChanged();
    }
}

void KisMirrorAxis::drawDecoration(QPainter& gc, const QRectF& updateArea, const KisCoordinatesConverter* converter, KisCanvas2* canvas)
{
    Q_UNUSED(updateArea);
    Q_UNUSED(converter);
    Q_UNUSED(canvas);

    if (!view()->isCurrent()) {
        return;
    }

    gc.save();

    auto display = [canvas](const QColor &color) {
        KoColor c;
        c.fromQColor(color);
        return canvas->displayRendererInterface()->convertColorToDisplayColorSpace(c);
    };
    // a thin line over a soft dark one: seen on light and dark paint alike, without hiding it
    QPen halo(display(QColor(0, 0, 0, 70)), 3 * decorationThickness(), Qt::SolidLine, Qt::RoundCap);
    halo.setCosmetic(true);
    QPen line(display(QColor(255, 255, 255, 190)), decorationThickness(), Qt::SolidLine, Qt::RoundCap);
    line.setCosmetic(true);
    gc.setRenderHints(QPainter::Antialiasing | QPainter::SmoothPixmapTransform);

    QOpenGLContext *ctx = QOpenGLContext::currentContext();
    bool hasMultisample = false;
    if (ctx) {
        hasMultisample = ((gc.paintEngine()->type() == QPaintEngine::OpenGL2)
                          && (ctx->hasExtension("GL_ARB_multisample") || ctx->hasExtension("GL_EXT_multisample_compatibility")));

        // QPainter cannot anti-alias the edges of circles etc. when using OpenGL
        // So instead, use native OpenGL anti-aliasing when available.
        if (hasMultisample) {
            gc.beginNativePainting();
            ctx->functions()->glEnable(GL_MULTISAMPLE_EXT);
            gc.endNativePainting();
        }
    }

    // the handle is grabbed over all of handleSize, drawn as a small dot on the line
    const float halfHandleSize = d->config.handleSize() / 2;
    const qreal dotRadius = 5.5 * decorationThickness();
    auto drawAxis = [&](const QLineF &axis, const QRectF &handle, bool locked) {
        gc.setPen(halo);
        gc.drawLine(axis);
        gc.setPen(line);
        gc.drawLine(axis);
        if (locked) return;
        gc.setPen(QPen(display(QColor(0, 0, 0, 150)), 1.5 * decorationThickness()));
        gc.setBrush(display(QColor(255, 255, 255, 235)));
        gc.drawEllipse(handle.center(), dotRadius, dotRadius);
    };

    const qreal dpr = canvas->canvasWidget()->devicePixelRatioF();
    d->recomputeVisibleAxes(QRect(gc.viewport().topLeft() / dpr, gc.viewport().size() / dpr));

    if(d->config.mirrorHorizontal() && !d->config.hideHorizontalDecoration()) {
        if (!d->horizontalAxis.isNull()) {
            float horizontalHandlePosition = qBound<float>(d->minHandlePosition, d->config.horizontalHandlePosition(), d->horizontalAxis.length() - d->minHandlePosition);
            QPointF horizontalHandleCenter = d->horizontalAxis.unitVector().pointAt(horizontalHandlePosition);
            d->horizontalHandle = QRectF(horizontalHandleCenter.x() - halfHandleSize, horizontalHandleCenter.y() - halfHandleSize, d->config.handleSize(), d->config.handleSize());
            drawAxis(d->horizontalAxis, d->horizontalHandle, d->config.lockHorizontal());
        } else {
            d->horizontalHandle = QRectF();
        }
    }

    if(d->config.mirrorVertical() && !d->config.hideVerticalDecoration()) {
        if (!d->verticalAxis.isNull()) {
            float verticalHandlePosition = qBound<float>(d->minHandlePosition, d->config.verticalHandlePosition(), d->verticalAxis.length() - d->minHandlePosition);
            QPointF verticalHandleCenter = d->verticalAxis.unitVector().pointAt(verticalHandlePosition);
            d->verticalHandle = QRectF(verticalHandleCenter.x() - halfHandleSize, verticalHandleCenter.y() - halfHandleSize, d->config.handleSize(), d->config.handleSize());
            drawAxis(d->verticalAxis, d->verticalHandle, d->config.lockVertical());
        } else {
            d->verticalHandle = QRectF();
        }
    }

    if (hasMultisample) {
        gc.beginNativePainting();
        ctx->functions()->glDisable(GL_MULTISAMPLE_EXT);
        gc.endNativePainting();
    }

    gc.restore();

}

static KoPointerEvent *getKoPointerEvent(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseMove:
    case QEvent::MouseButtonRelease: {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        return new KoPointerEvent(me, me->pos());
    }
    case QEvent::TabletPress:
    case QEvent::TabletMove:
    case QEvent::TabletRelease: {
        QTabletEvent *te = static_cast<QTabletEvent *>(event);
        return new KoPointerEvent(te, te->pos());
    }
    case QEvent::TouchBegin:
    case QEvent::TouchUpdate:
    case QEvent::TouchEnd:
    case QEvent::TouchCancel: {
        QTouchEvent *te = static_cast<QTouchEvent *>(event);
        return new KoPointerEvent(te, te->touchPoints().at(0).pos());
    }
    default:
        return nullptr;
    }
}

bool KisMirrorAxis::eventFilter(QObject* target, QEvent* event)
{
    if (!visible()) return false;

    QObject *expectedCanvasWidget = view() ?
                view()->canvasBase()->canvasWidget() : 0;

    if (!expectedCanvasWidget || target != expectedCanvasWidget) return false;

    if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::TabletPress
        || event->type() == QEvent::TouchBegin) {
        const QScopedPointer<KoPointerEvent> pointerEvent(getKoPointerEvent(event));
        const QPoint pos = !pointerEvent.isNull() ? pointerEvent->pos() : QPoint(77,77);

        if(d->config.mirrorHorizontal() && d->horizontalHandle.contains(pos) && !d->config.lockHorizontal() && !d->config.hideHorizontalDecoration() ) {
            d->xActive = true;
            QApplication::setOverrideCursor(Qt::ClosedHandCursor);
            event->accept();
            return true;
        }

        if(d->config.mirrorVertical() && d->verticalHandle.contains(pos) && !d->config.lockVertical() && !d->config.hideVerticalDecoration()) {
            d->yActive = true;
            QApplication::setOverrideCursor(Qt::ClosedHandCursor);
            event->accept();
            return true;
        }
    }
    if (event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove
        || event->type() == QEvent::TouchUpdate) {
        const QScopedPointer<KoPointerEvent> pointerEvent(getKoPointerEvent(event));
        const QPoint pos = !pointerEvent.isNull() ? pointerEvent->pos() : QPoint(77,77);

        if(d->xActive) {
            float axisX = view()->viewConverter()->widgetToImage<QPoint>(pos).x();
            // axisX should be either int or int + 0.5
            axisX *= 2; // to be able to choose in the middle of the pixel
            axisX = round(axisX); // find the closest acceptable point
            axisX = axisX/2; // return to the original space


            d->setAxisPosition(axisX, d->config.axisPosition().y());
            d->config.setHorizontalHandlePosition(KisAlgebra2D::dotProduct<QPointF>(pos - d->horizontalAxis.p1(), d->horizontalAxis.unitVector().p2() - d->horizontalAxis.p1()));
            Q_EMIT sigConfigChanged();

            event->accept();
            view()->showFloatingMessage(i18n("X: %1 px",QString::number(d->config.axisPosition().x(), 'f', 1))
                                        , QIcon(), 1000, KisFloatingMessage::High,  Qt::AlignLeft | Qt::TextWordWrap | Qt::AlignVCenter);
            return true;
        }
        if(d->yActive) {
            float axisY = view()->viewConverter()->widgetToImage<QPoint>(pos).y();
            // axisX should be either int or int + 0.5
            axisY *= 2; // to be able to choose in the middle of the pixel
            axisY = round(axisY); // find the closest acceptable point
            axisY = axisY/2; // return to the original space

            d->setAxisPosition(d->config.axisPosition().x(), axisY);
            d->config.setVerticalHandlePosition(KisAlgebra2D::dotProduct<QPointF>(pos - d->verticalAxis.p1(), d->verticalAxis.unitVector().p2() - d->verticalAxis.p1()));
            Q_EMIT sigConfigChanged();

            event->accept();
            view()->showFloatingMessage(i18n("Y: %1 px",QString::number(d->config.axisPosition().y(), 'f', 1))
                                        , QIcon(), 1000, KisFloatingMessage::High, Qt::AlignLeft | Qt::TextWordWrap | Qt::AlignVCenter);
            return true;
        }
        if(d->config.mirrorHorizontal() && !d->config.hideHorizontalDecoration()) {
            if(d->horizontalHandle.contains(pos) && !d->config.lockHorizontal()) {
                if(!d->horizontalContainsCursor) {
                    QApplication::setOverrideCursor(Qt::OpenHandCursor);
                    d->horizontalContainsCursor = true;
                }
            } else if(d->horizontalContainsCursor) {
                QApplication::restoreOverrideCursor();
                d->horizontalContainsCursor = false;
            }
        }
        if(d->config.mirrorVertical() && !d->config.hideVerticalDecoration()) {
            if(d->verticalHandle.contains(pos) && !d->config.lockVertical()) {
                if(!d->verticalContainsCursor) {
                    QApplication::setOverrideCursor(Qt::OpenHandCursor);
                    d->verticalContainsCursor = true;
                }
            } else if(d->verticalContainsCursor) {
                QApplication::restoreOverrideCursor();
                d->verticalContainsCursor = false;
            }
        }
    }
    if (event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::TabletRelease
        || event->type() == QEvent::TouchEnd || event->type() == QEvent::TouchCancel) {

        if(d->xActive) {
            while (QApplication::overrideCursor()) {
                QApplication::restoreOverrideCursor();
            }
            d->xActive = false;
            event->accept();
            return true;
        }
        if(d->yActive) {
            while (QApplication::overrideCursor()) {
                QApplication::restoreOverrideCursor();
            }
            d->yActive = false;
            event->accept();
            return true;
        }
    }

    return QObject::eventFilter(target, event);
}

void KisMirrorAxis::mirrorModeChanged()
{
    if (!view()->isCurrent()) {
        return;
    }

    d->config.setMirrorHorizontal(d->resourceProvider->mirrorHorizontal());
    d->config.setMirrorVertical(d->resourceProvider->mirrorVertical());

    d->config.setLockHorizontal(d->resourceProvider->mirrorHorizontalLock());
    d->config.setLockVertical(d->resourceProvider->mirrorVerticalLock());

    d->config.setHideHorizontalDecoration(d->resourceProvider->mirrorHorizontalHideDecorations());
    d->config.setHideVerticalDecoration(d->resourceProvider->mirrorVerticalHideDecorations());

    setVisible(d->config.mirrorHorizontal() || d->config.mirrorVertical());

    Q_EMIT sigConfigChanged();
}

void KisMirrorAxis::setVisible(bool v)
{
    KisCanvasDecoration::setVisible(v);


    KisInputManager *inputManager = view() ? view()->canvasBase()->globalInputManager() : 0;
    if (!inputManager) return;

    if (v) {
        inputManager->attachPriorityEventFilter(this);
    } else {
        inputManager->detachPriorityEventFilter(this);
    }
}

void KisMirrorAxis::setMirrorAxisConfig(const KisMirrorAxisConfig &config)
{
    if (config != d->config) {
        KisSignalsBlocker blocker(d->resourceProvider);

        d->config = config;

        d->resourceProvider->setMirrorHorizontal(d->config.mirrorHorizontal());
        d->resourceProvider->setMirrorVertical(d->config.mirrorVertical());

        d->resourceProvider->setMirrorHorizontalLock(d->config.lockHorizontal());
        d->resourceProvider->setMirrorVerticalLock(d->config.lockVertical());

        d->resourceProvider->setMirrorHorizontalHideDecorations(d->config.hideHorizontalDecoration());
        d->resourceProvider->setMirrorVerticalHideDecorations(d->config.hideVerticalDecoration());

        if (view()) {
            view()->canvasBase()->updateCanvas();
        }
    }

    toggleMirrorActions();
    setVisible(d->config.mirrorHorizontal() || d->config.mirrorVertical());
}

const KisMirrorAxisConfig &KisMirrorAxis::mirrorAxisConfig() const
{
    return d->config;
}

void KisMirrorAxis::toggleMirrorActions()
{
    KisKActionCollection* collection = view()->viewManager()->actionCollection();
    // first uncheck the action, then set according to config;
    // otherwise the connected KisHighlightedToolButton's highlight color is not
    // properly set
    collection->action("hmirror_action")->setChecked(false);
    collection->action("vmirror_action")->setChecked(false);

    if (d->config.mirrorHorizontal()) {
        collection->action("hmirror_action")->setChecked(d->config.mirrorHorizontal());
    }

    if (d->config.mirrorVertical()) {
        collection->action("vmirror_action")->setChecked(d->config.mirrorVertical());
    }

    collection->action("mirrorX-lock")->setChecked(d->config.lockHorizontal());
    collection->action("mirrorY-lock")->setChecked(d->config.lockVertical());

    collection->action("mirrorX-hideDecorations")->setChecked(d->config.hideHorizontalDecoration());
    collection->action("mirrorY-hideDecorations")->setChecked(d->config.hideVerticalDecoration());
}

void KisMirrorAxis::moveHorizontalAxisToCenter()
{
    if (!view()->isCurrent()) {
        return;
    }

    d->setAxisPosition(d->image->width()/2, d->config.axisPosition().y());
    Q_EMIT sigConfigChanged();
}

void KisMirrorAxis::moveVerticalAxisToCenter()
{
    if (!view()->isCurrent()) {
        return;
    }

    d->setAxisPosition(d->config.axisPosition().x(), d->image->height()/2 );
    Q_EMIT sigConfigChanged();
}


void KisMirrorAxis::Private::setAxisPosition(float x, float y)
{
    QPointF newPosition = QPointF(x, y);

    config.setAxisPosition(newPosition);

    q->view()->canvasBase()->updateCanvas();
}


void KisMirrorAxis::Private::recomputeVisibleAxes(QRect viewport)
{
    KisCoordinatesConverter *converter = q->view()->viewConverter();

    QPointF samplePt1 = converter->imageToWidget<QPointF>(QPointF(config.axisPosition().x(), 0));
    QPointF samplePt2 = converter->imageToWidget<QPointF>(QPointF(config.axisPosition().x(), 100));

    horizontalAxis = QLineF(samplePt1, samplePt2);
    if (!KisAlgebra2D::intersectLineRect(horizontalAxis, viewport, true)) horizontalAxis = QLineF();

    samplePt1 = converter->imageToWidget<QPointF>(QPointF(0, config.axisPosition().y()));
    samplePt2 = converter->imageToWidget<QPointF>(QPointF(100, config.axisPosition().y()));
    verticalAxis = QLineF(samplePt1, samplePt2);
    if (!KisAlgebra2D::intersectLineRect(verticalAxis, viewport, true)) verticalAxis = QLineF();
}

