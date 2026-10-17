/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOMIXING_H
#define KISSTUDIOMIXING_H

class KisMainWindow;
class QWidget;

/**
 * How the current brush's paint mixes with paint (`KoMixing`): as Krita does, evenly or as pigment, three tiles
 * each showing blue mixed into yellow that way. Kept in the brush (`KoMixing::settingName()`).
 */
namespace KisStudioMixing
{
QWidget *chooser(KisMainWindow *mainWindow, QWidget *parent);
}

#endif // KISSTUDIOMIXING_H
