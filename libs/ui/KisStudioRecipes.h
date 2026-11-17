/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIORECIPES_H
#define KISSTUDIORECIPES_H

#include <QObject>
#include <QStringList>
#include <QVector>

class KisMainWindow;
class KUndo2Command;

/**
 * Recipes: steps of the history kept to be done again. Each history step a command made is remembered with that
 * command (the command run just before the step came, within a few seconds); a recipe is those commands in order,
 * run one after the other, each once the image is done with the last. Strokes and other steps no command made can't be
 * replayed and are left out. Every recipe is a command of the window too, so Find finds it, it takes a shortcut and
 * pins to the tools, with a picture of its initials.
 */
class KisStudioRecipes : public QObject
{
    Q_OBJECT
public:
    struct Recipe {
        QString id;          ///< the command's name, "skissa_recipe_…"
        QString name;
        QStringList actions; ///< the commands it runs, in order
    };

    static KisStudioRecipes *instance();

    /// Watches \p window's commands, and gives it the recipes as commands
    static void watch(KisMainWindow *window);

    /// The command that made \p step, or empty when none did (a stroke, a drag on the canvas)
    static QString commandOf(const KUndo2Command *step);
    /// What \p command is called, as menus say it
    static QString commandName(const QString &command);

    static QVector<Recipe> recipes();
    /// A new recipe of \p commands, named after them; its id
    static QString add(const QStringList &commands);
    static void rename(const QString &id, const QString &name);
    static void remove(const QString &id);
    static void run(const QString &id);

Q_SIGNALS:
    /// Recipes were added, renamed or removed (or that was undone)
    void changed();

private:
    KisStudioRecipes();
    struct Private;
    Private *d;
};

#endif // KISSTUDIORECIPES_H
