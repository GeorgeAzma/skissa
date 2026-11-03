/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISCANVASSIZECATCH_H
#define KISCANVASSIZECATCH_H

#include <QRect>

#include "kritaui_export.h"

/**
 * Sizes a canvas pulled by its edges catches on, lightly: the size it had, the other side's at a common ratio
 * (square, 4:3, 3:2, 16:9, an A sheet's, 2:1, either way up), and sizes images are often made at (HD, 4K, A sheets at
 * 300 dpi). As the new image stage does.
 */
namespace KisCanvasSizeCatch
{
/// \p rect with the \p moving edges moved to a size close by, within \p sizeReach image pixels (ratios a little less)
KRITAUI_EXPORT QRect caught(const QRect &rect, const QRect &original, Qt::Edges moving, qreal sizeReach);
}

#endif
