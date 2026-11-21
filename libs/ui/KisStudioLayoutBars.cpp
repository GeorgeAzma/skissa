/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"
#include "KisStudioLayout_p.h"

// The panel and brush rails, the top bar and its chips, settings and the main menu, peeking at the canvas

void KisStudioLayout::createRail()
{
    if (d->rail) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;

    QToolBar *rail = new QToolBar(i18nc("@title:window", "Panels"), mainWindow);
    // a new object name, so restoring a window state doesn't claim it as the old window toolbar
    rail->setObjectName(QStringLiteral("studioPanelStrip"));
    rail->setMovable(false);
    rail->setFloatable(false);
    rail->setOrientation(Qt::Horizontal);
    rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
    rail->setIconSize(QSize(20, 20));
    rail->setContextMenuPolicy(Qt::CustomContextMenu);
    KisStudioGlass::setSeeThrough(rail, true);
    connect(rail, &QWidget::customContextMenuRequested, this, &KisStudioLayout::showRailContextMenu);
    d->rail = rail;

    // sits between the buttons and the more button, and drags the pill
    auto *spacer = new DragGrip(rail);
    d->stripGrip = spacer;
    spacer->setMinimumHeight(0);
    spacer->setMaximumHeight(QWIDGETSIZE_MAX);
    spacer->setMinimumSize(24, 0);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    d->railSpacer = rail->addWidget(spacer);
    setPanelsTucked(d->panelsTucked);

    auto *addPanel = new QToolButton(rail);
    // "more panels", not a "+": a panel's own "+" (a new layer) sits right below it
    addPanel->setIcon(KisIconUtils::loadIcon(QStringLiteral("studio_more")));
    addPanel->setToolTip(i18nc("@info:tooltip", "More panels"));
    addPanel->setAccessibleName(addPanel->toolTip());
    d->addPanelButton = addPanel;
    connect(addPanel, &QToolButton::clicked, this, [this, addPanel]() {
        KisStudioPanelPicker::Rail rail;
        rail.key = panelsKey;
        rail.defaults = defaultRailPanels;
        rail.icon = [this](QDockWidget *dock) { return railIcon(dock); };
        auto *picker = new KisStudioPanelPicker(d->mainWindow, rail);
        connect(picker, &KisStudioPicker::sigPicked, this, [this](const QString &id) {
            QDockWidget *dock = d->mainWindow->dockWidget(id);
            if (!dock) return;
            if (!d->railPanels.contains(id)) {
                setRailPanels(d->railPanels + QStringList{id});
            }
            showPanel(dock);
        });
        // the picker may have starred, unstarred or reordered the rail's panels
        connect(picker, &QObject::destroyed, this, [this]() {
            const QStringList panels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
            if (panels != d->railPanels) setRailPanels(panels);
        });
        picker->popupBelow(addPanel);
    });
    rail->addWidget(addPanel);

    // shows where a dragged rail button will go
    auto *indicator = new QFrame(rail);
    indicator->setFrameShape(QFrame::NoFrame);
    indicator->setAutoFillBackground(true);
    indicator->setBackgroundRole(QPalette::Highlight);
    indicator->hide();
    d->dropIndicator = indicator;

    d->railPanels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
    rebuildRailButtons();
    // an undo or redo of the rail's panels
    connect(KisSettingsRegistry::instance(), &KisSettingsRegistry::valueChanged, rail, [this](const QString &id) {
        if (id != panelsKey) return;
        const QStringList panels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
        if (panels == d->railPanels) return;
        // a panel that lost its button closes, as one taken off the rail does; a pinned one in an undo or redo is
        // the arrangement's of the same step to put away, not a new step
        const bool replaying = KisSettingsRegistry::instance()->replaying();
        Q_FOREACH (const QString &gone, d->railPanels) {
            QDockWidget *dock = panels.contains(gone) ? nullptr : d->mainWindow->dockWidget(gone);
            if (!dock) continue;
            if (d->pinned.contains(gone)) {
                if (replaying) continue;
                setPinned(dock, false);
            }
            if (dock == d->flyout) closeFlyout();
            dock->hide();
        }
        d->railPanels = panels;
        rebuildRailButtons();
        arrangePinned();
    });
    // hidden until normalize() shows it in the column
    mainWindow->removeToolBar(rail);
    panelColumn();
}

void KisStudioLayout::rebuildRailButtons()
{
    QToolBar *rail = d->rail;
    if (!rail) {
        return;
    }

    Q_FOREACH (QAction *action, d->railActions) {
        rail->removeAction(action);
        action->deleteLater();
    }
    d->railActions.clear();

    QStringList panels;
    Q_FOREACH (const QString &id, d->railPanels) {
        QDockWidget *dock = d->mainWindow->dockWidget(id);
        if (!dock || panels.contains(id) || KisStudioPanelPicker::isRetired(id)) {
            continue;
        }
        panels << id;

        QAction *action = new QAction(railIcon(dock), dock->windowTitle(), rail);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, id](bool checked) {
            if (QDockWidget *dock = d->mainWindow->dockWidget(id)) {
                // while the panels are tucked away, a button brings them back with its own panel shown
                togglePanel(dock, checked || d->panelsTucked);
                syncRail();
            }
        });
        rail->insertAction(d->railSpacer, action);
        if (QWidget *button = rail->widgetForAction(action)) button->setProperty(KisModernStyle::tabButtonProperty, true);
        d->railActions.insert(id, action);
    }
    d->railPanels = panels;

    syncRail();
}

void KisStudioLayout::setRailPanels(const QStringList &panels, bool arranged)
{
    d->railPanels = panels;
    if (arranged && d->active && !d->reloadingArrangement) {
        // written with the arrangement, as one step
        d->pendingPanels = panels;
        d->panelsPending = true;
        commitArrangement();
    } else {
        KisSettingsRegistry::writeItems(panelsKey, panels);
    }
    rebuildRailButtons();
    arrangePinned();
}

