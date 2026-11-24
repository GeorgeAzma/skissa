/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_STUDIO_COMMAND_PREVIEW_H
#define KIS_STUDIO_COMMAND_PREVIEW_H

/// A menu command that changes the image shows what it would do on the canvas while it is hovered: Crop to the
/// selection dims what would go, Fill paints the selection in its color
namespace KisStudioCommandPreview
{
/// Watches every menu from now on; once is enough
void install();
}

#endif
