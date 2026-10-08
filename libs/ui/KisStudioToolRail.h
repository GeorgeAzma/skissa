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
 * The left rail: tool slots grouped by what you want to do, and anything
 * else you put there.
 *
 * It is a list of items, edited by hand:
 * - drag anything from Find (an effect, a command, a brush, a tool...) onto
 *   it, or use the "+" at its bottom, and it becomes a button there, with
 *   its shortcut on it;
 * - click between two buttons for a space, click a space to name the group
 *   below it;
 * - drag a button along the rail to move it, off the rail to remove it;
 * - long press or right click a button for its card: the shortcut as a key
 *   cap to click and press a new one, the slot's variants (drag one out onto
 *   the rail to make it its own button, right click one to hide it), and
 *   removing it.
 *
 * Every slot remembers the variant used last, e.g. the lasso for Select.
 * Tools that are better reached by a modifier or a gesture (zoom, pan,
 * color sampler) have no slot; Find has them.
 */
class KisStudioToolRail : public QWidget
{
    Q_OBJECT
public:
    explicit KisStudioToolRail(KisMainWindow *mainWindow, QWidget *parent = nullptr);
    ~KisStudioToolRail() override;

    /// Widgets below the tools, e.g. the color chip
    void addBottomWidget(QWidget *widget);

    /// Puts an item (a Find key) at the end, if it isn't there yet
    void addItem(const QString &key);
    /// Builds the buttons again, e.g. once the commands they stand for exist
    void refresh();

    /// "Rectangular Selection Tool" -> "Rectangular Selection"
    static QString toolName(KoToolFactoryBase *factory);
    /// Whether a canvas controller is the one of this window's active view; tool signals are global
    static bool isOwnController(KisMainWindow *mainWindow, KoCanvasController *controller);

protected:
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

private:
    void rebuild();
    void updatePlus();
    int gapAt(int y) const;
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
