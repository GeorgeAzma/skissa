/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioWindowFrame.h"

#include <QAbstractButton>
#include <QEvent>
#include <QHBoxLayout>
#include <QMainWindow>
#include <QPainter>


#include <klocalizedstring.h>

#include <KisModernStyle.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#ifndef ABM_GETAUTOHIDEBAREX
#define ABM_GETAUTOHIDEBAREX 0x0000000b
#endif
#endif

namespace {

enum class WindowButtonKind {
    Minimize,
    Maximize,
    Close,
};

/// A caption button drawn with thin strokes; Close turns red on hover
class WindowButton : public QAbstractButton
{
public:
    WindowButton(WindowButtonKind kind, QMainWindow *window, QWidget *parent)
        : QAbstractButton(parent), m_kind(kind), m_window(window)
    {
        setFixedSize(46, 32);
        setFocusPolicy(Qt::NoFocus);
        setAttribute(Qt::WA_Hover);
        window->installEventFilter(this);
        // no tooltip, as their glyphs are what every window has; named for screen readers and scripts
        switch (kind) {
        case WindowButtonKind::Minimize: setObjectName(QStringLiteral("studio_window_minimize")); setAccessibleName(i18nc("@action:button", "Minimize")); break;
        case WindowButtonKind::Maximize: setObjectName(QStringLiteral("studio_window_maximize")); setAccessibleName(i18nc("@action:button", "Maximize")); break;
        case WindowButtonKind::Close: setObjectName(QStringLiteral("studio_window_close")); setAccessibleName(i18nc("@action:button", "Close")); break;
        }
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        // the maximize icon differs between maximized and normal
        if (watched == m_window && event->type() == QEvent::WindowStateChange) update();
        return QAbstractButton::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);
        QColor ink = palette().color(QPalette::ButtonText);
        if (underMouse()) {
            if (m_kind == WindowButtonKind::Close) {
                painter.fillRect(rect(), QColor(196, 43, 28));
                ink = Qt::white;
            } else {
                QColor hover = ink;
                hover.setAlphaF(isDown() ? 0.16 : 0.09);
                painter.fillRect(rect(), hover);
            }
        }
        painter.setPen(QPen(ink, 1.0));
        painter.setBrush(Qt::NoBrush);
        const QPointF c = QRectF(rect()).center();
        const qreal s = 5.0;
        switch (m_kind) {
        case WindowButtonKind::Minimize:
            painter.drawLine(QPointF(c.x() - s, c.y()), QPointF(c.x() + s, c.y()));
            break;
        case WindowButtonKind::Maximize:
            if (m_window->isMaximized()) {
                painter.drawPath(KisModernStyle::squircle(QRectF(c.x() - s, c.y() - s + 2, 2 * s - 2, 2 * s - 2), 1.5));
                painter.drawPolyline(QPolygonF({QPointF(c.x() - s + 2, c.y() - s), QPointF(c.x() + s, c.y() - s),
                                                QPointF(c.x() + s, c.y() + s - 2)}));
            } else {
                painter.drawPath(KisModernStyle::squircle(QRectF(c.x() - s, c.y() - s, 2 * s, 2 * s), 1.5));
            }
            break;
        case WindowButtonKind::Close:
            painter.drawLine(QPointF(c.x() - s, c.y() - s), QPointF(c.x() + s, c.y() + s));
            painter.drawLine(QPointF(c.x() + s, c.y() - s), QPointF(c.x() - s, c.y() + s));
            break;
        }
    }

private:
    WindowButtonKind m_kind;
    QMainWindow *m_window;
};

#ifdef Q_OS_WIN
/// A system metric at the window's own DPI, where supported (Windows 10 1607+)
int frameMetric(HWND hwnd, int index)
{
    using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
    using GetSystemMetricsForDpiFn = int(WINAPI *)(int, UINT);
    static HMODULE user32 = GetModuleHandleW(L"user32.dll");
    static auto getDpiForWindow = reinterpret_cast<GetDpiForWindowFn>(reinterpret_cast<void *>(GetProcAddress(user32, "GetDpiForWindow")));
    static auto getMetricsForDpi =
        reinterpret_cast<GetSystemMetricsForDpiFn>(reinterpret_cast<void *>(GetProcAddress(user32, "GetSystemMetricsForDpi")));
    if (getDpiForWindow && getMetricsForDpi) {
        return getMetricsForDpi(index, getDpiForWindow(hwnd));
    }
    return GetSystemMetrics(index);
}

int frameThickness(HWND hwnd, bool vertical)
{
    constexpr int paddedBorder = 92; // SM_CXPADDEDBORDER
    return frameMetric(hwnd, vertical ? SM_CYFRAME : SM_CXFRAME) + frameMetric(hwnd, paddedBorder);
}

