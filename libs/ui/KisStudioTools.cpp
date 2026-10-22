/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioTools.h"

#include <KoToolManager.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisViewManager.h"

KoToolManager *KisStudioTools::manager(KisMainWindow *window)
{
    if (!window) window = KisPart::instance()->currentMainwindow();
    return window && window->viewManager() ? window->viewManager()->toolManager() : nullptr;
}

QString KisStudioTools::activeToolId(KisMainWindow *window)
{
    KoToolManager *tools = manager(window);
    return tools ? tools->activeToolId() : QString();
}

void KisStudioTools::switchTo(const QString &id, KisMainWindow *window)
{
    if (KoToolManager *tools = manager(window)) tools->switchToolRequested(id);
}
