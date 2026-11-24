/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"
#include "KisStudioLayout_p.h"

#include <kis_selection.h>

KisStudioLayout::KisStudioLayout(KisMainWindow *mainWindow)
    : QObject(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;
    everyLayout() << this;
    // snapshot the settings at startup, so later changes that need a restart show as pending
    KisSettingsRegistry::instance();
    // Krita's tags for the resources taken from it, now that they are read
    KisSettingsTransfer::takePendingKritaTags();
    KisSettingsTransfer::startWithoutTags();
    KisSettingsTransfer::useSkissaBrushes();
    // the day's backup once the window has settled
    QTimer::singleShot(20000, this, []() { KisSettingsTransfer::backUpDaily(); });

    // save the heights once a resize has settled

    // canvas-only mode restores the window state partly through the event queue, so normalize afterwards
    if (QAction *canvasOnly = mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"))) {
        connect(canvasOnly, &QAction::toggled, this, [this](bool on) {
            closeFlyout();
            // a fade from peeking would hide what the mode just brought back
            for (const QPointer<QWidget> &widget : d->peeking) KisStudioMotion::stopFade(widget);
            d->peeking.clear();
            QTimer::singleShot(0, this, &KisStudioLayout::normalize);
            // full screen and back without Windows animating the window from its restored size
            if (d->frame && d->active) {
                if (on) {
                    d->frame->setTransitionsEnabled(false);
                } else {
                    QTimer::singleShot(600, d->frame, [frame = d->frame]() { if (frame) frame->setTransitionsEnabled(true); });
                }
            }
        });
    }

    connect(mainWindow, &KisMainWindow::themeChanged, this, [this]() {
        if (!d->active) return;
        rebuildRailButtons();
        if (d->toolRail) d->toolRail->refresh();
        if (d->addPanelButton) d->addPanelButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_more")));
        updateContextBar();
    });
}

KisStudioLayout::~KisStudioLayout()
{
    if (d->active) {
        qApp->removeEventFilter(this);
    }
}

QString KisStudioLayout::arrangementNow()
{
    QJsonObject keys;
    const QMap<QString, QString> entries = KSharedConfig::openConfig()->group(QString()).entryMap();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (isArrangementKey(it.key())) keys.insert(it.key(), it.value());
    }
    return QString::fromUtf8(QJsonDocument(keys).toJson(QJsonDocument::Compact));
}

QString KisStudioLayout::committedArrangement()
{
    if (committedStore().isNull()) committedStore() = arrangementNow();
    return committedStore();
}

void KisStudioLayout::restoreArrangement(const QString &arrangement)
{
    committedStore() = arrangement;
    if (arrangement == arrangementNow()) return;
    KConfigGroup group = KSharedConfig::openConfig()->group(QString());
    const QJsonObject keys = QJsonDocument::fromJson(arrangement.toUtf8()).object();
    const QMap<QString, QString> entries = group.entryMap();
    for (auto it = entries.constBegin(); it != entries.constEnd(); ++it) {
        if (isArrangementKey(it.key()) && !keys.contains(it.key())) group.deleteEntry(it.key());
    }
    for (auto it = keys.constBegin(); it != keys.constEnd(); ++it) group.writeEntry(it.key(), it.value().toString());
    KisSettingsRegistry::instance()->scheduleSync();
    Q_FOREACH (const QPointer<KisStudioLayout> &layout, everyLayout()) {
        if (layout) layout->reloadArrangement();
    }
}

void KisStudioLayout::reloadArrangement()
{
    if (!d->active) return;
    QScopedValueRollback<bool> reloading(d->reloadingArrangement, true);
    const QSet<QString> wasPinned = d->pinned;
    readArrangement();
    // panels pinned since go back to their buttons; those unpinned since come back
    Q_FOREACH (QDockWidget *dock, d->panels) {
        const QString id = dock->objectName();
        const bool pinned = d->pinned.contains(id);
        if (pinned) {
            // put away with its button, or brought back
            const bool shown = !d->hiddenPinned.contains(id) && !arePanelsHidden();
            if (!wasPinned.contains(id)) {
                if (dock == d->flyout) releaseFlyout();
                dockAway(dock);
            }
            if (dock->isHidden() == shown) dock->setVisible(shown);
        } else if (wasPinned.contains(id)) {
            applyFold(dock, false);
            dock->hide();
            parkPanel(dock);
        }
    }
    // the rail first: the groups keep clear of it where it is now, not where it was
    if (auto *rail = static_cast<RailOverlay *>(d->brushRail.data())) {
        rail->setFlat(d->toolRailFlat);
        rail->setTucked(d->toolRailTucked);
        rail->setPlace(d->toolRailLeft, d->toolRailSpot);
    }
    // the groups take their places, sides and widths again
    d->laneWidths.clear();
    if (d->panelColumn) {
        d->laneWidths.insert(0, readWidths(0));
        d->panelColumn->setSpot(readSpot(panelSpotKey), readSpotHeight(panelSpotKey));
    }
    for (auto it = d->groupColumns.constBegin(); it != d->groupColumns.constEnd(); ++it) {
        KisStudioPanelColumn *column = it.value();
        if (!column) continue;
        const int group = it.key();
        d->laneWidths.insert(group, readWidths(group));
        if (group >= 2) column->setLeft(KisConfig(true).readEntry<QString>(groupKey(group, "Side"), QString()) == QLatin1String("left"));
        const QString spotKey = group == 1 ? farSpotKey : groupKey(group, "Spot");
        column->setSpot(readSpot(spotKey), readSpotHeight(spotKey));
    }
    d->arrangement.clear();
    arrangePinned();
    Q_FOREACH (const QString &id, d->pinned) applyFold(d->mainWindow->dockWidget(id), d->folded.contains(id));
    setPanelsTucked(d->panelsTucked);
    if (d->topPill) {
        d->topPill->setTucked(d->topBarTucked);
        d->topPill->setSpot(d->topBarSpot, readSpotHeight(topBarSpotKey));
    }
    updateCanvasInsets();
}

