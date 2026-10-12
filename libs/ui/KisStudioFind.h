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
 * "Find anything": one search field for everything Krita can do (tools,
 * commands, effects, brushes, blend modes, panels, layers, workspaces,
 * settings), ranked by how well they match and how often and recently they
 * were used. With nothing typed it lists the recent ones.
 */
class KRITAUI_EXPORT KisStudioFind : public QFrame
{
    Q_OBJECT
public:
    explicit KisStudioFind(KisMainWindow *mainWindow);
    ~KisStudioFind() override;

    /// How a panel gets shown, e.g. as a studio layout flyout
    void setPanelOpener(std::function<void(QDockWidget *)> opener);
    /// How a setting's card gets shown (see KisStudioSettings)
    void setSettingsOpener(std::function<void(const QString &cardId)> opener);
    /// Whether a result is on the tools panel, and how it goes on or off it (its pin, its card)
    void setTools(std::function<bool(const QString &key)> isOnTools, std::function<void(const QString &key, bool on)> setOnTools);

    void open(const QString &text = QString());
    /// Find as a chooser: what is chosen goes to \p picked instead of running
    void pick(const QString &placeholder, std::function<void(const QString &key)> picked, const QString &keyPrefix = QString());

    /// One thing Find knows: what the rail and the cards need to show and run it
    struct Item {
        QString key;
        QString title;
        QIcon icon;
        QPointer<QAction> action; ///< has its shortcut, may be null
        QStringList aliases;      ///< what other applications call it
    };
    /// Looks an item up by its key ("tool:…", "effect:…", "action:…", "brush:…", "panel:…")
    bool item(const QString &key, Item *item);
    bool run(const QString &key);

    /// Gives every button of the window that is no action of its own a hidden one: for its shortcut and for Find
    void registerButtons();

    /// Shows \p button's card beside it; false if it is no command Find knows
    bool showCard(QAbstractButton *button);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void rebuildIndex();
    void updateResults();
    /// "> ...": only what can be done, one list
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