QIcon KisStudioLayout::railIcon(QDockWidget *dock) const
{
    const QString iconName = railIconName(dock->objectName());
    if (!iconName.isEmpty()) {
        return KisIconUtils::loadIcon(iconName);
    }

    // the title's first letter, for panels without an icon of their own
    const int size = 44;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = d->rail ? d->rail->palette().color(QPalette::ButtonText) : QColor(Qt::gray);
    painter.setPen(QPen(color, 3));
    painter.drawPath(KisModernStyle::squircle(QRectF(3, 3, size - 6, size - 6), KisModernStyle::cardRadius));
    QFont font = painter.font();
    font.setPixelSize(24);
    font.setBold(true);
    painter.setFont(font);
    painter.drawText(QRect(0, 0, size, size), Qt::AlignCenter, dock->windowTitle().left(1).toUpper());
    painter.end();
    return QIcon(pixmap);
}

void KisStudioLayout::showRailContextMenu(const QPoint &pos)
{
    const QString clickedId = d->railActions.key(d->rail->actionAt(pos));
    if (!clickedId.isEmpty() && d->mainWindow->dockWidget(clickedId)) {
        QWidget *button = d->rail->widgetForAction(d->rail->actionAt(pos));
        showPanelCard(clickedId, button ? button : d->rail);
        return;
    }

    // clicked on the strip itself: panels are added with "+", so only Reset is offered
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(QStringLiteral("studio_more")),
                                       i18nc("@title panels rail", "Panels"), nullptr);
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("view-refresh")), i18nc("@info:tooltip", "Reset panels"),
                      [this]() {
                          // a pinned panel keeps its button, or it would close with it
                          QStringList panels = defaultRailPanels();
                          Q_FOREACH (const QString &id, d->railPanels) {
                              if (d->pinned.contains(id) && !panels.contains(id)) panels << id;
                          }
                          setRailPanels(panels);
                      }});
    auto *anchor = new QWidget(d->rail);
    anchor->setGeometry(QRect(pos, QSize(1, 1)));
    card->popupBeside(anchor);
    anchor->deleteLater();
}

void KisStudioLayout::takeOffRail(const QString &id)
{
    // close it too, so no panel stays open without a way back to it
    if (QDockWidget *dock = d->mainWindow->dockWidget(id)) {
        if (d->pinned.contains(id)) setPinned(dock, false);
        if (dock == d->flyout) closeFlyout();
        dock->hide();
    }
    QStringList panels = d->railPanels;
    panels.removeAll(id);
    setRailPanels(panels, true);
}

void KisStudioLayout::showPanelCard(const QString &id, QWidget *anchor)
{
    QDockWidget *dock = d->mainWindow->dockWidget(id);
    if (!dock) {
        return;
    }
    // the card edits its shortcut and can take it off the rail
    auto *card = new KisStudioItemCard(d->mainWindow, KisIconUtils::loadIcon(railIconName(id)),
                                       dock->windowTitle(), dock->toggleViewAction());
    card->addCommand({KisIconUtils::loadIcon(QStringLiteral("edit-delete")), i18nc("@info:tooltip", "Remove"),
                      [this, id]() { takeOffRail(id); }});
    card->popupBeside(anchor);
}

bool KisStudioLayout::handleRailDrag(QObject *watched, QEvent *event)
{
    Private::RailDrag &drag = d->railDrag;

    switch (event->type()) {
    case QEvent::MouseButtonPress: {
        auto *button = qobject_cast<QToolButton *>(watched);
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (!button || !d->rail || button->parentWidget() != d->rail || mouseEvent->button() != Qt::LeftButton) {
            return false;
        }
        const QString id = d->railActions.key(button->defaultAction());
        if (id.isEmpty()) {
            return false;
        }
        drag = Private::RailDrag();
        drag.button = button;
        drag.id = id;
        drag.pressPos = globalMousePos(mouseEvent);
        return false; // let the button show its pressed state
    }

    case QEvent::MouseMove: {
        auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (!drag.button || watched != drag.button || !(mouseEvent->buttons() & Qt::LeftButton)) {
            return false;
        }
        if (!drag.active) {
            if ((globalMousePos(mouseEvent) - drag.pressPos).manhattanLength() < KisModernStyle::dragDistance(mouseEvent)) {
                return false;
            }
            drag.active = true;
            drag.button->setDown(false);
        }

        // off the strip: show where the panel would land in the column or on the canvas
        const QPoint global = globalMousePos(mouseEvent);
        if (!d->overStrip(global)) {
            if (d->dropIndicator) d->dropIndicator->hide();
            d->showPanelDrop(global, drag.id);
            return true;
        }
        d->hidePanelDrop();

        // the gap between buttons closest to the pointer
        const int x = d->rail->mapFromGlobal(global).x();
        drag.targetIndex = d->railPanels.size();
        int indicatorX = 0;
        for (int i = 0; i < d->railPanels.size(); i++) {
            QWidget *widget = d->rail->widgetForAction(d->railActions.value(d->railPanels[i]));
            if (!widget) {
                continue;
            }
            indicatorX = widget->geometry().right() + 1;
            if (x < widget->geometry().center().x()) {
                drag.targetIndex = i;
                indicatorX = widget->geometry().left();
                break;
            }
        }
        if (d->dropIndicator) {
            d->dropIndicator->setGeometry(indicatorX - 1, 3, 2, d->rail->height() - 6);
            d->dropIndicator->show();
            d->dropIndicator->raise();
        }
        return true;
    }

    case QEvent::MouseButtonRelease: {
        if (!drag.button || watched != drag.button) {
            return false;
        }
        const bool wasActive = drag.active;
        const QString id = drag.id;
        int target = drag.targetIndex;
        QPointer<QToolButton> button = drag.button;
        drag = Private::RailDrag();
        if (d->dropIndicator) {
            d->dropIndicator->hide();
        }
        if (!wasActive) {
            // a plain click: let the button, still down, receive the release
            return false;
        }
        button->setDown(false);

        const QPoint release = globalMousePos(static_cast<QMouseEvent *>(event));
        d->hidePanelDrop();
        if (!d->overStrip(release)) {
            // pin it beside the canvas, or take it off the rail if dropped on the canvas
            QTimer::singleShot(0, this, [this, id, release]() { dropPanel(id, release, true); });
            return true;
        }

        QStringList panels = d->railPanels;
        const int from = panels.indexOf(id);
        if (from >= 0 && target >= 0) {
            panels.removeAt(from);
            if (target > from) {
                target--;
            }
            panels.insert(qBound(0, target, panels.size()), id);
            // deferred until the release is delivered, since the buttons are recreated
            QTimer::singleShot(0, this, [this, panels]() { setRailPanels(panels); });
        }
        return true;
    }

    default:
        return false;
    }
}

