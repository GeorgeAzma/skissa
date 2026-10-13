/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioMotion.h"

#include <QGraphicsOpacityEffect>
#include <QPointer>
#include <QVariantAnimation>
#include <QWidget>

#include "KisStudioGlass.h"
#include <KisModernStyle.h>

namespace {

const char *const fadingOutProperty = "studioFadingOut";
const char *const clickThroughProperty = "studioFadeClickThrough";

/// Lets clicks pass through a child widget while it fades out; windows are left to the system.
void setClickThrough(QWidget *widget, bool on)
{
    if (widget->isWindow()) return;
    if (on && !widget->testAttribute(Qt::WA_TransparentForMouseEvents)) {
        widget->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        widget->setProperty(clickThroughProperty, true);
    } else if (!on && widget->property(clickThroughProperty).toBool()) {
        widget->setAttribute(Qt::WA_TransparentForMouseEvents, false);
        widget->setProperty(clickThroughProperty, false);
    }
}

/// The fade running on \p widget, so a new fade can replace it and start from where it was
QVariantAnimation *runningFade(QWidget *widget)
{
    return widget->findChild<QVariantAnimation *>(QStringLiteral("studioFade"), Qt::FindDirectChildrenOnly);
}

qreal currentOpacity(QWidget *widget)
{
    if (widget->isWindow()) {
        return widget->windowOpacity();
    }
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());
    return effect ? effect->opacity() : 1.0;
}

void setOpacity(QWidget *widget, qreal opacity)
{
    if (widget->isWindow()) {
        widget->setWindowOpacity(opacity);
        KisStudioGlass::changed(widget);
        return;
    }
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(widget);
        widget->setGraphicsEffect(effect);
    }
    effect->setOpacity(opacity);
    // the glass backdrop fades with the widget
    KisStudioGlass::changed(widget);
}

/// Ends a fade at full opacity and drops the effect, which would keep rendering through a pixmap
void settle(QWidget *widget)
{
    if (widget->isWindow()) {
        widget->setWindowOpacity(1.0);
    } else if (qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect())) {
        widget->setGraphicsEffect(nullptr);
    }
    KisStudioGlass::changed(widget);
}

bool canFade(QWidget *widget)
{
    // leave widgets with some other graphics effect alone
    return widget->isWindow() || !widget->graphicsEffect() || qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());
}

void fade(QWidget *widget, qreal to, int duration, bool hideAtEnd)
{
    delete runningFade(widget);
    const qreal from = currentOpacity(widget);

    auto *animation = new QVariantAnimation(widget);
    animation->setObjectName(QStringLiteral("studioFade"));
    animation->setStartValue(from);
    animation->setEndValue(to);
    // scale the duration by the distance left, so an interrupted fade keeps its speed
    animation->setDuration(qMax(1, qRound(duration * qAbs(to - from))));
    animation->setEasingCurve(KisModernStyle::motionCurve());
    QPointer<QWidget> target = widget;
    QObject::connect(animation, &QVariantAnimation::valueChanged, widget, [target](const QVariant &value) {
        if (target) setOpacity(target, value.toReal());
    });
    QObject::connect(animation, &QVariantAnimation::finished, widget, [target, hideAtEnd]() {
        if (!target) return;
        if (hideAtEnd) {
            target->hide();
            target->setProperty(fadingOutProperty, false);
            setClickThrough(target, false);
        }
        settle(target);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
}

} // namespace

void KisStudioMotion::showFaded(QWidget *widget)
{
    if (!widget) {
        return;
    }
    const bool fadingOut = widget->property(fadingOutProperty).toBool();
    if (widget->isVisible() && !fadingOut) {
        return;
    }
    widget->setProperty(fadingOutProperty, false);
    setClickThrough(widget, false);
    if (!canFade(widget)) {
        widget->show();
        return;
    }
    const bool appearing = !widget->isVisible();
    if (appearing) {
        setOpacity(widget, 0.0);
        widget->show();
    }
    // fade only, no movement: moving glass would repaint everything under it every frame
    fade(widget, 1.0, KisModernStyle::enterDuration, false);
}

void KisStudioMotion::hideFaded(QWidget *widget)
{
    if (!widget || widget->isHidden() || widget->property(fadingOutProperty).toBool()) {
        return;
    }
    if (!canFade(widget)) {
        widget->hide();
        return;
    }
    widget->setProperty(fadingOutProperty, true);
    setClickThrough(widget, true);
    fade(widget, 0.0, KisModernStyle::exitDuration, true);
}

bool KisStudioMotion::isShown(const QWidget *widget)
{
    return widget && widget->isVisible() && !widget->property(fadingOutProperty).toBool();
}
