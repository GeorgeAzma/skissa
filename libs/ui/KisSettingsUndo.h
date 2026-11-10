/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSETTINGSUNDO_H
#define KISSETTINGSUNDO_H

#include <QtGlobal>

#include "kritaui_export.h"

class KUndo2Stack;

/**
 * Interleaves settings changes with a document's undo history, so Undo takes
 * back whichever was done or redone last and Redo repeats whatever was undone last.
 * Settings keep their own history (see KisSettingsRegistry); the document's
 * undo stack, and its modified state, are left untouched.
 */
namespace KisSettingsUndo
{
/// Called before \p stack undoes; returns true if a settings change was undone instead.
KRITAUI_EXPORT bool undoInstead(KUndo2Stack *stack);
/// Called before \p stack redoes; returns true if a settings change was redone instead.
KRITAUI_EXPORT bool redoInstead(KUndo2Stack *stack);

/// Undo and Redo with no image open: settings changes only. False if there was none.
KRITAUI_EXPORT bool undoSettings();
KRITAUI_EXPORT bool redoSettings();

/// Follows the steps \p stack does and redoes, so Undo can tell which came last, its or a setting's
KRITAUI_EXPORT void watch(KUndo2Stack *stack);
/// The next mark in the one order every step is done or redone in, settings' and each document's alike
KRITAUI_EXPORT quint64 nextStamp();

/// While one exists, undo and redo take back the document's own steps only: a dialog's Cancel taking back the
/// layer it made must not take back a setting instead
class KRITAUI_EXPORT DocumentOnly
{
public:
    DocumentOnly();
    ~DocumentOnly();
};
}

#endif // KISSETTINGSUNDO_H
