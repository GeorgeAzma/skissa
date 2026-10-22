/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHSHEET_H
#define KISSTUDIOBRUSHSHEET_H

#include "widgets/KisStudioPopup.h"

#include <memory>

class KisMainWindow;

/**
 * The whole brush on one screen, to tune it and to share it as a picture: a column for each part
 * (its pictures, then every setting, the ones that are off quiet), and below a grid of which pen
 * input drives which setting, each with its curve.
 */
class KRITAUI_EXPORT KisStudioBrushSheet : public KisStudioPopup
{
    Q_OBJECT
public:
    /// Opens the sheet over \p mainWindow, or closes it when it is open
    static void toggle(KisMainWindow *mainWindow);
    ~KisStudioBrushSheet() override;

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    explicit KisStudioBrushSheet(KisMainWindow *mainWindow);
    void rebuild();
    void updateHeader();
    void copyPicture();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOBRUSHSHEET_H
