/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KisStudioEffectBrush.h"

#include <QAction>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QTimer>
#include <QToolButton>

#include <KoColorSpaceRegistry.h>
#include <filter/kis_filter.h>
#include <filter/kis_filter_configuration.h>
#include <filter/kis_filter_registry.h>
#include <kactioncollection.h>
#include <kis_command_utils.h>
#include <kis_filter_mask.h>
#include <kis_icon_utils.h>
#include <kis_image.h>
#include <commands/kis_image_layer_remove_command.h>
#include <kis_layer.h>
#include <kis_paint_layer.h>
#include <kis_painter.h>
#include <kis_paint_device.h>
#include <kis_pixel_selection.h>
#include <kis_selection.h>
#include <kis_stroke_strategy_undo_command_based.h>
#include <kis_transaction.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisModernStyle.h"
#include "KisStudioGlass.h"
#include "KisStudioLayout.h"
#include "KisStudioTools.h"
#include "KisViewManager.h"
#include "kis_canvas_resource_provider.h"
#include "kis_node_commands_adapter.h"
#include "kis_node_manager.h"

struct KisStudioEffectBrush::Private {
    QPointer<KisMainWindow> mainWindow;
    KisLayerSP layer;
    KisFilterMaskSP mask;
    /// The paint color before painting the effect
    KoColor color;
    bool finished {false};
    /// The mask has been picked: picking another from now on keeps it
    bool entered {false};
};

void KisStudioEffectBrush::start(KisMainWindow *mainWindow, KisLayerSP layer, KisFilterConfigurationSP config, const QString &name)
{
    KisViewManager *view = mainWindow ? mainWindow->viewManager() : nullptr;
    KisImageSP image = view ? view->image() : nullptr;
    if (!image || !layer || !config) return;
    KisFilterMaskSP mask = new KisFilterMask(image, name);
    // showing nowhere until painted
    mask->initSelection(layer);
    mask->selection()->pixelSelection()->clear();
    mask->selection()->updateProjection();
    mask->setFilter(config->cloneWithResourcesSnapshot());
    if (!layer->allowAsChild(mask)) return;
    KisNodeCommandsAdapter adapter(view);
    adapter.addNode(mask, layer, layer->lastChild());
    // picked once the layer panel has it: after the adding is done and its queued signals are through
    view->blockUntilOperationsFinished(image);
    auto *bar = new KisStudioEffectBrush(mainWindow, layer, mask, name);
    bar->show();
}

KisStudioEffectBrush::KisStudioEffectBrush(KisMainWindow *mainWindow, KisLayerSP layer, KisFilterMaskSP mask, const QString &name)
    : QWidget(mainWindow->centralWidget())
    , d(new Private)
{
    d->mainWindow = mainWindow;
    d->layer = layer;
    d->mask = mask;
    setObjectName(QStringLiteral("studioEffectBrush"));
    setAttribute(Qt::WA_DeleteOnClose);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(KisModernStyle::space3, KisModernStyle::space1, KisModernStyle::space1, KisModernStyle::space1);
    row->setSpacing(KisModernStyle::hairline);
    auto *title = new QLabel(name, this);
    QFont strong = title->font();
    strong.setWeight(QFont::DemiBold);
    title->setFont(strong);
    title->setAttribute(Qt::WA_TransparentForMouseEvents);
    row->addWidget(title);
    row->addSpacing(KisModernStyle::space2);
    auto button = [this, row](const QString &icon, const QString &tip, void (KisStudioEffectBrush::*done)()) {
        auto *b = new QToolButton(this);
        b->setIcon(KisIconUtils::loadIcon(icon));
        b->setToolTip(tip);
        b->setAutoRaise(true);
        b->setFocusPolicy(Qt::NoFocus);
        b->setFixedSize(28, 26);
        connect(b, &QToolButton::clicked, this, [this, done]() { (this->*done)(); });
        KisStudioGlass::setSeeThrough(b, true);
        row->addWidget(b);
        return b;
    };
    button(QStringLiteral("studio_cancel"), i18nc("@info:tooltip take the painted effect away", "Drop"), &KisStudioEffectBrush::drop);
    button(QStringLiteral("filterMask"), i18nc("@info:tooltip leave the painted effect as a mask to edit later", "Keep as a mask"), &KisStudioEffectBrush::keep);
    QToolButton *apply = button(QStringLiteral("studio_done"), i18nc("@info:tooltip make the painted effect the layer's pixels", "Apply"), &KisStudioEffectBrush::apply);
    // only a paint layer has pixels to take it
    apply->setEnabled(qobject_cast<KisPaintLayer *>(layer.data()));

    // painting with the brush, white: where the effect shows
    KisViewManager *view = mainWindow->viewManager();
    KisStudioTools::switchTo(QStringLiteral("KritaShape/KisToolBrush"));
    if (QAction *erase = mainWindow->actionCollection()->action(QStringLiteral("erase_action"))) erase->setChecked(false);
    if (KisCanvasResourceProvider *resources = view->canvasResourceProvider()) {
        d->color = resources->fgColor();
        resources->setFGColor(KoColor(Qt::white, KoColorSpaceRegistry::instance()->rgb8()));
    }
    // another layer picked (or the mask taken back by an undo): it is kept as it is
    connect(view->nodeManager(), &KisNodeManager::sigNodeActivated, this, [this](KisNodeSP node) {
        if (node.data() == d->mask.data()) d->entered = true;
        else if (d->entered) finish(false);
    });
    QTimer::singleShot(0, this, [this]() {
        KisViewManager *view = d->mainWindow ? d->mainWindow->viewManager() : nullptr;
        if (!view || !d->mask->parent()) return;
        if (view->activeNode().data() == d->mask.data()) d->entered = true;
        else view->nodeManager()->slotNonUiActivatedNode(d->mask);
    });
    connect(mainWindow, &KisMainWindow::activeViewChanged, this, [this]() { finish(false); });

    parentWidget()->installEventFilter(this);
    adjustSize();
    place();
    KisStudioLayout::keepAbove(this);
}

