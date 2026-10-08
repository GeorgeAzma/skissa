/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOWINDOWFRAME_H
#define KISSTUDIOWINDOWFRAME_H

#include <QObject>
#include <QPoint>
#include <QPointer>

#include <functional>

class QMainWindow;
class QWidget;

/**
 * The window without the system's title bar: the top bar is the title bar.
 * Its empty parts drag the window (double click maximizes, snapping works),
 * the edges still resize it, and the window buttons sit at the end of the
 * top bar. On Windows only; elsewhere nothing changes.
 *
 * The window keeps its native frame styles, so the system still animates,
 * snaps and shadows it; only the non-client area is given to the client.
 */
class KisStudioWindowFrame : public QObject
{
public:
    /// \p isCaption tells whether a point, in window coordinates, is free top bar
    KisStudioWindowFrame(QMainWindow *window, std::function<bool(const QPoint &)> isCaption);

    static bool isSupported();

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /// From the window's nativeEvent(): \p result is a long* in Qt 5, a qintptr* in Qt 6
    bool handleNativeEvent(void *message, void *result);

    /// Minimize, maximize and close, painted like the rest of the top bar
    QWidget *createButtons(QWidget *parent);

private:
    QMainWindow *m_window {nullptr};
    std::function<bool(const QPoint &)> m_isCaption;
    bool m_enabled {false};
};

#endif // KISSTUDIOWINDOWFRAME_H
