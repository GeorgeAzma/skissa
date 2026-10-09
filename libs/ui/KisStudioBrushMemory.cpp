/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioBrushMemory.h"

#include <QApplication>
#include <QMouseEvent>
#include <QRegularExpression>
#include <QTabletEvent>
#include <QtMath>

#include <KoToolManager.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisStrokeLog.h"
#include "KisStudioBrushPicker.h"
#include "KisViewManager.h"
#include "canvas/kis_canvas2.h"
#include "kis_canvas_resource_provider.h"
#include "kis_config.h"
#include "kis_coordinates_converter.h"
#include "kis_floating_message.h"
#include "kis_icon_utils.h"
#include "kis_image.h"
#include "kis_node.h"
#include "kis_node_manager.h"
#include "kis_paintop_preset.h"

namespace {

const QString settingKey = QStringLiteral("StudioLayersRememberBrushes");

/// The tools that paint with the brush: the ones a brush taken from the canvas or a layer is for
bool paintToolActive()
{
    static const QStringList tools = {
        QStringLiteral("KritaShape/KisToolBrush"), QStringLiteral("KritaShape/KisToolDyna"),
        QStringLiteral("KritaShape/KisToolMultiBrush"), QStringLiteral("KritaShape/KisToolLine"),
    };
    return tools.contains(KoToolManager::instance()->activeToolId());
}

/// "d) Ink-2 Fineliner" -> "Ink-2 Fineliner"
QString titleOf(const QString &brush)
{
    static const QRegularExpression sortPrefix(QStringLiteral("^[a-z]\\)\\s+"));
    return QString(brush).remove(sortPrefix);
}

} // namespace

KisStudioBrushMemory::KisStudioBrushMemory(KisMainWindow *mainWindow)
    : QObject(mainWindow)
    , m_mainWindow(mainWindow)
{
    qApp->installEventFilter(this);
    if (KisViewManager *view = mainWindow->viewManager()) {
        connect(view->nodeManager(), &KisNodeManager::sigUserActivatedNode, this, &KisStudioBrushMemory::layerChosen);
    }
}

KisStudioBrushMemory::~KisStudioBrushMemory()
{
    qApp->removeEventFilter(this);
}

bool KisStudioBrushMemory::layersRememberBrushes()
{
    return KisConfig(true).readEntry<bool>(settingKey, true);
}

void KisStudioBrushMemory::setLayersRememberBrushes(bool on)
{
    KisConfig(false).writeEntry(settingKey, on);
}

void KisStudioBrushMemory::layerChosen(KisNodeSP node)
{
    KisViewManager *view = m_mainWindow->viewManager();
    if (!node || !view || !layersRememberBrushes() || !paintToolActive()) {
        return;
    }
    KisStrokeLog::Stroke stroke;
    KisCanvasResourceProvider *resources = view->canvasResourceProvider();
    if (!node->strokeLog().lastBrush(&stroke) || !resources || !resources->currentPreset()) {
        return;
    }
    // already in hand: nothing to say
    if (resources->currentPreset()->name() == stroke.brush) {
        return;
    }
    KisStudioBrushPicker::use(view, stroke.brush);
    if (resources->currentPreset() && resources->currentPreset()->name() == stroke.brush) {
        resources->setSize(stroke.brushSize);
        view->showFloatingMessage(i18nc("@info a brush taken from the layer just chosen, %1 its name", "%1 · this layer's brush",
                                        titleOf(stroke.brush)),
                                  KisIconUtils::loadIcon(QStringLiteral("paintop_settings_01")), 1600, KisFloatingMessage::Low);
    }
}

bool KisStudioBrushMemory::pickAt(const QPointF &widgetPos)
{
    KisViewManager *view = m_mainWindow->viewManager();
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    KisImageSP image = view ? KisImageSP(view->image()) : KisImageSP();
    if (!canvas || !image) {
        return false;
    }
    // the pixel under the pointer: where it is in the image (the view may be rotated, mirrored, zoomed)
    const QPointF imagePoint = canvas->coordinatesConverter()->widgetToImage(widgetPos);
    const QPoint imagePos(qFloor(imagePoint.x()), qFloor(imagePoint.y()));
    KisStrokeLog::Pick pick;
    if (!KisStrokeLog::pick(image->root(), imagePos, &pick)) {
        return false;
    }
    const KisStrokeLog::Stroke &stroke = pick.stroke;
    KisStudioBrushPicker::use(view, stroke.brush);
    KisCanvasResourceProvider *resources = view->canvasResourceProvider();
    if (!resources || !resources->currentPreset() || resources->currentPreset()->name() != stroke.brush) {
        // the brush is gone (removed, renamed): nothing taken, but what it was is said
        view->showFloatingMessage(i18nc("@info a brush found on the canvas that is no longer there, %1 its name", "%1 · not found",
                                        titleOf(stroke.brush)),
                                  KisIconUtils::loadIcon(QStringLiteral("paintop_settings_01")), 1400, KisFloatingMessage::Low);
        return true;
    }
    resources->setSize(stroke.brushSize);
    view->showFloatingMessage(i18nc("@info a brush taken from the canvas, %1 its name, %2 its size", "%1 · %2 px",
                                    titleOf(stroke.brush), QString::number(stroke.brushSize, 'f', stroke.brushSize < 10 ? 1 : 0)),
                              KisIconUtils::loadIcon(QStringLiteral("paintop_settings_01")), 1400, KisFloatingMessage::Low);
    return true;
}

bool KisStudioBrushMemory::eventFilter(QObject *watched, QEvent *event)
{
    const QEvent::Type type = event->type();
    if (type != QEvent::MouseButtonPress && type != QEvent::TabletPress && type != QEvent::MouseMove
        && type != QEvent::TabletMove && type != QEvent::MouseButtonRelease && type != QEvent::TabletRelease
        && type != QEvent::MouseButtonDblClick) {
        return false;
    }
    KisViewManager *view = m_mainWindow->viewManager();
    KisCanvas2 *canvas = view ? view->canvasBase() : nullptr;
    if (!canvas || watched != canvas->canvasWidget()) {
        return false;
    }

    // the rest of a press that took a brush
    if (m_swallowing) {
        if (type == QEvent::MouseButtonRelease || type == QEvent::TabletRelease) {
            m_swallowing = false;
        }
        event->accept();
        return true;
    }

    if (type != QEvent::MouseButtonPress && type != QEvent::TabletPress) {
        return false;
    }
    QPointF pos;
    Qt::KeyboardModifiers modifiers;
    if (type == QEvent::TabletPress) {
        auto *tablet = static_cast<QTabletEvent *>(event);
        if (tablet->button() != Qt::LeftButton) return false;
        pos = tablet->posF();
        modifiers = tablet->modifiers();
    } else {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton) return false;
        pos = mouse->localPos();
        modifiers = mouse->modifiers();
    }
    // Alt alone, with a tool that paints: never a dab, whether there is a brush to take or not
    if (modifiers != Qt::AltModifier || !paintToolActive()) {
        return false;
    }
    if (!pickAt(pos)) {
        view->showFloatingMessage(i18nc("@info Alt+click on the canvas found no brush stroke to take a brush from", "No brush here"),
                                  KisIconUtils::loadIcon(QStringLiteral("paintop_settings_01")), 1000, KisFloatingMessage::Low);
    }
    m_swallowing = true;
    event->accept();
    return true;
}