void KisStudioLayout::createBrushRail()
{
    if (d->brushRail) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;
    KisViewManager *viewManager = mainWindow->viewManager();
    if (!viewManager || !viewManager->paintOpBox()) {
        return;
    }

    auto *rail = new RailOverlay(mainWindow->centralWidget());
    auto *toolRail = new KisStudioToolRail(mainWindow, rail);
    d->toolRail = toolRail;
    if (KisStudioFind *find = mainWindow->studioFind()) {
        QPointer<KisStudioToolRail> target = toolRail;
        find->setTools([target](const QString &key) { return target && target->hasItem(key); },
                       [target](const QString &key, bool on) {
                           if (!target) return;
                           if (on) target->addItem(key);
                           else target->removeItem(key);
                       });
    }

    auto *contents = new QWidget(rail);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(0, KisModernStyle::hairline, 0, 0);
    layout->setSpacing(0);

    KisCanvasResourceProvider *resources = viewManager->canvasResourceProvider();
    auto *chip = new ColorChip(contents);
    auto refresh = [chip, resources]() {
        chip->setColors(resources->fgColor().toQColor(), resources->bgColor().toQColor());
    };
    refresh();
    connect(resources, &KisCanvasResourceProvider::sigFGColorChanged, chip, refresh);
    connect(resources, &KisCanvasResourceProvider::sigBGColorChanged, chip, refresh);
    // the colors are only known once there is a canvas
    connect(mainWindow, &KisMainWindow::activeViewChanged, chip, refresh);
    chip->swapColors = [resources]() {
        const KoColor foreground = resources->fgColor();
        resources->setFGColor(resources->bgColor());
        resources->setBGColor(foreground);
    };
    chip->pickColor = [chip, resources](bool background) {
        // the picker of that one color, beside the chip; the field is the chip's, gone when its picker closes
        auto *field = new KisStudioColorField(chip);
        field->setGeometry(chip->rect());
        field->hide();
        field->setColor((background ? resources->bgColor() : resources->fgColor()).toQColor());
        QObject::connect(field, &KisStudioColorField::colorChanged, chip, [resources, background](const QColor &color) {
            const KoColor picked(color, resources->fgColor().colorSpace());
            if (background) resources->setBGColor(picked);
            else resources->setFGColor(picked);
        });
        field->openPicker();
        if (auto *popup = field->findChild<KisStudioPopup *>()) QObject::connect(popup, &QObject::destroyed, field, &QObject::deleteLater);
    };
    chip->openColors = [this]() {
        if (QDockWidget *colors = d->mainWindow->dockWidget(KisStudioColorPanel::id())) {
            showPanel(colors);
        }
    };
    layout->addWidget(chip, 0, Qt::AlignHCenter);

    toolRail->addBottomWidget(contents);
    rail->layout()->addWidget(toolRail);
    rail->moved = [this](bool left, const QPointF &spot) {
        d->toolRailSpot = spot;
        writeSpot(toolRailSpotKey, spot);
        setToolRailSide(left);
        commitArrangement();
    };
    rail->tapped = [this]() { setToolRailTucked(!d->toolRailTucked); };
    // the panels on its side go beside it
    rail->reachChanged = [this]() {
        Q_FOREACH (KisStudioPanelColumn *column, d->columns()) column->place();
        updateCanvasInsets();
    };
    rail->setPlace(d->toolRailLeft, d->toolRailSpot);
    rail->setTucked(d->toolRailTucked);
    d->brushRail = rail;
}

