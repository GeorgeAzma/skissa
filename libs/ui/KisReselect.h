/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISRESELECT_H
#define KISRESELECT_H

#include <QObject>
#include <QPointer>

#include <functional>

#include <kundo2magicstring.h>

#include "kritaui_export.h"

class KisCanvas2;
class KUndo2Command;
class KUndo2Stack;

/**
 * A selection made by a click, made again when a setting of it changes (a magic wand's threshold): its step is
 * taken back and the click runs again, so the selection follows the value as it is dragged. Only while that step
 * is the last one done; one at a time, the last value waiting for the one before.
 */
class KRITAUI_EXPORT KisReselect : public QObject
{
    Q_OBJECT
public:
    explicit KisReselect(QObject *parent = nullptr);

    /// A click made a selection, its step named \p step; \p again makes it once more with the settings then
    void made(KisCanvas2 *canvas, const KUndo2MagicString &step, std::function<void()> again);
    /// A setting changed: the last click's selection is made again, if its step is still the newest
    void settingChanged();
    /// Forgets the click (the tool is put away)
    void forget();

private:
    void indexChanged();
    void runAgain();

    QPointer<KUndo2Stack> m_stack;
    QString m_text;
    std::function<void()> m_again;
    /// The click's step, once it arrived; compared only, never used
    const KUndo2Command *m_step {nullptr};
    /// Waiting for the step of the selection just asked for
    bool m_waiting {false};
    /// A setting changed while waiting
    bool m_pending {false};
};

#endif