void KisStudioLayout::commitArrangement()
{
    if (!d->active || d->reloadingArrangement || d->commitPending) return;
    d->commitPending = true;
    // once this event is done with, so every write of the gesture is in the step
    QTimer::singleShot(0, this, [this]() {
        d->commitPending = false;
        const bool panels = d->panelsPending;
        d->panelsPending = false;
        KisSettingsRegistry *registry = KisSettingsRegistry::instance();
        if (!d->active || d->reloadingArrangement || registry->replaying()) {
            if (panels) KisSettingsRegistry::writeItems(panelsKey, d->pendingPanels);
            return;
        }
        // the panel buttons first: an undo goes back in the reverse order, the arrangement first, so a button that
        // goes leads to no pinned panel to close
        QVector<QPair<QString, QVariant>> values;
        if (panels) {
            const QString items = QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(d->pendingPanels)).toJson(QJsonDocument::Compact));
            values << qMakePair(panelsKey, QVariant(items));
        }
        values << qMakePair(QStringLiteral("StudioLayoutArrangement"), QVariant(arrangementNow()));
        registry->setValues(values);
    });
}

void KisStudioLayout::keepAbove(QWidget *overlay)
{
    static const char *const overlayProperty = "studioOverlay";
    overlay->setProperty(overlayProperty, true);
    QWidget *parent = overlay->parentWidget();
    if (!parent) return;
    const QObjectList &siblings = parent->children();
    for (int i = siblings.indexOf(overlay) + 1; i < siblings.size(); i++) {
        auto *sibling = qobject_cast<QWidget *>(siblings[i]);
        if (sibling && !sibling->isWindow() && !sibling->isHidden() && !sibling->property(overlayProperty).toBool()) {
            overlay->raise();
            return;
        }
    }
}

qreal KisStudioLayout::spotShare(int offset, int room, int grown)
{
    return shareOf(offset, room, grown);
}

int KisStudioLayout::spotOffset(qreal share, int room, int grown)
{
    return offsetOf(share, room, grown);
}

QSize KisStudioLayout::spotGrowth(const QWidget *widget)
{
    return roomGrowth(widget);
}

bool KisStudioLayout::isEnabledInConfig()
{
    return KisConfig(true).readEntry<bool>(activeKey, true);
}

bool KisStudioLayout::isActive() const
{
    return d->active;
}

void KisStudioLayout::setActive(bool active)
{
    if (d->active == active) {
        return;
    }
    KisConfig cfg(false);
    // save the classic layout to return to
    if (active && !isEnabledInConfig()) {
        cfg.writeEntry(classicStateKey, d->mainWindow->saveState());
    }
    cfg.writeEntry(activeKey, active);
    d->active = active;
    if (active) {
        enter();
    } else {
        leave();
    }
}

void KisStudioLayout::readArrangement()
{
    KisConfig cfg(true);
    auto readSet = [&cfg](const QString &key) {
        const QStringList list = cfg.readEntry<QStringList>(key);
        return QSet<QString>(list.begin(), list.end());
    };
    d->pinned = readSet(pinnedKey);
    // a panel the Studio no longer offers isn't kept beside the canvas either
    for (auto it = d->pinned.begin(); it != d->pinned.end();) {
        if (KisStudioPanelPicker::isRetired(*it)) it = d->pinned.erase(it);
        else ++it;
    }
    d->groups.clear();
    Q_FOREACH (const QString &id, readSet(farKey)) {
        d->groups.insert(id, 1);
    }
    Q_FOREACH (const QString &entry, cfg.readEntry<QStringList>(groupsKey)) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        const int group = separator > 0 ? entry.mid(separator + 1).toInt() : 0;
        if (group >= 2) d->groups.insert(entry.left(separator), group);
    }
    d->lanes.clear();
    Q_FOREACH (const QString &entry, cfg.readEntry<QStringList>(lanesKey)) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        const int lane = separator > 0 ? entry.mid(separator + 1).toInt() : 0;
        if (lane > 0) d->lanes.insert(entry.left(separator), lane);
    }
    d->folded = readSet(foldedKey);
    d->hiddenPinned = readSet(hiddenPinnedKey);
    const QString panelSide = cfg.readEntry<QString>(panelSideKey, QString());
    d->panelsLeft = panelSide == QLatin1String("left")
                    || (panelSide.isEmpty() && !d->pinned.isEmpty() && readSet(leftKey).contains(d->pinned));
    d->toolRailLeft = cfg.readEntry<QString>(toolRailSideKey, QStringLiteral("left")) != QLatin1String("right");
    d->toolRailSpot = readSpot(toolRailSpotKey);
    d->topBarSpot = readSpot(topBarSpotKey);
    d->panelsTucked = cfg.readEntry<bool>(panelsHiddenKey, false);
    d->toolRailTucked = cfg.readEntry<bool>(toolRailHiddenKey, false);
    d->toolRailFlat = cfg.readEntry<bool>(toolRailFlatKey, false);
    d->topBarTucked = cfg.readEntry<bool>(topBarHiddenKey, false);
    d->heights.clear();
    Q_FOREACH (const QString &entry, cfg.readEntry<QStringList>(heightsKey)) {
        const int separator = entry.lastIndexOf(QLatin1Char('='));
        if (separator > 0) {
            d->heights.insert(entry.left(separator), entry.mid(separator + 1).toInt());
        }
    }
}

