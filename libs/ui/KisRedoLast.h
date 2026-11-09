/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISREDOLAST_H
#define KISREDOLAST_H

#include <QObject>
#include <QPointer>

#include <functional>

#include <kundo2magicstring.h>

#include "kritaui_export.h"

class KisCanvas2;
class KUndo2Command;
class KUndo2Stack;

/**
 * The last thing a click did (a fill, a magic wand's selection), done again when a setting of it changes: its step
 * is taken back and the click runs again, so the result follows the value as it is dragged. Only while that step
 * is the last one done; one at a time, the last value waiting for the one before. Its step is found by its name as
 * it arrives in the history, a moment after the click.
 */
class KRITAUI_EXPORT KisRedoLast : public QObject
{
    Q_OBJECT
public:
    explicit KisRedoLast(QObject *parent = nullptr);

    /// A click is making a step named \p step in \p canvas's history; \p again does it once more with the settings then
    void made(KisCanvas2 *canvas, const KUndo2MagicString &step, std::function<void()> again);
    void made(KUndo2Stack *stack, const KUndo2MagicString &step, std::function<void()> again);
    /// A setting changed: the last click's step is done again, if it is still the newest
    void settingChanged();
    /// Takes the click's step back while it is the newest: false if it isn't (or hasn't arrived)
    bool takeBack();
    /// Forgets the click (the tool is put away)
    void forget();

private:
    void indexChanged();
    void runAgain();
    /// Whether the click's step is the last one done
    bool isNewest() const;

    QPointer<KUndo2Stack> m_stack;
    QString m_text;
    std::function<void()> m_again;
    /// The click's step, once it arrived; compared only, never used
    const KUndo2Command *m_step {nullptr};
    /// Waiting for the step of the click just made
    bool m_waiting {false};
    /// A setting changed while waiting
    bool m_pending {false};
};

#endif
