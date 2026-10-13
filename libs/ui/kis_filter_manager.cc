/*
 *  SPDX-FileCopyrightText: 2007 Boudewijn Rempt <boud@valdyas.org>
 *  SPDX-FileCopyrightText: 2007 Cyrille Berger <cberger@cberger.net>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "kis_filter_manager.h"
#include <KisProfile.h>
#include "KisUpdatePacer.h"


#include <QHash>
#include <KisSignalMapper.h>

#include <QMessageBox>
#include <kactionmenu.h>
#include <kactioncollection.h>

#include <KoID.h>
#include <KisMainWindow.h>

// krita/image
#include <filter/kis_filter.h>
#include <filter/kis_filter_registry.h>
#include <filter/kis_filter_configuration.h>
#include <kis_paint_device.h>
#include <kis_paint_device_frames_interface.h>
#include <kis_image_animation_interface.h>
#include <kis_raster_keyframe_channel.h>
#include <kis_time_span.h>
#include <kis_image_config.h>
#include <KisAnimAutoKey.h>

// krita/ui
#include "KisViewManager.h"
#include <cmath>

#include "kis_canvas2.h"
#include "kis_coordinates_converter.h"
#include <kis_bookmarked_configuration_manager.h>

#include "kis_action.h"
#include "kis_action_manager.h"
#include "kis_canvas_resource_provider.h"
#include "dialogs/kis_dlg_filter.h"
#include "strokes/kis_filter_stroke_strategy.h"
#include "krita_utils.h"
#include "kis_icon_utils.h"
#include "kis_layer_utils.h"
#include <KisGlobalResourcesInterface.h>

struct KisFilterManager::Private {
    KisAction* reapplyAction = nullptr;
    KisAction* reapplyActionReprompt = nullptr;
    QHash<QString, KActionMenu*> filterActionMenus;
    QHash<KisFilter*, QAction *> filters2Action;
    KisKActionCollection *actionCollection = nullptr;
    KisActionManager *actionManager = nullptr;
    KisViewManager *view = nullptr;

    KisFilterConfigurationSP lastConfiguration;
    /// the running preview's coarse pass and full pass (see KisFilterStrokeStrategy::setProgressivePreview)
    KisFilterStrokeStrategy::PreviewHandleSP preview;
    KisFilterConfigurationSP currentlyAppliedConfiguration;
    KisStrokeId currentStrokeId;
    KisFilterStrokeStrategy::ExternalCancelUpdatesStorageSP externalCancelUpdatesStorage;
    KisFilterStrokeStrategy::IdleBarrierData::IdleBarrierCookie idleBarrierCookie;

    bool filterAllSelectedFrames = false;

    KisSignalMapper actionsMapper;

    /*!
     * \brief The filter dialog shown to the user
     * \note parent QWidget is set to mainwindow, so we delegate deletion of this widget to Qt (we don't `delete` it ourselves)
     */
    KisDlgFilter *filterDialog = nullptr;

    /// Replaces the dialog when set, e.g. by the studio layout's overlay on the canvas
    KisFilterManager::InlineEditor inlineEditor;
    /// The image the running preview stroke was started on
    KisImageWSP strokeImage;
};

KisFilterManager::KisFilterManager(KisViewManager * view)
    : d(new Private)
{
    d->view = view;
}

KisFilterManager::~KisFilterManager()
{
}

void KisFilterManager::setView(QPointer<KisView>imageView)
{
    Q_UNUSED(imageView);
}


void KisFilterManager::setup(KisKActionCollection * ac, KisActionManager *actionManager)
{
    d->actionCollection = ac;
    d->actionManager = actionManager;

    // Setup reapply action
    d->reapplyAction = d->actionManager->createAction("filter_apply_again");
    d->reapplyAction->setActivationFlags(KisAction::ACTIVE_DEVICE);
    d->reapplyAction->setEnabled(false);

    d->reapplyActionReprompt = d->actionManager->createAction("filter_apply_reprompt");
    d->reapplyActionReprompt->setActivationFlags(KisAction::ACTIVE_DEVICE);
    d->reapplyActionReprompt->setEnabled(false);

    connect(d->reapplyAction, SIGNAL(triggered()), SLOT(reapplyLastFilter()));
    connect(d->reapplyActionReprompt, SIGNAL(triggered()), SLOT(reapplyLastFilterReprompt()));

    connect(&d->actionsMapper, SIGNAL(mapped(QString)), SLOT(showFilterDialog(QString)));

    // Setup list of filters
    QStringList keys = KisFilterRegistry::instance()->keys();
    keys.sort();
    Q_FOREACH (const QString &filterName, keys) {
        insertFilter(filterName);
    }

    connect(KisFilterRegistry::instance(), SIGNAL(filterAdded(QString)), SLOT(insertFilter(QString)));
}

