/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_STUDIO_IMAGE_SIZE_H
#define KIS_STUDIO_IMAGE_SIZE_H

class KisViewManager;

/// Scaling the image in the Studio: its width and height as rows to drag, linked unless unchained, and the ways to
/// resample as pictures of the image's busiest part, in place of Krita's dialog
namespace KisStudioImageSize
{
void open(KisViewManager *viewManager);
}

#endif
