/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_STUDIO_SELECTION_HANDLES_H
#define KIS_STUDIO_SELECTION_HANDLES_H

class QObject;
class KisViewManager;
class KisStudioSelectionAdjust;

/// Two grips on the selection's edge while a selection tool is in use: the one on its right grows it (drag out) or
/// shrinks it (drag in), the one at its bottom softens its edge (drag down). Gone with \p owner.
namespace KisStudioSelectionHandles
{
void attach(QObject *owner, KisViewManager *viewManager, KisStudioSelectionAdjust *adjust);
}

#endif
