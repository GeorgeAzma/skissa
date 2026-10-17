/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOMAKEBRUSH_H
#define KISSTUDIOMAKEBRUSH_H

class KisViewManager;

/**
 * A brush made of what is selected: the visible pixels there become a picture tip (an even paper color around
 * them made see-through, cropped to the paint), gray ones a shape painted in the brush's color, colored ones keeping
 * their colors. A new pixel brush with that tip is saved (in the brush panel's tag, when it is one of the user's),
 * picked, and the quick editor opens on its tip.
 */
namespace KisStudioMakeBrush
{
void fromSelection(KisViewManager *view);
}

#endif // KISSTUDIOMAKEBRUSH_H
