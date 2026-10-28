/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSTARTPAGE_H
#define KISSTUDIOSTARTPAGE_H

#include <QWidget>

#include <functional>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * What the window shows with no image open, in the Studio layout: over the last
 * image, blurred, a pane with the new images drawn to scale and Open; then the
 * last image large and the others in rows that keep their shapes. Typing filters
 * them, the arrow keys and Enter pick, and files dropped anywhere on it open.
 */
class KRITAUI_EXPORT KisStudioStartPage : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioStartPage(KisMainWindow *mainWindow);
    ~KisStudioStartPage() override;

    /// The new image tiles: a size makes the image at once, Custom opens its sheet under them; \p picked runs
    /// first. \p after is a tile of the caller's, placed after Custom.
    static QWidget *createNewTiles(KisMainWindow *mainWindow, QWidget *parent, std::function<void()> picked = {}, QWidget *after = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    /// Tiles of the images open in this window, to go back to
    void showOpenImages();

    struct Private;
    Private *const d;
};

#endif // KISSTUDIOSTARTPAGE_H
