/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOPANELPAGES_H
#define KISSTUDIOPANELPAGES_H

#include <functional>

class QDockWidget;

/**
 * Skissa's own pages over some of Krita's panels (Overview, Onion skins; History has its own, KisStudioHistoryPage): value rows,
 * color fields and buttons that drive the panel's own controls, which are kept out of sight, so its logic and settings
 * stay Krita's.
 */
namespace KisStudioPanelPages
{
/// Puts the page over \p dock if it has one; what is returned takes it away again (leaving the Studio)
std::function<void()> apply(QDockWidget *dock);
}

#endif // KISSTUDIOPANELPAGES_H