void KisStudioLayout::createTopBar()
{
    if (d->topBar) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;

    QToolBar *bar = new QToolBar(i18nc("@title:window", "Tool"), mainWindow);
    bar->setObjectName(QStringLiteral("studioTopBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);

    auto *toolChip = new ToolChip(bar);
    toolChip->setIconSize(bar->iconSize());
    // a brush tool's chip carries its smoothing, dragged sideways; a click opens the brush editor
    const QPointer<KisMainWindow> window = mainWindow;
    toolChip->amount = [window]() { return KisStudioSmoothing::amount(window); };
    toolChip->setAmount = [window](qreal amount) { KisStudioSmoothing::setAmount(window, amount); };
    toolChip->amountText = [window]() { return KisStudioSmoothing::text(window); };
    toolChip->resetAmount = [window]() { KisStudioSmoothing::reset(window); };
    connect(toolChip, &QToolButton::clicked, this, [this, toolChip]() {
        if (KisStudioStrokePage::applies(d->mainWindow)) {
            KisStudioBrushEditor::toggle(d->mainWindow, toolChip, Qt::BottomEdge);
        } else {
            toggleToolOptions();
        }
    });
    KisStudioSmoothing::watch(mainWindow);
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigToolOptionsRequested, this, [this](KisMainWindow *window) {
        if (window != d->mainWindow) return;
        if (KisStudioBrushEditor::isOpen(d->mainWindow)) KisStudioBrushEditor::toggle(d->mainWindow, nullptr, Qt::BottomEdge);
        toggleToolOptions();
    });
    connect(KisStudioSmoothing::instance(), &KisStudioSmoothing::sigChanged, toolChip, qOverload<>(&QWidget::update));
    d->toolChipAction = bar->addWidget(toolChip);
    d->toolChip = toolChip;

    auto *strip = new KisStudioToolStrip(bar);
    d->toolStripAction = bar->addWidget(strip);
    d->toolStripAction->setVisible(false);
    d->toolStrip = strip;

    // size and opacity, only for the tools that paint with them
    if (KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr) {
        auto *values = new QWidget(bar);
        auto *valuesLayout = new QHBoxLayout(values);
        valuesLayout->setContentsMargins(KisModernStyle::space2, 0, 0, 0);
        valuesLayout->setSpacing(KisModernStyle::hairline);
        const QVector<QPair<QString, KisStudioScrubber::Kind>> scrubbers = {
            {QStringLiteral("size"), KisStudioScrubber::Kind::Size},
            {QStringLiteral("opacity"), KisStudioScrubber::Kind::Opacity},
        };
        for (const auto &entry : scrubbers) {
            KisDoubleSliderSpinBox *source = paintopBox->brushSlider(entry.first);
            if (!source) {
                continue;
            }
            auto *scrubber = new KisStudioScrubber(entry.second,
                                                   entry.second == KisStudioScrubber::Kind::Size
                                                       ? i18nc("@info:tooltip", "Brush size")
                                                       : i18nc("@info:tooltip", "Opacity"),
                                                   values);
            scrubber->setObjectName(entry.second == KisStudioScrubber::Kind::Size ? QStringLiteral("studioSize") : QStringLiteral("studioOpacity"));
            scrubber->setRange(entry.second == KisStudioScrubber::Kind::Size ? qMax(source->minimum(), 1.0) : source->minimum(),
                               source->maximum());
            scrubber->setValue(source->value());
            valuesLayout->addWidget(scrubber);
            if (entry.second == KisStudioScrubber::Kind::Opacity) d->opacityValue = scrubber;
            connect(scrubber, &KisStudioScrubber::valueCommitted, source, [source](qreal value) {
                source->setValue(value);
            });
            connect(paintopBox, &KisPaintopBox::sigBrushSliderValueChanged, scrubber,
                    [scrubber, id = entry.first](const QString &sliderID, qreal value) {
                        if (sliderID == id) {
                            scrubber->setValue(value);
                        }
                    });
            // the dab it paints, under it while it changes, as large as the canvas shows it
            QPointer<KisMainWindow> window = mainWindow;
            QPointer<KisDoubleSliderSpinBox> size = paintopBox->brushSlider(QStringLiteral("size"));
            QPointer<KisDoubleSliderSpinBox> opacity = paintopBox->brushSlider(QStringLiteral("opacity"));
            scrubber->setLook([window, size, opacity]() {
                KisStudioScrubber::Look look;
                KisViewManager *view = window ? window->viewManager() : nullptr;
                look.color = view && view->canvasResourceProvider() ? view->canvasResourceProvider()->fgColor().toQColor() : QColor(Qt::black);
                look.color.setAlpha(255);
                if (size) look.size = size->value();
                if (opacity) look.opacity = (opacity->value() - opacity->minimum()) / qMax(1e-6, opacity->maximum() - opacity->minimum());
                if (KisCanvas2 *canvas = view ? view->canvasBase() : nullptr) {
                    const KisCoordinatesConverter *converter = canvas->coordinatesConverter();
                    look.zoom = QLineF(converter->imageToWidget(QPointF(0, 0)), converter->imageToWidget(QPointF(1, 0))).length();
                }
                return look;
            });
            // a right click resets to the brush's value from when it was picked
            scrubber->setDefaultValue(source->value());
            if (KisCanvasResourceProvider *resources = mainWindow->viewManager()->canvasResourceProvider()) {
                connect(resources, &KisCanvasResourceProvider::sigPaintOpPresetChanged, scrubber, [scrubber, source]() {
                    QTimer::singleShot(0, scrubber, [scrubber, source]() { scrubber->setDefaultValue(source->value()); });
                });
            }
        }
        d->brushValuesAction = bar->addWidget(values);
        d->brushValuesAction->setVisible(false);
    }

    // shows the document's name, with every open document a click away
    auto *documents = new QToolButton(mainWindow);
    documents->setObjectName(QStringLiteral("studio_documents"));
    documents->setAutoRaise(true);
    documents->setFocusPolicy(Qt::NoFocus);
    documents->setToolButtonStyle(Qt::ToolButtonTextOnly);
    documents->setPopupMode(QToolButton::InstantPopup);
    documents->setToolTip(i18nc("@info:tooltip", "Open documents"));
    documents->hide();
    documents->setMenu(new QMenu(documents));
    d->documentButton = documents;

    KisStudioGlass::setSeeThrough(bar, true);
    d->topBar = bar;

    auto *buttonsBar = new QToolBar(i18nc("@title:window", "Buttons"), mainWindow);
    buttonsBar->setObjectName(QStringLiteral("studioButtonsBar"));
    buttonsBar->setMovable(false);
    buttonsBar->setFloatable(false);
    buttonsBar->setContextMenuPolicy(Qt::PreventContextMenu);
    buttonsBar->addWidget(new KisStudioBarButtons(mainWindow, buttonsBar));
    KisStudioGlass::setSeeThrough(buttonsBar, true);
    d->buttonsBar = buttonsBar;

    // placed on the window pill by placeWindowButtons()
    auto *menuButton = new PillButton(QStringLiteral("studio_menu"), i18nc("@info:tooltip", "Menu"), mainWindow);
    menuButton->setAccessibleName(i18nc("@action:button", "Menu"));
    menuButton->hide();
    connect(menuButton, &QAbstractButton::clicked, this, [this, menuButton]() { showMainMenu(menuButton); });
    d->menuButton = menuButton;

    auto *settingsButton = new PillButton(QStringLiteral("studio_settings"), i18nc("@info:tooltip", "Settings\nRight-click: Quick"), mainWindow);
    settingsButton->hide();
    settingsButton->setProperty("ownContextMenu", true);
    connect(settingsButton, &QAbstractButton::clicked, this, [this]() { openSettings(QString(), true); });
    settingsButton->contextClicked = [this, settingsButton]() {
        if (d->settings && d->active) d->settings->showQuick(settingsButton);
    };
    d->settingsButton = settingsButton;

    auto *field = new PillButton(QStringLiteral("studio_find"), QString(), mainWindow);
    // its key is all its tooltip says
    field->setProperty("keyOnlyTip", true);
    field->setProperty("runsAction", QStringLiteral("find_anything"));
    field->setAccessibleName(i18nc("@info:placeholder", "Find anything"));
    field->hide();
    connect(field, &QAbstractButton::clicked, this, [this]() {
        if (QAction *find = d->mainWindow->actionCollection()->action(QStringLiteral("find_anything"))) {
            find->trigger();
        }
    });
    d->findField = field;
}

void KisStudioLayout::adoptTopBars()
{
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area) {
        return;
    }
    if (!d->windowHandle) {
        auto *handle = new WindowHandle(area);
        handle->clicked = [this]() { showHome(true); };
        d->windowHandle = handle;
    }
    ensureSettings();
    if (!d->topPill) {
        d->topPill = new TopPill(area, false);
        d->topPill->changed = [self = QPointer<KisStudioLayout>(this)]() { if (self) self->updateCanvasInsets(); };
        d->topPill->grip()->tapped = [this]() { setTopBarTucked(!d->topBarTucked); };
        d->topPill->moved = [this](const QPointF &spot) {
            d->topBarSpot = spot;
            writeSpot(topBarSpotKey, spot, d->topPill->spotWidth());
            updateCanvasInsets();
            commitArrangement();
        };
        d->topPill->setTucked(d->topBarTucked);
        d->topPill->setSpot(d->topBarSpot, readSpotHeight(topBarSpotKey));
    }
    if (!d->windowPill) {
        d->windowPill = new TopPill(area, true);
        d->windowPill->changed = [self = QPointer<KisStudioLayout>(this)]() { if (self) self->updateCanvasInsets(); };
        auto *bar = new QToolBar(i18nc("@title:window", "Window"), d->windowPill);
        bar->setObjectName(QStringLiteral("studioWindowBar"));
        bar->setMovable(false);
        bar->setFloatable(false);
        bar->setContextMenuPolicy(Qt::PreventContextMenu);
        KisStudioGlass::setSeeThrough(bar, true);
        d->windowPill->insert(0, bar);
        d->windowBar = bar;
    }

    // move the tool bar and brush bar into the top pill; restoring a window state or Configure Toolbars can take the brush bar back
    const bool tools = d->active && !isRailHidden();
    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    int index = 1;
    for (QToolBar *bar : {d->topBar.data(), brushBar, d->buttonsBar.data()}) {
        if (!bar) continue;
        if (bar->parentWidget() != d->topPill->contents()) {
            d->mainWindow->removeToolBar(bar);
            bar->setMovable(false);
            KisStudioGlass::setSeeThrough(bar, true);
            d->topPill->insert(index, bar);
        }
        index++;
    }
    // show them after inserting: removeToolBar() hides them, as does restoring a window state with no place for them
    if (d->topBar) d->topBar->setVisible(tools);
    if (d->buttonsBar) d->buttonsBar->setVisible(tools);
    d->topPill->setSqueezable(d->toolChip);
    if (brushBar) brushBar->show();
    if (d->windowBar) d->windowBar->show();

    // the window pill shows on the start page too, as its title bar
    const bool window = d->active && (!isCanvasOnly() || d->peeking.contains(QPointer<QWidget>(d->windowPill.data())));
    d->topPill->setVisible(tools);
    d->windowHandle->setVisible(tools);
    d->windowPill->setVisible(window);
    if (tools) d->topPill->place();
    if (window) d->windowPill->place();
    updateCanvasInsets();
}

