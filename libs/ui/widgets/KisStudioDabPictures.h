/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIODABPICTURES_H
#define KISSTUDIODABPICTURES_H

#include <QColor>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <QVector>

#include "kritaui_export.h"

class QPainter;

/**
 * Little drawings of what a brush setting does: a row of dabs going from without it to with it,
 * so a setting can be told by what it looks like.
 */
namespace KisStudioDabPictures
{
/// A color that shows changes of hue and lightness: \p color, or \p fallback when it is too grey or dark for that
KRITAUI_EXPORT QColor paintColor(const QColor &color, const QColor &fallback);

/// What the brush setting \p id (KisPaintOpOption::id()) does, in \p ink or, for color, \p paint; false when there is no drawing for it
KRITAUI_EXPORT bool effect(QPainter &painter, const QRectF &rect, const QString &id, const QColor &ink, const QColor &paint);

/// \p count dabs laid along \p rect as a short stroke, overlapping
KRITAUI_EXPORT QVector<QPointF> strokeDabs(const QRectF &rect, int count, qreal radius);
}

#endif // KISSTUDIODABPICTURES_H
