/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBARBUTTONS_H
#define KISSTUDIOBARBUTTONS_H

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>

class KisMainWindow;
class KisStudioItemCard;
class QMenu;

/**
 * The top bar's switches and commands (symmetry, shapes, the brush editor, eraser mode…), arranged like the
 * tool rail: drag one to move it, drag it off to remove it, right-click it for its card, right-click
 * between them to bring one back or add any command Find knows.
 */
class KisStudioBarButtons : public QWidget
{
    Q_OBJECT
public:
    /// Where the buttons are: the top bar, or the pill with the zoom (its own buttons, none at first)
    enum Place { TopBar, ViewPill };

    explicit KisStudioBarButtons(KisMainWindow *mainWindow, QWidget *parent = nullptr, Place place = TopBar);
    ~KisStudioBarButtons() override;

    /// Shows the buttons that apply to the active tool (the brush's own only with the tools that use it)
    void updateForTool();
    QStringList items() const;
    void setItems(const QStringList &items);

    /// The bar of \p mainWindow
    static KisStudioBarButtons *of(KisMainWindow *mainWindow);
    /// Whether \p key is on the bar of \p mainWindow
    static bool has(KisMainWindow *mainWindow, const QString &key);
    /// Puts \p key at the end of the bar, or takes it off
    static void toggle(KisMainWindow *mainWindow, const QString &key);
    /// Drags \p key from \p source, to be dropped on a bar (or the Controls panel); returns what the drop did
    static Qt::DropAction dragItem(const QString &key, QWidget *source);

    /// The card to bring buttons back or add any command, at \p globalPos, with \p extra commands of the place's own
    void showRestoreCard(const QPoint &globalPos, const QList<std::function<void(KisStudioItemCard *)>> &extra = {});

Q_SIGNALS:
    void sigItemsChanged();

protected:
    void contextMenuEvent(QContextMenuEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    void rebuild();
    void startDrag(int index, QWidget *source, const QPixmap &pixmap);
    void showCard(int index);
    int dropIndex(int x) const;
    /// The menu with the symmetry modes and their settings
    QMenu *symmetryMenu();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOBARBUTTONS_H
