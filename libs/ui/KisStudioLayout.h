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
 * The canvas-first "studio" layout of the main window: a tool rail and glass
 * pills over the canvas, pinned panels in a column over its edge, unpinned
 * ones as flyouts. The panels are still Krita's dockers, only presented
 * differently; call normalize() after something restores a window state.
 */
class KRITAUI_EXPORT KisStudioLayout : public QObject
{
    Q_OBJECT
public:
    explicit KisStudioLayout(KisMainWindow *mainWindow);
    /// The settings panel, made the first time it is needed
    void ensureSettings();
    ~KisStudioLayout() override;

    /// Enabled in kritarc (the default)
    static bool isEnabledInConfig();

    bool isActive() const;
    /// Switches between the studio layout and Krita's classic dockers
    void setActive(bool active);

    /// Brings a restored window state back to the studio arrangement; never shows a hidden panel
    void normalize();

    void closeFlyout();

    /// Pinned panels in the column, others as a flyout
    void showPanel(QDockWidget *dock);
    /// Settings at \p cardId, closed instead if \p toggle and open; the dialog without the studio layout
    void openSettings(const QString &cardId = QString(), bool toggle = false);

    bool eventFilter(QObject *watched, QEvent *event) override;

    /// The window's native events, for the custom title bar
    bool handleNativeEvent(void *message, void *result);

private:
    void enter();
    void leave();
    /// Clicks and the wheel on the brush bar's preset button
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
    KisStudioPanelColumn *panelColumn();
    void setPanelsSide(bool left);
    void setToolRailSide(bool left);
    /// Hidden by a tap on their grip: the pinned panels (not their strip), the tool rail's tools, the top bar
    void setPanelsTucked(bool tucked);
    void setToolRailTucked(bool tucked);
    void setTopBarTucked(bool tucked);
    /// Back among the window's docks, hidden
    void parkPanel(QDockWidget *dock);
    /// The column shown when it has panels, and the canvas insets
    void updateColumns();
    /// Tells the canvas area what covers its edges, and (\p views) refits the views
    void updateCanvasInsets(bool views = true);
    /// A panel dragged by its grip or rail button, let go at \p globalPos
    void dropPanel(const QString &id, const QPoint &globalPos, bool fromRail);
    void showPanelCard(const QString &id, QWidget *anchor);
    /// Off the rail, and closed
    void takeOffRail(const QString &id);
    /// No longer floating, also a panel that never was in a dock area
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
    /// The tool chip: the brush's name for the brush tools, else the tool's
    void updateToolChip();
    /// Canvas only: the bars, the tool rail and the panels back while the pointer is at their edge
    void peek(const QPoint &global);
    /// The document, menu, Settings, Find and window buttons on the window pill
    void placeWindowButtons();
    void showMainMenu(QWidget *button);
    /// The tool and brush bars into the top pill; the pills shown or hidden
    void adoptTopBars();
    /// The brush bar back in the window's top row
    void releaseTopBars();
    void syncRail();
    /// The start page clear of the tool rail over it
    void placeWelcomePage();
    /// The window handle in the top left corner, the top pill after it
    void placeWindowHandle();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOLAYOUT_H
