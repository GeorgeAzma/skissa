/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISEFFECTPANELS_H
#define KISEFFECTPANELS_H

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>

#include <functional>

#include <kis_types.h>

#include "kritaui_export.h"

class KisConfigWidget;
class QPainter;
class QRectF;
class QWidget;

/**
 * Skissa's panels for Krita's own effects: what each changes, as pictures of the layer and a few value rows, writing
 * Krita's settings as Krita's own panel does, so files and masks stay Krita's.
 */
namespace KisEffectPanels
{
/// The panel for \p filterId, or null where Krita's own is shown
KRITAUI_EXPORT KisConfigWidget *create(const QString &filterId, QWidget *parent, KisPaintDeviceSP device);

/// A copy of \p device that keeps reading its full-size pixels while the image shows a coarse preview (whose
/// level of detail a plain copy would follow, reading pixels it doesn't have); its tiles are shared until either changes
KRITAUI_EXPORT KisPaintDeviceSP fullSizeCopy(KisPaintDeviceSP device);

/**
 * Whether \p picture shows too little to see an effect on: nearly all of it clear or one color, as thin line art on
 * a clear layer or on paper. Effects' pictures then show another picture instead.
 */
KRITAUI_EXPORT bool showsLittle(const QImage &picture);

/// The middle of the most textured part of \p bounds, where changes to fine detail show
KRITAUI_EXPORT QPoint busiestPoint(KisPaintDeviceSP device, const QRect &bounds);

/// A tile's picture: \p picture filling it, its middle shown, or all of it with \p whole; \p crisp keeps its pixels
KRITAUI_EXPORT std::function<void(QPainter &, const QRectF &)> tilePicture(const QImage &picture, bool whole = false, bool crisp = false);
}

#endif
