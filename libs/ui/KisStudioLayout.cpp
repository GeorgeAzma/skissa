/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <functional>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>

#include <kactioncollection.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisResourceServerProvider.h"
#include "KisStudioBrushSlider.h"
#include "KisViewManager.h"
#include "kis_paintop_box.h"
#include "kis_slider_spin_box.h"
#include "kis_workspace_resource.h"
#include "KisUiFont.h"
#include "kis_config.h"
#include "kis_icon_utils.h"

namespace {

const QString toolBoxId = QStringLiteral("ToolBox");

const QString activeKey = QStringLiteral("StudioLayoutActive");
const QString panelsKey = QStringLiteral("StudioLayoutPanels");
const QString pinnedKey = QStringLiteral("StudioLayoutPinnedPanels");
const QString classicStateKey = QStringLiteral("StudioLayoutClassicState");

/// The panels that get a button on the rail; every other docker is in the "All panels" menu
QStringList defaultRailPanels()
{
    return {
        QStringLiteral("KisLayerBox"),
        QStringLiteral("PresetDocker"),
        QStringLiteral("ColorSelectorNg"),
        QStringLiteral("sharedtooldocker"),
        QStringLiteral("PaletteDocker"),
        QStringLiteral("History"),
    };
}

QPoint globalMousePos(const QMouseEvent *event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    return event->globalPosition().toPoint();
#else
    return event->globalPos();
#endif
}

QString railIconName(const QString &id)
{
    static const QHash<QString, QString> icons = {
        {QStringLiteral("KisLayerBox"), QStringLiteral("all-layers")},
        {QStringLiteral("PresetDocker"), QStringLiteral("paintop_settings_01")},
        {QStringLiteral("ColorSelectorNg"), QStringLiteral("wheel-rings")},
        {QStringLiteral("WideGamutColorSelector"), QStringLiteral("wheel-sectors")},
        {QStringLiteral("sharedtooldocker"), QStringLiteral("configure-thicker")},
        {QStringLiteral("PaletteDocker"), QStringLiteral("palette-library")},
        {QStringLiteral("History"), QStringLiteral("edit-undo")},
    };
    return icons.value(id);
}

/**
 * The title bar of a studio panel: its name, a pin button and a close button.
 * Panels can't be dragged around; where they go is decided by pinning.
 */
class StudioPanelHeader : public QWidget
{
public:
    explicit StudioPanelHeader(QDockWidget *dock)
        : QWidget(dock)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 2, 2, 2);
        layout->setSpacing(0);

        QFont titleFont = KisUiFont::dockFont();
        titleFont.setBold(true);
        m_title = new QLabel(dock->windowTitle(), this);
        m_title->setFont(titleFont);
        layout->addWidget(m_title, 1);
        connect(dock, &QWidget::windowTitleChanged, m_title, &QLabel::setText);

        m_pin = createButton();
        m_pin->setCheckable(true);
        layout->addWidget(m_pin);

        m_close = createButton();
        m_close->setIcon(KisIconUtils::loadIcon(QStringLiteral("close-tab")));
        m_close->setToolTip(i18nc("@info:tooltip", "Close panel"));
        m_close->setAccessibleName(m_close->toolTip());
        layout->addWidget(m_close);

        setPinned(false);
    }

    QToolButton *pinButton() const { return m_pin; }
    QToolButton *closeButton() const { return m_close; }

    void setPinned(bool pinned)
    {
        QSignalBlocker blocker(m_pin);
        m_pin->setChecked(pinned);
        m_pin->setIcon(KisIconUtils::loadIcon(pinned ? QStringLiteral("locked") : QStringLiteral("unlocked")));
        m_pin->setToolTip(pinned ? i18nc("@info:tooltip", "Unpin panel")
                                 : i18nc("@info:tooltip", "Pin panel next to the canvas"));
        m_pin->setAccessibleName(m_pin->toolTip());
    }

protected:
    void mouseDoubleClickEvent(QMouseEvent *event) override
    {
        // don't let QDockWidget float or redock the panel on double click
        event->accept();
    }

private:
    QToolButton *createButton()
    {
        auto *button = new QToolButton(this);
        button->setAutoRaise(true);
        button->setIconSize(QSize(14, 14));
        button->setFocusPolicy(Qt::NoFocus);
        return button;
    }

    QLabel *m_title {nullptr};
    QToolButton *m_pin {nullptr};
    QToolButton *m_close {nullptr};
};

} // namespace

