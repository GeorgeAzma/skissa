/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioKeyHints.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QAbstractButton>

#include <KisModernStyle.h>

#include "KisStudioItemCard.h"
#include "KisStudioTips.h"

namespace
{
class Hints : public QWidget
{
public:
    explicit Hints(QWidget *window)
        : QWidget(window)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setGeometry(window->rect());
        const QList<QAbstractButton *> buttons = window->findChildren<QAbstractButton *>();
        for (QAbstractButton *button : buttons) {
            if (!button->isVisible() || button->width() < 8) continue;
            const QKeySequence keys = KisStudioTips::shortcutOf(button);
            if (keys.isEmpty()) continue;
            const QRect rect(button->mapTo(window, QPoint()), button->size());
            if (!rect.intersects(window->rect())) continue;
            m_keys.append({rect, KisStudioShortcuts::badgeText(keys)});
        }
        show();
        raise();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        for (const auto &key : qAsConst(m_keys)) KisStudioShortcuts::paintBadge(painter, key.first, key.second, palette());
    }

private:
    QVector<QPair<QRect, QString>> m_keys;
};

class Watch : public QObject
{
public:
    QTimer held;
    QPointer<Hints> shown;

    Watch()
    {
        held.setSingleShot(true);
        held.setInterval(2 * KisModernStyle::holdInterval());
        connect(&held, &QTimer::timeout, this, [this]() {
            QWidget *window = QApplication::activeWindow();
            if (!window || QApplication::queryKeyboardModifiers() != Qt::AltModifier) return;
            shown = new Hints(window);
        });
    }

    bool eventFilter(QObject *, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::KeyPress: {
            auto *key = static_cast<QKeyEvent *>(event);
            if (key->isAutoRepeat()) break;
            if (key->key() == Qt::Key_Alt && key->modifiers() == Qt::AltModifier) {
                held.start();
                break;
            }
            hide();
            break;
        }
        case QEvent::KeyRelease:
            if (!static_cast<QKeyEvent *>(event)->isAutoRepeat()) hide();
            break;
        case QEvent::MouseButtonPress:
        case QEvent::TabletPress:
        case QEvent::Wheel:
        case QEvent::WindowDeactivate:
            hide();
            break;
        default:
            break;
        }
        return false;
    }

    void hide()
    {
        held.stop();
        delete shown;
    }
};
} // namespace

void KisStudioKeyHints::install()
{
    static Watch *watch = nullptr;
    if (watch) return;
    watch = new Watch;
    qApp->installEventFilter(watch);
}
