/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_STUDIO_ITEM_DROP_H
#define KIS_STUDIO_ITEM_DROP_H

#include <QString>

#include "kis_types.h"

class QMimeData;
class QPoint;
class KisView;

/// Effects and brushes dragged out of their pickers (or Find) and dropped where they should act: an effect on the
/// layer it lands on, a brush anywhere on the canvas to paint with it
namespace KisStudioItemDrop
{
/// Whether \p data carries an effect or a brush
bool carries(const QMimeData *data);
/// The effect \p data carries, or none
QString effectOf(const QMimeData *data);
/// Whether a dropped effect can work on \p node
bool takesEffect(KisNodeSP node);
/// Runs what \p data carries; an effect on the layer that shows most at \p canvasPos (canvas widget coordinates)
void dropOnCanvas(KisView *view, const QMimeData *data, const QPoint &canvasPos);
/// Opens the effect \p id on \p node, inside the selection if there is one
void applyEffect(KisNodeSP node, const QString &id);
}

#endif
