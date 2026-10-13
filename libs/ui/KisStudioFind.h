/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOFIND_H
#define KISSTUDIOFIND_H

#include <QAction>
#include <QFrame>
#include <QIcon>
#include <QPointer>

#include <functional>
#include <memory>

#include "kritaui_export.h"

class KisMainWindow;
class QAbstractButton;
class QKeyEvent;
class QDockWidget;

/**
 * The "Find anything" popup: one search field over everything Krita can do
 * (tools, commands, effects, brushes, blend modes, panels, layers, workspaces
 * and settings). Results are ranked by how well they match and by how often and
 * how recently they were used; an empty query lists the recent ones.
 */
class KRITAUI_EXPORT KisStudioFind : public QFrame
{
    Q_OBJECT
public:
    explicit KisStudioFind(KisMainWindow *mainWindow);
    ~KisStudioFind() override;

    /// Sets how a panel is shown, e.g. as a flyout of the studio layout.
    void setPanelOpener(std::function<void(QDockWidget *)> opener);
    /// Sets how a settings card is opened (see KisStudioSettings).
    void setSettingsOpener(std::function<void(const QString &cardId)> opener);
    /// Sets how to check and change whether an item is pinned to the tools panel.
    void setTools(std::function<bool(const QString &key)> isOnTools, std::function<void(const QString &key, bool on)> setOnTools);

    void open(const QString &text = QString());
    /// Opens Find as a chooser: the chosen item's key goes to \p picked instead of being run.
    void pick(const QString &placeholder, std::function<void(const QString &key)> picked, const QString &keyPrefix = QString());

    /// What other widgets need to show and run an item Find knows about.
    struct Item {
        QString key;
        QString title;
        QIcon icon;
        QPointer<QAction> action; ///< holds the shortcut; may be null
        QStringList aliases;      ///< names other applications use for it
    };
    /// Looks up an item by key ("tool:…", "effect:…", "action:…", "brush:…", "panel:…").
    bool item(const QString &key, Item *item);
    bool run(const QString &key);

    /// Gives each window button without an action of its own a hidden one, so it can be found and get a shortcut.
    void registerButtons();

    /// Shows the item card for \p button beside it; false if Find doesn't know the button.
    bool showCard(QAbstractButton *button);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void rebuildIndex();
    void updateResults();
    /// Results for a query starting with ">": commands only, in a single list
    void updateCommands(const QString &query);
    void appendResult(int index, const QString &detail);
    void appendHeader(const QString &title);
    void appendMessage(const QString &text);
    void selectFirst();
    void moveSelection(int delta);
    void runSelected();
    void runRow(int row);
    void adjustGeometry();
    void stopRecording();
    void recordShortcut(QKeyEvent *event);
    void popupCard(const QIcon &icon, const QString &title, QAction *action, const QString &key, QWidget *anchor);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOFIND_H
