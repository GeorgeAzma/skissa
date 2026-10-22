/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISDEVHOOKS_H
#define KISDEVHOOKS_H

class QCoreApplication;

/**
 * Developer-only test hooks (KRITA_DEV_PROFILE), driven by posted window messages: KritaDevCapture, KritaDevMouse,
 * KritaDevKey, KritaDevExport, KritaDevTree, KritaDevShortcuts and KritaDevColorDrop. Their output goes to TEMP.
 */
namespace KisDevHooks
{
void install(QCoreApplication &app);
}

#endif // KISDEVHOOKS_H
