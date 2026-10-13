/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTOOLRAIL_H
#define KISSTUDIOTOOLRAIL_H

#include <QWidget>

#include <memory>

class KisMainWindow;
class KoCanvasController;
class KoToolFactoryBase;

/**
 * The tool rail: tool slots grouped by task, each remembering the variant used
 * last, plus anything from Find dragged onto it or added with its "+". Click
 * between two buttons to add a gap and click a gap to name it. Drag a button
 * along the rail to move it, or off it to remove it. A long press or right
 * click shows the button's card (shortcut, variants).
 */
class KisStudioToolRail : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioToolRail(KisMainWindow *mainWindow, QWidget *parent = nullptr);
    ~KisStudioToolRail() override;

    /// Adds a widget below the tools, e.g. the color chip
    void addBottomWidget(QWidget *widget);

    /// Appends the item with Find key \p key, unless it is already on the rail
    void addItem(const QString &key);
    bool hasItem(const QString &key) const;
    void removeItem(const QString &key);
    /// Rebuilds the buttons, e.g. once the commands they represent exist
    void refresh();

    /// "Rectangular Selection Tool" -> "Rectangular Selection"
    static QString toolName(KoToolFactoryBase *factory);
    /// Whether \p controller belongs to this window's active view; tool signals are global
    static bool isOwnController(KisMainWindow *mainWindow, KoCanvasController *controller);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    void enterEvent(QEnterEvent *event) override;
#else
    void enterEvent(QEvent *event) override;
#endif
    void leaveEvent(QEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

private:
    void rebuild();
    void updatePlus();
    int gapAt(int y) const;
    void clearGap();
    void showIndicator(int itemIndex, int thickness);
    void startDrag(int index, QWidget *source, const QPixmap &pixmap);
    void setItems(const QStringList &items);
    int dropIndex(int y) const;
    void showCard(int index);
    void showRestoreCard(const QPoint &globalPos);
    void runItem(int index);

    void activateSlot(const QString &slotId);
    void activateTool(const QString &slotId, const QString &toolId);
    void updateActiveSlot();
    void rememberPreset();
    void loadPresetForSlot(const QString &slotId);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOTOOLRAIL_H