void KisFilterManager::insertFilter(const QString & filterName)
{
    Q_ASSERT(d->actionCollection);

    KisFilterSP filter = KisFilterRegistry::instance()->value(filterName);
    Q_ASSERT(filter);

    if (d->filters2Action.contains(filter.data())) {
        warnKrita << "Filter" << filterName << " has already been inserted";
        return;
    }

    KoID category = filter->menuCategory();
    KActionMenu* actionMenu = d->filterActionMenus[ category.id()];
    if (!actionMenu) {
        actionMenu = new KActionMenu(category.name(), this);
        d->actionCollection->addAction(category.id(), actionMenu);
        d->filterActionMenus[category.id()] = actionMenu;
    }

    KisAction *action = new KisAction(filter->menuEntry(), this);
    action->setDefaultShortcut(filter->shortcut());
    action->setActivationFlags(KisAction::ACTIVE_DEVICE);

    d->actionManager->addAction(QString("krita_filter_%1").arg(filterName), action);
    d->filters2Action[filter.data()] = action;

    actionMenu->addAction(action);

    d->actionsMapper.setMapping(action, filterName);
    connect(action, SIGNAL(triggered()), &d->actionsMapper, SLOT(map()));
}

void KisFilterManager::updateGUI()
{
    if (!d->view) return;

    bool enable = false;

    KisNodeSP activeNode = d->view->activeNode();
    enable = activeNode && activeNode->hasEditablePaintDevice();

    d->reapplyAction->setEnabled(enable);

    for (QHash<KisFilter*, QAction *>::iterator it = d->filters2Action.begin();
            it != d->filters2Action.end(); ++it) {

        bool localEnable = enable;

        it.value()->setEnabled(localEnable);
    }
}

void KisFilterManager::reapplyLastFilter()
{
    if (!d->lastConfiguration) return;

    apply(d->lastConfiguration);
    finish();
}

void KisFilterManager::reapplyLastFilterReprompt()
{
    if (!d->lastConfiguration) return;

    showFilterDialog(d->lastConfiguration->name(), d->lastConfiguration);
}

void KisFilterManager::setInlineEditor(const InlineEditor &editor)
{
    d->inlineEditor = editor;
}

