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
 * "Find anything": one search field for everything Krita can do.
 *
 * It searches tools, commands, effects, brushes, blend modes, panels, the
 * layers of the current image, workspaces and settings pages, ranks the
 * results by how well they match and by how often and how recently they
 * were used, and runs the chosen one. With an empty query it shows the
 * recently used results, so it doubles as "do that again".
 */
class KRITAUI_EXPORT KisStudioFind : public QFrame
{
    Q_OBJECT
public:
    explicit KisStudioFind(KisMainWindow *mainWindow);
    ~KisStudioFind() override;

    /// How a panel gets shown, e.g. as a studio layout flyout
    void setPanelOpener(std::function<void(QDockWidget *)> opener);
    /// Whether a result is on the tools panel, and how it goes on or off it (its pin, its card)
    void setTools(std::function<bool(const QString &key)> isOnTools, std::function<void(const QString &key, bool on)> setOnTools);

    void open(const QString &text = QString());
    /**
     * Find as a chooser: what is chosen goes to \p picked instead of running.
     * With nothing typed it lists every tool and effect, to choose by tapping.
     */
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

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

public:
    /// Every button of the window that is no action of its own gets one: for its shortcut and for Find
    void registerButtons();

    /**
     * \p button's card, beside it: its shortcut, and a place on the tools
     * panel. False if it is no command Find knows (a button of a list, say).
     */
    bool showCard(QAbstractButton *button);

private:
    void rebuildIndex();
    void updateResults();
    /// "> ...": only what can be done, one list
    void updateCommands(const QString &query);
    void appendResult(int index, const QString &detail);
    void appendHeader(const QString &title);
    static bool isCommandKind(int kind);
    void moveSelection(int delta);
    void runSelected();
    void runRow(int row);
    void adjustGeometry();
    void stopRecording();
    /// The keys pressed while a result's key cap records: its new shortcut
    void recordShortcut(QKeyEvent *event);

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOFIND_H
