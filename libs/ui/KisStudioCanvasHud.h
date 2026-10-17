/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCANVASHUD_H
#define KISSTUDIOCANVASHUD_H

#include <QWidget>

#include <memory>

class KisMainWindow;

/**
 * A pill over the canvas (its bottom-right corner, until dragged elsewhere) showing the view's state: zoom, rotation,
 * mirroring, modes that change what a stroke does, and the progress of long operations. It replaces the status bar.
 * Buttons can be added to it like the top bar's; its right click adds them or hides it.
 */
class KisStudioCanvasHud : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioCanvasHud(KisMainWindow *mainWindow);
    ~KisStudioCanvasHud() override;

    /// Borrows the status bar's progress bar while active and returns it when deactivated
    void setActive(bool active);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void viewChanged();
    /// Shows the badge of each mode that is on and hides the rest
    void updateBadges();
    void updateZoom(qreal zoom);
    void updateRotation(qreal angle);
    void reposition();
    /// The pill follows a drag begun on it or on one of its own buttons; true while it does
    bool drag(QMouseEvent *event, QWidget *from);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOCANVASHUD_H
