/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisStudioLayout.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include <functional>
#include <QHash>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
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
    return icons.value(id, QStringLiteral("hamburger_menu_dots"));
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
    QMenu *allPanelsMenu {nullptr};
    QHash<QString, QAction *> railActions;

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
    QAction *canvasOnly = d->mainWindow->actionCollection()->action(QStringLiteral("view_show_canvas_only"));
    return d->mainWindow->isWelcomeScreenShown() || (canvasOnly && canvasOnly->isChecked());
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
    rail->setContextMenuPolicy(Qt::PreventContextMenu);
    mainWindow->addToolBar(Qt::RightToolBarArea, rail);
    d->rail = rail;

    const QStringList ids = KisConfig(true).readEntry<QStringList>(panelsKey, defaultRailPanels());
    Q_FOREACH (const QString &id, ids) {
        QDockWidget *dock = mainWindow->dockWidget(id);
        if (!dock || !d->headers.contains(dock)) {
            continue;
        }

        QAction *action = rail->addAction(KisIconUtils::loadIcon(railIconName(id)), dock->windowTitle());
        action->setCheckable(true);
        connect(action, &QAction::triggered, this, [this, id](bool checked) {
            if (QDockWidget *dock = d->mainWindow->dockWidget(id)) {
                togglePanel(dock, checked);
            }
        });
        d->railActions.insert(id, action);
    }

    auto *spacer = new QWidget(rail);
    spacer->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    rail->addWidget(spacer);

    auto *allPanels = new QToolButton(rail);
    allPanels->setIcon(KisIconUtils::loadIcon(QStringLiteral("hamburger_menu_dots")));
    allPanels->setToolTip(i18nc("@info:tooltip", "All panels"));
    allPanels->setAccessibleName(allPanels->toolTip());
    allPanels->setPopupMode(QToolButton::InstantPopup);
    d->allPanelsMenu = new QMenu(allPanels);
    allPanels->setMenu(d->allPanelsMenu);
    connect(d->allPanelsMenu, &QMenu::aboutToShow, this, &KisStudioLayout::populateMoreMenu);
    rail->addWidget(allPanels);
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
