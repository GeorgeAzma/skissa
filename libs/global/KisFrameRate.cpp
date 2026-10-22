/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisFrameRate.h"

#include <QAnimationDriver>
#include <QBasicTimer>
#include <QGuiApplication>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QTimerEvent>

#include <atomic>

namespace
{
/// Ticks animations once per screen frame; Qt's default driver ticks every 16 ms.
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

/// The fastest screen's rate as last measured on the GUI thread: paint ops ask from the stroke threads,
/// where the screens can't be read while one is plugged in or out
std::atomic<double> measuredRate {0.0};

void measureRate()
{
    qreal rate = 0;
    Q_FOREACH (QScreen *screen, qGuiApp->screens()) {
        rate = qMax(rate, screen->refreshRate());
    }
    measuredRate = rate;
}

/// Starts keeping measuredRate current; on the GUI thread
void watchScreens()
{
    static bool watching = false;
    if (watching) return;
    watching = true;
    auto watch = [](QScreen *screen) { QObject::connect(screen, &QScreen::refreshRateChanged, qGuiApp, measureRate); };
    Q_FOREACH (QScreen *screen, qGuiApp->screens()) {
        watch(screen);
    }
    QObject::connect(qGuiApp, &QGuiApplication::screenAdded, qGuiApp, [watch](QScreen *screen) {
        watch(screen);
        measureRate();
    });
    // the screen is still listed while the signal is sent
    QObject::connect(qGuiApp, &QGuiApplication::screenRemoved, qGuiApp, []() { QTimer::singleShot(0, qGuiApp, measureRate); });
    measureRate();
}
} // namespace

qreal KisFrameRate::screenRate()
{
    if (qGuiApp && QThread::currentThread() == qGuiApp->thread()) watchScreens();
    const qreal rate = measuredRate;
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
    watchScreens();
    driver = new FrameDriver(qGuiApp);
    driver->install();
}
