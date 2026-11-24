/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOLAYOUT_P_H
#define KISSTUDIOLAYOUT_P_H

#include "KisStudioPanelColumn_p.h"

// What KisStudioLayout keeps, shared by the files that make it up: KisStudioLayout.cpp (entering, leaving,
// arrangement, events), KisStudioLayoutPanels.cpp (panels, flyouts, groups) and KisStudioLayoutBars.cpp (the
// rails, the top bar and their chips)

struct KisStudioLayout::Private
{
    KisMainWindow *mainWindow {nullptr};
    bool active {false};
    /// Arranging again after an undo: nothing done then is a step of its own
    bool reloadingArrangement {false};
    bool commitPending {false};
    /// The panel buttons as a gesture left them, written in its step (see setRailPanels())
    bool panelsPending {false};
    QStringList pendingPanels;

    /// The strip of panel buttons above the pinned panels
    QPointer<QToolBar> rail;
    QPointer<QToolButton> addPanelButton;
    /// The tool rail, with the color chip under its tools
    QPointer<QWidget> brushRail;
    QPointer<KisStudioToolRail> toolRail;
    QPointer<QWidget> windowHandle;
    /// Holds the tool chip, the tool options, size and opacity
    QPointer<QToolBar> topBar;
    /// The bar's arrangeable switches and commands, after the brush's own controls
    QPointer<QToolBar> buttonsBar;
    QPointer<QWidget> findField;
    QPointer<QAction> findFieldAction;
    QPointer<QWidget> settingsButton;
    QPointer<QAction> settingsButtonAction;
    QPointer<KisStudioSettings> settings;
    /// When the main menu last closed: a press on its button closes it, and the click that follows mustn't open it again
    /// The brush bar whose menu buttons were switched to open on a long press, and those buttons
    QPointer<QToolBar> placedOnBar;
    QList<QPointer<QToolButton>> delayedPopups;
    /// Toolbar entries that duplicate the top bar's scrubbers
    QList<QPointer<QAction>> hiddenToolbarActions;
    /// Toolbar entries shown only for the tools they matter to
    QList<QPointer<QAction>> contextActions;
    /// Toolbars that only duplicate the main menu or shortcuts
    QList<QPointer<QToolBar>> hiddenToolBars;
    /// The side and order the pinned panels were last arranged in
    QString arrangement;
    QStringList railPanels;
    QHash<QString, QAction *> railActions;
    /// Rail buttons are inserted before this
    QAction *railSpacer {nullptr};

    /// State of dragging a rail button to reorder it
    struct RailDrag {
        QPointer<QToolButton> button;
        QString id;
        QPoint pressPos;
        bool active {false};
        int targetIndex {-1};
    } railDrag;
    QPointer<QFrame> dropIndicator;
    /// Shows where a dragged panel would land beside the canvas
    QPointer<QFrame> panelIndicator;

    /// Looked up once: canvas-only mode asks at every pointer move
    QPointer<QAction> canvasOnlyAction;
    QPointer<QMdiArea> mdiArea;
    QSet<QString> pinned;
    /// Pinned panels put away with their button: normalize() leaves them hidden
    QSet<QString> hiddenPinned;
    QHash<QString, int> heights;
    QSet<QString> folded;
    QHash<QString, QPointer<PanelGrip>> grips;
    QPointer<KisStudioPanelColumn> panelColumn;
    /// The groups of pinned panels other than the column with the panel buttons: 1 across the canvas from it,
    /// then floating ones, each a column of its own
    QMap<int, QPointer<KisStudioPanelColumn>> groupColumns;
    /// Shown in place of Krita's welcome page, which is kept aside meanwhile
    QPointer<KisStudioStartPage> startPage;
    QPointer<QWidget> classicWelcome;
    /// Which group each pinned panel is in, where it isn't the column with the panel buttons
    QHash<QString, int> groups;
    QPointer<TopPill> topPill;
    QPointer<TopPill> windowPill;
    /// In canvas-only mode, what the pointer brought back by nearing its edge (see peek())
    QList<QPointer<QWidget>> peeking;
    QPointer<QToolBar> windowBar;
    bool panelsLeft {false};
    bool toolRailLeft {true};
    /// Where the tool rail and the top bar were dragged to (see shareOf())
    QPointF toolRailSpot;
    QPointF topBarSpot;
    bool panelsTucked {false};
    bool toolRailTucked {false};
    bool toolRailFlat {false};
    bool topBarTucked {false};
    /// Where the bars were when the groups were last placed: the groups keep off them (see barRects)
    QVector<QRect> barsPlacedFor;
    QPointer<QWidget> stripGrip;
    /// The pinned panels' lanes in their groups, for those not in the first (see lanesKey)
    QHash<QString, int> lanes;
    /// Each group's lanes' widths, left to right
    QHash<int, QVector<int>> laneWidths;