KisStudioEffectBrush::~KisStudioEffectBrush()
{
    delete d;
}

void KisStudioEffectBrush::apply()
{
    KisViewManager *view = d->mainWindow ? d->mainWindow->viewManager() : nullptr;
    KisImageSP image = view ? view->image() : nullptr;
    KisFilterConfigurationSP config = d->mask->filter();
    KisFilterSP filter = config ? KisFilterRegistry::instance()->value(config->name()) : nullptr;
    auto *layer = qobject_cast<KisPaintLayer *>(d->layer.data());
    if (!image || !filter || !layer) return;
    KisSelectionSP selection = d->mask->selection();
    const QRect rect = selection->selectedExactRect();
    KisNodeSP node = d->layer;
    KisNodeSP mask = d->mask;
    finish(true);

    // the effect where it was painted, as the layer's pixels, and the mask gone: one step
    KisStrokeStrategyUndoCommandBased *strategy = new KisStrokeStrategyUndoCommandBased(kundo2_i18n("Apply Effect"), false, image.data());
    KisStrokeId stroke = image->startStroke(strategy);
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(
                              KUndo2CommandSP(new KisCommandUtils::LambdaCommand([node, filter, config, selection, rect]() mutable -> KUndo2Command * {
                                  KisPaintDeviceSP device = node->paintDevice();
                                  if (!device || rect.isEmpty()) return nullptr;
                                  KisTransaction transaction(device);
                                  KisPaintDeviceSP result = new KisPaintDevice(*device);
                                  filter->process(device, result, selection, rect, config);
                                  KisPainter::copyAreaOptimized(rect.topLeft(), result, device, rect);
                                  node->setDirty(rect);
                                  return transaction.endAndTake();
                              })),
                              false, KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->addJob(stroke, new KisStrokeStrategyUndoCommandBased::Data(KUndo2CommandSP(new KisImageLayerRemoveCommand(image, mask)), false,
                                                                     KisStrokeJobData::SEQUENTIAL, KisStrokeJobData::EXCLUSIVE));
    image->endStroke(stroke);
}

void KisStudioEffectBrush::keep()
{
    finish(true);
}

void KisStudioEffectBrush::drop()
{
    KisViewManager *view = d->mainWindow ? d->mainWindow->viewManager() : nullptr;
    KisNodeSP mask = d->mask;
    finish(true);
    if (view && mask && mask->parent()) {
        KisNodeCommandsAdapter adapter(view);
        adapter.removeNode(mask);
    }
}

void KisStudioEffectBrush::finish(bool pickLayer)
{
    if (d->finished) return;
    d->finished = true;
    KisViewManager *view = d->mainWindow ? d->mainWindow->viewManager() : nullptr;
    if (view) {
        // the color back, unless one was picked meanwhile
        KisCanvasResourceProvider *resources = view->canvasResourceProvider();
        if (resources && resources->fgColor().toQColor() == QColor(Qt::white)) resources->setFGColor(d->color);
        if (pickLayer && d->layer && d->layer->image()) view->nodeManager()->slotNonUiActivatedNode(d->layer);
    }
    close();
}

void KisStudioEffectBrush::place()
{
    QWidget *room = parentWidget();
    if (!room) return;
    // over the bottom of the canvas, clear of the zoom pill
    move((room->width() - width()) / 2, room->height() - height() - 72);
}

bool KisStudioEffectBrush::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == parentWidget() && event->type() == QEvent::Resize) place();
    return QWidget::eventFilter(watched, event);
}

void KisStudioEffectBrush::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF pill = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    const QPainterPath shape = KisStudioGlass::paint(painter, this, pill, pill.height() / 2);
    QColor border = palette().color(QPalette::WindowText);
    border.setAlphaF(0.14);
    KisStudioGlass::paintBorder(painter, this, shape, border);
}