void KisStudioLayout::placeWindowHandle()
{
    QWidget *handle = d->windowHandle;
    TopPill *pill = d->topPill;
    if (!handle || !pill) return;
    const int height = pill->height();
    QRect place(TopPill::margin, TopPill::margin, height, height);
    // cap a tool rail in the top left corner, matching its width
    QWidget *rail = d->brushRail;
    QWidget *area = handle->parentWidget();
    if (rail && rail->isVisible() && rail->parentWidget() == area && rail->x() <= area->property(leftPillProperty).toInt() + 16
        && rail->y() < place.bottom() + 24) {
        place.setLeft(rail->x());
        place.setWidth(rail->width());
    }
    if (handle->geometry() != place) handle->setGeometry(place);
    keepAbove(handle);
    area->setProperty(handleBottomProperty, handle->isVisible() ? place.bottom() + 1 : 0);
    TopPill *window = d->windowPill;
    pill->setInsets(handle->isVisible() ? place.right() + 1 : 0, window && window->isVisible() ? area->width() - window->x() : 0);
}

void KisStudioLayout::releaseTopBars()
{
    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    if (brushBar && brushBar->parentWidget() != d->mainWindow) {
        const bool shown = !brushBar->isHidden();
        KisStudioGlass::setSeeThrough(brushBar, false);
        brushBar->setMovable(true);
        d->mainWindow->addToolBar(Qt::TopToolBarArea, brushBar);
        brushBar->setVisible(shown);
    }
    for (QToolButton *button : qAsConst(d->delayedPopups)) {
        if (button) button->setPopupMode(QToolButton::MenuButtonPopup);
    }
    d->delayedPopups.clear();
    d->placedOnBar = nullptr;
    if (d->topPill) d->topPill->hide();
    if (d->windowPill) d->windowPill->hide();
    if (d->windowHandle) d->windowHandle->hide();
    if (d->settings) d->settings->hide();
    updateCanvasInsets();
}

