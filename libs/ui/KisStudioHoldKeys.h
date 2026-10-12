/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOHOLDKEYS_H
#define KISSTUDIOHOLDKEYS_H

class KisMainWindow;

/**
 * Every shortcut, tapped or held. Tapped, it does what it always did. Held,
 * what it turned on lasts while it is held: a tool used while its key is
 * down is put back when the key is let go (a spring loaded tool), a toggle
 * (the mirror view...) is turned back off.
 */
namespace KisStudioHoldKeys
{
/// For the shortcuts of \p mainWindow's actions (once per window)
void install(KisMainWindow *mainWindow);

/// How long a key is down before it counts as held, not tapped (milliseconds)
constexpr int holdTime = 350;
}

#endif // KISSTUDIOHOLDKEYS_H
