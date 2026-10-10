/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPANELPICKER_H
#define KISSTUDIOPANELPICKER_H

#include <functional>
#include <memory>

#include <QIcon>

#include "widgets/KisStudioPicker.h"
#include "kritaui_export.h"

class QDockWidget;
class KisMainWindow;
struct KisStudioPanelPickerState;

/**
 * All the panels, each as it looks: a picture of it from the last time it
 * was on screen, kept between sessions (one never opened shows its icon). The panels on the rail are yours, first and in the rail's
 * order: starring, unstarring and reordering them here changes the rail.
 */
class KRITAUI_EXPORT KisStudioPanelPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    struct Rail {
        /// Where the rail's panels are kept (kritarc)
        QString key;
        std::function<QStringList()> defaults;
        /// A panel's icon, for one with no picture yet
        std::function<QIcon(QDockWidget *dock)> icon;
    };

    KisStudioPanelPicker(KisMainWindow *mainWindow, const Rail &rail);
    ~KisStudioPanelPicker() override;

    /// Keeps how the panel looks now, for the picker to show (call while it is laid out on screen)
    static void rememberLook(QDockWidget *dock);

private:
    explicit KisStudioPanelPicker(std::shared_ptr<KisStudioPanelPickerState> state);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOPANELPICKER_H