void KisStudioLayout::ensureSettings()
{
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (d->settings || !area) {
        return;
    }
    d->settings = new KisStudioSettings(d->mainWindow, area);
    // below the window pill on the right, as tall as the canvas allows
    d->settings->setPlacement([this, area](const QSize &wanted) {
        const int margin = TopPill::margin;
        const QRect pill = d->windowPill && d->windowPill->isVisible() ? d->windowPill->geometry() : QRect(area->width() - margin, margin, 0, 0);
        const int top = pill.bottom() + 1 + margin;
        const int width = qMin(wanted.width(), area->width() - 2 * margin);
        const int right = pill.isEmpty() ? area->width() - margin : pill.right() + 1;
        return QRect(qMax(margin, right - width), top, width, qMax(120, area->height() - top - margin));
    });
}

void KisStudioLayout::openSettings(const QString &cardId, bool toggle)
{
    // works with the Studio layout off too, as there is no other settings UI
    ensureSettings();
    if (!d->settings) {
        return;
    }
    if (toggle && d->settings->isVisible() && cardId.isEmpty()) d->settings->hide();
    else d->settings->open(cardId);
}

void KisStudioLayout::showMainMenu(QWidget *button)
{
    // the most used file commands, then the menu bar's menus; rebuilt each time to pick up recent files
    if (!d->mainMenu) d->mainMenu = new QMenu(d->mainWindow);
    QMenu *menu = d->mainMenu;
    menu->clear();
    qDeleteAll(menu->findChildren<QMenu *>(QString(), Qt::FindDirectChildrenOnly));
    KisKActionCollection *actions = d->mainWindow->actionCollection();
    QSet<QAction *> shown;
    auto add = [&](std::initializer_list<const char *> names) {
        for (const char *name : names) {
            if (QAction *action = actions->action(QLatin1String(name))) {
                menu->addAction(action);
                shown.insert(action);
            }
        }
        menu->addSeparator();
    };
    add({"file_new", "file_open", "file_open_recent"});
    add({"file_save", "file_save_as", "file_export_file"});
    Q_FOREACH (QAction *top, d->mainWindow->menuBar()->actions()) {
        if (!top->menu() || !top->isVisible() || !hasContent(top->menu())) continue;
        if (top->menu()->objectName() != QLatin1String("file")) {
            menu->addAction(top);
            continue;
        }
        // File without the commands listed above it
        QMenu *file = menu->addMenu(top->text());
        bool separate = false;
        Q_FOREACH (QAction *action, top->menu()->actions()) {
            if (action->isSeparator()) {
                separate = !file->isEmpty();
            } else if (!shown.contains(action) && action->isVisible() && action->objectName() != QLatin1String("file_quit")) {
                if (separate) file->addSeparator();
                separate = false;
                file->addAction(action);
            }
        }
    }
    menu->addSeparator();
    add({"file_quit"});

    // right-aligned under the button
    const QSize size = menu->sizeHint();
    const QPoint corner = button->mapToGlobal(QPoint(button->width(), button->height() + 4));
    menu->popup(QPoint(corner.x() - size.width(), corner.y()));
}

void KisStudioLayout::placeWindowButtons()
{
    // add each widget to the window pill's bar once
    auto place = [this](QWidget *widget, QPointer<QAction> &action) {
        if (!widget || action) return;
        auto *widgetAction = new QWidgetAction(this);
        widgetAction->setDefaultWidget(widget);
        action = widgetAction;
        if (d->windowBar) d->windowBar->addAction(widgetAction);
    };
    place(d->documentButton, d->documentButtonAction);
    place(d->menuButton, d->menuButtonAction);
    place(d->settingsButton, d->settingsButtonAction);
    place(d->findField, d->findFieldAction);
    if (d->frame && d->frame->isEnabled() && !d->windowButtonsAction) {
        place(d->frame->createButtons(d->mainWindow), d->windowButtonsAction);
    }

    QToolBar *brushBar = d->mainWindow->findChild<QToolBar *>(QStringLiteral("BrushesAndStuff"));
    if (!brushBar || brushBar == d->placedOnBar) {
        return;
    }
    d->placedOnBar = brushBar;
    // menus open on a long press instead of by a separate arrow
    Q_FOREACH (QToolButton *button, brushBar->findChildren<QToolButton *>()) {
        if (button->menu() && button->popupMode() == QToolButton::MenuButtonPopup) {
            button->setPopupMode(QToolButton::DelayedPopup);
            d->delayedPopups << button;
        }
    }
}

