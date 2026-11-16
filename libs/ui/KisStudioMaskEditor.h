/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOMASKEDITOR_H
#define KISSTUDIOMASKEDITOR_H

#include <kis_types.h>

class KisViewManager;

/**
 * A filter mask's effect edited in the Studio: its own panel of settings (the effect's Studio panel, never Krita's
 * list of every filter), in a glass panel that stays beside the canvas without holding it, so the picture can be
 * panned, zoomed and painted meanwhile. The mask shows each change at once; ✓ keeps the changes as one step, ✕ or
 * Escape takes them back.
 */
namespace KisStudioMaskEditor
{
/// Opens the editor for \p mask; false if the mask's effect has no settings to show there
bool edit(KisViewManager *view, KisFilterMaskSP mask);
}

#endif // KISSTUDIOMASKEDITOR_H
