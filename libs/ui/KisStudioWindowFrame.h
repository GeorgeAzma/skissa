/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOWINDOWFRAME_H
#define KISSTUDIOWINDOWFRAME_H

#include <QObject>
#include <QPoint>

#include <functional>

class QMainWindow;
class QWidget;

/**
 * The window without the system's title bar (Windows only): the caption
 * areas the layout reports drag the window, the edges still resize it, and
 * the system still snaps, animates and shadows it.
 */
class KisStudioWindowFrame : public QObject
{
public:
    /// \p isCaption tells whether a point, in window coordinates, drags the window
    KisStudioWindowFrame(QMainWindow *window, std::function<bool(const QPoint &)> isCaption);

    void setEnabled(bool enabled);
    bool isEnabled() const { return m_enabled; }

    /// From the window's nativeEvent(): \p result is a long* in Qt 5, a qintptr* in Qt 6
    bool handleNativeEvent(void *message, void *result);

    /// Minimize, maximize and close
    QWidget *createButtons(QWidget *parent);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QMainWindow *m_window {nullptr};
    std::function<bool(const QPoint &)> m_isCaption;
    bool m_enabled {false};
};

#endif // KISSTUDIOWINDOWFRAME_H
