/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCANVASSIZE_H
#define KISSTUDIOCANVASSIZE_H

class KisViewManager;

/**
 * Canvas size on the image instead of in a dialog: the Crop tool's Canvas kind with handles on the canvas's own edges
 * at once (KisToolCrop::beginCanvasResize), with room around the image to pull them into. Enter keeps it, Esc drops
 * it, either goes back to the tool in use before.
 */
namespace KisStudioCanvasSize
{
void start(KisViewManager *view);
}

#endif
