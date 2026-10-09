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

namespace {

const char *const fadingOutProperty = "studioFadingOut";

/// The fade running on a widget, so that a new one replaces it and starts where it was
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
        return;
    }
    auto *effect = qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());
    if (!effect) {
        effect = new QGraphicsOpacityEffect(widget);
        widget->setGraphicsEffect(effect);
    }
    effect->setOpacity(opacity);
}

/// Done: fully opaque again with nothing in between (an effect renders through a pixmap)
void settle(QWidget *widget)
{
    if (widget->isWindow()) {
        widget->setWindowOpacity(1.0);
    } else if (qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect())) {
        widget->setGraphicsEffect(nullptr);
    }
}

bool canFade(QWidget *widget)
{
    // another kind of effect is the widget's own: leave it alone
    return widget->isWindow() || !widget->graphicsEffect() || qobject_cast<QGraphicsOpacityEffect *>(widget->graphicsEffect());
}

void fade(QWidget *widget, qreal to, int duration, QEasingCurve::Type curve, bool hideAtEnd)
{
    delete runningFade(widget);
    const qreal from = currentOpacity(widget);

    auto *animation = new QVariantAnimation(widget);
    animation->setObjectName(QStringLiteral("studioFade"));
    animation->setStartValue(from);
    animation->setEndValue(to);
    // the time of the whole way, for the part of it that is left
    animation->setDuration(qMax(1, qRound(duration * qAbs(to - from))));
    animation->setEasingCurve(curve);
    QPointer<QWidget> target = widget;
    QObject::connect(animation, &QVariantAnimation::valueChanged, widget, [target](const QVariant &value) {
        if (target) setOpacity(target, value.toReal());
    });
    QObject::connect(animation, &QVariantAnimation::finished, widget, [target, hideAtEnd]() {
        if (!target) return;
        if (hideAtEnd) {
            target->hide();
            target->setProperty(fadingOutProperty, false);
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
    if (!canFade(widget)) {
        widget->show();
        return;
    }
    if (!widget->isVisible()) {
        setOpacity(widget, 0.0);
        widget->show();
    }
    fade(widget, 1.0, 110, QEasingCurve::OutCubic, false);
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
    fade(widget, 0.0, 80, QEasingCurve::InCubic, true);
}
