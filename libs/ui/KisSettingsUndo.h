/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSETTINGSUNDO_H
#define KISSETTINGSUNDO_H

#include "kritaui_export.h"

class KUndo2Stack;

/**
 * Interleaves settings changes with a document's undo history, so Undo takes
 * back whichever happened last and Redo repeats whatever was undone last.
 * Settings keep their own history (see KisSettingsRegistry); the document's
 * undo stack, and its modified state, are left untouched.
 */
namespace KisSettingsUndo
{
/// Called before \p stack undoes; returns true if a settings change was undone instead.
KRITAUI_EXPORT bool undoInstead(KUndo2Stack *stack);
/// Called before \p stack redoes; returns true if a settings change was redone instead.
KRITAUI_EXPORT bool redoInstead(KUndo2Stack *stack);

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