/// Extends the frame into the client area (or undoes it) and has the system recompute the client area
void applyFrame(HWND hwnd, bool extend)
{
    // a 1 px glass edge keeps the system shadow on the borderless window
    struct Margins { int left, right, top, bottom; };
    using DwmExtendFn = HRESULT(WINAPI *)(HWND, const Margins *);
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    static auto dwmExtend = dwm ? reinterpret_cast<DwmExtendFn>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"))) : nullptr;
    if (dwmExtend) {
        const Margins margins = extend ? Margins{0, 0, 1, 0} : Margins{0, 0, 0, 0};
        dwmExtend(hwnd, &margins);
    }
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

/// Leaves a gap on an auto-hiding taskbar's edge so the pointer can still reveal it over a maximized window
/// (Windows takes a window that covers the whole screen for a full-screen one, and keeps the taskbar under it)
void leaveRoomForTaskbar(RECT &rect)
{
    using AppBarFn = UINT_PTR(WINAPI *)(DWORD, PAPPBARDATA);
    static HMODULE shell = LoadLibraryW(L"shell32.dll");
    static auto appBar = shell ? reinterpret_cast<AppBarFn>(reinterpret_cast<void *>(GetProcAddress(shell, "SHAppBarMessage"))) : nullptr;
    if (!appBar) return;
    APPBARDATA data {};
    data.cbSize = sizeof(data);
    if (!(appBar(ABM_GETSTATE, &data) & ABS_AUTOHIDE)) return;
    MONITORINFO monitor {};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    for (const UINT edge : {ABE_BOTTOM, ABE_TOP, ABE_LEFT, ABE_RIGHT}) {
        data.uEdge = edge;
        data.rc = monitor.rcMonitor;
        if (!appBar(ABM_GETAUTOHIDEBAREX, &data)) continue;
        switch (edge) {
        case ABE_BOTTOM: rect.bottom -= 2; break;
        case ABE_TOP: rect.top += 2; break;
        case ABE_LEFT: rect.left += 2; break;
        default: rect.right -= 2; break;
        }
        return;
    }
}
#endif

} // namespace

KisStudioWindowFrame::KisStudioWindowFrame(QMainWindow *window, std::function<bool(const QPoint &)> isCaption)
    : QObject(window)
    , m_window(window)
    , m_isCaption(std::move(isCaption))
{
    window->installEventFilter(this);
}

bool KisStudioWindowFrame::eventFilter(QObject *watched, QEvent *event)
{
#ifdef Q_OS_WIN
    // Qt recreates the native window for some changes; the new one needs the frame applied again
    if (watched == m_window && event->type() == QEvent::WinIdChange && m_enabled && m_window->internalWinId()) {
        applyFrame(reinterpret_cast<HWND>(m_window->internalWinId()), true);
    }
#endif
    return QObject::eventFilter(watched, event);
}

void KisStudioWindowFrame::setTransitionsEnabled(bool enabled)
{
#ifdef Q_OS_WIN
    using DwmSetAttributeFn = HRESULT(WINAPI *)(HWND, DWORD, LPCVOID, DWORD);
    static HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    static auto dwmSetAttribute = dwm ? reinterpret_cast<DwmSetAttributeFn>(reinterpret_cast<void *>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) : nullptr;
    HWND hwnd = reinterpret_cast<HWND>(m_window->internalWinId());
    if (dwmSetAttribute && hwnd) {
        constexpr DWORD transitionsForceDisabled = 3; // DWMWA_TRANSITIONS_FORCEDISABLED
        const BOOL disabled = !enabled;
        dwmSetAttribute(hwnd, transitionsForceDisabled, &disabled, sizeof(disabled));
    }
#else
    Q_UNUSED(enabled);
#endif
}

bool KisStudioWindowFrame::coverScreen(bool on)
{
#ifdef Q_OS_WIN
    HWND hwnd = reinterpret_cast<HWND>(m_window->internalWinId());
    if (!m_enabled || !hwnd || on == m_covering) return false;
    if (on && !IsZoomed(hwnd)) return false;
    m_covering = on;
    // restored meanwhile (Win+Down): nothing to undo
    if (!IsZoomed(hwnd)) return true;
    RECT rect;
    if (on) {
        GetWindowRect(hwnd, &rect);
        m_uncovered = QRect(QPoint(rect.left, rect.top), QPoint(rect.right - 1, rect.bottom - 1));
        // the monitor and the frame a maximized window overhangs it by, which WM_NCCALCSIZE trims
        MONITORINFO monitor {};
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) return true;
        rect = monitor.rcMonitor;
        InflateRect(&rect, frameThickness(hwnd, false), frameThickness(hwnd, true));
    } else {
        rect = {m_uncovered.left(), m_uncovered.top(), m_uncovered.right() + 1, m_uncovered.bottom() + 1};
    }
    SetWindowPos(hwnd, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    return true;
#else
    Q_UNUSED(on);
    return false;
#endif
}

void KisStudioWindowFrame::setEnabled(bool enabled)
{
#ifdef Q_OS_WIN
    if (m_enabled == enabled) {
        return;
    }
    if (!enabled) coverScreen(false);
    m_enabled = enabled;
    applyFrame(reinterpret_cast<HWND>(m_window->winId()), enabled);
#else
    Q_UNUSED(enabled);
#endif
}

