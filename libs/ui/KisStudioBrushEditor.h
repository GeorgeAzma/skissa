/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBRUSHEDITOR_H
#define KISSTUDIOBRUSHEDITOR_H

#include "widgets/KisStudioPopup.h"

#include <memory>

class KisMainWindow;
class KisPaintOpOption;
class KisPaintOpSettingsWidget;
class KisStudioValueRow;

/**
 * The quick brush editor: a test stroke, the brush's parts as tabs (Tip, Mask, Texture, Color),
 * each with its pictures and the settings in use as rows. Dragging a row changes it, clicking it
 * shows how the pen drives it, and Everything opens the full editor.
 */
class KRITAUI_EXPORT KisStudioBrushEditor : public KisStudioPopup
{
    Q_OBJECT
public:
    /// Opens the editor beside \p anchor (on its \p edge side), or closes it when it is open
    static void toggle(KisMainWindow *mainWindow, QWidget *anchor, Qt::Edge edge);
    /// Whether the editor is open in \p mainWindow
    static bool isOpen(KisMainWindow *mainWindow);

    /// The current brush's settings in \p mainWindow
    static KisPaintOpSettingsWidget *brushSettings(KisMainWindow *mainWindow);
    /// The current brush's setting called \p id (KisPaintOpOption::id()), if it has one
    static KisPaintOpOption *brushOption(KisMainWindow *mainWindow, const QString &id);
    /// The key that pins \p option to the top bar
    static QString pinKey(KisPaintOpOption *option);
    /// Keeps \p row showing \p option's value and changing it
    static void bindRow(KisStudioValueRow *row, KisPaintOpOption *option, KisPaintOpSettingsWidget *settings);

    ~KisStudioBrushEditor() override;

protected:
    void showEvent(QShowEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    explicit KisStudioBrushEditor(KisMainWindow *mainWindow);
    void rebuild();
    void showPart(int part);
    void updateHeader();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOBRUSHEDITOR_H