void KisStudioLayout::enter()
{
    // what is arranged on the way in (a first start's places) is where undo stops
    QTimer::singleShot(0, this, []() { committedStore() = arrangementNow(); });
    KisMainWindow *mainWindow = d->mainWindow;
    // the pills and panels live in the main window's canvas area
    mainWindow->setCanvasDetached(false);

    KisConfig cfg(true);
    // a first start shows the panels painting needs together, not one at a time
    const bool firstStart = cfg.readEntry<QString>(pinnedKey, QStringLiteral("\x1f")) == QLatin1String("\x1f");
    readArrangement();
    if (firstStart) {
        d->pinned = {QStringLiteral("StudioColor"), QStringLiteral("PresetDocker"), QStringLiteral("KisLayerBox")};
    }
    if (firstStart && cfg.readEntry<QStringList>(groupsKey).isEmpty()) {
        // color and brushes together at the bottom by the tools, away from the hand that paints; the layers with the
        // panel buttons
        d->groups.insert(QStringLiteral("StudioColor"), 2);
        d->groups.insert(QStringLiteral("PresetDocker"), 2);
        KisConfig out(false);
        out.writeEntry(groupKey(2, "Side"), QStringLiteral("left"));
        out.writeEntry(groupKey(2, "Spot"), QStringLiteral("0,1"));
    }
    d->arrangement.clear();

    Q_FOREACH (QDockWidget *dock, mainWindow->dockWidgets()) {
        QWidget *titleBar = dock->titleBarWidget();
        // e.g. the animation timeline keeps its controls in the title bar; as long as the frames, it lies under the
        // canvas, across it
        if (titleBar && titleBar->inherits("KisUtilityTitleBar")) {
            if (KisStudioPanelPicker::isRetired(dock->objectName())) {
                // left out: hidden, also when Krita puts back a saved window layout, and not offered as a command
                QAction *toggle = dock->toggleViewAction();
                dock->hide();
                d->connections << connect(dock, &QDockWidget::visibilityChanged, this, [dock](bool visible) {
                    if (visible) QTimer::singleShot(0, dock, &QWidget::hide);
                });
                toggle->setVisible(false);
                QPointer<QAction> shown = toggle;
                d->pageUndos << [shown]() {
                    if (shown) shown->setVisible(true);
                };
                continue;
            }
            if (dock->objectName() == QLatin1String("TimelineDocker")) {
                // shown for an image that is animated; a still one can still open it, to start animating
                QPointer<QDockWidget> timeline = dock;
                auto followImage = [mainWindow, timeline]() {
                    KisView *view = mainWindow->activeView();
                    if (!timeline || !view || !view->image()) return;
                    timeline->setVisible(view->image()->animationInterface()->hasAnimation());
                };
                d->connections << connect(mainWindow, &KisMainWindow::activeViewChanged, this, followImage);
                QTimer::singleShot(0, dock, followImage);
            }
            // also after Krita puts back a saved window layout, or the panel is dragged elsewhere
            // once on the way in, as a saved layout may have them side by side
            auto placed = std::make_shared<bool>(false);
            auto toBottom = [mainWindow, dock, placed]() {
                if (dock->isHidden()) return;
                if (*placed && !dock->isFloating() && mainWindow->dockWidgetArea(dock) == Qt::BottomDockWidgetArea) return;
                *placed = true;
                dock->setFloating(false);
                // one above another: side by side, their wide title bars would widen the window
                mainWindow->addDockWidget(Qt::BottomDockWidgetArea, dock, Qt::Vertical);
                dock->show();
            };
            d->connections << connect(dock, &QDockWidget::visibilityChanged, this, [dock, toBottom](bool visible) {
                if (visible) QTimer::singleShot(0, dock, toBottom);
            });
            d->connections << connect(dock, &QDockWidget::dockLocationChanged, this, [dock, toBottom]() { QTimer::singleShot(0, dock, toBottom); });
            d->connections << connect(dock, &QDockWidget::topLevelChanged, this, [dock, toBottom]() { QTimer::singleShot(0, dock, toBottom); });
            QTimer::singleShot(0, dock, toBottom);
            if (auto undo = KisStudioPanelPages::apply(dock)) d->pageUndos << undo;
            continue;
        }

        d->classicDocks.insert(dock, {titleBar, dock->features()});

        if (dock->objectName() == toolBoxId) {
            dock->setTitleBarWidget(new QWidget(dock));
            dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
            continue;
        }

        auto *grip = new PanelGrip(dock);
        const QString id = dock->objectName();
        grip->dragged = [this, id](const QPoint &pos) { d->showPanelDrop(pos, id); };
        // deferred until the release is delivered, since the drop may rearrange the panel and its grip
        grip->dropped = [this, id](const QPoint &pos) {
            QTimer::singleShot(0, this, [this, id, pos]() { dropPanel(id, pos, false); });
        };
        grip->closeRequested = [this, id]() {
            if (QDockWidget *panel = d->mainWindow->dockWidget(id)) togglePanel(panel, false);
        };
        grip->clicked = [this, id]() { gripClicked(id); };
        d->grips.insert(id, grip);
        dock->setTitleBarWidget(grip);
        // Krita's panels that run their contents to their edges (the color selectors, the mixer) get the panels' padding,
        // clear of the rounded corners
        static const QStringList edgeToEdge = {QStringLiteral("ColorSelectorNg"), QStringLiteral("WideGamutColorSelector"),
                                               QStringLiteral("DigitalMixer")};
        // one name for the brushes, as the top bar has it; other names say what the panel is for
        if (id == QLatin1String("PresetDocker")) dock->setWindowTitle(i18nc("@title:window the brushes panel", "Brushes"));
        if (id == QLatin1String("History")) dock->setWindowTitle(i18nc("@title:window the undo history panel", "History"));
        if (id == QLatin1String("SvgSymbolCollectionDocker")) dock->setWindowTitle(i18nc("@title:window the vector symbols panel", "Symbols"));
        dock->setProperty("studioIcon", railIcon(dock));
        if (auto undo = KisStudioPanelPages::apply(dock)) d->pageUndos << undo;
        if (edgeToEdge.contains(id) && dock->widget()) {
            dock->widget()->setContentsMargins(KisModernStyle::space2, 0, KisModernStyle::space2, KisModernStyle::space2);
        }
        // snapshot for the panel picker as the panel goes off screen
        d->connections << connect(dock, &QDockWidget::visibilityChanged, this, [dock](bool visible) {
            if (!visible) KisStudioPanelPicker::rememberLook(dock);
        });
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        d->panels.insert(dock);

        // named so a shortcut set on its card is saved, and added to the window so the shortcut works
        QAction *toggle = dock->toggleViewAction();
        // a panel left out isn't offered as a command either
        if (KisStudioPanelPicker::isRetired(dock->objectName()) && toggle->isVisible()) {
            toggle->setVisible(false);
            QPointer<QAction> shown = toggle;
            d->pageUndos << [shown]() {
                if (shown) shown->setVisible(true);
            };
        }
        if (toggle->objectName().isEmpty()) {
            toggle->setObjectName(QStringLiteral("studio_panel_") + dock->objectName());
            KisStudioShortcuts::restore(toggle);
            mainWindow->addAction(toggle);
        }

        d->connections << connect(toggle, &QAction::toggled, this, [this, dock](bool visible) {
            dockToggled(dock, visible);
        });
    }

    showStartPage(true);
    createRail();
    createBrushRail();
    createTopBar();

    // the effects and brush pickers, opened at the pointer from Find, a shortcut or the top bar
    auto addPicker = [mainWindow](const QString &name, const QString &icon, const QString &text, std::function<void()> open) {
        if (mainWindow->actionCollection()->action(name)) return;
        QAction *action = new QAction(KisIconUtils::loadIcon(icon), text, mainWindow);
        action->setObjectName(name);
        KisStudioShortcuts::restore(action);
        mainWindow->actionCollection()->addAction(name, action);
        mainWindow->addAction(action);
        connect(action, &QAction::triggered, mainWindow, open);
    };
    addPicker(QStringLiteral("studio_effects"), QStringLiteral("studio_effects"), i18nc("@action", "Effects"), [mainWindow]() {
        (new KisStudioEffectsPicker(mainWindow))->popupNear(QCursor::pos());
    });
    // a brush of what is selected (also in the selection's menu)
    addPicker(QStringLiteral("studio_make_brush"), QStringLiteral("krita_tool_freehand"), i18nc("@action", "Make Brush from Selection"), [mainWindow]() {
        KisStudioMakeBrush::fromSelection(mainWindow->viewManager());
    });
    KisSkissaUpdater::addActions(mainWindow);
    // brush tweaks not saved yet come back after a restart
    KisStudioBrushTweaks::watch(mainWindow);
    addPicker(QStringLiteral("studio_home"), QStringLiteral("krita-branding"), i18nc("@action your artworks: the start page", "Home"), [this]() {
        showHome(!d->mainWindow->isWelcomeScreenShown());
    });
    addPicker(QStringLiteral("studio_brush_picker"), QStringLiteral("paintop_settings_01"), i18nc("@action", "Brushes"), [mainWindow]() {
        (new KisStudioBrushPicker(mainWindow))->popupNear(QCursor::pos());
    });
    if (!d->brushMemory) {
        d->brushMemory = new KisStudioBrushMemory(mainWindow);
    }
    if (!d->canvasRing) {
        d->canvasRing = new KisStudioCanvasRing(mainWindow);
    }
    // preload the brushes (about a tenth of a second) ahead of the first right press or brush chip scroll
    QTimer::singleShot(3000, this, []() { KisStudioBrushPicker::yourBrushes(); });
    // so the buttons' shortcuts work before Find is first opened
    QTimer::singleShot(0, this, [this]() {
        if (KisStudioFind *find = d->mainWindow->studioFind()) find->registerButtons();
    });
    KisViewManager *viewManager = mainWindow->viewManager();
    if (KisPaintopBox *paintopBox = viewManager ? viewManager->paintOpBox() : nullptr) {
        paintopBox->setStudioButtonsHidden(true);
        // the brush editor's action (its shortcut, Find) opens the quick editor, under the bar's button if it has one
        disconnect(paintopBox, &KisPaintopBox::sigQuickEditorRequested, this, nullptr);
        connect(paintopBox, &KisPaintopBox::sigQuickEditorRequested, this, [this]() {
            // below the brush's chip, or a bar button of its own if it was put back
            QWidget *button = d->mainWindow->findChild<QWidget *>(QStringLiteral("studio_bar_action_show_brush_editor"));
            if (!button || !button->isVisible()) button = d->toolChip;
            KisStudioBrushEditor::toggle(d->mainWindow, button && button->isVisible() ? button : nullptr, Qt::BottomEdge);
        });
        d->presetChip = paintopBox->presetChip();
        if (d->presetChip) {
            d->presetChipToolTip = d->presetChip->toolTip();
            d->presetChip->setToolTip(i18nc("@info:tooltip", "Brushes\nScroll: next brush\nRight-click: manage"));
            // the brush chip shows the brush's picture and opens the brushes
            d->presetChip->setVisible(false);
        }
    }

    // the top bar's scrubbers replace the toolbar's size and opacity sliders
    for (int i = 1; i <= 5; i++) {
        const QString slider = cfg.toolbarSlider(i);
        QAction *action = mainWindow->actionCollection()->action(QStringLiteral("brushslider%1").arg(i));
        if (action && action->isVisible() && (slider == QLatin1String("size") || slider == QLatin1String("opacity"))) {
            action->setVisible(false);
            d->hiddenToolbarActions << action;
        }
    }
    // the toolbars' own show and hide commands: the Studio has no toolbars. The window makes them after this
    QTimer::singleShot(0, this, [this, mainWindow]() {
        if (!d->active) return;
        Q_FOREACH (QWidget *toolBar, mainWindow->guiFactory()->containers(QStringLiteral("ToolBar"))) {
            QAction *action = mainWindow->actionCollection()->action(toolBar->objectName());
            if (action && action->isVisible()) {
                action->setVisible(false);
                d->hiddenToolbarActions << action;
            }
        }
    });
    // clipping to the layers below: Krita's alpha inheritance, as a switch showing the current layer's
    if (viewManager) {
        auto *clip = new QAction(KisIconUtils::loadIcon(QStringLiteral("studio_clip")), i18nc("@action", "Clip to layers below"), mainWindow);
        clip->setObjectName(QStringLiteral("studio_clip_below"));
        clip->setCheckable(true);
        clip->setToolTip(i18nc("@info:tooltip", "Clip to layers below: paints only where they are"));
        mainWindow->actionCollection()->addAction(clip->objectName(), clip);
        const QPointer<KisViewManager> view = viewManager;
        auto shown = [clip, view]() {
            KisLayer *layer = view ? qobject_cast<KisLayer *>(view->activeNode().data()) : nullptr;
            const QSignalBlocker blocker(clip);
            clip->setEnabled(layer);
            clip->setChecked(layer && layer->alphaChannelDisabled());
        };
        connect(clip, &QAction::triggered, clip, [view, shown]() {
            if (view && view->nodeManager()) view->nodeManager()->toggleInheritAlpha();
            shown();
        });
        connect(viewManager->nodeManager(), &KisNodeManager::sigNodeActivated, clip, shown);
        // the layer's switch changed elsewhere (the layer panel, an undo)
        auto watched = QSharedPointer<QMetaObject::Connection>::create();
        connect(viewManager, &KisViewManager::viewChanged, clip, [view, shown, watched]() {
            QObject::disconnect(*watched);
            if (view && view->image()) *watched = QObject::connect(view->image().data(), &KisImage::sigNodeChanged, view.data(), [shown](KisNodeSP) { shown(); }, Qt::QueuedConnection);
            shown();
        });
    }

    // one name for one thing: the brush editors call it so
    if (QAction *reload = mainWindow->actionCollection()->action(QStringLiteral("reload_preset_action"))) {
        reload->setText(i18nc("@action drop the brush's unsaved changes", "Back to the saved brush"));
        reload->setToolTip(reload->text());
    }
        // the commands as they are now; those added or renamed later are tidied as menus get them (eventFilter)
    Q_FOREACH (QAction *action, mainWindow->actionCollection()->actions()) tidyCommand(action);

    qApp->installEventFilter(this);
    KisStudioEmptyStates::install();
    if (KisCanvasResourceProvider *resources = viewManager ? viewManager->canvasResourceProvider() : nullptr) {
        d->connections << connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, this, [this](const KisPaintOpPresetSP preset) {
            updateToolChip();
            // every brush taken up, however it was picked, is recent
            if (preset) KisStudioBrushPicker::recordUse(preset->name());
        });
    }
    KoToolManager *tools = viewManager ? viewManager->toolManager() : nullptr;
    if (tools) {
        d->connections << connect(tools, &KoToolManager::changedTool, this, [this](KoCanvasController *controller) {
            if (KisStudioToolRail::isOwnController(d->mainWindow, controller)) updateContextBar();
        });
        d->connections << connect(tools, &KoToolManager::toolOptionWidgetsChanged, this,
                                  [this](KoCanvasController *controller, const QList<QPointer<QWidget>> &widgets) {
                                      // ignore tool changes in other windows
                                      if (!KisStudioToolRail::isOwnController(d->mainWindow, controller) || !d->toolStrip) return;
                                      d->toolStrip->setOptionWidgets(widgets);
                                      if (d->toolStripAction) {
                                          d->toolStripAction->setVisible(d->toolStrip->hasOptions() && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
                                      }
                                  });
    }
    if (QMdiArea *area = mdiArea()) {
        d->connections << connect(area, &QMdiArea::subWindowActivated, this, [this]() {
            adoptDocuments();
            updateDocumentButton();
            updateCanvasInsets();
        });
    }
    d->connections << connect(mainWindow, &QWidget::windowTitleChanged, this, &KisStudioLayout::updateDocumentButton);

    if (!d->hud) {
        d->hud = new KisStudioCanvasHud(mainWindow);
    }
    d->hud->setActive(true);
    mainWindow->setProperty("studioLayoutActive", true);

    if (!d->frame) {
        d->frame = new KisStudioWindowFrame(mainWindow, [this](const QPoint &pos) { return isCaptionAt(pos); });
    }
    d->frame->setEnabled(cfg.readEntry<bool>("StudioCustomTitleBar", true));

    // effects are adjusted on the canvas, not in a dialog
    if (!d->effectOverlay) {
        d->effectOverlay = new KisStudioEffectOverlay(mainWindow);
    }
    if (viewManager) {
        QPointer<KisStudioEffectOverlay> overlay = d->effectOverlay;
        viewManager->filterManager()->setInlineEditor({
            [overlay](KisFilterSP filter, KisFilterConfigurationSP config) { return overlay && overlay->open(filter, config); },
            [overlay]() { if (overlay) overlay->accept(); },
            [overlay]() { if (overlay) overlay->reject(); },
            [overlay](KisNodeSP node) { return overlay && overlay->edit(node); },
        });
    }

    // fill the strip with the current tool's options without waiting for a tool change
    if (mainWindow->activeView() && tools) {
        tools->initializeCurrentToolForCanvas();
    }

    normalize();
    updateContextBar();
}