bool KisStudioWindowFrame::handleNativeEvent(void *message, void *resultPointer)
{
#ifdef Q_OS_WIN
    if (!m_enabled || !message) {
        return false;
    }
    MSG *msg = static_cast<MSG *>(message);
    // not winId(): this can run while the native window is being created, and winId() would recreate it and crash
    HWND hwnd = reinterpret_cast<HWND>(m_window->internalWinId());
    if (!hwnd || msg->hwnd != hwnd) {
        return false;
    }
    auto setResult = [resultPointer](LRESULT value) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        *static_cast<qintptr *>(resultPointer) = value;
#else
        *static_cast<long *>(resultPointer) = long(value);
#endif
    };
    const bool fullScreen = m_window->isFullScreen() || (m_covering && IsZoomed(hwnd));

    switch (msg->message) {
    case WM_NCCALCSIZE: {
        if (msg->wParam != TRUE) {
            return false;
        }
        // the whole window is client area; a maximized window overhangs the screen by its frame, so trim that.
        // Asks Windows, not Qt: leaving full screen, Qt still reports it while the window is maximized again
        if (IsZoomed(hwnd)) {
            RECT &rect = reinterpret_cast<NCCALCSIZE_PARAMS *>(msg->lParam)->rgrc[0];
            // the screen's work area (all of the screen while covering it), however far the window's styles make it overhang
            MONITORINFO monitor {};
            monitor.cbSize = sizeof(monitor);
            if (GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &monitor)) {
                const RECT &room = m_covering ? monitor.rcMonitor : monitor.rcWork;
                rect.left = qMax(rect.left, room.left);
                rect.top = qMax(rect.top, room.top);
                rect.right = qMin(rect.right, room.right);
                rect.bottom = qMin(rect.bottom, room.bottom);
            }
            if (!m_covering) leaveRoomForTaskbar(rect);
        }
        setResult(0);
        return true;
    }
    case WM_NCHITTEST: {
        const POINT screen {GET_X_LPARAM(msg->lParam), GET_Y_LPARAM(msg->lParam)};
        if (!IsZoomed(hwnd) && !fullScreen) {
            RECT window;
            GetWindowRect(hwnd, &window);
            const int border = frameThickness(hwnd, false);
            const bool left = screen.x < window.left + border;
            const bool right = screen.x >= window.right - border;
            const bool top = screen.y < window.top + border;
            const bool bottom = screen.y >= window.bottom - border;
            LRESULT edge = 0;
            if (top && left) edge = HTTOPLEFT;
            else if (top && right) edge = HTTOPRIGHT;
            else if (bottom && left) edge = HTBOTTOMLEFT;
            else if (bottom && right) edge = HTBOTTOMRIGHT;
            else if (top) edge = HTTOP;
            else if (bottom) edge = HTBOTTOM;
            else if (left) edge = HTLEFT;
            else if (right) edge = HTRIGHT;
            if (edge) {
                setResult(edge);
                return true;
            }
        }
        POINT client = screen;
        ScreenToClient(hwnd, &client);
        const qreal dpr = m_window->devicePixelRatioF();
        const QPoint local(qRound(client.x / dpr), qRound(client.y / dpr));
        setResult(!fullScreen && m_isCaption && m_isCaption(local) ? HTCAPTION : HTCLIENT);
        return true;
    }
    case WM_NCRBUTTONDOWN:
    case WM_NCRBUTTONDBLCLK:
    case WM_NCRBUTTONUP:
        // ignore right clicks on the resize edges; a right click on the caption still opens the window menu
        if (msg->wParam >= HTLEFT && msg->wParam <= HTBOTTOMRIGHT) {
            setResult(0);
            return true;
        }
        return false;
    default:
        return false;
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(resultPointer);
    return false;
#endif
}

QWidget *KisStudioWindowFrame::createButtons(QWidget *parent)
{
    auto *buttons = new QWidget(parent);
    auto *layout = new QHBoxLayout(buttons);
    layout->setContentsMargins(KisModernStyle::space2, 0, 0, 0);
    layout->setSpacing(0);

    auto *minimize = new WindowButton(WindowButtonKind::Minimize, m_window, buttons);
    connect(minimize, &QAbstractButton::clicked, m_window, &QWidget::showMinimized);
    auto *maximize = new WindowButton(WindowButtonKind::Maximize, m_window, buttons);
    connect(maximize, &QAbstractButton::clicked, m_window, [this]() {
        if (m_window->isMaximized()) m_window->showNormal();
        else m_window->showMaximized();
    });
    auto *close = new WindowButton(WindowButtonKind::Close, m_window, buttons);
    connect(close, &QAbstractButton::clicked, m_window, &QWidget::close);
    layout->addWidget(minimize);
    layout->addWidget(maximize);
    layout->addWidget(close);
    return buttons;
}
