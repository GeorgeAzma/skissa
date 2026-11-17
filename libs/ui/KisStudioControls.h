/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOCONTROLS_H
#define KISSTUDIOCONTROLS_H

#include <QStringList>



#include "kritaui_export.h"

class KisMainWindow;
class KisStudioValueRow;

/**
 * Settings pinned outside their own place: a brush's ("brushsetting:<option id>") or Krita's
 * ("setting:<registry id>"), each as a row on the top bar that changes it there.
 * A number drags, a switch and a choice are clicked, a right click resets.
 */
namespace KisStudioControls
{
/// The row for \p key, for the current brush (made again when it changes); null when there is none
KRITAUI_EXPORT KisStudioValueRow *create(KisMainWindow *mainWindow, const QString &key, QWidget *parent);
/// Whether \p key names something that can be pinned
KRITAUI_EXPORT bool isControl(const QString &key);
/// The key that pins Krita's setting \p id
KRITAUI_EXPORT QString settingKey(const QString &id);
/// Whether setting \p id can be pinned (a switch, a number or a choice that applies at once)
KRITAUI_EXPORT bool canPinSetting(const QString &id);
}

#endif // KISSTUDIOCONTROLS_H