bool KisStudioLayout::handleNativeEvent(void *message, void *result)
{
    return d->active && d->frame && d->frame->handleNativeEvent(message, result);
}

bool KisStudioLayout::coverScreen(bool on)
{
    return d->active && d->frame && d->frame->coverScreen(on);
}

bool KisStudioLayout::isCaptionAt(const QPoint &pos) const
{
    KisMainWindow *mainWindow = d->mainWindow;
    // Windows asks at every pointer move, mostly over the canvas: away from the pills and the top toolbars nothing
    // is caption, known without searching the widget tree
    bool inPill = false;
    for (TopPill *pill : {d->topPill.data(), d->windowPill.data()}) {
        if (pill && pill->isVisible() && QRect(pill->mapTo(mainWindow, QPoint()), pill->size()).contains(pos)) inPill = true;
    }
    if (!inPill) {
        int bottom = -1;
        Q_FOREACH (QToolBar *toolBar, mainWindow->findChildren<QToolBar *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (toolBar->isVisible() && mainWindow->toolBarArea(toolBar) == Qt::TopToolBarArea) {
                bottom = qMax(bottom, toolBar->geometry().bottom());
            }
        }
        if (pos.y() > bottom) return false;
    }
    QWidget *widget = mainWindow->childAt(pos);
    // in a pill, empty space and drag grips act as caption; controls don't, nor does TopGrip, which moves the bar
    for (QWidget *w = widget; w; w = w->parentWidget()) {
        if (w->property(captionProperty).toBool()) {
            if (dynamic_cast<TopGrip *>(widget)) return false;
            return widget == w || dynamic_cast<DragGrip *>(widget) || qobject_cast<QToolBar *>(widget)
                || QByteArray(widget->metaObject()->className()) == "QWidget";
        }
    }
    if (!widget) {
        // between and around the toolbars in the window's top row
        int bottom = 0;
        Q_FOREACH (QToolBar *toolBar, mainWindow->findChildren<QToolBar *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (toolBar->isVisible() && mainWindow->toolBarArea(toolBar) == Qt::TopToolBarArea) {
                bottom = qMax(bottom, toolBar->geometry().bottom());
            }
        }
        return pos.y() <= bottom;
    }
    QWidget *container = widget;
    while (container && !qobject_cast<QToolBar *>(container)) {
        container = container->parentWidget();
    }
    QToolBar *toolBar = qobject_cast<QToolBar *>(container);
    if (!toolBar || mainWindow->toolBarArea(toolBar) != Qt::TopToolBarArea) {
        return false;
    }
    return widget == toolBar || QByteArray(widget->metaObject()->className()) == "QWidget";
}

