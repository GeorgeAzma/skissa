/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_DEV_TOUR_H
#define KIS_DEV_TOUR_H

/// Development tool (KRITA_DEV_TOUR=<folder>, dev profile only): walks the whole UI saving pictures of it and
/// maps of its menus, actions and settings. KRITA_DEV_TOUR_PHASES picks phases, e.g. "menus,dialogs".
namespace KisDevTour
{
void start();
}

#endif
