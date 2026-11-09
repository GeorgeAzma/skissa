/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISNEWERAUTOSAVE_H
#define KISNEWERAUTOSAVE_H

#include <QString>

#include "kritaui_export.h"

/**
 * An image's autosave that holds work newer than the image: what a crash or a closed program left unsaved. Turned
 * down autosaves go to the Recycle Bin ("Don't save"), so one still newer than its image is the work, and it opens
 * in place of the image without asking.
 */
namespace KisNewerAutosave
{

/// The autosave beside \p path (hidden or not, as either setting names it) newer than it; empty if there is none
KRITAUI_EXPORT QString of(const QString &path);

} // namespace KisNewerAutosave

#endif
