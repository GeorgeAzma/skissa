/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOTOOLRAIL_H
#define KISSTUDIOTOOLRAIL_H

#include <QIcon>
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
    /// Its tools in a row, lying along the top or bottom of the canvas, or in a column
    void setFlat(bool flat);
    bool isFlat() const;

    /// Appends the item with Find key \p key, unless it is already on the rail
    void addItem(const QString &key);
    bool hasItem(const QString &key) const;
    void removeItem(const QString &key);
    /// Puts a slot's tool back within reach: its slot if that was taken off, the variant if it was hidden
    void bringBack(const QString &toolId);
    struct SlotEntry {
        QString id;
        QString title;
        QIcon icon;
    };
    /// The tool slots this build has, for Find
    QVector<SlotEntry> slotEntries() const;
    /// Picks the slot's tool, putting the slot back on the rail if it was taken off
    void pickSlot(const QString &slotId);
    /// Rebuilds the buttons, e.g. once the commands they represent exist
    void refresh();
    /// Whether an item had no command yet when the buttons were last built
    bool hasMissingItems() const;

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
    int gapAt(const QPoint &pos) const;
    /// A gap before the item at \p index
    void insertGap(int index);
    void clearGap();
    void showIndicator(int itemIndex, int thickness);
    void startDrag(int index, const QPixmap &pixmap);
    void setItems(const QStringList &items);
    int dropIndex(const QPoint &pos) const;
    /// \p touch: opened by a long press, so it offers what a pointer does by holding between items
    void showCard(int index, bool touch = false);
    void showRestoreCard(const QPoint &globalPos);
    void runItem(int index);
    /// The \p n th button from the top (or left), as a click on it unless it is in use
    void pickButton(int n);

    void activateSlot(const QString &slotId);
    void activateTool(const QString &slotId, const QString &toolId);
    void updateActiveSlot();
    void rememberPreset();
    void loadPresetForSlot(const QString &slotId);
    /// Into or out of erasing: the eraser and the brush slot each have their brush (the same one at first), and the
    /// eraser always erases, whatever its brush says (Krita keeps erasing in the brush itself)
    void setErasing(bool on);
    /// Paints with the brush pinned as \p key (Find's key for it)
    void activatePin(const QString &key);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOTOOLRAIL_H
