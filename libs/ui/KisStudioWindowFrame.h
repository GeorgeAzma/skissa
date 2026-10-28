/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOWINDOWFRAME_H
#define KISSTUDIOWINDOWFRAME_H

#include <QObject>
#include <QPoint>
#include <QRect>

#include <functional>

class QMainWindow;
class QWidget;

/**
 * Removes the system title bar (Windows only). Caption areas reported by the
 * layout drag the window, the edges still resize it, and the system keeps
 * snapping, animating and shadowing it.
 */
class KisStudioWindowFrame : public QObject
{
public:
    /// \p isCaption tells whether a point, in window coordinates, drags the window
    KisStudioWindowFrame(QMainWindow *window, std::function<bool(const QPoint &)> isCaption);

    void setEnabled(bool enabled);
    /// Windows' animations between window states (maximize, restore…)
    void setTransitionsEnabled(bool enabled);
    /**
     * Makes a maximized window cover its whole screen, taskbar too, or back. Qt's full screen leaves the
     * maximized state through the restored size, which shows for a moment and lays everything out there.
     * Returns false when it doesn't apply (not maximized, or not covering), for Qt's full screen to do it.
     */
    bool coverScreen(bool on);
    bool isEnabled() const { return m_enabled; }

    /// Call from the window's nativeEvent(); \p result is a long* in Qt 5 and a qintptr* in Qt 6.
    bool handleNativeEvent(void *message, void *result);

    /// Creates the minimize, maximize and close buttons
    QWidget *createButtons(QWidget *parent);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QMainWindow *m_window {nullptr};
    std::function<bool(const QPoint &)> m_isCaption;
    bool m_enabled {false};
    bool m_covering {false};
    /// The maximized window's place before it covered the screen
    QRect m_uncovered;
};

#endif // KISSTUDIOWINDOWFRAME_H
