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
 * Settings changes and a document's steps in one history: Undo takes back
 * whichever was done last, Redo does again what was taken back last. The
 * settings keep a history of their own (see KisSettingsRegistry); nothing
 * is added to the document's, which stays as it was (and so does whether
 * it is saved).
 */
namespace KisSettingsUndo
{
/// Before \p stack undoes: true if a settings change was undone instead
KRITAUI_EXPORT bool undoInstead(KUndo2Stack *stack);
/// Before \p stack redoes: true if a settings change was redone instead
KRITAUI_EXPORT bool redoInstead(KUndo2Stack *stack);
}

#endif // KISSETTINGSUNDO_H
