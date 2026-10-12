/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "KisUpdatePacer.h"

#include "KisMainWindow.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QFile>
#include <QHash>
#include <QPaintEvent>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <QWindow>

#include <atomic>
#include <cmath>
#include <functional>
#include <thread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace
{
/// A pixel that is "changed" to make the window present a frame; it draws nothing
class Heartbeat : public QWidget
{
public:
    explicit Heartbeat(QWidget *window)
        : QWidget(window)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
        // on top: under another widget its update could be dropped as hidden
        setGeometry(0, 0, 1, 1);
        raise();
        show();
    }

protected:
    void paintEvent(QPaintEvent *) override {}
};

#ifdef Q_OS_WIN
/**
 * An invisible (alpha 1/255) pixel the compositor must blend, so it always composes the window. Otherwise
 * a fullscreen window gets independent flip, and VRR screens then refresh only as Krita presents
 * frames, flickering as the rate changes.
 */
class Composed : public QObject
{
public:
    explicit Composed(QWidget *owner)
        : QObject(owner)
    {
        static const ATOM windowClass = []() {
            WNDCLASSW c = {};
            c.lpfnWndProc = DefWindowProcW;
            c.hInstance = GetModuleHandleW(nullptr);
            c.hbrBackground = HBRUSH(GetStockObject(BLACK_BRUSH));
            c.lpszClassName = L"KritaComposed";
            return RegisterClassW(&c);
        }();
        Q_UNUSED(windowClass);
        // owned by the window: above it, minimized and destroyed with it; never clicked or focused
        m_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"KritaComposed", L"", WS_POPUP, 0, 0, 1, 1,
                                   HWND(owner->winId()), nullptr, GetModuleHandleW(nullptr), nullptr);
        // not 0: a fully transparent window is skipped by the compositor
        SetLayeredWindowAttributes(m_window, 0, 1, LWA_ALPHA);
    }

    ~Composed() override
    {
        // destroyed already if its owner went first
        if (IsWindow(m_window)) DestroyWindow(m_window);
    }

    /// At the window's client corner (a maximized window's frame is off screen)
    void place()
    {
        POINT corner = {0, 0};
        ClientToScreen(HWND(static_cast<QWidget *>(parent())->winId()), &corner);
        SetWindowPos(m_window, nullptr, corner.x, corner.y, 1, 1, SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    }

private:
    HWND m_window {nullptr};
};

/**
 * A sub-millisecond timer calling back on the GUI thread. Qt's timers often fall to Windows' 15.6 ms
 * steps; this one waits on a high resolution timer in its own thread and posts a window message,
 * which Windows delivers before input (Qt 5 holds its own posted events back while input waits).
 */
class FrameTimer
{
public:
    explicit FrameTimer(std::function<void()> fired)
        : m_fired(std::move(fired))
    {
        static const UINT message = RegisterWindowMessageW(L"KritaFrameClock");
        m_message = message;
        static const ATOM windowClass = []() {
            WNDCLASSW c = {};
            c.lpfnWndProc = &FrameTimer::windowProc;
            c.hInstance = GetModuleHandleW(nullptr);
            c.lpszClassName = L"KritaFrameClock";
            return RegisterClassW(&c);
        }();
        Q_UNUSED(windowClass);
        m_window = CreateWindowExW(0, L"KritaFrameClock", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowLongPtrW(m_window, GWLP_USERDATA, LONG_PTR(this));
        // high resolution from Windows 10 1803
        m_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!m_timer) m_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        m_arm = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        m_thread = std::thread([this]() { run(); });
    }

    ~FrameTimer()
    {
        m_quit = true;
        SetEvent(m_arm);
        LARGE_INTEGER now;
        now.QuadPart = -1;
        SetWaitableTimer(m_timer, &now, 0, nullptr, nullptr, FALSE);
        m_thread.join();
        CloseHandle(m_timer);
        CloseHandle(m_arm);
        DestroyWindow(m_window);
    }

    /// Calls back in \p ns nanoseconds (0: as soon as the GUI thread is free)
    void start(qint64 ns)
    {
        m_active = true;
        if (ns <= 0) {
            PostMessageW(m_window, m_message, 0, 0);
            return;
        }
        m_delay = ns;
        SetEvent(m_arm);
    }