QMdiArea *KisStudioLayout::mdiArea() const
{
    if (!d->mdiArea) d->mdiArea = d->mainWindow->findChild<QMdiArea *>();
    return d->mdiArea;
}

void KisStudioLayout::adoptDocuments()
{
    QMdiArea *area = mdiArea();
    if (!d->active || !area) {
        return;
    }
    // each document fills the canvas area without tabs or a frame; the document button switches between them
    if (area->viewMode() != QMdiArea::SubWindowView) {
        area->setViewMode(QMdiArea::SubWindowView);
    }
    Q_FOREACH (QMdiSubWindow *window, area->subWindowList()) {
        if (!(window->windowFlags() & Qt::FramelessWindowHint)) {
            window->setWindowFlags(window->windowFlags() | Qt::FramelessWindowHint);
        }
        if (!window->isMaximized()) {
            window->showMaximized();
        }
    }
}

void KisStudioLayout::releaseDocuments()
{
    QMdiArea *area = mdiArea();
    if (!area) {
        return;
    }
    Q_FOREACH (QMdiSubWindow *window, area->subWindowList()) {
        if (window->windowFlags() & Qt::FramelessWindowHint) {
            window->setWindowFlags(window->windowFlags() & ~Qt::FramelessWindowHint);
            window->showMaximized();
        }
    }
    area->setViewMode(QMdiArea::ViewMode(KisConfig(true).readEntry<int>("mdi_viewmode", int(QMdiArea::TabbedView))));
}

