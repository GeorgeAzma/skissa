/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSKISSAUPDATER_H
#define KISSKISSAUPDATER_H

#include <QString>

#include "kritaui_export.h"

class KisMainWindow;

/**
 * A release keeps itself up to date: it looks for a newer release on the homepage now and then,
 * downloads and unpacks it beside its own folder, and the two folders swap once the app has quit
 * (the previous one kept as "<folder>.old"). Settings, brushes and autosaves live elsewhere, and
 * anything the user put in the program's folder moves along.
 */
namespace KisSkissaUpdater
{
/// This version, as released
KRITAUI_EXPORT QString version();
/// Whether this copy can update itself: a release unpacked from its zip, never a dev build (Windows)
KRITAUI_EXPORT bool canUpdate();
/// When a downloaded update waits, hands it to the swap, which then starts the new version; this start ends at once
KRITAUI_EXPORT bool startWaitingUpdate();
/// Adds Check for updates and Restart to update to \p window, and starts looking (once)
KRITAUI_EXPORT void addActions(KisMainWindow *window);
/// Closes the windows (asking about unsaved images) and starts again, updated when an update is ready
KRITAUI_EXPORT void restart();
}

#endif // KISSKISSAUPDATER_H
