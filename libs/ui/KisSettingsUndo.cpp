/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSettingsUndo.h"

#include <QApplication>
#include <QHash>
#include <QIcon>
#include <QMutex>
#include <QMutexLocker>
#include <QPointer>
#include <QVector>

#include <kundo2stack.h>
#include <klocalizedstring.h>

#include "KisMainWindow.h"
#include "KisPart.h"
#include "KisSettingsRegistry.h"
#include "KisViewManager.h"

namespace
{

/// What each undo took back, most recent last; Redo walks it backwards. A null stack is the settings.
struct Undone {
    bool settings {false};
    QPointer<KUndo2Stack> stack;
};
QVector<Undone> &undone()
{
    static QVector<Undone> list;
    return list;
}
/// More than either history keeps
constexpr int undoneKept = 1000;

void rememberUndone(const Undone &entry)
{
    undone() << entry;
    if (undone().size() > undoneKept) undone().removeFirst();
}

int documentOnly = 0;

/**
 * Where each document's steps stand in the one order of what was done: the mark of each command when it was last
 * done or redone. Steps are pushed from the image's threads too, so this is guarded.
 */
struct Steps {
    QHash<const KUndo2Command *, quint64> stamps;
    const KUndo2Command *top {nullptr};
    int index {0};
};
QMutex &stepsMutex()
{
    static QMutex mutex;
    return mutex;
}
QHash<const KUndo2Stack *, Steps> &steps()
{
    static QHash<const KUndo2Stack *, Steps> all;
    return all;
}
quint64 lastStamp = 0;

/// \p stack's index became \p index: a command newly on top that went up (pushed or redone) is the newest step
void stepped(KUndo2Stack *stack, int index)
{
    QMutexLocker locker(&stepsMutex());
    Steps &state = steps()[stack];
    const KUndo2Command *top = stack->command(index - 1);
    // a merge into the top keeps its place; an undo goes down to a command already marked
    if (top && (!state.stamps.contains(top) || (top != state.top && index >= state.index))) {
        state.stamps.insert(top, ++lastStamp);
    }
    state.top = top;
    state.index = index;
    if (state.stamps.size() > stack->count() + 16) {
        // commands deleted since (redo steps dropped, the oldest past the limit) go, so their addresses can't be taken
        // for new ones
        QHash<const KUndo2Command *, quint64> kept;
        for (int i = 0; i < stack->count(); i++) {
            const KUndo2Command *command = stack->command(i);
            const auto it = state.stamps.constFind(command);
            if (it != state.stamps.constEnd()) kept.insert(command, *it);
        }
        state.stamps = kept;
    }
}

/// The mark of \p stack's step that Undo would take back, or 0 for one older than any mark
quint64 topStamp(const KUndo2Stack *stack)
{
    QMutexLocker locker(&stepsMutex());
    const auto state = steps().constFind(stack);
    if (state == steps().constEnd()) return 0;
    return state->stamps.value(stack->command(stack->index() - 1), 0);
}

void tell(const QString &text)
{
    KisMainWindow *window = KisPart::instance()->currentMainwindow();
    if (window && window->viewManager()) window->viewManager()->showFloatingMessage(text, QIcon(), 1500);
}

bool undoSettingsNow(KisSettingsRegistry *settings)
{
    const QString name = settings->undoName();
    if (!settings->undoChange()) return false;
    rememberUndone({true, nullptr});
    tell(i18nc("@info:status a setting's change undone", "Undo: %1", name));
    return true;
}

bool redoSettingsNow(KisSettingsRegistry *settings)
{
    const QString name = settings->redoName();
    if (!settings->redoChange()) return false;
    tell(i18nc("@info:status a setting's change redone", "Redo: %1", name));
    return true;
}

} // namespace

KisSettingsUndo::DocumentOnly::DocumentOnly()
{
    documentOnly++;
}

KisSettingsUndo::DocumentOnly::~DocumentOnly()
{
    documentOnly--;
}

quint64 KisSettingsUndo::nextStamp()
{
    QMutexLocker locker(&stepsMutex());
    return ++lastStamp;
}

void KisSettingsUndo::watch(KUndo2Stack *stack)
{
    {
        QMutexLocker locker(&stepsMutex());
        steps()[stack].index = stack->index();
    }
    // direct: the image's threads push steps, and the mark must be the order they came in
    QObject::connect(stack, &KUndo2QStack::indexChanged, qApp, [stack](int index) { stepped(stack, index); }, Qt::DirectConnection);
    QObject::connect(stack, &QObject::destroyed, qApp, [stack]() {
        QMutexLocker locker(&stepsMutex());
        steps().remove(stack);
    }, Qt::DirectConnection);
}

bool KisSettingsUndo::undoInstead(KUndo2Stack *stack)
{
    if (documentOnly > 0) return false;
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    const quint64 settingsStamp = settings->undoStamp();
    const bool documentCan = stack->canUndo();
    // the settings change if it was done or redone after the document's step, or the document has nothing to undo
    if (settingsStamp > 0 && (!documentCan || settingsStamp > topStamp(stack)) && undoSettingsNow(settings)) return true;
    if (documentCan) rememberUndone({false, stack});
    return false;
}

bool KisSettingsUndo::redoInstead(KUndo2Stack *stack)
{
    if (documentOnly > 0) return false;
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    // the most recently undone that can still be redone, of the settings or this document; another image's undone
    // steps wait for it. A new change in either history clears that history's redo steps.
    for (int i = undone().size() - 1; i >= 0; i--) {
        const Undone entry = undone()[i];
        if (!entry.settings && entry.stack && entry.stack != stack) continue;
        undone().remove(i);
        if (entry.settings && settings->canRedoChange()) return redoSettingsNow(settings);
        if (!entry.settings && entry.stack && stack->canRedo()) return false;
    }
    if (!stack->canRedo() && settings->canRedoChange()) return redoSettingsNow(settings);
    return false;
}

bool KisSettingsUndo::undoSettings()
{
    return undoSettingsNow(KisSettingsRegistry::instance());
}

bool KisSettingsUndo::redoSettings()
{
    KisSettingsRegistry *settings = KisSettingsRegistry::instance();
    for (int i = undone().size() - 1; i >= 0; i--) {
        if (undone()[i].settings) {
            undone().remove(i);
            break;
        }
    }
    return settings->canRedoChange() && redoSettingsNow(settings);
}