void KisStudioLayout::updateDocumentButton()
{
    if (!d->documentButton) {
        return;
    }
    QMdiArea *area = mdiArea();
    auto nameOf = [](QMdiSubWindow *window) {
        KisView *view = qobject_cast<KisView *>(window->widget());
        KisDocument *document = view ? view->document() : nullptr;
        if (!document) {
            return window->windowTitle();
        }
        const QString path = document->path();
        QString name = path.isEmpty() ? i18nc("a document that was never saved", "Untitled") : QFileInfo(path).fileName();
        if (document->isModified()) {
            name += QStringLiteral(" •");
        }
        return name;
    };

    QMdiSubWindow *current = area ? area->currentSubWindow() : nullptr;
    d->documentButton->setText(current ? nameOf(current) : i18nc("the image name button with no image open", "Images"));
}

void KisStudioLayout::leave()
{
    KisMainWindow *mainWindow = d->mainWindow;
    closeFlyout();
    qApp->removeEventFilter(this);
    // folds still running would size the classic docks when they end
    Q_FOREACH (QVariantAnimation *fold, findChildren<QVariantAnimation *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (fold->objectName().startsWith(QLatin1String("studioFold_")) && fold->state() == QAbstractAnimation::Running) {
            fold->setCurrentTime(fold->duration());
        }
    }
    for (const QPointer<QWidget> &widget : d->peeking) KisStudioMotion::stopFade(widget);
    d->peeking.clear();
    if (d->presetChip) {
        d->presetChip->setToolTip(d->presetChipToolTip);
        d->presetChip->setVisible(true);
    }
    delete d->brushMemory;
    delete d->canvasRing;
    if (d->hud) {
        d->hud->setActive(false);
    }
    showStartPage(false);
    if (auto *welcome = mainWindow->findChild<KisWelcomePageWidget *>()) {
        welcome->setContentsMargins(QMargins());
    }
    mainWindow->setProperty("studioLayoutActive", false);
    if (d->frame) {
        d->frame->setTransitionsEnabled(true);
        d->frame->setEnabled(false);
    }
    if (d->effectOverlay && d->effectOverlay->isVisible()) {
        d->effectOverlay->reject();
    }
    if (KisViewManager *viewManager = mainWindow->viewManager()) {
        viewManager->filterManager()->setInlineEditor({});
        viewManager->showHideScrollbars();
    }
    releaseDocuments();
    mainWindow->statusBar()->setVisible(KisConfig(true).showStatusBar());
    for (QAction *action : {d->windowButtonsAction.data(), d->toolStripAction.data(), d->documentButtonAction.data(),
                            d->settingsButtonAction.data(), d->findFieldAction.data()}) {
        if (action) action->setVisible(false);
    }

    Q_FOREACH (const QMetaObject::Connection &connection, d->connections) {
        disconnect(connection);
    }
    d->connections.clear();

    const bool showTitleBars = KisConfig(true).showDockerTitleBars();
    for (auto it = d->classicDocks.begin(); it != d->classicDocks.end(); ++it) {
        QDockWidget *dock = it.key();
        applyFold(dock, false);
        QWidget *studioTitleBar = dock->titleBarWidget();
        dock->setTitleBarWidget(it->titleBar);
        dock->setFeatures(it->features);
        if (studioTitleBar && studioTitleBar != it->titleBar) {
            studioTitleBar->deleteLater();
        }
        if (it->titleBar) {
            it->titleBar->setVisible(showTitleBars || dock->isFloating());
        }
    }
    d->classicDocks.clear();
    for (const auto &undo : d->pageUndos) undo();
    d->pageUndos.clear();
    d->panels.clear();
    d->grips.clear();
    Q_FOREACH (KisStudioPanelColumn *column, d->columns()) {
        Q_FOREACH (QDockWidget *dock, column->panels()) {
            const bool shown = !dock->isHidden();
            parkPanel(dock);
            dock->setVisible(shown);
        }
        column->hide();
    }
    d->arrangement.clear();
    updateCanvasInsets();

    if (d->rail) {
        d->rail->hide();
    }
    if (d->brushRail) {
        d->brushRail->hide();
    }
    if (d->topBar) {
        d->topBar->hide();
    }
    if (d->buttonsBar) {
        d->buttonsBar->hide();
    }
    if (KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
        paintopBox->setStudioButtonsHidden(false);
    }
    releaseTopBars();
    if (!isCanvasOnly()) {
        mainWindow->menuBar()->show();
    }
    const QList<QPointer<QAction>> hiddenActions = d->hiddenToolbarActions + d->contextActions;
    for (QAction *action : hiddenActions) {
        if (action) action->setVisible(true);
    }
    d->hiddenToolbarActions.clear();
    d->contextActions.clear();
    Q_FOREACH (QToolBar *toolBar, d->hiddenToolBars) {
        if (toolBar) toolBar->show();
    }
    d->hiddenToolBars.clear();

    KisConfig cfg(true);
    const QByteArray classicState = cfg.readEntry<QByteArray>(classicStateKey);
    if (!classicState.isEmpty()) {
        mainWindow->restoreWorkspaceState(classicState);
        return;
    }
    // no saved classic layout yet: use the workspace a fresh Krita shows
    const QString workspaceName = cfg.readEntry<QString>(QStringLiteral("CurrentWorkspace"), QStringLiteral("Default"));
    KoResourceServer<KisWorkspaceResource> *server = KisResourceServerProvider::instance()->workspaceServer();
    KisWorkspaceResourceSP workspace = server->resource(QString(), QString(), workspaceName);
    if (!workspace) {
        workspace = server->resource(QString(), QString(), QStringLiteral("Default"));
    }
    if (workspace) {
        mainWindow->restoreWorkspace(workspace);
    }
}