void KisStudioLayout::updateToolChip()
{
    if (!d->toolChip) {
        return;
    }
    const QString tool = KisStudioTools::activeToolId();
    KoToolFactoryBase *factory = KoToolRegistry::instance()->value(tool);
    if (d->toolChipAction) {
        d->toolChipAction->setVisible(factory && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
    }
    if (!factory) {
        return;
    }
    d->toolChip->setIcon(KisIconUtils::loadIcon(factory->iconName()));
    // brush tools show the brush's name; the tool shows as its icon, with its name on hover
    const bool brushTool = tool == QLatin1String("KritaShape/KisToolBrush") || tool == QLatin1String("KritaShape/KisToolDyna")
        || tool == QLatin1String("KritaShape/KisToolMultiBrush");
    KisViewManager *view = d->mainWindow->viewManager();
    KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
    const QString brush = brushTool && resources && resources->currentPreset() ? KisStudioBrushPicker::title(resources->currentPreset()->name()) : QString();
    // on the bar beside its icon, which already says what kind it is: the selections by their shape alone
    static const QHash<QString, QString> shortNames = {
        {QStringLiteral("KisToolSelectRectangular"), i18nc("the rectangular selection tool, short", "Rectangle")},
        {QStringLiteral("KisToolSelectElliptical"), i18nc("the elliptical selection tool, short", "Ellipse")},
        {QStringLiteral("KisToolSelectPolygonal"), i18nc("the polygonal selection tool, short", "Polygon")},
        {QStringLiteral("KisToolSelectOutline"), i18nc("the freehand selection tool, short", "Lasso")},
        {QStringLiteral("KisToolSelectContiguous"), i18nc("the contiguous selection tool, short", "Magic wand")},
        {QStringLiteral("KisToolSelectSimilar"), i18nc("the similar color selection tool, short", "Same color")},
        {QStringLiteral("KisToolSelectPath"), i18nc("the bezier curve selection tool, short", "Curve")},
        {QStringLiteral("KisToolSelectMagnetic"), i18nc("the magnetic selection tool, short", "Magnetic")},
    };
    const QString toolName = shortNames.value(tool, KisStudioToolRail::toolName(factory));
    d->toolChip->setText(brush.isEmpty() ? toolName : brush);
    // a right click resets the smoothing, instead of showing the chip's card
    d->toolChip->setProperty("ownContextMenu", bool(KisStudioSmoothing::options(d->mainWindow)));
    if (KisStudioSmoothing::options(d->mainWindow)) {
        d->toolChip->setToolTip(i18nc("@info:tooltip the brush chip: drag sideways for smoothing, click for the brush's settings", "Drag: smoothing\nClick: brush settings\nRight-click: reset"));
    } else if (KisStudioStrokePage::applies(d->mainWindow)) {
        d->toolChip->setToolTip(i18nc("@info:tooltip", "Brush settings"));
    } else {
        d->toolChip->setToolTip(brush.isEmpty() ? i18nc("@info:tooltip", "Tool options") : i18nc("@info:tooltip tool chip: the tool, its options", "%1 options", toolName));
    }
    d->toolChip->update();
    d->toolChip->updateGeometry();
}

void KisStudioLayout::updateContextBar()
{
    if (!d->active) {
        return;
    }

    const QString tool = KisStudioTools::activeToolId();
    auto any = [&tool](std::initializer_list<const char *> ids) {
        for (const char *id : ids) {
            if (tool == QLatin1String(id)) return true;
        }
        return false;
    };

    const bool shapeTool = any({"KritaShape/KisToolRectangle", "KritaShape/KisToolEllipse", "KritaShape/KisToolSuperellipse", "KisToolPolygon",
                                "KisToolPolyline", "KisToolPath", "KisToolPencil"});
    // tools that put down paint with the current brush preset
    const bool paints = shapeTool || any({"KritaShape/KisToolBrush", "KritaShape/KisToolDyna",
                                          "KritaShape/KisToolMultiBrush", "KritaShape/KisToolLine", "KritaShape/KisToolCurveStroke"});
    // the smart patch marks what to patch with the brush's outline: only its size counts
    const bool sizeOnly = any({"KritaShape/KisToolSmartPatch"});
    const bool fills = any({"KritaFill/KisToolFill", "KisToolEncloseAndFill", "KritaShape/KisToolLazyBrush"});
    const bool gradient = any({"KritaFill/KisToolGradient"});

    const QVector<QPair<const char *, bool>> rules = {
        {"paintops", paints},
        {"paintop_options", paints},
        {"composite_actions", paints},
        // moved into Symmetry on the tool strip
        {"mirror_actions", false},
        {"gradients", gradient},
        {"patterns", fills || shapeTool},
        // the color chip on the rail replaces it
        {"dual", false},
        // set up once rather than per stroke; available in Find
        {"workspaces", false},
        {"select_layout", false},
    };

    if (d->brushValuesAction) {
        d->brushValuesAction->setVisible((paints || sizeOnly) && (!isRailHidden() || d->peeking.contains(QPointer<QWidget>(d->topPill.data()))));
        if (d->opacityValue) d->opacityValue->setVisible(!sizeOnly);
    }
    updateToolChip();
    if (d->flyout && d->flyoutAnchor) {
        // the new tool's options arrive later, at a different size
        QTimer::singleShot(0, this, &KisStudioLayout::positionFlyout);
    }

    KisKActionCollection *actions = d->mainWindow->actionCollection();
    for (const auto &rule : rules) {
        QAction *action = actions->action(QLatin1String(rule.first));
        if (!action || d->hiddenToolbarActions.contains(action)) {
            continue;
        }
        if (!d->contextActions.contains(action)) {
            d->contextActions << action;
        }
        action->setVisible(rule.second);
    }
}

void KisStudioLayout::syncRail()
{
    for (auto it = d->railActions.begin(); it != d->railActions.end(); ++it) {
        QDockWidget *dock = d->mainWindow->dockWidget(it.key());
        QSignalBlocker blocker(it.value());
        it.value()->setChecked(dock && !dock->isHidden());
    }
}

bool KisStudioLayout::handlePresetChip(QEvent *event)
{
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick:
        // the picker opens on the release
        return static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton;
    case QEvent::MouseButtonRelease: {
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (mouse->button() != Qt::LeftButton) {
            return false;
        }
        // only if released over the chip, so not after a long press that ends elsewhere
        if (d->presetChip->rect().contains(mouse->pos())) {
            auto *picker = new KisStudioBrushPicker(d->mainWindow);
            picker->popupBelow(d->presetChip);
        }
        return true;
    }
    case QEvent::ContextMenu:
        // Krita's preset popup: tags, importing, editing
        QMetaObject::invokeMethod(d->presetChip, "showPopupWidget");
        return true;
    case QEvent::Wheel: {
        // a notch is one brush; a touchpad's small steps add up to one
        const int steps = KisModernStyle::wheelSteps(static_cast<QWheelEvent *>(event), d->presetChip);
        if (!steps) {
            return true;
        }
        const int step = steps < 0 ? 1 : -1;
        KisViewManager *view = d->mainWindow->viewManager();
        KisCanvasResourceProvider *resources = view ? view->canvasResourceProvider() : nullptr;
        if (resources && resources->currentPreset()) {
            const QString next = KisStudioBrushPicker::neighbour(d->mainWindow, resources->currentPreset()->name(), step);
            if (!next.isEmpty()) {
                KisStudioBrushPicker::use(view, next);
            }
        }
        return true;
    }
    default:
        return false;
    }
}

