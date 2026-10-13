/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISSTROKESHAPE_H
#define KISSTROKESHAPE_H

#include <QPointF>
#include <QVector>

#include "kritaui_export.h"

/**
 * A clean shape for a hand-drawn stroke: a line, a few straight segments, a polygon, an arc or an
 * ellipse. Only strokes that are clearly one of them are recognized; anything else (a scribble, a
 * wave, a spiral) is left as it is drawn.
 */
struct KRITAUI_EXPORT KisStrokeShape
{
    enum Kind { None, Line, Polyline, Polygon, Arc, Ellipse };

    Kind kind {None};
    /// Points along the shape, a couple of pixels apart, from where the stroke began and the way it went
    QVector<QPointF> outline;
    /// Where along the stroke (0 to 1) each outline point lies, to carry the stroke's pressure over
    QVector<qreal> along;

    /// \p points in image pixels; \p pixel is how many image pixels a screen pixel covers
    static KisStrokeShape recognize(const QVector<QPointF> &points, qreal pixel);
};

#endif