    /// The unpinned panel open over the canvas, and the widget it opened from (it is placed below it)
    QPointer<QDockWidget> flyout;
    QPointer<FlyoutWindow> flyoutWindow;
    QPointer<QWidget> flyoutAnchor;
    QPointer<QToolButton> toolChip;
    /// The flyout needs refitting to its contents
    bool flyoutRefit {false};
    QPointer<QAction> toolChipAction;
    QPointer<KisStudioToolStrip> toolStrip;
    QPointer<QAction> toolStripAction;
    QPointer<QAction> brushValuesAction;
    /// The opacity among them, left out for tools that only use the brush's size
    QPointer<QWidget> opacityValue;
    /// The brush bar's preset button: a click opens the brush picker, the wheel steps through your brushes
    QPointer<QWidget> presetChip;
    QString presetChipToolTip;
    QPointer<KisStudioBrushMemory> brushMemory;
    QPointer<KisStudioCanvasRing> canvasRing;
    QPointer<QToolButton> documentButton;
    QPointer<QAction> documentButtonAction;
    QPointer<KisStudioCanvasHud> hud;
    QPointer<KisStudioEffectOverlay> effectOverlay;
    KisStudioWindowFrame *frame {nullptr};
    QPointer<QAction> windowButtonsAction;

    /// A docker's title bar and features, restored when leaving the Studio layout
    struct ClassicDock {
        QPointer<QWidget> titleBar;
        QDockWidget::DockWidgetFeatures features;
    };
    QHash<QDockWidget *, ClassicDock> classicDocks;
    /// Take away Skissa's pages over Krita's panels (KisStudioPanelPages)
    QList<std::function<void()>> pageUndos;
    /// Every docker except the toolbox
    QSet<QDockWidget *> panels;
    QVector<QMetaObject::Connection> connections;

    /// The pinned panels in stacking order: rail order first, then the rest
    QStringList pinnedOrder() const
    {
        QStringList order;
        Q_FOREACH (const QString &id, railPanels) {
            if (pinned.contains(id)) order << id;
        }
        Q_FOREACH (const QString &id, pinned) {
            if (!order.contains(id)) order << id;
        }
        return order;
    }

    int groupOf(const QString &id) const { return groups.value(id, 0); }
    int laneOf(const QString &id) const { return lanes.value(id, 0); }

    /// Every column of pinned panels, the one with the panel buttons first
    QList<KisStudioPanelColumn *> columns() const
    {
        QList<KisStudioPanelColumn *> all;
        if (panelColumn) all << panelColumn;
        for (const QPointer<KisStudioPanelColumn> &column : groupColumns) {
            if (column) all << column;
        }
        return all;
    }

    /// The pinned panels shown in a group (in one of its lanes, unless \p lane is -1), top to bottom, excluding \p without
    QList<QDockWidget *> column(const QString &without, int group = 0, int lane = -1) const
    {
        QList<QDockWidget *> docks;
        Q_FOREACH (const QString &id, pinnedOrder()) {
            QDockWidget *dock = mainWindow->dockWidget(id);
            if (id != without && groupOf(id) == group && (lane < 0 || laneOf(id) == lane) && dock && dock != flyout && !dock->isHidden()
                && !dock->isFloating()) {
                docks << dock;
            }
        }
        std::sort(docks.begin(), docks.end(), [this](QDockWidget *a, QDockWidget *b) { return placeOf(a).y() < placeOf(b).y(); });
        return docks;
    }

    /// \p dock's geometry in main window coordinates (panels in the column aren't its direct children)
    QRect placeOf(const QWidget *dock) const
    {
        return QRect(dock->mapTo(mainWindow, QPoint()), dock->size());
    }

