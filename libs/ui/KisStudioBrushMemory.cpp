/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushMemory.h"

#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisStrokeLog.h"
#include "KisStudioBrushPicker.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_floating_message.h"
#include "kis_icon_utils.h"
#include "kis_node.h"
#include "kis_node_manager.h"
#include "kis_paintop_preset.h"

KisStudioBrushMemory::KisStudioBrushMemory(KisMainWindow *mainWindow)
    : QObject(mainWindow)
    , m_mainWindow(mainWindow)
{
    if (KisViewManager *view = mainWindow->viewManager()) {
        connect(view->nodeManager(), &KisNodeManager::sigUserActivatedNode, this, &KisStudioBrushMemory::layerChosen);
    }
}

bool KisStudioBrushMemory::layersRememberBrushes()
{
    return KisConfig(true).readEntry<bool>(QStringLiteral("StudioLayersRememberBrushes"), true);
}

void KisStudioBrushMemory::setLayersRememberBrushes(bool on)
{
    KisConfig(false).writeEntry(QStringLiteral("StudioLayersRememberBrushes"), on);
}

void KisStudioBrushMemory::layerChosen(KisNodeSP node)
{
    KisViewManager *view = m_mainWindow->viewManager();
    if (!node || !view || !layersRememberBrushes() || !KisStudioBrushPicker::paintToolActive()) {
        return;
    }
    KisStrokeLog::Stroke stroke;
    KisCanvasResourceProvider *resources = view->canvasResourceProvider();
    if (!node->strokeLog().lastBrush(&stroke) || !resources || !resources->currentPreset()) {
        return;
    }
    if (resources->currentPreset()->name() == stroke.brush) {
        return;
    }
    KisStudioBrushPicker::use(view, stroke.brush);
    if (resources->currentPreset() && resources->currentPreset()->name() == stroke.brush) {
        resources->setSize(stroke.brushSize);
        view->showFloatingMessage(i18nc("@info a brush taken from the layer just chosen, %1 its name", "%1 · this layer's brush",
                                        KisStudioBrushPicker::title(stroke.brush)),
                                  KisIconUtils::loadIcon(QStringLiteral("paintop_settings_01")), 1600, KisFloatingMessage::Low);
    }
}
