/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOHOLDKEYS_H
#define KISSTUDIOHOLDKEYS_H

class KisMainWindow;

/**
 * Every shortcut, tapped or held. Tapped, it does what it always did. Held
 * (longer than a tap), what it turned on lasts while it is held: a tool's key
 * gives the tool until it is let go, then the one before is back (a spring
 * loaded tool); a toggle's key (the value view, the mirror view...) turns it
 * on while held, and off again when let go. Nothing to set: any key given to
 * a tool or a toggle works both ways.
 */
namespace KisStudioHoldKeys
{
/// For the shortcuts of \p mainWindow's actions (once per window)
void install(KisMainWindow *mainWindow);

/// How long a key is down before it counts as held, not tapped (milliseconds)
constexpr int holdTime = 350;
}

#endif // KISSTUDIOHOLDKEYS_H
