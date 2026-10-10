/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisUpdatePacer.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QHash>
#include <QPointer>
#include <QScreen>
#include <QWidget>
#include <QWindow>

namespace
{
/// A pixel of the window that is "changed" to have the window shown again: nothing is drawn
class Heartbeat : public QWidget
{
public:
    explicit Heartbeat(QWidget *window)
        : QWidget(window)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        // over the rest (under another widget an update could be dropped as hidden): it draws nothing
        setGeometry(0, 0, 1, 1);
        raise();
        show();
    }

protected:
    void paintEvent(QPaintEvent *) override {}
};

class Pacer : public QObject
{
public:
    explicit Pacer(QObject *parent)
        : QObject(parent)
    {
        m_sinceFrame.start();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // one move from the system is first a QWindow's: counted once, there
        if (event->type() != QEvent::MouseMove && event->type() != QEvent::TabletMove) {
            return false;
        }
        QWindow *window = qobject_cast<QWindow *>(watched);
        if (!window || m_busy) {
            return false;
        }
        const qreal rate = window->screen() ? window->screen()->refreshRate() : 60.0;
        const qint64 frame = qint64(1e9 / qBound(30.0, rate, 1000.0));
        if (m_sinceFrame.nsecsElapsed() < frame) {
            return false;
        }
        m_sinceFrame.restart();
        m_busy = true;
        /**
         * A frame for the screen, whether or not this move changes anything:
         * a screen with a variable refresh rate (G-Sync, FreeSync) shows the
         * pointer as often as the window shows frames. Over the canvas a move
         * redraws the brush outline, so it is smooth; over a panel only what
         * the move lights up is redrawn, rarely, and the screen would slow
         * down (the pointer stutters) and speed up again (some screens
         * flicker as they do).
         */
        if (QWidget *top = topWidget(window)) {
            QPointer<Heartbeat> &beat = m_beats[top];
            if (!beat) beat = new Heartbeat(top);
            beat->update();
        }
        // and what the moves before it asked to repaint, now: Qt 5 holds posted events back while input waits
        QCoreApplication::sendPostedEvents(nullptr, QEvent::UpdateRequest);
        m_busy = false;
        return false;
    }

private:
    static QWidget *topWidget(QWindow *window)
    {
        Q_FOREACH (QWidget *widget, QApplication::topLevelWidgets()) {
            if (widget->windowHandle() == window) {
                // only the windows the canvas is in: popups and dialogs show their own changes
                return widget->inherits("KisMainWindow") ? widget : nullptr;
            }
        }
        return nullptr;
    }

    QElapsedTimer m_sinceFrame;
    QHash<QWidget *, QPointer<Heartbeat>> m_beats;
    bool m_busy {false};
};
} // namespace

void KisUpdatePacer::install()
{
#ifdef Q_OS_WIN
    static Pacer *pacer = nullptr;
    if (pacer) return;
    pacer = new Pacer(qApp);
    qApp->installEventFilter(pacer);
#endif
}
