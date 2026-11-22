/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef KISLANCZOS_H
#define KISLANCZOS_H

#include <QImage>

#include "kritaglobal_export.h"

/// Pictures made smaller through Lanczos 3, as Smooth resamples images: sharper than Qt's smooth scale,
/// without its blur. Enlarging goes through Qt's smooth scale.
namespace KisLanczos
{
KRITAGLOBAL_EXPORT QImage scaled(const QImage &image, const QSize &size, Qt::AspectRatioMode mode = Qt::IgnoreAspectRatio);
KRITAGLOBAL_EXPORT QImage scaled(const QImage &image, int width, int height, Qt::AspectRatioMode mode = Qt::IgnoreAspectRatio);
KRITAGLOBAL_EXPORT QImage scaledToWidth(const QImage &image, int width);
}

#endif
