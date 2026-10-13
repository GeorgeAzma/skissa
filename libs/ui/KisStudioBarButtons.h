/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOBARBUTTONS_H
#define KISSTUDIOBARBUTTONS_H

#include <QPointer>
#include <QWidget>

#include <memory>

class KisMainWindow;
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
    explicit KisStudioBarButtons(KisMainWindow *mainWindow, QWidget *parent = nullptr);
    ~KisStudioBarButtons() override;

    /// Shows the buttons that apply to the active tool (symmetry and shapes only paint with brushes)
    void updateForTool();
    QStringList items() const;
    void setItems(const QStringList &items);

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
    void showRestoreCard(const QPoint &globalPos);
    int dropIndex(int x) const;
    /// The menu with the symmetry modes and their settings
    QMenu *symmetryMenu();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOBARBUTTONS_H
