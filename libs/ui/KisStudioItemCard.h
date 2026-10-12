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
class QKeyEvent;
class QPainter;

/// Shortcuts the studio layout shows on its buttons and lets you change in place
namespace KisStudioShortcuts
{
/// "Ctrl+Shift+K" -> "Ctrl ⇧K": short enough for a badge in a button corner
QString badgeText(const QKeySequence &sequence);

/// A small key cap in the bottom right corner of \p rect
void paintBadge(QPainter &painter, const QRect &rect, const QString &text, const QPalette &palette);

/// Gives \p action \p sequence (empty: none) and saves it; returns the name of the action that lost it, if any
QString assign(KisMainWindow *mainWindow, QAction *action, const QKeySequence &sequence);

/// Gives \p action the shortcut saved for its name, if one was
void restore(QAction *action);

/// Lets every menu's shortcuts be changed in place (once, for the whole application)
KRITAUI_EXPORT void enableMenuEditing();

/// The mime type of an item dragged around the studio layout; the data is the item's key
QString itemMimeType();

/// Every action of the window: its collections' and the ones added to it directly
QList<QAction *> allActions(KisMainWindow *mainWindow);

/// "&Gaussian Blur..." -> "Gaussian Blur"
QString plainText(QString text);

bool isModifierKey(int key);

enum class KeyPress {
    Modifier, ///< a modifier alone: not a shortcut yet
    Cancel,   ///< Escape
    Clear,    ///< Backspace or Delete alone
    Shortcut,
};

/**
 * What \p event means while a shortcut is recorded, the shortcut in \p sequence.
 * A symbol typed with Shift keeps no Shift: Qt fires "!" for Shift+1, never "Shift+!".
 */
KeyPress recordKey(const QKeyEvent *event, QKeySequence *sequence);
}

/**
 * What a long press or right click on a button shows: what it is, its
 * shortcut as a key cap that records a new one when clicked, its variants,
 * and what can be done with it (pin, remove...).
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
    /// Ends recording; with \p assign, \p sequence becomes the shortcut
    void finishRecording(bool assign, const QKeySequence &sequence = QKeySequence());
    void updateKeyCap();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOITEMCARD_H
