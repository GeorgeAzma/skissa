/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSELECTIONGESTURES_H
#define KISSELECTIONGESTURES_H

#include <QPointF>
#include <QVector>

#include <kis_types.h>

#include "kritaui_export.h"

/**
 * What a freehand selection's line means: held still at its end, a rough box or ellipse becomes a clean one;
 * with Follow edges on, each new point moves to the strongest edge within a band across the line.
 */
namespace KisSelectionGestures
{
/// How long the pointer rests at the end before a rough shape snaps clean
constexpr int holdMs = 350;
/// The band, in screen pixels, where the line looks for an edge
constexpr qreal edgeBand = 12.0;

KRITAUI_EXPORT bool followsEdges();
KRITAUI_EXPORT void setFollowsEdges(bool on);

/// The clean box or ellipse \p points roughly drew, or nothing when they are neither
KRITAUI_EXPORT QVector<QPointF> cleanShape(const QVector<QPointF> &points);

/// \p to moved across the line from \p from to the strongest edge of \p picture within \p band image pixels,
/// or \p to itself where there is no edge
KRITAUI_EXPORT QPointF onEdge(KisPaintDeviceSP picture, const QPointF &from, const QPointF &to, qreal band);
}

#endif
