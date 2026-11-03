/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioCanvasSize.h"

#include <QPointer>
#include <QTimer>

#include <KoToolBase.h>
#include <KoToolManager.h>

#include "KisStudioTools.h"
#include "KisViewManager.h"
#include "kis_action.h"
#include "kis_action_manager.h"
#include "kis_canvas2.h"

namespace KisStudioCanvasSize
{

void start(KisViewManager *view)
{
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    KoToolManager *tools = view ? KisStudioTools::manager(view->mainWindow()) : nullptr;
    if (!canvas || !canvas->canvasWidget() || !tools) return;
    const QString returnTo = tools->activeToolId();

    // the whole canvas with room around it to pull its edges into: fitted between the bars and panels, two steps back
    for (const char *name : {"zoom_to_fit", "view_zoom_out", "view_zoom_out"}) {
        if (KisAction *action = view->actionManager()->actionByName(QLatin1String(name))) action->trigger();
    }

    tools->switchToolRequested(QStringLiteral("KisToolCrop"));
    // the tool is active once the switch is through
    QPointer<KisCanvas2> alive(canvas);
    QPointer<KoToolManager> manager(tools);
    QTimer::singleShot(0, canvas, [alive, manager, returnTo]() {
        if (!alive || !manager) return;
        if (KoToolBase *tool = manager->toolById(alive, QStringLiteral("KisToolCrop"))) {
            QMetaObject::invokeMethod(tool, "beginCanvasResize", Q_ARG(QString, returnTo));
        }
    });
}

} // namespace KisStudioCanvasSize
