/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPANELPICKER_H
#define KISSTUDIOPANELPICKER_H

#include <functional>

#include <QIcon>

#include "widgets/KisStudioPicker.h"
#include "kritaui_export.h"

class QDockWidget;
class KisMainWindow;

/**
 * Every panel, as it last looked on screen (kept between sessions). The
 * panels on the rail come first; starring or reordering them changes the rail.
 */
class KRITAUI_EXPORT KisStudioPanelPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    struct Rail {
        /// The kritarc key of the rail's panels
        QString key;
        std::function<QStringList()> defaults;
        /// Shown for a panel with no picture yet
        std::function<QIcon(QDockWidget *dock)> icon;
    };

    KisStudioPanelPicker(KisMainWindow *mainWindow, const Rail &rail);

    /// Keeps how the panel looks now (call while it is on screen)
    static void rememberLook(QDockWidget *dock);
};

#endif // KISSTUDIOPANELPICKER_H
