/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOEMPTYSTATES_H
#define KISSTUDIOEMPTYSTATES_H

#include "kritaui_export.h"

/**
 * Shows a quiet centered message in empty item views, such as a search with no
 * matches or an empty palette. Any view can set its own message through the
 * "studioEmptyText" property; resource lists and palettes have built-in ones.
 */
namespace KisStudioEmptyStates
{
KRITAUI_EXPORT void install();
}

#endif // KISSTUDIOEMPTYSTATES_H
