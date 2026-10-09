/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOITEMCARD_H
#define KISSTUDIOITEMCARD_H

#include <QFrame>
#include <QIcon>
#include <QKeySequence>
#include <QPointer>

#include <functional>
#include <memory>

#include "kritaui_export.h"

class KisMainWindow;
class QAction;
class QPainter;

/// Shortcuts the studio layout shows on its buttons and lets you change in place
namespace KisStudioShortcuts
{
/// "Ctrl+Shift+K" -> "Ctrl ⇧K": short enough for a badge in a button corner
QString badgeText(const QKeySequence &sequence);

/// A small key cap in the bottom right corner of \p rect
void paintBadge(QPainter &painter, const QRect &rect, const QString &text, const QPalette &palette);

/**
 * Gives \p action the \p sequence (empty: no shortcut), saved like the
 * shortcuts dialog saves them. Another action that had it loses it; its
 * name is returned so the user can be told.
 */
QString assign(KisMainWindow *mainWindow, QAction *action, const QKeySequence &sequence);

/// The mime type of an item dragged around the studio layout; the data is the item's key
QString itemMimeType();
}

/**
 * What a long press or right click on a button shows: what the button is,
 * its shortcut as a key cap that records a new one when clicked, its
 * variants if it has some, and what can be done with it (pin, remove...).
 * One card for every kind of button, so there is one thing to learn.
 */
class KRITAUI_EXPORT KisStudioItemCard : public QFrame
{
    Q_OBJECT
public:
    struct Choice {
        QString key;           ///< the item key, so that a choice can be dragged out as an item
        QIcon icon;
        QString title;
        QAction *action {nullptr}; ///< for the shortcut badge
        bool current {false};
        std::function<void()> pick;
        std::function<void()> hide; ///< offered on right click, if set
    };

    struct Command {
        QIcon icon;
        QString toolTip;
        std::function<void()> run;
        bool checkable {false};
        bool checked {false};
    };

    KisStudioItemCard(KisMainWindow *mainWindow, const QIcon &icon, const QString &title, QAction *shortcutAction);
    ~KisStudioItemCard() override;

    void setChoices(const QVector<Choice> &choices);
    void addCommand(const Command &command);

    /// Shows the card next to \p anchor: to its right for the left rail, to its left for the right one
    void popupBeside(QWidget *anchor);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void startRecording();
    void finishRecording(const QKeySequence &sequence, bool clear);
    void updateKeyCap();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOITEMCARD_H
