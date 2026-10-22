/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UPDATE_PACER_H
#define KIS_UPDATE_PACER_H

#include "kritaui_export.h"

class QString;

/**
 * Windows only: canvas windows repaint once per screen refresh rather than per mouse report, and
 * are kept composed by the desktop so VRR screens don't flicker.
 */

namespace KisUpdatePacer
{
KRITAUI_EXPORT void install();

/// Development aid: appends \p what, timestamped, to the KRITA_DEV_FRAMELOG frame log if enabled.
KRITAUI_EXPORT void log(const QString &what);
/// Whether log() writes anywhere, to skip building lines nobody reads
KRITAUI_EXPORT bool logging();
}

#endif
