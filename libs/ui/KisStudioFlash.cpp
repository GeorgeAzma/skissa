/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioFlash.h"

#include <QElapsedTimer>
#include <QHash>
#include <QPainter>
#include <QPointer>
#include <QVariantAnimation>
#include <QWidget>

#include <KisModernStyle.h>

namespace
{

/// How long the light takes to fade
constexpr int fadeDuration = 700;
const char *const lightName = "studioFlashLight";

/// A light over a control, fading
class Light : public QWidget
{
public:
    explicit Light(QWidget *target)
        : QWidget(target)
    {
        setObjectName(QLatin1String(lightName));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(target->rect());
        auto *fade = new QVariantAnimation(this);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setDuration(fadeDuration);
        fade->setEasingCurve(KisModernStyle::motionCurve());
        connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_strength = value.toReal();
            update();
        });
        connect(fade, &QVariantAnimation::finished, this, &QObject::deleteLater);
        show();
        raise();
        fade->start();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QColor ink = palette().color(QPalette::WindowText);
        ink.setAlphaF(0.22 * m_strength);
        painter.setPen(Qt::NoPen);
        painter.setBrush(ink);
        painter.drawPath(KisModernStyle::squircle(QRectF(rect()), KisModernStyle::cornerRadius));
    }

private:
    qreal m_strength {1};
};

} // namespace

namespace KisStudioFlash
{

QWidget *shown(QWidget *from, const char *name)
{
    if (!from) return nullptr;
    QWidget *window = from->window();
    if (!window || !window->isVisible() || window->isMinimized()) return nullptr;
    // asked at every step of a rotation and every repeat of a key: the window's widgets of that name are searched
    // again only now and then
    struct Found {
        QVector<QPointer<QWidget>> widgets;
        QElapsedTimer age;
    };
    static QHash<QPair<QWidget *, QByteArray>, Found> found;
    Found &entry = found[qMakePair(window, QByteArray(name))];
    if (!entry.age.isValid() || entry.age.elapsed() > 1000) {
        entry.widgets.clear();
        Q_FOREACH (QWidget *candidate, window->findChildren<QWidget *>(QLatin1String(name))) entry.widgets << candidate;
        entry.age.start();
    }
    for (const QPointer<QWidget> &candidate : entry.widgets) {
        if (candidate && candidate->isVisible() && !candidate->rect().isEmpty()) return candidate;
    }
    return nullptr;
}

bool flash(QWidget *target)
{
    if (!target || !target->isVisible()) return false;
    // one light at a time: a key held down lights it again from full
    Q_FOREACH (QWidget *old, target->findChildren<QWidget *>(QLatin1String(lightName), Qt::FindDirectChildrenOnly)) old->deleteLater();
    new Light(target);
    return true;
}

bool flash(QWidget *from, const char *name)
{
    return flash(shown(from, name));
}

} // namespace KisStudioFlash
