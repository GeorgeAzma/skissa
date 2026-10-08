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
 * The view's own state, on the view: a small pill in the bottom right corner
 * of the canvas with the zoom, the rotation (only while the canvas is
 * rotated), mirroring, a value check and the progress of long operations.
 * It replaces the status bar, whose other entries (color space, profile,
 * memory, size) describe the document rather than the view and live in the
 * document properties.
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
    void updateZoom(qreal zoom);
    void updateRotation(qreal angle);
    void setValueCheck(bool enabled);
    void reposition();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOCANVASHUD_H
