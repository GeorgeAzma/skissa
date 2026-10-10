/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisFrameRate.h"

#include <QAnimationDriver>
#include <QBasicTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QTimerEvent>

namespace
{
/// Advances the animations on a timer of one screen frame (Qt's own driver ticks every 16 ms)
class FrameDriver : public QAnimationDriver
{
public:
    explicit FrameDriver(QObject *parent)
        : QAnimationDriver(parent)
    {
    }

protected:
    void start() override
    {
        QAnimationDriver::start();
        m_timer.start(KisFrameRate::frameMs(), Qt::PreciseTimer, this);
    }

    void stop() override
    {
        m_timer.stop();
        QAnimationDriver::stop();
    }

    void timerEvent(QTimerEvent *event) override
    {
        if (event->timerId() != m_timer.timerId()) {
            QAnimationDriver::timerEvent(event);
            return;
        }
        advance();
    }

private:
    QBasicTimer m_timer;
};
} // namespace

qreal KisFrameRate::screenRate()
{
    qreal rate = 0;
    if (qGuiApp) {
        Q_FOREACH (QScreen *screen, qGuiApp->screens()) {
            rate = qMax(rate, screen->refreshRate());
        }
    }
    return rate >= 24 ? rate : 60.0;
}

int KisFrameRate::frameMs()
{
    return qMax(1, int(1000.0 / screenRate()));
}

void KisFrameRate::installAnimationDriver()
{
    static FrameDriver *driver = nullptr;
    if (driver || !qGuiApp) return;
    driver = new FrameDriver(qGuiApp);
    driver->install();
}
