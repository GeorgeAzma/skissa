/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSENTENCECASE_H
#define KISSENTENCECASE_H

#include "kritawidgetutils_export.h"

#include <QString>

/**
 * Krita writes its commands and panels in Title Case ("Open Recent"); the app's own text is in
 * sentence case ("Open recent"). In English, Krita's text is shown the app's way; translations
 * are left as they are.
 */
namespace KisSentenceCase
{
/// \p text in sentence case when the interface is in English: names and acronyms keep their capitals
KRITAWIDGETUTILS_EXPORT QString of(const QString &text);
}

#endif // KISSENTENCECASE_H
