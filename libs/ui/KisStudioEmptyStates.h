/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOEMPTYSTATES_H
#define KISSTUDIOEMPTYSTATES_H

#include "kritaui_export.h"

/**
 * A list with nothing in it says why, quietly, in its middle: a search that finds
 * nothing, a tag with no brushes yet, an empty palette. Any item view can say what
 * it says with the property "studioEmptyText"; resource lists and palettes say it
 * by themselves.
 */
namespace KisStudioEmptyStates
{
KRITAUI_EXPORT void install();
}

#endif // KISSTUDIOEMPTYSTATES_H