void KisStudioLayout::normalize()
{
    if (!d->active) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;
    const bool panelsHidden = arePanelsHidden();
    const bool railHidden = isRailHidden();
    const bool canvasOnly = isCanvasOnly();

    // the pinned panels decide what is shown, not a restored workspace or window state
    Q_FOREACH (QDockWidget *dock, d->panels) {
        if (dock == d->flyout) {
            continue;
        }
        // never leave a panel floating: restoring a floating panel crashes
        if (dock->isFloating()) {
            dock->hide();
            dockAway(dock);
        }
        if (!d->pinned.contains(dock->objectName())) {
            dock->hide();
        } else if (d->hiddenPinned.contains(dock->objectName())) {
            // put away: a restored state showing it again left it out of what an undo puts back
            dock->hide();
        } else if (!panelsHidden) {
            dock->show();
        }
    }
    // in the rail's order, whatever order a restored state had: a restored state puts panels back in the window's
    // dock areas, otherwise (a setting changed) they are where they were arranged
    bool restored = false;
    Q_FOREACH (QDockWidget *dock, d->panels) {
        if (dock != d->flyout && d->pinned.contains(dock->objectName()) && dock->parentWidget() == mainWindow) restored = true;
    }
    if (restored) d->arrangement.clear();
    arrangePinned();
    updateColumns();

    // the tool rail replaces the toolbox
    if (QDockWidget *toolBox = mainWindow->dockWidget(toolBoxId)) {
        toolBox->hide();
    }

    if (d->rail) {
        d->rail->setVisible(!railHidden);
    }
    if (d->brushRail) {
        d->brushRail->setVisible(!railHidden);
    }
    adoptTopBars();
    // the rail's commands only exist once the GUI is built, which happens after the rail; this runs at
    // every setting change, so only then
    if (d->toolRail && d->toolRail->hasMissingItems()) {
        d->toolRail->refresh();
    }
    placeWindowButtons();
    // also on the start page, where the window pill acts as the title bar
    const bool windowShown = !canvasOnly || d->peeking.contains(QPointer<QWidget>(d->windowPill.data()));
    for (QAction *action : {d->documentButtonAction.data(), d->settingsButtonAction.data(), d->findFieldAction.data()}) {
        if (action) action->setVisible(windowShown);
    }
    if (d->windowButtonsAction) {
        d->windowButtonsAction->setVisible(d->frame && d->frame->isEnabled() && windowShown);
    }

    // hide these again, since restoring a window state brings them back
    for (const char *name : {"mainToolBar", "editToolBar"}) {
        QToolBar *toolBar = mainWindow->findChild<QToolBar *>(QLatin1String(name), Qt::FindDirectChildrenOnly);
        if (toolBar && !toolBar->isHidden()) {
            toolBar->hide();
            if (!d->hiddenToolBars.contains(toolBar)) {
                d->hiddenToolBars << toolBar;
            }
        }
    }
    // the menu button has the menus; canvas-only mode manages the menu bar itself
    if (!canvasOnly) {
        mainWindow->menuBar()->hide();
    }
    // the canvas HUD takes over the status bar's view information
    mainWindow->statusBar()->hide();
    adoptDocuments();
    updateDocumentButton();
    if (d->toolStripAction && d->toolStrip) {
        d->toolStripAction->setVisible(d->toolStrip->hasOptions() && !railHidden);
    }

    syncRail();
}

void KisStudioLayout::dockToggled(QDockWidget *dock, bool visible)
{
    syncRail();
    updateColumns();

    if (!visible && dock == d->flyout) {
        // hidden by its shortcut, its card or a workspace: don't leave an empty flyout behind
        QTimer::singleShot(0, this, [this, dock]() {
            if (dock == d->flyout && dock->isHidden()) closeFlyout();
        });
    } else if (visible && dock != d->flyout && !d->pinned.contains(dock->objectName())) {
        // shown by the Dockers menu, a shortcut or a workspace: open it as a flyout, unless normalize() hid it again
        QTimer::singleShot(0, this, [this, dock]() {
            if (d->active && !dock->isHidden() && dock != d->flyout
                && !d->pinned.contains(dock->objectName()) && !arePanelsHidden()) {
                // below the button that opened it, when one says so (the text tool's properties, the timeline's onion skins)
                showFlyout(dock, qobject_cast<QWidget *>(dock->property("studioAnchor").value<QObject *>()));
            }
        });
    }
}

bool KisStudioLayout::arePanelsHidden() const
{
    QAction *showDockers = d->mainWindow->actionCollection()->action(QStringLiteral("view_toggledockers"));
    return isRailHidden() || (showDockers && !showDockers->isChecked());
}

bool KisStudioLayout::isRailHidden() const
{
    return d->mainWindow->isWelcomeScreenShown() || isCanvasOnly();
}

bool KisStudioLayout::isCanvasOnly() const
{
    if (!d->canvasOnlyAction) d->canvasOnlyAction = d->mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"));
    return d->canvasOnlyAction && d->canvasOnlyAction->isChecked();
}

namespace
{
/// Whether \p widget is the canvas an image is painted on
bool isCanvas(const QWidget *widget)
{
    return widget && (widget->inherits("KisOpenGLCanvas2") || widget->inherits("KisQPainterCanvas"));
}
} // namespace