void KisStudioLayout::peek(const QPoint &global)
{
    QMdiArea *mdi = mdiArea();
    QWidget *area = mdi ? mdi->parentWidget() : nullptr;
    if (!area || !isCanvasOnly() || !d->active) {
        return;
    }
    const QPoint at = area->mapFromGlobal(global);
    if (!area->rect().contains(at)) {
        return;
    }
    const int reach = 6;
    // most moves are nowhere near an edge, with nothing brought back to keep
    if (d->peeking.isEmpty() && area->rect().adjusted(reach, reach, -reach, -reach).contains(at)) {
        return;
    }
    // what each edge brings back: the bars at the top, the tool rail and the panels at their sides
    const bool toolsLeft = d->brushRail && d->brushRail->x() < area->width() / 2;
    struct Edge {
        QWidget *widget;
        bool near;
    };
    QList<Edge> edges = {
        // first, so the top bar is placed beside it: the menu and the window's buttons, the way out of the mode
        {d->windowPill.data(), at.y() < reach},
        {d->topPill.data(), d->topPill && !d->topPill->isAtTop() ? at.y() >= area->height() - reach : at.y() < reach},
        {d->brushRail.data(), toolsLeft ? at.x() < reach : at.x() >= area->width() - reach},
        {d->panelColumn.data(), d->panelColumn && d->panelColumn->isLeft() ? at.x() < reach : at.x() >= area->width() - reach},
    };
    Q_FOREACH (KisStudioPanelColumn *other, d->columns()) {
        if (other == d->panelColumn) continue;
        edges.append({other, other->panelsShown() && (other->isLeft() ? at.x() < reach : at.x() >= area->width() - reach)});
    }
    for (const Edge &edge : edges) {
        QWidget *widget = edge.widget;
        if (!widget) continue;
        const bool shown = d->peeking.contains(widget);
        // keep it while the pointer is over or near it
        const bool over = shown && widget->geometry().adjusted(-48, -48, 48, 48).contains(widget->parentWidget()->mapFrom(area, at));
        if (edge.near && !shown) {
            d->peeking << widget;
            if (widget == d->topPill) {
                // show it whole: canvas-only mode hides its tool and brush parts
                if (d->topBar) d->topBar->show();
                if (d->buttonsBar) d->buttonsBar->show();
                if (d->toolStripAction && d->toolStrip) d->toolStripAction->setVisible(d->toolStrip->hasOptions());
                updateContextBar();
                d->topPill->place();
            } else if (widget == d->windowPill) {
                // canvas-only mode hides its buttons and its bar
                for (QAction *action : {d->menuButtonAction.data(), d->settingsButtonAction.data(), d->findFieldAction.data()}) {
                    if (action) action->setVisible(true);
                }
                if (d->windowButtonsAction) d->windowButtonsAction->setVisible(d->frame && d->frame->isEnabled());
                if (d->windowBar) d->windowBar->show();
                d->windowPill->place();
            }
            widget->raise();
            KisStudioMotion::showFaded(widget);
        } else if (shown && !edge.near && !over) {
            d->peeking.removeAll(widget);
            KisStudioMotion::hideFaded(widget);
        }
    }
}

void KisStudioLayout::tidyCommand(QAction *action)
{
    if (!action || action->isSeparator() || action->objectName().isEmpty()) return;
    // what only the classic layout has goes, also when Krita makes it again (the toolbars' menu, About KDE)
    if (classicOnlyActions().contains(action->objectName())) {
        if (action->isVisible()) {
            action->setVisible(false);
            d->hiddenToolbarActions << action;
        }
        return;
    }
    // commands Krita names in code keep its Title Case ("Gaussian Blur…", "Merge with Layer Below"); items without
    // a name of their own (recent files, workspaces) are the user's and keep theirs
    QString text = KisSentenceCase::of(action->text());
    // the transform tool's liquify mode, as its own command, is called what the bar calls it
    if (action->objectName() == QLatin1String("KisToolTransformLiquify")) {
        text = i18nc("@action the transform tool's liquify mode", "Push");
        // on a layer with nothing on it, Push takes the topmost one that has paint
        if (!action->property("skissaPushFindsPaint").toBool()) {
            action->setProperty("skissaPushFindsPaint", true);
            connect(action, &QAction::triggered, this, [this, action]() {
                KisViewManager *view = d->mainWindow ? d->mainWindow->viewManager() : nullptr;
                KisImageSP image = view ? view->image() : nullptr;
                if (!image) return;
                auto painted = [](KisNodeSP node) {
                    return node && node->paintDevice() && qobject_cast<KisLayer *>(node.data()) && !node->paintDevice()->exactBounds().isEmpty();
                };
                if (painted(view->activeNode())) return;
                std::function<KisNodeSP(KisNodeSP)> topmost = [&](KisNodeSP parent) -> KisNodeSP {
                    for (KisNodeSP child = parent->lastChild(); child; child = child->prevSibling()) {
                        if (!child->visible() || !qobject_cast<KisLayer *>(child.data())) continue;
                        if (child->childCount() && !qobject_cast<KisPaintLayer *>(child.data())) {
                            if (KisNodeSP inside = topmost(child)) return inside;
                        } else if (painted(child)) {
                            return child;
                        }
                    }
                    return nullptr;
                };
                if (KisNodeSP found = topmost(image->root())) {
                    // the transform tool started on the empty layer: put away, the layer picked, and Push again, so it
                    // starts afresh on it in Push (a layer picked under it would restart it in its first mode)
                    KisStudioTools::switchTo(QStringLiteral("KritaShape/KisToolBrush"));
                    view->nodeManager()->slotNonUiActivatedNode(found);
                    QTimer::singleShot(0, action, [action]() { action->trigger(); });
                } else {
                    view->showFloatingMessage(i18nc("@info:status", "Nothing to push"), KisIconUtils::loadIcon(QStringLiteral("studio_push")), 2000);
                }
            });
        }
    }
    if (text != action->text()) action->setText(text);
}