    PanelDrop panelDropAt(const QPoint &globalPos, const QString &moving) const
    {
        PanelDrop drop;
        QWidget *canvasArea = mainWindow->centralWidget();
        if (!canvasArea) return drop;
        const QPoint pos = mainWindow->mapFromGlobal(globalPos);
        const QRect canvas = canvasArea->geometry();
        // the column with the panel buttons first, then the one across the canvas, then the floating groups
        QList<int> order = {0, 1};
        for (auto it = groupColumns.constBegin(); it != groupColumns.constEnd(); ++it) {
            if (it.key() >= 2 && it.value()) order << it.key();
        }
        for (int group : order) {
            const QList<QDockWidget *> docks = column(moving, group);
            const KisStudioPanelColumn *own = group == 0 ? panelColumn.data() : groupColumns.value(group).data();
            const bool left = group == 0 ? panelsLeft : group == 1 ? !panelsLeft : own && own->isLeft();
            QRect area;
            Q_FOREACH (QDockWidget *dock, docks) {
                area |= placeOf(dock);
            }
            // a group with other panels: all of its pill, the dragged panel's place too
            if (!area.isEmpty() && own && own->isVisible()) {
                const QRect pill = own->targetGeometry();
                area |= QRect(own->parentWidget()->mapTo(mainWindow, pill.topLeft()), pill.size()).adjusted(1, 1, -1, -1);
            }
            // over a group, or near the canvas edge where the two edge columns have no panels
            const bool overColumn = !area.isEmpty() && area.adjusted(-8, -24, 8, 24).contains(pos);
            const bool nearEdge = area.isEmpty() && group <= 1 && canvas.contains(pos)
                                  && (left ? pos.x() < canvas.left() + edgeDropZone : pos.x() > canvas.right() - edgeDropZone);
            if (!overColumn && !nearEdge) continue;

            drop.column = true;
            drop.left = left;
            drop.group = group;
            if (area.isEmpty()) {
                drop.indicator = QRect(left ? canvas.left() + 2 : canvas.right() - 4, canvas.top() + 6, 3, canvas.height() - 12);
                return drop;
            }
            // the lanes it has, in window coordinates; near the group's left or right edge, a new lane there
            QVector<QRect> lanes;
            for (int i = 0; own && i < own->stacks().size(); i++) {
                lanes << QRect(own->parentWidget()->mapTo(mainWindow, own->laneRect(i).topLeft()), own->laneRect(i).size());
            }
            const int side = 20;
            if (pos.x() < area.left() + side || pos.x() > area.right() - side) {
                drop.newLane = true;
                drop.lane = pos.x() < area.left() + side ? 0 : lanes.size();
                drop.indicator = QRect(pos.x() < area.left() + side ? area.left() - 1 : area.right() - 1, area.top() + 6, 3, area.height() - 12);
                return drop;
            }
            for (int i = 0; i < lanes.size(); i++) {
                if (pos.x() >= lanes[i].left()) drop.lane = i;
            }
            const QList<QDockWidget *> inLane = column(moving, group, drop.lane);
            QRect laneArea = lanes.value(drop.lane, area);
            laneArea.setTop(area.top());
            laneArea.setBottom(area.bottom());
            int y = area.bottom() - 1;
            Q_FOREACH (QDockWidget *dock, inLane) {
                if (pos.y() < placeOf(dock).center().y()) {
                    drop.before = dock->objectName();
                    y = placeOf(dock).top() - 1;
                    break;
                }
            }
            if (drop.before.isEmpty() && !inLane.isEmpty()) {
                QRect below;
                Q_FOREACH (QDockWidget *dock, inLane) below |= placeOf(dock);
                y = below.bottom() - 1;
            }
            drop.indicator = QRect(laneArea.left() + 4, qMax(area.top(), y - 1), laneArea.width() - 8, 3);
            return drop;
        }
        // anywhere else on the canvas: a floating group of its own, there
        if (canvas.contains(pos)) {
            const int width = panelColumn ? panelColumn->columnWidth() : 270;
            drop.column = true;
            drop.fresh = true;
            drop.at = pos;
            drop.indicator = QRect(pos.x() - width / 2, pos.y() - 1, width, 3);
        }
        return drop;
    }

    void showPanelDrop(const QPoint &globalPos, const QString &moving)
    {
        if (!panelIndicator) {
            panelIndicator = new QFrame(mainWindow);
            panelIndicator->setAutoFillBackground(true);
            panelIndicator->setBackgroundRole(QPalette::Highlight);
        }
        const PanelDrop drop = panelDropAt(globalPos, moving);
        if (!drop.column) {
            panelIndicator->hide();
            return;
        }
        panelIndicator->setGeometry(drop.indicator);
        panelIndicator->show();
        panelIndicator->raise();
    }

    void hidePanelDrop()
    {
        if (panelIndicator) panelIndicator->hide();
    }

    /// Whether \p global is on the strip of panel buttons, or near it
    bool overStrip(const QPoint &global) const
    {
        return rail && QRect(rail->mapToGlobal(QPoint(0, 0)), rail->size()).adjusted(0, -12, 0, 12).contains(global);
    }
};


#endif // KISSTUDIOLAYOUT_P_H
