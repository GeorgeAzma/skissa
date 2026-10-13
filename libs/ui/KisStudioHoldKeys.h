/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOHOLDKEYS_H
#define KISSTUDIOHOLDKEYS_H

class KisMainWindow;

/**
 * Lets every shortcut be tapped or held. A tap works as usual. Holding the key
 * makes the effect temporary: a tool used while its key is down is switched
 * back on release (a spring-loaded tool), and a toggle such as the mirror view
 * is turned back off.
 */
namespace KisStudioHoldKeys
{
/// Handles the shortcuts of \p mainWindow's actions; call once per window.
void install(KisMainWindow *mainWindow);

/// How long a key must be down to count as held rather than tapped, in milliseconds
constexpr int holdTime = 350;
}

#endif // KISSTUDIOHOLDKEYS_H