struct KisStudioLayout::Private
{
    KisMainWindow *mainWindow {nullptr};
    bool active {false};

    QPointer<QToolBar> rail;
    /// Brush size and opacity on the left edge
    QPointer<QToolBar> brushRail;
    /// Replaces the menu bar with a single menu button while a document is open
    QPointer<QToolBar> topBar;
    QMenu *mainMenu {nullptr};
    /// Toolbar entries that duplicate the brush rail, hidden while active
    QList<QPointer<QAction>> hiddenToolbarActions;
    QMenu *allPanelsMenu {nullptr};
    /// The panels on the rail, in order
    QStringList railPanels;
    QHash<QString, QAction *> railActions;
    /// Rail buttons are inserted in front of it
    QAction *railSpacer {nullptr};

    /// Reordering a rail button by dragging it
    struct RailDrag {
        QPointer<QToolButton> button;
        QString id;
        QPoint pressPos;
        bool active {false};
        int targetIndex {-1};
    } railDrag;
    QPointer<QFrame> dropIndicator;

    /// The unpinned panel that is currently open over the canvas
    QPointer<QDockWidget> flyout;
    QSet<QString> pinned;

    /// What a docker looked like before entering the studio layout
    struct ClassicDock {
        QPointer<QWidget> titleBar;
        QDockWidget::DockWidgetFeatures features;
    };
    QHash<QDockWidget *, ClassicDock> classicDocks;
    QHash<QDockWidget *, StudioPanelHeader *> headers;
    QVector<QMetaObject::Connection> connections;
};

KisStudioLayout::KisStudioLayout(KisMainWindow *mainWindow)
    : QObject(mainWindow)
    , d(new Private)
{
    d->mainWindow = mainWindow;

    /**
     * Canvas-only mode restores a saved window state when it is toggled,
     * partly through the event queue, so normalize after it is done.
     */
    if (QAction *canvasOnly = mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"))) {
        connect(canvasOnly, &QAction::toggled, this, [this]() {
            closeFlyout();
            QTimer::singleShot(0, this, &KisStudioLayout::normalize);
        });
    }
}

KisStudioLayout::~KisStudioLayout()
{
    if (d->active) {
        qApp->removeEventFilter(this);
    }
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

    // remember the classic layout to come back to, when switching away from it
    if (active && !isEnabledInConfig()) {
        cfg.writeEntry(classicStateKey, d->mainWindow->saveState());
    }
    cfg.writeEntry(activeKey, active);

    if (active) {
        d->active = true;
        enter();
    } else {
        d->active = false;
        leave();
    }
}

void KisStudioLayout::enter()
{
    KisMainWindow *mainWindow = d->mainWindow;
    const QStringList pinned = KisConfig(true).readEntry<QStringList>(pinnedKey);
    d->pinned = QSet<QString>(pinned.begin(), pinned.end());

    Q_FOREACH (QDockWidget *dock, mainWindow->dockWidgets()) {
        QWidget *titleBar = dock->titleBarWidget();
        if (titleBar && titleBar->inherits("KisUtilityTitleBar")) {
            // e.g. the animation timeline keeps its controls in the title bar
            continue;
        }

        d->classicDocks.insert(dock, {titleBar, dock->features()});

        if (dock->objectName() == toolBoxId) {
            dock->setTitleBarWidget(new QWidget(dock));
            dock->setFeatures(QDockWidget::NoDockWidgetFeatures);
            continue;
        }

        auto *header = new StudioPanelHeader(dock);
        dock->setTitleBarWidget(header);
        // closable only: panels are moved by pinning, not by dragging
        dock->setFeatures(QDockWidget::DockWidgetClosable);
        d->headers.insert(dock, header);

        d->connections << connect(header->pinButton(), &QToolButton::toggled, this, [this, dock](bool pinned) {
            setPinned(dock, pinned);
        });
        d->connections << connect(header->closeButton(), &QToolButton::clicked, this, [this, dock]() {
            togglePanel(dock, false);
        });
        d->connections << connect(dock->toggleViewAction(), &QAction::toggled, this, [this, dock](bool visible) {
            dockToggled(dock, visible);
        });
    }

    createRail();
    createBrushRail();
    createTopBar();

    // the brush rail already has the size and opacity sliders
    KisConfig cfg(true);
    for (int i = 1; i <= 5; i++) {
        const QString slider = cfg.toolbarSlider(i);
        QAction *action = mainWindow->actionCollection()->action(QStringLiteral("brushslider%1").arg(i));
        if (action && action->isVisible() && (slider == QLatin1String("size") || slider == QLatin1String("opacity"))) {
            action->setVisible(false);
            d->hiddenToolbarActions << action;
        }
    }

    qApp->installEventFilter(this);
    normalize();
}

