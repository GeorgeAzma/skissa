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
 * The canvas-first Studio layout of the main window: a tool rail and glass
 * pills over the canvas, pinned panels in a column over its edge, and unpinned
 * ones as flyouts. The panels are still Krita's dockers, presented differently;
 * call normalize() after anything restores a window state.
 */
class KRITAUI_EXPORT KisStudioLayout : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioLayout(KisMainWindow *mainWindow);
    ~KisStudioLayout() override;

    /// Whether the Studio layout is enabled in kritarc (on by default)
    static bool isEnabledInConfig();

    /// Where a pill dragged anywhere sits along one axis, as a share of the room it moves in: the edges and the middle catch it
    static qreal spotShare(int offset, int room);
    static int spotOffset(qreal share, int room);

    bool isActive() const;
    /// Switches between the studio layout and Krita's classic dockers
    void setActive(bool active);

    /// Brings a restored window state back to the studio arrangement; never shows a hidden panel
    void normalize();

    void closeFlyout();

    /// Shows a pinned panel in the column and any other as a flyout
    void showPanel(QDockWidget *dock);
    /// Opens Settings at \p cardId; with \p toggle, closes them instead if they are open
    void openSettings(const QString &cardId = QString(), bool toggle = false);
    /// New image sizes as tiles in a popup under the top of the window; Custom there opens Krita's dialog
    void showNewImagePopup();

    bool eventFilter(QObject *watched, QEvent *event) override;

    /// Handles the window's native events for the custom title bar
    bool handleNativeEvent(void *message, void *result);

private:
    void enter();
    void leave();
    /// Creates the settings panel the first time it is needed
    void ensureSettings();
    /// Handles clicks and the wheel on the brush bar's preset button
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
    /// Each lane's width in \p group, as saved (see KisStudioPanelColumn::laneWidths())
    QVector<int> readWidths(int group) const;
    void saveWidths(int group);
    QString widthKeyOf(int group) const;
    /// \p column dragged to \p placed (canvas area coordinates) and perhaps \p dropped there: whether it joins a
    /// group there, shown while dragged and done once dropped
    bool joinAt(KisStudioPanelColumn *column, const QRect &placed, bool dropped);
    /// Puts \p from's panels beside \p to (\p edge: which of its sides), or into \p lane at its top or bottom
    void joinGroups(KisStudioPanelColumn *from, KisStudioPanelColumn *to, Qt::Edge edge, int lane, const QRect &placed);
    /// The lanes of \p column from \p lane on, as a group of their own
    void separateLanes(KisStudioPanelColumn *column, int lane);
    /// Each lane's panels' ids, top to bottom
    QList<QStringList> lanesOf(const KisStudioPanelColumn *column) const;
    /// A group number no group has
    int freeGroup() const;
    /// The column with the panel buttons
    KisStudioPanelColumn *panelColumn();
    /// The column of a group of pinned panels: 0 the one with the panel buttons, 1 across the canvas from it,
    /// then floating groups
    KisStudioPanelColumn *groupColumn(int group);
    /// A new floating group, dropped at \p windowPos: its number, with its place written
    int newPanelGroup(const QPoint &windowPos);
    KisStudioPanelColumn *createColumn(int group, bool left);
    void setPanelsSide(bool left);
    void setToolRailSide(bool left);
    /// Tucked away by tapping their grip: the pinned panels (but not their strip), the tool rail's tools, the top bar
    void setPanelsTucked(bool tucked);
    void setToolRailTucked(bool tucked);
    void setTopBarTucked(bool tucked);
    /// Returns \p dock to the window's dock areas, hidden
    void parkPanel(QDockWidget *dock);
    /// Shows the column when it has panels and updates the canvas insets
    void updateColumns();
    /// Tells the canvas area what covers its edges and, if \p views, refits the views
    void updateCanvasInsets(bool views = true);
    /// Handles a panel dragged by its grip or rail button and dropped at \p globalPos
    void dropPanel(const QString &id, const QPoint &globalPos, bool fromRail);
    void showPanelCard(const QString &id, QWidget *anchor);
    /// Removes the panel from the rail and closes it
    void takeOffRail(const QString &id);
    /// Makes \p dock non-floating, even one that was never in a dock area
    void dockAway(QDockWidget *dock);
    /// Empties the flyout window and forgets the flyout, without parking it
    void releaseFlyout();
    /// Folds a pinned panel to its grip or unfolds it; closes a flyout
    void gripClicked(const QString &id);
    void applyFold(QDockWidget *dock, bool fold);

    void dockToggled(QDockWidget *dock, bool visible);

    /// The start page, canvas-only mode and "Show Dockers" off hide all panels
    bool arePanelsHidden() const;
    /// Only the start page and canvas-only mode hide the rail
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
    /// Updates the tool chip: the brush's name for brush tools, otherwise the tool's
    void updateToolChip();
    /// In canvas-only mode, brings back the bars, tool rail and panels while the pointer is at their edge
    void peek(const QPoint &global);
    /// Puts the document, menu, Settings, Find and window buttons on the window pill
    void placeWindowButtons();
    void showMainMenu(QWidget *button);
    /// Moves the tool and brush bars into the top pill and shows or hides the pills
    void adoptTopBars();
    /// Returns the brush bar to the window's top row
    void releaseTopBars();
    void syncRail();
    /// Keeps the start page clear of the tool rail over it
    /// The Studio's start page in place of Krita's welcome page, or Krita's back
    void showStartPage(bool show);
    void placeWelcomePage();
    /// Places the window handle in the top-left corner and the top pill after it
    void placeWindowHandle();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOLAYOUT_H
