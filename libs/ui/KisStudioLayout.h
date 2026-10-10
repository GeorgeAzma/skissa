/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOLAYOUT_H
#define KISSTUDIOLAYOUT_H

#include <QIcon>
#include <QObject>

#include <memory>

#include "kritaui_export.h"

class KisMainWindow;
class KisStudioPanelColumn;
class QDockWidget;
class QMdiArea;

/**
 * The canvas-first "studio" layout of the main window.
 *
 * It doesn't replace Krita's dockers, it only changes how they are
 * presented, so every docker, workspace and canvas observer keeps working:
 *
 *  - the toolbox sits on the left edge without a title bar
 *  - a rail of panel buttons sits on the right edge
 *  - an unpinned panel opens as a flyout floating over the edge of the
 *    canvas and closes when you click anywhere else in the window
 *  - pinned panels stack in a column of glass over the canvas's edge, on
 *    the right or the left: the canvas goes on under them
 *
 * The docks are Krita's dockers all the same. After something restores a
 * window state, call normalize() so that it is brought back to the studio
 * arrangement.
 */
class KRITAUI_EXPORT KisStudioLayout : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioLayout(KisMainWindow *mainWindow);
    ~KisStudioLayout() override;

    /// Whether the studio layout is enabled in kritarc (the default)
    static bool isEnabledInConfig();

    bool isActive() const;

    /// Switches between the studio layout and Krita's classic dockers
    void setActive(bool active);

    /**
     * Brings the current window state back to the studio arrangement after
     * it has been restored from a workspace, canvas-only mode etc. Never
     * shows a hidden panel, so it is safe while the dockers are hidden.
     */
    void normalize();

    void closeFlyout();

    /// Shows a panel the studio way: pinned panels docked, others as a flyout
    void showPanel(QDockWidget *dock);

    bool eventFilter(QObject *watched, QEvent *event) override;

    /// The window's native events, for the title bar drawn by the top bar
    bool handleNativeEvent(void *message, void *result);

private:
    void enter();
    void leave();
    /// Clicks and the wheel on the toolbar's brush picture
    bool handlePresetChip(QEvent *event);

    void togglePanel(QDockWidget *dock, bool show);
    void showFlyout(QDockWidget *dock, QWidget *anchor = nullptr);
    void toggleToolOptions();
    QMdiArea *mdiArea() const;
    bool isCaptionAt(const QPoint &pos) const;
    void adoptDocuments();
    void releaseDocuments();
    void updateDocumentButton();
    void positionFlyout();
    void setPinned(QDockWidget *dock, bool pinned);
    void savePinned();
    void arrangePinned();
    void restoreHeights();
    void saveHeights();
    /// The column of pinned panels on a side, over the canvas
    KisStudioPanelColumn *columnOf(bool left);
    /// Back among the window's docks, hidden: where it keeps the panels it does not show
    void parkPanel(QDockWidget *dock);
    /// The columns shown if they have panels to show, and what is in the canvas area kept clear of them
    void updateColumns();
    void updateCanvasInsets();
    void resizePanel(QDockWidget *dock, int height);
    /// A panel dragged by its grip, or by its rail button, let go at \p globalPos
    void dropPanel(const QString &id, const QPoint &globalPos, bool fromRail);
    void showPanelCard(const QString &id, QWidget *anchor);
    /// Off the rail, and closed
    void takeOffRail(const QString &id);
    /// No longer floating, also a panel that never was in a column
    void dockAway(QDockWidget *dock);
    /// The flyout window lets go of its panel, and goes
    void releaseFlyout();
    /// A click on a panel's grip: folds it (or unfolds it) next to the canvas, tucks it away over it
    void gripClicked(const QString &id);
    void applyFold(QDockWidget *dock, bool fold);
    void setPanelSide(const QString &id, bool left);

    void dockToggled(QDockWidget *dock, bool visible);

    /// The welcome page, canvas-only mode and "Show Dockers" off hide all panels
    bool arePanelsHidden() const;
    /// The rail stays visible when only the dockers are hidden
    bool isRailHidden() const;
    bool isCanvasOnly() const;

    void createRail();
    void rebuildRailButtons();
    void setRailPanels(const QStringList &panels);
    QIcon railIcon(QDockWidget *dock) const;
    void showRailContextMenu(const QPoint &pos);
    bool handleRailDrag(QObject *watched, QEvent *event);
    void createBrushRail();
    void createTopBar();
    /// Shows only the toolbar entries that matter to the active tool
    void updateContextBar();
    void placeFindField();
    void moveTopBarToFront();
    void syncRail();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOLAYOUT_H
