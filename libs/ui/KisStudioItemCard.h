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

/// Helpers for shortcuts shown on studio buttons and edited in place
namespace KisStudioShortcuts
{
/// Compact form for a badge in a button corner: "Ctrl+Shift+K" -> "Ctrl ⇧K"
QString badgeText(const QKeySequence &sequence);
/// The badge in a button's corner: a key alone, none for a key with modifiers
QString cornerBadgeText(const QKeySequence &sequence);

/// Paints a small key cap in the bottom right corner of \p rect
void paintBadge(QPainter &painter, const QRect &rect, const QString &text, const QPalette &palette);

/// Sets and saves \p sequence (empty clears it) as \p action's shortcut; returns the name of the action it was taken from, if any
QString assign(KisMainWindow *mainWindow, QAction *action, const QKeySequence &sequence);

/// Applies the shortcut saved for \p action, if there is one
void restore(QAction *action);

/// Makes shortcuts editable directly in every menu; installed once for the whole application
KRITAUI_EXPORT void enableMenuEditing();

/// Mime type for items dragged around the studio layout; the data is the item's key
QString itemMimeType();

/// All of the window's actions: those in its action collections plus those added directly
QList<QAction *> allActions(KisMainWindow *mainWindow);

/// The tools whose own shortcut \p action is, working only while one of them is in use (Krita's tool actions); empty: everywhere
QStringList toolsOf(const QAction *action);

/// Whether one key on both would clash: both work everywhere, or both in a tool they share. A tool's own key
/// covers a key that works everywhere only while that tool is in use, so those two don't.
bool shareContext(const QStringList &toolsOfA, const QStringList &toolsOfB);

/// "&Gaussian Blur..." -> "Gaussian Blur"
QString plainText(QString text);

bool isModifierKey(int key);

enum class KeyPress {
    Modifier, ///< only a modifier so far
    Cancel,   ///< Escape
    Clear,    ///< Backspace or Delete alone
    Shortcut,
};

/**
 * Interprets \p event while recording a shortcut and stores the result in \p sequence.
 * Shift is dropped from symbols, since Qt reports Shift+1 as "!" rather than "Shift+!".
 */
KeyPress recordKey(const QKeyEvent *event, QKeySequence *sequence);
}

/**
 * Popup shown on a long press or right click on a studio button: its title,
 * its shortcut as a clickable key cap that records a new one, its variants,
 * and commands such as pin or remove.
 */
class KRITAUI_EXPORT KisStudioItemCard : public QFrame
{
    Q_OBJECT
public:
    struct Choice {
        QString key;           ///< item key, so the choice can be dragged out as an item
        QIcon icon;
        QString title;
        QAction *action {nullptr}; ///< shown as a shortcut badge
        bool current {false};
        std::function<void()> pick;
        std::function<void()> hide; ///< run on right click, if set
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

    /// Shows the card beside \p anchor, on whichever side faces the middle of the screen
    void popupBeside(QWidget *anchor);

protected:
    bool event(QEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    void startRecording();
    /// Stops recording; if \p assign is set, \p sequence becomes the new shortcut
    void finishRecording(bool assign, const QKeySequence &sequence = QKeySequence());
    void updateKeyCap();

    struct Private;
    const std::unique_ptr<Private> d;
};

#endif // KISSTUDIOITEMCARD_H
