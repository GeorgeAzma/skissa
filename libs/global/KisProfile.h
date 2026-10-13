/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISPROFILE_H
#define KISPROFILE_H

#include <QString>

#include "kritaglobal_export.h"

/**
 * Where this fork keeps what it writes, apart from a regular Krita's: settings, logs,
 * resources and autosaves. Nothing a regular Krita writes is read or written here.
 *
 * - config files and logs: <LocalAppData>/<name>/ instead of loose in <LocalAppData>
 * - resources and everything else Qt calls app data: <AppData>/<name>/krita, as the
 *   organization name (see apply())
 * - unsaved images' autosaves: <Temp>/<name>
 *
 * The dev profile (KRITA_DEV_PROFILE) keeps the old layout inside Qt's test folders, so its
 * settings stay where they are; KRITA_DEV_APART gives it this layout there instead, for testing.
 */
namespace KisProfile
{
/// The fork's name, also the name of its folders
KRITAGLOBAL_EXPORT QString name();

/// Whether this run keeps its own folders (always, except the dev profile's old layout)
KRITAGLOBAL_EXPORT bool isApart();

/// The folder for config files (kritarc, kritadisplayrc, kritashortcutsrc…) and logs
KRITAGLOBAL_EXPORT QString configFolder();

/// \p fileName in configFolder()
KRITAGLOBAL_EXPORT QString configFile(const QString &fileName);

/// The folder for autosaves of images never saved
KRITAGLOBAL_EXPORT QString tempFolder();

/// Points Qt's app data at the fork's own folder; call once the application's data is set
KRITAGLOBAL_EXPORT void apply();
}

#endif // KISPROFILE_H
