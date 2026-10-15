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
 * What the window shows with no image open, in the Studio layout: new images
 * as pictures of their shape, the recent ones as large thumbnails, and Open.
 * Files dropped anywhere on it open.
 */
class KRITAUI_EXPORT KisStudioStartPage : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioStartPage(KisMainWindow *mainWindow);
    ~KisStudioStartPage() override;

    /// The new image tiles: a size makes the image at once, Custom opens the full dialog; \p picked runs first
    static QWidget *createNewTiles(KisMainWindow *mainWindow, QWidget *parent, std::function<void()> picked = {});

protected:
    void paintEvent(QPaintEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    struct Private;
    Private *const d;
};

#endif // KISSTUDIOSTARTPAGE_H