    bool isActive() const
    {
        return m_active;
    }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        auto *self = reinterpret_cast<FrameTimer *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == self->m_message) {
            self->m_active = false;
            self->m_fired();
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    void run()
    {
        while (true) {
            WaitForSingleObject(m_arm, INFINITE);
            if (m_quit) return;
            LARGE_INTEGER due;
            // relative, in 100 ns units
            due.QuadPart = -qMax<qint64>(1, m_delay / 100);
            SetWaitableTimer(m_timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(m_timer, INFINITE);
            if (m_quit) return;
            PostMessageW(m_window, m_message, 0, 0);
        }
    }

    std::function<void()> m_fired;
    UINT m_message {0};
    HWND m_window {nullptr};
    HANDLE m_timer {nullptr};
    HANDLE m_arm {nullptr};
    std::thread m_thread;
    std::atomic<qint64> m_delay {0};
    std::atomic<bool> m_quit {false};
    bool m_active {false};
};

/// The development frame log (see FrameLog) and its clock
QFile *frameLog = nullptr;
QElapsedTimer *frameLogClock = nullptr;

/// How long the clock keeps running after a canvas window is shown or resized
constexpr qint64 settleNs = 3'000'000'000;

/**
 * The frame clock of the canvas windows. Qt 5 repaints a window on every change, as often as the mouse
 * reports, queueing frames behind the pointer. Here repaints wait for the clock: one frame per refresh
 * (or per few, if frames take longer). The first after a pause is shown at once; the clock stops when
 * idle.
 */
class Pacer : public QObject
{
public:
    explicit Pacer(QObject *parent)
        : QObject(parent)
    {
        m_clock.start();
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::Move:
            // keeps the Composed pixel inside it
            if (watched->isWidgetType() && static_cast<QWidget *>(watched)->isWindow() && static_cast<QWidget *>(watched)->isVisible()
                && watched->inherits("KisMainWindow")) {
                composedOf(static_cast<QWidget *>(watched))->place();
            }
            return false;
        case QEvent::Show:
        case QEvent::Expose:
        case QEvent::Resize:
        case QEvent::WindowStateChange: {
            // a canvas window shown or resized: frames for a while regardless, as frames presented while it
            // settles may not reach the screen (its glass would only show after the next input)
            QWidget *top = nullptr;
            if (QWindow *window = qobject_cast<QWindow *>(watched)) {
                top = topWidget(window);
            } else if (watched->isWidgetType() && static_cast<QWidget *>(watched)->isWindow() && watched->inherits("KisMainWindow")) {
                top = static_cast<QWidget *>(watched);
            }
            if (top) {
                heartbeatOf(top);
                if (top->isVisible()) composedOf(top)->place();
                m_settleUntil = m_clock.nsecsElapsed() + settleNs;
                wake();
            }
            return false;
        }
        case QEvent::UpdateRequest: {
            // held for the clock's next frame
            if (m_delivering || !watched->isWidgetType()) return false;
            QWidget *widget = static_cast<QWidget *>(watched);
            if (!widget->isWindow() || !widget->inherits("KisMainWindow")) return false;
            m_waiting.insert(widget, widget);
            wake();
            return true;
        }
        default:
            return false;
        }
    }

private:
    /// The main window widget of \p window, or none (popups and dialogs present themselves); cached, the lookup is slow
    QWidget *topWidget(QWindow *window)
    {
        QWidget *widget = nullptr;
        auto known = m_tops.constFind(window);
        if (known != m_tops.constEnd()) {
            widget = known.value().data();
        } else {
            Q_FOREACH (QWidget *candidate, QApplication::topLevelWidgets()) {
                if (candidate->windowHandle() == window) {
                    widget = candidate;
                    break;
                }
            }
            if (widget) {
                m_tops.insert(window, widget);
                // forgotten when destroyed: another window may reuse the address
                connect(window, &QObject::destroyed, this, [this, window]() { m_tops.remove(window); });
            }
        }
        // checked each time: a widget's class is only settled once constructed
        return widget && widget->inherits("KisMainWindow") ? widget : nullptr;
    }

    Heartbeat *heartbeatOf(QWidget *top)
    {
        QPointer<Heartbeat> &heartbeat = m_beats[top];
        if (!heartbeat) heartbeat = new Heartbeat(top);
        return heartbeat;
    }

    Composed *composedOf(QWidget *top)
    {
        QPointer<Composed> &composed = m_composed[top];
        if (!composed) composed = new Composed(top);
        return composed;
    }

    /// One screen refresh, in ns
    qreal refresh() const
    {
        qreal rate = 60;
        for (auto it = m_beats.constBegin(); it != m_beats.constEnd(); ++it) {
            if (it.value() && it.key()->windowHandle() && it.key()->windowHandle()->screen()) {
                rate = it.key()->windowHandle()->screen()->refreshRate();
                break;
            }
        }
        return 1e9 / qBound(30.0, rate, 1000.0);
    }

    /// The clock's frame period: as many refreshes as a frame takes (see measured())
    qint64 period() const
    {
        return qint64(refresh() * m_refreshes);
    }

    /**
     * Adapts the rate to how long frames take, without flapping (which flickers): slower when four of
     * the last sixteen frames were late, faster after frames fit the faster rate for a while (0.5 s,
     * doubling up to 8 s each time it did not hold, reset after 10 s without slowing)
     */
    void measured(qint64 took)
    {
        const qint64 now = m_clock.nsecsElapsed();
        const qreal r = refresh();
        // a bit per late frame of the last sixteen
        const bool late = took > r * m_refreshes * 0.95;
        m_late = quint16((m_late << 1) | (late ? 1 : 0));
        if (late) {
            int count = 0;
            for (quint16 bits = m_late; bits; bits &= quint16(bits - 1)) count++;
            if (count >= 4 && m_refreshes < 4) {
                m_refreshes++;
                m_late = 0;
                // it had just gone faster: wait twice as long before trying again
                m_upWait = now - m_wentFaster < 2 * m_upWait ? qMin<qint64>(m_upWait * 2, 8'000'000'000) : 500'000'000;
                m_fastEnoughSince = now;
                m_wentSlower = now;
            }
            return;
        }
        if (now - m_wentSlower > 10'000'000'000) m_upWait = 500'000'000;
        if (m_refreshes > 1 && took > r * (m_refreshes - 1) * 0.7) {
            m_fastEnoughSince = now;
        } else if (m_refreshes > 1 && now - m_fastEnoughSince > m_upWait) {
            m_refreshes--;
            m_wentFaster = now;
            m_fastEnoughSince = now;
        }
    }

    void wake()
    {
        if (m_tick.isActive()) {
            return;
        }
        // after a pause, the first frame at once
        m_next = m_clock.nsecsElapsed();
        m_tick.start(0);
    }

    void schedule()
    {
        const qint64 now = m_clock.nsecsElapsed();
        m_next += period();
        // fell behind: restart from now
        if (m_next < now) m_next = now;
        m_tick.start(m_next - now);
    }

    void tick()
    {
        const bool settling = m_clock.nsecsElapsed() < m_settleUntil;
        if (settling) {
            // a settling window is presented regardless, by its heartbeat if nothing changed
            for (auto it = m_beats.constBegin(); it != m_beats.constEnd(); ++it) {
                if (it.value() && it.key()->isVisible() && !m_waiting.contains(it.key())) show(it.key(), it.value());
            }
        } else if (m_waiting.isEmpty()) {
            // idle: stop until the next repaint
            m_upWait = 500'000'000;
            return;
        }
        deliver();
        schedule();
    }

    void deliver()
    {
        const QHash<QWidget *, QPointer<QWidget>> waiting = m_waiting;
        m_waiting.clear();
        for (const QPointer<QWidget> &window : waiting) {
            // hidden ones too: Qt waits for this repaint before requesting another
            if (window) show(window, nullptr);
        }
    }

    static QWidget *canvasOf(QWidget *window)
    {
        KisMainWindow *mainWindow = qobject_cast<KisMainWindow *>(window);
        KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
        KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
        QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
        return widget && widget->isVisible() && widget->window() == window ? widget : nullptr;
    }

    /**
     * Presents \p window: its pending repaint, or with \p heartbeat an unchanged frame. An unchanged frame
     * redraws one canvas pixel: changing only the OpenGL canvas keeps Qt from repainting every panel
     * over it (7 ms instead of 1). Without a canvas, the heartbeat widget repaints.
     */
    void show(QWidget *window, Heartbeat *heartbeat)
    {
        QElapsedTimer took;
        took.start();
        m_delivering = true;
        QWidget *canvas = heartbeat ? canvasOf(window) : nullptr;
        if (heartbeat && !canvas) {
            heartbeat->repaint();
        } else {
            if (canvas) canvas->update(QRect(0, 0, 1, 1));
            QEvent request(QEvent::UpdateRequest);
            QCoreApplication::sendEvent(window, &request);
        }
        m_delivering = false;
        measured(took.nsecsElapsed());
        if (frameLog) {
            frameLog->write(QStringLiteral("%1 shown %2 us, every %3 refreshes%4\n").arg(m_clock.nsecsElapsed() / 1000).arg(took.nsecsElapsed() / 1000)
                                .arg(m_refreshes).arg(heartbeat ? QStringLiteral(", heartbeat") : QString()).toUtf8());
        }
    }

    QElapsedTimer m_clock;
    FrameTimer m_tick {[this]() { tick(); }};
    qint64 m_settleUntil {0};
    qint64 m_next {0};
    /// Refreshes per frame; late frames bitmask; since when frames fit the faster rate
    int m_refreshes {1};
    quint16 m_late {0};
    qint64 m_fastEnoughSince {0};
    /// How long frames must fit before going faster, and when the rate last changed
    qint64 m_upWait {500'000'000};
    qint64 m_wentFaster {-100'000'000'000};
    qint64 m_wentSlower {-100'000'000'000};
    QHash<QWidget *, QPointer<Heartbeat>> m_beats;
    QHash<QWidget *, QPointer<Composed>> m_composed;
    QHash<QWindow *, QPointer<QWidget>> m_tops;
    QHash<QWidget *, QPointer<QWidget>> m_waiting;
    bool m_delivering {false};
};

/// For development (KRITA_DEV_FRAMELOG=file): logs pointer moves, repaints and the clock's frames
class FrameLog : public QObject
{
public:
    FrameLog(const QString &path, QObject *parent)
        : QObject(parent)
        , m_file(path)
    {
        m_file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
        frameLog = &m_file;
        m_clock.start();
        frameLogClock = &m_clock;
        m_flush.setInterval(500);
        connect(&m_flush, &QTimer::timeout, this, [this]() { m_file.flush(); });
        m_flush.start();
        // Qt's warnings too (QML errors among them), still passed on
        previousHandler() = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &context, const QString &message) {
            if (type != QtDebugMsg && type != QtInfoMsg) KisUpdatePacer::log(QStringLiteral("qt ") + message);
            if (previousHandler()) previousHandler()(type, context, message);
        });
    }

    static QtMessageHandler &previousHandler()
    {
        static QtMessageHandler handler = nullptr;
        return handler;
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (event->type() == QEvent::UpdateRequest && watched->isWidgetType() && static_cast<QWidget *>(watched)->isWindow()) {
            m_file.write(QStringLiteral("%1 frame %2\n").arg(m_clock.nsecsElapsed() / 1000).arg(nameOf(watched)).toUtf8());
        } else if ((event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove) && watched->isWindowType()) {
            m_file.write(QStringLiteral("%1 move\n").arg(m_clock.nsecsElapsed() / 1000).toUtf8());
        } else if (event->type() == QEvent::Paint && watched->isWidgetType()) {
            const QRect r = static_cast<QPaintEvent *>(event)->rect();
            m_file.write(QStringLiteral("%1 paint %2 %3,%4 %5x%6\n").arg(m_clock.nsecsElapsed() / 1000).arg(nameOf(watched))
                             .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).toUtf8());
        }
        return false;
    }

private:
    static QString nameOf(QObject *object)
    {
        const QString name = object->objectName();
        return QString::fromLatin1(object->metaObject()->className()) + (name.isEmpty() ? QString() : QStringLiteral("#") + name);
    }

    QFile m_file;
    QElapsedTimer m_clock;
    QTimer m_flush;
};
#endif
} // namespace

void KisUpdatePacer::log(const QString &what)
{
#ifdef Q_OS_WIN
    if (frameLog) {
        frameLog->write(QStringLiteral("%1 %2\n").arg(frameLogClock ? frameLogClock->nsecsElapsed() / 1000 : 0).arg(what).toUtf8());
    }
#else
    Q_UNUSED(what);
#endif
}

void KisUpdatePacer::install()
{
#ifdef Q_OS_WIN
    static Pacer *pacer = nullptr;
    // for development: KRITA_DEV_NOPACER disables it, to compare
    if (pacer || (qEnvironmentVariableIsSet("KRITA_DEV_PROFILE") && qEnvironmentVariableIsSet("KRITA_DEV_NOPACER"))) return;
    pacer = new Pacer(qApp);
    // installed after the pacer, the frame log sees repaints before the pacer holds them
    qApp->installEventFilter(pacer);
    if (qEnvironmentVariableIsSet("KRITA_DEV_PROFILE") && qEnvironmentVariableIsSet("KRITA_DEV_FRAMELOG")) {
        qApp->installEventFilter(new FrameLog(qEnvironmentVariable("KRITA_DEV_FRAMELOG"), qApp));
    }
#endif
}