void KisFilterManager::showFilterDialog(const QString &filterId, KisFilterConfigurationSP overrideDefaultConfig)
{
    if (!d->view->activeNode()->isEditable()) {
        d->view->showFloatingMessage(i18n("Cannot apply filter to locked layer."),
                                      KisIconUtils::loadIcon("object-locked"));
        return;
    }

    if (d->currentStrokeId && !d->filterDialog && d->inlineEditor.reject) {
        d->inlineEditor.reject();
    }

    if (d->filterDialog && d->filterDialog->isVisible()) {
        KisFilterSP filter = KisFilterRegistry::instance()->value(filterId);
        d->filterDialog->setFilter(filter, overrideDefaultConfig);
        return;
    }

    connect(d->view->image(),
            SIGNAL(sigStrokeCancellationRequested()),
            SLOT(slotStrokeCancelRequested()),
            Qt::UniqueConnection);

    connect(d->view->image(),
            SIGNAL(sigStrokeEndRequested()),
            SLOT(slotStrokeEndRequested()),
            Qt::UniqueConnection);

    /**
     * The UI should show only after every running stroke is finished,
     * so a virtual barrier is added here.
     */
    if (!d->view->blockUntilOperationsFinished(d->view->image())) {
        return;
    }

    Q_ASSERT(d->view);
    Q_ASSERT(d->view->activeNode());

    KisPaintDeviceSP dev = d->view->activeNode()->paintDevice();
    if (!dev) {
        warnKrita << "KisFilterManager::showFilterDialog(): Filtering was requested for illegal active layer!" << d->view->activeNode();
        return;
    }

    KisFilterSP filter = KisFilterRegistry::instance()->value(filterId);

    if (dev->colorSpace()->willDegrade(filter->colorSpaceIndependence())) {
        // Warning bells!
        if (filter->colorSpaceIndependence() == TO_LAB16) {
            if (QMessageBox::warning(d->view->mainWindow(),
                                     KisProfile::name(),
                                     i18n("The %1 filter will convert your %2 data to 16-bit L*a*b* and vice versa. ",
                                          filter->name(),
                                          dev->colorSpace()->name()),
                                     QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok)
                    != QMessageBox::Ok) return;

        } else if (filter->colorSpaceIndependence() == TO_RGBA16) {
            if (QMessageBox::warning(d->view->mainWindow(),
                                     KisProfile::name(),
                                     i18n("The %1 filter will convert your %2 data to 16-bit RGBA and vice versa. ",
                                          filter->name() , dev->colorSpace()->name()),
                                     QMessageBox::Ok | QMessageBox::Cancel, QMessageBox::Ok)
                    != QMessageBox::Ok) return;
        }
    }

    if (filter->showConfigurationWidget()) {
        if (d->inlineEditor.open && d->inlineEditor.open(filter, overrideDefaultConfig)) {
            return;
        }
        if (!d->filterDialog) {
            d->filterDialog = new KisDlgFilter(d->view , d->view->activeNode(), this, d->view->mainWindow());
            d->filterDialog->setAttribute(Qt::WA_DeleteOnClose); // make sure that the dialog is deleted when calling `done()`
            connect(d->filterDialog, SIGNAL(finished(int)),
                    this, SLOT(filterDialogHasFinished(int)));
        }

        d->filterDialog->setFilter(filter, overrideDefaultConfig);
        d->filterDialog->setVisible(true);
    } else {
        KisFilterConfigurationSP defaultConfiguration =
            overrideDefaultConfig ? overrideDefaultConfig : filter->defaultConfiguration(KisGlobalResourcesInterface::instance());
        apply(defaultConfiguration);
        finish();
    }
}

void KisFilterManager::apply(KisFilterConfigurationSP _filterConfig)
{
    KisFilterConfigurationSP filterConfig = _filterConfig->cloneWithResourcesSnapshot();
    KisUpdatePacer::log(QStringLiteral("filter asked"));

    KisFilterSP filter = KisFilterRegistry::instance()->value(filterConfig->name());
    KisImageWSP image = d->view->image();

    if (d->currentStrokeId) {
        // the stroke belongs to the image it was started on, which may no longer be the view's
        if (KisImageSP strokeImage = d->strokeImage) {
            strokeImage->cancelStroke(d->currentStrokeId);
        }

        d->currentStrokeId.clear();
        d->idleBarrierCookie.clear();
    } else {
        image->waitForDone();
    }

    if (!d->externalCancelUpdatesStorage) {
        // Lazily initialize the cancel updates storage, just in case
        // if the stroke has been cancelled in the meantime.

        d->externalCancelUpdatesStorage.reset(new KisFilterStrokeStrategy::ExternalCancelUpdatesStorage());
    }

    KoCanvasResourceProvider *resourceManager =
        d->view->canvasResourceProvider()->resourceManager();

    KisResourcesSnapshotSP resources =
        new KisResourcesSnapshot(image,
                                 d->view->activeNode(),
                                 resourceManager);

    KisFilterStrokeStrategy *strategy = new KisFilterStrokeStrategy(filter,
                                                                    KisFilterConfigurationSP(filterConfig),
                                                                    resources,
                                                                    d->externalCancelUpdatesStorage.toWeakRef());
    {
        KConfigGroup group( KSharedConfig::openConfig(), "filterdialog");
        strategy->setForceLodModeIfPossible(group.readEntry("forceLodMode", true));
    // at full resolution, a coarse pass first (shown at once), then the full one in its place. No coarser than
    // one level below what the canvas shows: a much coarser pass looks far softer than the result, which then
    // seems to undo the effect when it lands
    int coarsest = 6;
    if (KisCanvas2 *canvas = d->view->canvasBase()) {
        const qreal zoom = canvas->coordinatesConverter()->effectivePhysicalZoom();
        if (zoom > 0) coarsest = qMax(0, int(std::floor(std::log2(1.0 / zoom)))) + 1;
    }
    d->preview = strategy->setProgressivePreview(true, coarsest);
    }

    d->currentStrokeId =
        image->startStroke(strategy);
    d->strokeImage = image;

    // Apply filter preview to active, visible frame only.
    KisImageConfig imgConf(true);
    image->addJob(d->currentStrokeId, new KisFilterStrokeStrategy::FilterJobData());

    {
        KisFilterStrokeStrategy::IdleBarrierData *data =
            new KisFilterStrokeStrategy::IdleBarrierData();
        d->idleBarrierCookie = data->idleBarrierCookie();
        image->addJob(d->currentStrokeId, data);
    }

    d->currentlyAppliedConfiguration = filterConfig;
}

