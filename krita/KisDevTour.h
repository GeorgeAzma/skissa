/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_DEV_TOUR_H
#define KIS_DEV_TOUR_H

/**
 * A development tool for reviewing the whole UI: with KRITA_DEV_TOUR set to a
 * folder (dev profile builds only), Krita walks through its own UI and saves a
 * picture of everything it can show: the window, menus, panels, tool options,
 * context menus, popups, every dialog with all of its pages, and tooltips, plus
 * a map of the menus, actions and settings. KRITA_DEV_TOUR_PHASES picks phases
 * (comma separated), e.g. "menus,dialogs".
 */
namespace KisDevTour
{
void start();
}

#endif
