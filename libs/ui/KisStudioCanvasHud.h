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
 * A pill in the canvas's bottom right corner with the view's state: zoom, rotation, mirroring, the modes that change
 * what a stroke does, and the progress of long operations. It replaces the status bar.
 */
class KisStudioCanvasHud : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioCanvasHud(KisMainWindow *mainWindow);
    ~KisStudioCanvasHud() override;

    /// Takes the progress bar from the status bar while active, and gives it back
    void setActive(bool active);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void viewChanged();
    /// The badges of the modes on now: shown, the others not
    void updateBadges();
    void updateZoom(qreal zoom);
    void updateRotation(qreal angle);
    void reposition();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOCANVASHUD_H
