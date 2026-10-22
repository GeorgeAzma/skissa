/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTOOLS_H
#define KISSTUDIOTOOLS_H

#include <QString>

#include "kritaui_export.h"

class KisMainWindow;
class KoToolManager;

/// The tools of a window: Krita keeps a tool manager per window (its view manager's)
namespace KisStudioTools
{
/// The tool manager of \p window, or of the window in use; null when there is none
KRITAUI_EXPORT KoToolManager *manager(KisMainWindow *window = nullptr);
/// The id of the tool in use there; empty when there is no window
KRITAUI_EXPORT QString activeToolId(KisMainWindow *window = nullptr);
/// Switches that window to the tool \p id
KRITAUI_EXPORT void switchTo(const QString &id, KisMainWindow *window = nullptr);
}

#endif // KISSTUDIOTOOLS_H
