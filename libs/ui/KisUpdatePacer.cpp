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

#ifdef Q_OS_WIN
/**
 * A pixel over the window that the desktop has to blend in (1 of 255 opaque:
 * no one sees it), so that it composes the window always, as it does any
 * other. A window covering a screen is handed the screen on its own
 * otherwise ("independent flip"), and a screen with a variable refresh rate
 * (G-Sync, FreeSync) then takes it for a game: it refreshes only as Krita
 * shows frames (at 48 Hz at rest, at 60 if that is how fast frames come,
 * the pointer moving as slowly), and many such screens flicker as that rate
 * changes. Composed, the screen keeps its full rate whatever Krita does.
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
        // owned by the window: over it, minimized and gone with it; never clicked, never focused
        m_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"KritaComposed", L"", WS_POPUP, 0, 0, 1, 1,
                                   HWND(owner->winId()), nullptr, GetModuleHandleW(nullptr), nullptr);
        // not 0: a window no one could see is left out
        SetLayeredWindowAttributes(m_window, 0, 1, LWA_ALPHA);
    }

    ~Composed() override
    {
        // gone already if its window went first
        if (IsWindow(m_window)) DestroyWindow(m_window);
    }

    /// At the window's corner, inside it (the frame of a maximized window is off the screen)
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
 * Calls back on the GUI thread at a time to within a fraction of a
 * millisecond. Qt's timers keep to the system's 15.6 ms steps whenever
 * Windows coarsens timers, which is often: this one waits on a high
 * resolution timer in a thread of its own, and wakes the GUI thread with a
 * message, which Windows hands over before input (Qt 5 holds its own posted
 * events back while input waits).
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
        // high resolution where there is one (Windows 10 1803 on)
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
            // relative, in 100 ns
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

/// The development frame log (see FrameLog), if there is one, and its clock
QFile *frameLog = nullptr;
QElapsedTimer *frameLogClock = nullptr;

/// How long the clock goes on after a window with the canvas is shown or sized
constexpr qint64 settleNs = 3'000'000'000;

/**
 * The frame clock of the windows the canvas is in. Qt 5 shows a window
 * again whenever something in it changed: with the pointer moving, as
 * often as the mouse reports (a thousand times a second, say), each frame
 * queued behind the last for the screen, the window always behind the hand.
 *
 * So the window's repaints wait for the clock: a frame every refresh of the
 * screen, or every second (third...) one if a frame takes longer than that.
 * The first after a pause is shown at once; the clock stops when there is
 * nothing to show. (The window is composed by the desktop, see Composed:
 * the screen keeps its rate whether frames come or not.)
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
            // only to keep its pixel inside it
            if (watched->isWidgetType() && static_cast<QWidget *>(watched)->isWindow() && static_cast<QWidget *>(watched)->isVisible()
                && watched->inherits("KisMainWindow")) {
                composedOf(static_cast<QWidget *>(watched))->place();
            }
            return false;
        case QEvent::Show:
        case QEvent::Expose:
        case QEvent::Resize:
        case QEvent::WindowStateChange: {
            /**
             * A window with the canvas shown, uncovered, sized or maximized:
             * frames for a while, whatever changes. As a window is shown and
             * settles (maximized, its panels laid out and their glass drawn)
             * frames it shows may not reach the screen, and it would show the
             * glass only after the next input.
             */
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
            // the window's repaint, held for the clock's next frame
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
    /**
     * The main window \p window is of, or none (a popup, a dialog: they show
     * their own changes). The widget of a window is looked up once: all the
     * widgets there are are gone through for it (QApplication::topLevelWidgets),
     * too much for each move of the pointer.
     */
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
                // a window is its widget's for as long as it is there: forgotten when it goes (another may come at its address)
                connect(window, &QObject::destroyed, this, [this, window]() { m_tops.remove(window); });
            }
        }
        // asked each time, not kept: what a widget is is only settled once it is made
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

    /// The time of a refresh of the screen
    qreal refresh() const
    {
        qreal rate = 60;
        for (auto it = m_beats.constBegin(); it != m_beats.constEnd(); ++it) {
            // a window gone took its heartbeat with it
            if (it.value() && it.key()->windowHandle() && it.key()->windowHandle()->screen()) {
                rate = it.key()->windowHandle()->screen()->refreshRate();
                break;
            }
        }
        return 1e9 / qBound(30.0, rate, 1000.0);
    }

    /// The time of the clock's frames: as many refreshes as a frame takes (see measured())
    qint64 period() const
    {
        return qint64(refresh() * m_refreshes);
    }

    /**
     * How long a frame took: the clock's rate follows, but it must not keep
     * changing (that is what flickers): slower at once when frames do not
     * fit in it (four of the last sixteen), faster again after half a second of
     * frames well within the faster rate; each time the faster rate did not
     * hold, after twice as long (up to eight seconds), until it has held for
     * ten seconds
     */
    void measured(qint64 took)
    {
        const qint64 now = m_clock.nsecsElapsed();
        const qreal r = refresh();
        // the last sixteen frames, a bit for each that came late
        const bool late = took > r * m_refreshes * 0.95;
        m_late = quint16((m_late << 1) | (late ? 1 : 0));
        if (late) {
            // four late of sixteen: slower (not for a hiccup of the system: each change of the rate is a step a screen may show)
            int count = 0;
            for (quint16 bits = m_late; bits; bits &= quint16(bits - 1)) count++;
            if (count >= 4 && m_refreshes < 4) {
                m_refreshes++;
                m_late = 0;
                // soon after it went faster: it is waited for twice as long before the next try
                m_upWait = now - m_wentFaster < 2 * m_upWait ? qMin<qint64>(m_upWait * 2, 8'000'000'000) : 500'000'000;
                m_fastEnoughSince = now;
                m_wentSlower = now;
            }
            return;
        }
        // a while without slowing down: what slowed it then (opening a document, say) counts no more
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
        // fallen behind: from now on
        if (m_next < now) m_next = now;
        m_tick.start(m_next - now);
    }

    void tick()
    {
        const bool settling = m_clock.nsecsElapsed() < m_settleUntil;
        if (settling) {
            // a window settling is shown whatever changed, by its heartbeat alone if nothing did
            for (auto it = m_beats.constBegin(); it != m_beats.constEnd(); ++it) {
                if (it.value() && it.key()->isVisible() && !m_waiting.contains(it.key())) show(it.key(), it.value());
            }
        } else if (m_waiting.isEmpty()) {
            // nothing came in a frame: stopped, until the next repaint; a slow moment before (opening a document, say) counts no more
            m_upWait = 500'000'000;
            return;
        }
        deliver();
        schedule();
    }

    /// The held repaints, now
    void deliver()
    {
        const QHash<QWidget *, QPointer<QWidget>> waiting = m_waiting;
        m_waiting.clear();
        for (const QPointer<QWidget> &window : waiting) {
            // hidden too: Qt counts on its repaint to come, and would not ask for another
            if (window) show(window, nullptr);
        }
    }

    /// The canvas widget of \p window, if it shows one
    static QWidget *canvasOf(QWidget *window)
    {
        KisMainWindow *mainWindow = qobject_cast<KisMainWindow *>(window);
        KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
        KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
        QWidget *widget = canvas ? canvas->canvasWidget() : nullptr;
        return widget && widget->isVisible() && widget->window() == window ? widget : nullptr;
    }

    /**
     * \p window shown: what changed in it repainted, or (\p heartbeat) a
     * frame of it as it is.
     *
     * Its canvas is drawn into a texture: if only such widgets changed, Qt
     * draws them and puts the window together again; if anything else
     * changed too, it repaints all of the canvas' area, every panel and
     * button over it with it (7 ms a frame, not 1). So a frame of the window
     * as it is is a pixel of its canvas drawn again, nothing else. (A widget
     * of its own would be a change of the other kind, and where something
     * opaque covers it, no frame at all.) Without a canvas, the heartbeat
     * widget, at once.
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
    /// Until when a window shown or sized keeps the clock going; when the next frame is due
    qint64 m_settleUntil {0};
    qint64 m_next {0};
    /// The clock's frames in refreshes of the screen; late frames in a row; since when frames would fit faster ones
    int m_refreshes {1};
    quint16 m_late {0};
    qint64 m_fastEnoughSince {0};
    /// How long frames must fit a faster rate before it is tried (longer each time it did not hold), and when it last was
    qint64 m_upWait {500'000'000};
    qint64 m_wentFaster {-100'000'000'000};
    qint64 m_wentSlower {-100'000'000'000};
    QHash<QWidget *, QPointer<Heartbeat>> m_beats;
    QHash<QWidget *, QPointer<Composed>> m_composed;
    QHash<QWindow *, QPointer<QWidget>> m_tops;
    QHash<QWidget *, QPointer<QWidget>> m_waiting;
    bool m_delivering {false};
};

/**
 * For development (KRITA_DEV_FRAMELOG=file): the pointer's moves, each
 * window drawn again and what in it (where), and each frame of the clock,
 * how long it took: to see what keeps the screen's refresh rate changing
 */
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
        // Qt's warnings too (QML errors among them), and on to wherever they went before
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
    // for development: without it, to compare
    if (pacer || (qEnvironmentVariableIsSet("KRITA_DEV_PROFILE") && qEnvironmentVariableIsSet("KRITA_DEV_NOPACER"))) return;
    pacer = new Pacer(qApp);
    // the frame log first: installed later, it sees the repaints before the pacer holds them
    qApp->installEventFilter(pacer);
    if (qEnvironmentVariableIsSet("KRITA_DEV_PROFILE") && qEnvironmentVariableIsSet("KRITA_DEV_FRAMELOG")) {
        qApp->installEventFilter(new FrameLog(qEnvironmentVariable("KRITA_DEV_FRAMELOG"), qApp));
    }
#endif
}
