/*
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KIS_UPDATE_PACER_H
#define KIS_UPDATE_PACER_H

#include "kritaui_export.h"

class QString;

/**
 * Windows only: canvas windows repaint on a frame clock (one frame per screen refresh, not per mouse
 * report), and are always composed by the desktop so VRR screens do not flicker.
 */

namespace KisUpdatePacer
{
KRITAUI_EXPORT void install();

/// For development: \p what, timestamped, in the frame log (KRITA_DEV_FRAMELOG) if there is one
KRITAUI_EXPORT void log(const QString &what);
}

#endif