bool KisStudioLayout::eventFilter(QObject *watched, QEvent *event)
{
    if ((event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionChanged) && qobject_cast<QMenu *>(watched)) {
        tidyCommand(static_cast<QActionEvent *>(event)->action());
    }
    // Escape the tool had nothing to cancel with (it went past the canvas to the view): the selection goes
    if (event->type() == QEvent::KeyPress && watched->inherits("KisView")) {
        auto *key = static_cast<QKeyEvent *>(event);
        QAction *deselect = d->mainWindow->actionCollection()->action(QStringLiteral("deselect"));
        KisViewManager *view = d->mainWindow->viewManager();
        if (key->key() == Qt::Key_Escape && key->modifiers() == Qt::NoModifier && !key->isAutoRepeat() && deselect && deselect->isEnabled()
            && view && view->selection()) {
            deselect->trigger();
            return true;
        }
    }
    // a number box's right click is Qt's menu of Undo, Cut, Paste and Step up: none of it is wanted on a number (the
    // app's own values reset on a right click)
    if (event->type() == QEvent::ContextMenu) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && (qobject_cast<QAbstractSpinBox *>(widget) || qobject_cast<QAbstractSpinBox *>(widget->parentWidget()))) {
            return true;
        }
    }
    if ((event->type() == QEvent::MouseMove || event->type() == QEvent::TabletMove) && isCanvasOnly()) {
        // only moves over this window
        auto *widget = qobject_cast<QWidget *>(watched);
        const bool mouse = event->type() == QEvent::MouseMove;
        const QPoint global = mouse ? static_cast<QMouseEvent *>(event)->globalPos() : static_cast<QTabletEvent *>(event)->globalPos();
        // a stroke or a drag passing an edge brings nothing back, and takes nothing away
        const bool pressed = mouse ? static_cast<QMouseEvent *>(event)->buttons() != Qt::NoButton
                                   : static_cast<QTabletEvent *>(event)->buttons() != Qt::NoButton;
        if (widget && widget->window() == d->mainWindow && !pressed) peek(global);
    }
    // what the pointer brought back goes when it leaves the window (to another screen), unless it went to a menu
    // or popup of the window's own
    if (event->type() == QEvent::Leave && watched == d->mainWindow && isCanvasOnly() && !d->peeking.isEmpty()) {
        QTimer::singleShot(300, this, [this]() {
            if (!isCanvasOnly() || QApplication::activePopupWidget() || QApplication::widgetAt(QCursor::pos())) return;
            for (const QPointer<QWidget> &widget : d->peeking) {
                if (widget) KisStudioMotion::hideFaded(widget);
            }
            d->peeking.clear();
        });
    }
    // a text field takes Undo and Redo only while it has typing of its own to undo; otherwise they go to the window
    if (event->type() == QEvent::ShortcutOverride) {
        auto *key = static_cast<QKeyEvent *>(event);
        const bool undo = key->matches(QKeySequence::Undo);
        const bool redo = key->matches(QKeySequence::Redo);
        if (undo || redo) {
            QLineEdit *edit = qobject_cast<QLineEdit *>(watched);
            if (!edit) {
                if (auto *spin = qobject_cast<QAbstractSpinBox *>(watched)) edit = spin->findChild<QLineEdit *>();
            }
            if (edit && edit->window() == d->mainWindow && !(undo ? edit->isUndoAvailable() : edit->isRedoAvailable())) {
                event->ignore();
                return true;
            }
        }
    }
    // dock panels before the window state is saved, since restoring a floating panel crashes Qt
    if (watched == d->mainWindow && event->type() == QEvent::Close) {
        if (d->flyout) closeFlyout();
        Q_FOREACH (QDockWidget *dock, d->mainWindow->dockWidgets()) {
            if (dock->isFloating() && dock->isHidden()) dockAway(dock);
        }
    }

    if (watched == d->startPage && d->startPage && (event->type() == QEvent::Show || event->type() == QEvent::Hide)) {
        QTimer::singleShot(0, this, &KisStudioLayout::normalize);
    }

    if (watched == d->brushRail && d->brushRail) {
        const QEvent::Type type = event->type();
        if (type == QEvent::Move || type == QEvent::Resize || type == QEvent::Show || type == QEvent::Hide) {
            placeWelcomePage();
            placeWindowHandle();
        }
    }

    // refit an open flyout once when its contents change size (e.g. options revealed by a checkbox)
    if (event->type() == QEvent::LayoutRequest && d->flyout && !d->flyoutRefit) {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (widget && d->flyout->isAncestorOf(widget)) {
            d->flyoutRefit = true;
            QTimer::singleShot(0, this, [this]() {
                d->flyoutRefit = false;
                positionFlyout();
            });
        }
    }

    if (handleRailDrag(watched, event)) {
        return true;
    }

    if (d->presetChip && watched == d->presetChip && handlePresetChip(event, d->presetChip)) {
        return true;
    }
    // the brush chip's wheel goes through the brushes, as the picker's chip it stands for did
    if (d->toolChip && watched == d->toolChip && event->type() == QEvent::Wheel && static_cast<ToolChip *>(d->toolChip.data())->pictureClicked
        && handlePresetChip(event, d->toolChip)) {
        return true;
    }

    // right-clicking or holding any button in the window shows its card, to give it a shortcut
    if (event->type() == QEvent::ContextMenu) {
        auto *button = qobject_cast<QAbstractButton *>(watched);
        if (button && button->window() == d->mainWindow && button->contextMenuPolicy() == Qt::DefaultContextMenu
            && !dynamic_cast<KisSelectionActionsPanelButton *>(button)) {
            bool ownCard = false;
            // these have their own cards or menus, as does the canvas's selection bar
            for (QWidget *w = button; w && !ownCard; w = w->parentWidget()) {
                ownCard = w->inherits("KisStudioToolRail") || w == d->rail || w->property("ownContextMenu").toBool();
            }
            KisStudioFind *find = d->mainWindow->studioFind();
            if (!ownCard && find && find->showCard(button)) {
                return true;
            }
        }
    }

    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        // a press anywhere else in the interface closes the flyout; the rail handles its own buttons. Painting doesn't:
        // a panel opened to use while drawing (layers, colors) stays
        if (d->flyout && d->flyout->isVisible() && !isCanvas(qobject_cast<QWidget *>(watched))) {
            QWidget *widget = qobject_cast<QWidget *>(watched);
            // test by position, since a press the flyout ignores propagates to its parent
            const QPoint at = event->type() == QEvent::MouseButtonPress ? static_cast<QMouseEvent *>(event)->pos()
                                                                         : static_cast<QTabletEvent *>(event)->pos();
            const bool inFlyout = widget && d->flyoutWindow && d->flyoutWindow->isVisible()
                                  && d->flyoutWindow->rect().contains(d->flyoutWindow->mapFromGlobal(widget->mapToGlobal(at)));
            const bool onRail = d->rail && widget && (widget == d->rail || d->rail->isAncestorOf(widget));
            // the chip toggles the tool options itself, and they should follow a tool picked on the tool rail
            const bool onChip = d->toolChip && widget == d->toolChip;
            const bool pickingTool = d->flyoutAnchor && d->flyoutAnchor == d->toolChip && d->brushRail && widget
                                     && d->brushRail->isAncestorOf(widget);
            if (widget && widget->window() == d->mainWindow && !inFlyout && !onRail && !onChip && !pickingTool) {
                closeFlyout();
            }
        }
        break;

    case QEvent::KeyPress:
        // Esc closes a flyout, whether the keys are in it or still on the canvas (its button takes no focus)
        if (d->flyout && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
            QWidget *focus = QApplication::focusWidget();
            const bool canvasKeys = !focus || (d->mainWindow->activeView() && d->mainWindow->activeView()->canvasBase()
                                               && focus == d->mainWindow->activeView()->canvasBase()->canvasWidget());
            if ((d->flyout->isAncestorOf(focus) || canvasKeys) && watched == (focus ? static_cast<QObject *>(focus) : d->mainWindow)) {
                closeFlyout();
                return true;
            }
        }
        break;

    case QEvent::Move:
    case QEvent::Resize:
    case QEvent::WindowStateChange:
    case QEvent::Show:
        // also when restored from minimized, as it would have been placed far off
        if (d->flyout && (watched == d->mainWindow || watched == d->mainWindow->centralWidget())) {
            QTimer::singleShot(0, this, &KisStudioLayout::positionFlyout);
        }
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}
