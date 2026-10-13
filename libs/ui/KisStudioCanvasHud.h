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
 * A pill in the canvas's bottom-right corner showing the view's state: zoom, rotation, mirroring, modes that change
 * what a stroke does, and the progress of long operations. It replaces the status bar.
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

private:
    void viewChanged();
    /// Shows the badge of each mode that is on and hides the rest
    void updateBadges();
    void updateZoom(qreal zoom);
    void updateRotation(qreal angle);
    void reposition();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOCANVASHUD_H
