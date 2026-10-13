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
 * Picks panels by a snapshot of how each last looked on screen, cached between
 * sessions. Panels on the rail come first; starring or reordering them updates the rail.
 */
class KRITAUI_EXPORT KisStudioPanelPicker : public KisStudioPicker
{
    Q_OBJECT
public:
    struct Rail {
        /// The kritarc key that stores the rail's panels
        QString key;
        std::function<QStringList()> defaults;
        /// Icon for a panel that has no snapshot yet
        std::function<QIcon(QDockWidget *dock)> icon;
    };

    KisStudioPanelPicker(KisMainWindow *mainWindow, const Rail &rail);

    /// Takes a snapshot of \p dock; call while it is on screen.
    static void rememberLook(QDockWidget *dock);
};

#endif // KISSTUDIOPANELPICKER_H