void KisStudioLayout::leave()
{
    closeFlyout();
    qApp->removeEventFilter(this);

    Q_FOREACH (const QMetaObject::Connection &connection, d->connections) {
        disconnect(connection);
    }
    d->connections.clear();

    const bool showTitleBars = KisConfig(true).showDockerTitleBars();
    for (auto it = d->classicDocks.begin(); it != d->classicDocks.end(); ++it) {
        QDockWidget *dock = it.key();
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
    d->headers.clear();

    if (d->rail) {
        d->rail->hide();
    }
    if (d->brushRail) {
        d->brushRail->hide();
    }
    if (d->topBar) {
        d->topBar->hide();
    }
    if (!isCanvasOnly()) {
        d->mainWindow->menuBar()->show();
    }
    Q_FOREACH (QAction *action, d->hiddenToolbarActions) {
        if (action) {
            action->setVisible(true);
        }
    }
    d->hiddenToolbarActions.clear();

    KisConfig cfg(true);
    const QByteArray classicState = cfg.readEntry<QByteArray>(classicStateKey);
    if (!classicState.isEmpty()) {
        d->mainWindow->restoreWorkspaceState(classicState);
    } else {
        // never used the classic layout yet, show what a fresh Krita would
        const QString workspaceName = cfg.readEntry<QString>(QStringLiteral("CurrentWorkspace"), QStringLiteral("Default"));
        KoResourceServer<KisWorkspaceResource> *server = KisResourceServerProvider::instance()->workspaceServer();
        KisWorkspaceResourceSP workspace = server->resource(QString(), QString(), workspaceName);
        if (!workspace) {
            workspace = server->resource(QString(), QString(), QStringLiteral("Default"));
        }
        if (workspace) {
            d->mainWindow->restoreWorkspace(workspace);
        }
    }
}

void KisStudioLayout::normalize()
{

    if (!d->active) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;
    const bool panelsHidden = arePanelsHidden();

    /**
     * The pinned panels from the config decide what is shown, not the
     * restored state: it may come from a classic workspace or from the
     * window state saved before the studio layout was enabled.
     */
    for (auto it = d->headers.begin(); it != d->headers.end(); ++it) {
        QDockWidget *dock = it.key();
        if (dock == d->flyout) {
            continue;
        }

        // a flyout never outlives the moment it was opened for
        if (dock->isFloating()) {
            dock->hide();
            dock->setFloating(false);
        }

        it.value()->show();

        const bool isPinned = d->pinned.contains(dock->objectName());
        it.value()->setPinned(isPinned);
        if (isPinned) {
            if (mainWindow->dockWidgetArea(dock) != Qt::RightDockWidgetArea) {
                mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
            }
            if (!panelsHidden) {
                dock->show();
            }
        } else {
            dock->hide();
        }
    }

    if (QDockWidget *toolBox = mainWindow->dockWidget(toolBoxId)) {
        if (mainWindow->dockWidgetArea(toolBox) != Qt::LeftDockWidgetArea) {
            mainWindow->addDockWidget(Qt::LeftDockWidgetArea, toolBox);
        }
        if (!panelsHidden) {
            toolBox->show();
        }
    }

    if (d->rail) {
        d->rail->setVisible(!isRailHidden());
    }
    if (d->brushRail) {
        d->brushRail->setVisible(!isRailHidden());
    }
    if (d->topBar) {
        d->topBar->setVisible(!isRailHidden());
        moveTopBarToFront();
    }
    // canvas-only mode manages the menu bar itself, the welcome page has no top bar
    if (!isCanvasOnly()) {
        mainWindow->menuBar()->setVisible(mainWindow->isWelcomeScreenShown());
    }

    syncRail();
}

void KisStudioLayout::dockToggled(QDockWidget *dock, bool visible)
{

    syncRail();

    if (visible && dock != d->flyout && !d->pinned.contains(dock->objectName())) {
        /**
         * Something else showed an unpinned panel, e.g. the Dockers menu,
         * a shortcut or a restored workspace. Open it as a flyout, unless
         * it has been hidden again in the meantime, which is what
         * normalize() does after restoring a workspace.
         */
        QTimer::singleShot(0, this, [this, dock]() {
            if (d->active && !dock->isHidden() && dock != d->flyout
                && !d->pinned.contains(dock->objectName()) && !arePanelsHidden()) {
                showFlyout(dock);
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
    QAction *canvasOnly = d->mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"));
    return canvasOnly && canvasOnly->isChecked();
}

void KisStudioLayout::closeFlyout()
{

    QDockWidget *dock = d->flyout;
    d->flyout = nullptr;
    if (!dock) {
        return;
    }

    dock->hide();
    dock->setFloating(false);
    syncRail();
}

void KisStudioLayout::togglePanel(QDockWidget *dock, bool show)
{

    if (show) {
        if (d->pinned.contains(dock->objectName())) {
            dock->show();
            dock->raise();
        } else {
            showFlyout(dock);
        }
    } else if (dock == d->flyout) {
        closeFlyout();
    } else {
        dock->hide();
    }
}

void KisStudioLayout::showFlyout(QDockWidget *dock)
{

    if (d->flyout && d->flyout != dock) {
        closeFlyout();
    }

    d->flyout = dock;
    dock->setFloating(true);
    positionFlyout();
    dock->show();
    dock->raise();
    syncRail();
}

void KisStudioLayout::positionFlyout()
{
    QWidget *canvasArea = d->mainWindow->centralWidget();
    if (!d->flyout || !canvasArea) {
        return;
    }

    // a column along the right edge of the canvas, next to the pinned panels
    const QRect area(canvasArea->mapToGlobal(QPoint(0, 0)), canvasArea->size());
    const int margin = 6;
    const int contentsWidth = d->flyout->widget() ? d->flyout->widget()->sizeHint().width() : 0;
    const int width = qBound(260, contentsWidth + 2, 420);

    d->flyout->setGeometry(area.right() + 1 - margin - width, area.top() + margin,
                           width, area.height() - 2 * margin);
}

void KisStudioLayout::setPinned(QDockWidget *dock, bool pinned)
{

    const QString id = dock->objectName();

    if (pinned) {
        d->pinned.insert(id);
        if (dock == d->flyout) {
            d->flyout = nullptr;
        }
        dock->setFloating(false);
        d->mainWindow->addDockWidget(Qt::RightDockWidgetArea, dock);
        dock->show();
    } else {
        d->pinned.remove(id);
        const bool wasVisible = !dock->isHidden();
        dock->hide();
        if (wasVisible) {
            showFlyout(dock);
        }
    }

    if (StudioPanelHeader *header = d->headers.value(dock)) {
        header->setPinned(pinned);
    }
    savePinned();
    syncRail();
}

void KisStudioLayout::savePinned()
{
    QStringList pinned = d->pinned.values();
    pinned.sort();
    KisConfig(false).writeEntry(pinnedKey, pinned);
}

void KisStudioLayout::createRail()
{
    if (d->rail) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;

    QToolBar *rail = new QToolBar(i18nc("@title:window", "Panels"), mainWindow);
    rail->setObjectName(QStringLiteral("studioPanelRail"));
    rail->setMovable(false);
    rail->setFloatable(false);
    rail->setOrientation(Qt::Vertical);
    rail->setToolButtonStyle(Qt::ToolButtonIconOnly);
    rail->setIconSize(QSize(22, 22));
    rail->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(rail, &QWidget::customContextMenuRequested, this, &KisStudioLayout::showRailContextMenu);
    mainWindow->addToolBar(Qt::RightToolBarArea, rail);
    d->rail = rail;

    auto *spacer = new QWidget(rail);
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    d->railSpacer = rail->addWidget(spacer);

    auto *allPanels = new QToolButton(rail);
    allPanels->setIcon(KisIconUtils::loadIcon(QStringLiteral("hamburger_menu_dots")));
    allPanels->setToolTip(i18nc("@info:tooltip", "All panels"));
    allPanels->setAccessibleName(allPanels->toolTip());
    d->allPanelsMenu = new QMenu(allPanels);
    connect(allPanels, &QToolButton::clicked, this, [this, allPanels]() {
        populateMoreMenu();
        // open to the left of the rail
        const QSize size = d->allPanelsMenu->sizeHint();
        d->allPanelsMenu->popup(allPanels->mapToGlobal(QPoint(-size.width(), allPanels->height() - size.height())));
    });
    rail->addWidget(allPanels);

    // shows where a dragged rail button will go
    auto *indicator = new QFrame(rail);
    indicator->setFrameShape(QFrame::NoFrame);
    indicator->setAutoFillBackground(true);
    indicator->setBackgroundRole(QPalette::Highlight);
    indicator->hide();
    d->dropIndicator = indicator;

    d->railPanels = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
    rebuildRailButtons();
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
        if (!dock || panels.contains(id)) {
            continue;
        }
        panels << id;

        QAction *action = new QAction(railIcon(dock), dock->windowTitle(), rail);
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, id](bool checked) {
            if (QDockWidget *dock = d->mainWindow->dockWidget(id)) {
                togglePanel(dock, checked);
            }
        });
        rail->insertAction(d->railSpacer, action);
        d->railActions.insert(id, action);
    }
    d->railPanels = panels;

    syncRail();
}

void KisStudioLayout::setRailPanels(const QStringList &panels)
{
    d->railPanels = panels;
    KisConfig(false).writeEntry(panelsKey, panels);
    rebuildRailButtons();
}

QIcon KisStudioLayout::railIcon(QDockWidget *dock) const
{
    const QString iconName = railIconName(dock->objectName());
    if (!iconName.isEmpty()) {
        return KisIconUtils::loadIcon(iconName);
    }

    // the first letter of the title, for panels without an icon of their own
    const int size = 44;
    QPixmap pixmap(size, size);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor color = d->rail ? d->rail->palette().color(QPalette::ButtonText) : QColor(Qt::gray);
    painter.setPen(QPen(color, 3));
    painter.drawRoundedRect(QRectF(3, 3, size - 6, size - 6), 9, 9);
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
    QMenu menu;

    const QString clickedId = d->railActions.key(d->rail->actionAt(pos));
    if (!clickedId.isEmpty()) {
        menu.addAction(i18nc("@action:inmenu", "Remove from Rail"), this, [this, clickedId]() {
            QStringList panels = d->railPanels;
            panels.removeAll(clickedId);
            setRailPanels(panels);
        });
        menu.addSeparator();
    }

    menu.addSection(i18nc("@title:menu", "Panels on the Rail"));
    QList<QDockWidget *> docks = d->headers.keys();
    std::sort(docks.begin(), docks.end(), [](QDockWidget *a, QDockWidget *b) {
        return QString::localeAwareCompare(a->windowTitle(), b->windowTitle()) < 0;
    });
    Q_FOREACH (QDockWidget *dock, docks) {
        const QString id = dock->objectName();
        QAction *action = menu.addAction(dock->windowTitle());
        action->setCheckable(true);
        action->setChecked(d->railPanels.contains(id));
        connect(action, &QAction::toggled, this, [this, id](bool onRail) {
            QStringList panels = d->railPanels;
            panels.removeAll(id);
            if (onRail) {
                panels << id;
            }
            setRailPanels(panels);
        });
    }

    menu.addSeparator();
    menu.addAction(i18nc("@action:inmenu", "Reset Rail"), this, [this]() {
        setRailPanels(defaultRailPanels());
    });

    menu.exec(d->rail->mapToGlobal(pos));
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
            if ((globalMousePos(mouseEvent) - drag.pressPos).manhattanLength() < QApplication::startDragDistance()) {
                return false;
            }
            drag.active = true;
            drag.button->setDown(false);
        }

        // the gap between rail buttons that is closest to the cursor
        const int y = d->rail->mapFromGlobal(globalMousePos(mouseEvent)).y();
        drag.targetIndex = d->railPanels.size();
        int indicatorY = 0;
        for (int i = 0; i < d->railPanels.size(); i++) {
            QWidget *widget = d->rail->widgetForAction(d->railActions.value(d->railPanels[i]));
            if (!widget) {
                continue;
            }
            indicatorY = widget->geometry().bottom() + 1;
            if (y < widget->geometry().center().y()) {
                drag.targetIndex = i;
                indicatorY = widget->geometry().top();
                break;
            }
        }
        if (d->dropIndicator) {
            d->dropIndicator->setGeometry(3, indicatorY - 1, d->rail->width() - 6, 2);
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
        drag.button->setDown(false);
        drag = Private::RailDrag();
        if (d->dropIndicator) {
            d->dropIndicator->hide();
        }
        if (!wasActive) {
            return false; // a plain click
        }

        QStringList panels = d->railPanels;
        const int from = panels.indexOf(id);
        if (from >= 0 && target >= 0) {
            panels.removeAt(from);
            if (target > from) {
                target--;
            }
            panels.insert(qBound(0, target, panels.size()), id);
            // the buttons are recreated, do it after the release has been delivered
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
    KisPaintopBox *paintopBox = mainWindow->viewManager() ? mainWindow->viewManager()->paintOpBox() : nullptr;
    if (!paintopBox) {
        return;
    }

    QToolBar *rail = new QToolBar(i18nc("@title:window", "Brush"), mainWindow);
    rail->setObjectName(QStringLiteral("studioBrushRail"));
    rail->setMovable(false);
    rail->setFloatable(false);
    rail->setOrientation(Qt::Vertical);
    rail->setContextMenuPolicy(Qt::PreventContextMenu);

    auto *contents = new QWidget(rail);
    auto *layout = new QVBoxLayout(contents);
    layout->setContentsMargins(2, 8, 2, 8);
    layout->setSpacing(16);

    struct SliderInfo {
        QString id;
        QString title;
        qreal exponent;
        std::function<QString(qreal)> format;
    };
    const QVector<SliderInfo> sliders = {
        // the same response curve as the size slider in the toolbar
        {QStringLiteral("size"), i18nc("@info:tooltip", "Brush size"), 3.0,
         [](qreal value) { return QString::number(value, 'f', value < 10.0 ? 1 : 0); }},
        {QStringLiteral("opacity"), i18nc("@info:tooltip", "Opacity"), 1.0,
         [](qreal value) { return i18nc("brush opacity, %1 is the number of percent", "%1%", qRound(value)); }},
    };

    Q_FOREACH (const SliderInfo &info, sliders) {
        KisDoubleSliderSpinBox *source = paintopBox->brushSlider(info.id);
        if (!source) {
            continue;
        }

        auto *slider = new KisStudioBrushSlider(info.title, contents);
        slider->setValueFormatter(info.format);
        slider->setRange(source->minimum(), source->maximum(), info.exponent);
        slider->setValue(source->value());
        layout->addWidget(slider, 0, Qt::AlignHCenter);

        // setting the toolbar slider applies the value to the current preset
        connect(slider, &KisStudioBrushSlider::valueCommitted, source, [source](qreal value) {
            source->setValue(value);
        });
        connect(paintopBox, &KisPaintopBox::sigBrushSliderValueChanged, slider,
                [slider, id = info.id](const QString &sliderID, qreal value) {
                    if (sliderID == id) {
                        slider->setValue(value);
                    }
                });
    }
    layout->addStretch(1);

    rail->addWidget(contents);
    mainWindow->addToolBar(Qt::LeftToolBarArea, rail);
    d->brushRail = rail;
}

void KisStudioLayout::createTopBar()
{
    if (d->topBar) {
        return;
    }

    KisMainWindow *mainWindow = d->mainWindow;

    QToolBar *bar = new QToolBar(i18nc("@title:window", "Main Menu"), mainWindow);
    bar->setObjectName(QStringLiteral("studioTopBar"));
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);

    auto *menuButton = new QToolButton(bar);
    menuButton->setIcon(KisIconUtils::loadIcon(QStringLiteral("hamburger_menu")));
    menuButton->setToolTip(i18nc("@info:tooltip", "Main menu"));
    menuButton->setAccessibleName(menuButton->toolTip());
    menuButton->setAutoRaise(true);

    // the menus of the hidden menu bar; collected on every show, since
    // plugins and xmlgui can change them
    d->mainMenu = new QMenu(menuButton);
    connect(d->mainMenu, &QMenu::aboutToShow, this, [this]() {
        d->mainMenu->clear();
        // QMenu skips the hidden ones itself
        d->mainMenu->addActions(d->mainWindow->menuBar()->actions());
    });
    // not setMenu(), it would draw a drop-down arrow next to the icon
    connect(menuButton, &QToolButton::clicked, this, [this, menuButton]() {
        d->mainMenu->popup(menuButton->mapToGlobal(QPoint(0, menuButton->height())));
    });
    bar->addWidget(menuButton);

    mainWindow->addToolBar(Qt::TopToolBarArea, bar);
    d->topBar = bar;
}

void KisStudioLayout::moveTopBarToFront()
{
    KisMainWindow *mainWindow = d->mainWindow;
    if (!d->topBar) {
        return;
    }

    // the leftmost toolbar of the first row, the xmlgui ones are created
    // after the studio layout and restored states can move them around
    QToolBar *first = nullptr;
    Q_FOREACH (QToolBar *toolBar, mainWindow->findChildren<QToolBar *>(QString(), Qt::FindDirectChildrenOnly)) {
        if (toolBar == d->topBar || toolBar->isHidden() || mainWindow->toolBarArea(toolBar) != Qt::TopToolBarArea) {
            continue;
        }
        const QPoint pos = toolBar->pos();
        if (!first || pos.y() < first->pos().y() || (pos.y() == first->pos().y() && pos.x() < first->pos().x())) {
            first = toolBar;
        }
    }

    if (first && first->pos().y() <= d->topBar->pos().y() && first->pos().x() < d->topBar->pos().x()) {
        mainWindow->insertToolBar(first, d->topBar);
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

void KisStudioLayout::populateMoreMenu()
{
    d->allPanelsMenu->clear();

    QList<QDockWidget *> docks = d->headers.keys();
    std::sort(docks.begin(), docks.end(), [](QDockWidget *a, QDockWidget *b) {
        return QString::localeAwareCompare(a->windowTitle(), b->windowTitle()) < 0;
    });

    Q_FOREACH (QDockWidget *dock, docks) {
        QAction *action = d->allPanelsMenu->addAction(dock->windowTitle());
        action->setCheckable(true);
        action->setChecked(!dock->isHidden());
        connect(action, &QAction::triggered, this, [this, dock](bool checked) {
            togglePanel(dock, checked);
        });
    }
}

bool KisStudioLayout::eventFilter(QObject *watched, QEvent *event)
{
    if (handleRailDrag(watched, event)) {
        return true;
    }

    switch (event->type()) {
    case QEvent::Paint: {
        // a flyout has no window frame, give it a hairline border
        QDockWidget *dock = qobject_cast<QDockWidget *>(watched);
        if (dock && dock->isFloating() && d->headers.contains(dock)) {
            QColor border = dock->palette().color(QPalette::WindowText);
            border.setAlphaF(0.25);
            QPainter painter(dock);
            painter.setPen(border);
            painter.drawRect(dock->rect().adjusted(0, 0, -1, -1));
        }
        break;
    }

    case QEvent::MouseButtonPress:
    case QEvent::TabletPress:
        // clicking anywhere else in the window closes the flyout, the
        // rail handles clicks on its own buttons
        if (d->flyout && d->flyout->isVisible()) {
            QWidget *widget = qobject_cast<QWidget *>(watched);
            const bool onRail = d->rail && widget && (widget == d->rail || d->rail->isAncestorOf(widget));
            if (widget && widget->window() == d->mainWindow && !onRail) {
                closeFlyout();
            }
        }
        break;

    case QEvent::KeyPress:
        if (d->flyout && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape
            && d->flyout->isAncestorOf(QApplication::focusWidget())) {
            closeFlyout();
            return true;
        }
        break;

    case QEvent::Move:
    case QEvent::Resize:
        if (d->flyout && (watched == d->mainWindow || watched == d->mainWindow->centralWidget())) {
            positionFlyout();
        }
        break;

    default:
        break;
    }

    return QObject::eventFilter(watched, event);
}
