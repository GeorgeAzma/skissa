/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSAFESAVE_H
#define KISSAFESAVE_H

#include <QFileDevice>

#include "kritaui_export.h"

class QTemporaryFile;

/// Saving without ever leaving a half-written file where the old one was (Windows)
namespace KisSafeSave
{
/**
 * Puts \p written, a finished file, in \p location's place: copied beside it, made sure to be on the disk,
 * then swapped in, keeping the old file's permissions, alternate streams and cloud version history. When
 * the swap can't be done, the old file is set aside until the new one is complete. NoError on success.
 */
KRITAUI_EXPORT QFileDevice::FileError replace(QTemporaryFile &written, const QString &location);

/// Where a file that writes itself (TIFF, EXR) is written before it takes \p location's place: beside it, its extension kept
KRITAUI_EXPORT QString besides(const QString &location);

/**
 * Puts \p written, a finished file beside \p location (see besides()), in its place as replace() does. On failure the
 * old file is untouched and \p written stays where it is. NoError on success.
 */
KRITAUI_EXPORT QFileDevice::FileError replaceWith(const QString &written, const QString &location);
}

#endif // KISSAFESAVE_H