void KisFilterManager::finish()
{
    Q_ASSERT(d->currentStrokeId);

    if (d->filterAllSelectedFrames) {   // Apply filter to the other selected frames...
        KisImageSP image = d->view->image();
        KisPaintDeviceSP paintDevice = d->view->activeNode()->paintDevice();
        KisNodeSP node = d->view->activeNode();

        // Filter selected times to only those with keyframes...
        QSet<int> selectedTimes = image->animationInterface()->activeLayerSelectedTimes();
        selectedTimes = KisLayerUtils::filterTimesForOnlyRasterKeyedTimes(node, selectedTimes);
        QSet<int> uniqueFrames = KisLayerUtils::fetchUniqueFrameTimes(node, selectedTimes, true);

        Q_FOREACH(const int& frameTime, uniqueFrames) {
            image->addJob(d->currentStrokeId, new KisFilterStrokeStrategy::FilterJobData(frameTime));
        }
    }

    // its full pass at once, not waiting for the settings to settle
    if (d->preview) {
        d->preview->release();
        d->preview.clear();
    }
    if (KisImageSP strokeImage = d->strokeImage) {
        strokeImage->endStroke(d->currentStrokeId);
    }

    KisFilterSP filter = KisFilterRegistry::instance()->value(d->currentlyAppliedConfiguration->name());
    if (filter->bookmarkManager()) {
        filter->bookmarkManager()->save(KisBookmarkedConfigurationManager::ConfigLastUsed,
                                       d->currentlyAppliedConfiguration.data());
    }

    d->lastConfiguration = d->currentlyAppliedConfiguration;
    d->reapplyAction->setEnabled(true);
    d->reapplyAction->setText(i18n("Apply Filter Again: %1", filter->name()));

    d->idleBarrierCookie.clear();
    d->currentlyAppliedConfiguration.clear();
}

void KisFilterManager::cancelRunningStroke()
{
    Q_ASSERT(d->currentStrokeId);

    // we should to notify the stroke that it should do the updates itself.
    d->externalCancelUpdatesStorage->shouldIssueCancellationUpdates.ref();
    if (KisImageSP strokeImage = d->strokeImage) {
        strokeImage->cancelStroke(d->currentStrokeId);
    }

    d->currentStrokeId.clear();
    d->idleBarrierCookie.clear();
    d->preview.clear();
    d->currentlyAppliedConfiguration.clear();
    d->externalCancelUpdatesStorage.clear();
}

void KisFilterManager::cancelDialog()
{
    cancelRunningStroke();

    if (d->filterDialog) {
        d->filterDialog->reject();
    } else if (d->inlineEditor.reject) {
        d->inlineEditor.reject();
    }
}

bool KisFilterManager::isStrokeRunning() const
{
    return bool(d->currentStrokeId);
}

bool KisFilterManager::isIdle() const
{
    // a coarse pass shown is enough: the next settings are sent, not waiting for the full pass
    return !d->idleBarrierCookie || (d->preview && d->preview->coarseShown.loadAcquire());
}

void KisFilterManager::setFilterAllSelectedFrames(bool filterAllSelectedFrames)
{
    d->filterAllSelectedFrames = filterAllSelectedFrames;
}

bool KisFilterManager::filterAllSelectedFrames()
{
    return d->filterAllSelectedFrames;
}

void KisFilterManager::slotStrokeEndRequested()
{
    if (d->currentStrokeId && d->filterDialog) {
        d->filterDialog->accept();
    } else if (d->currentStrokeId && d->inlineEditor.accept) {
        d->inlineEditor.accept();
    }
}

void KisFilterManager::slotStrokeCancelRequested()
{
    if (d->currentStrokeId && d->filterDialog) {
        d->filterDialog->reject();
    } else if (d->currentStrokeId && d->inlineEditor.reject) {
        d->inlineEditor.reject();
    }
}
void KisFilterManager::filterDialogHasFinished(int)
{
    // as far as we are concerned, filterDialog has been deleted
    d->filterDialog = nullptr;
}
