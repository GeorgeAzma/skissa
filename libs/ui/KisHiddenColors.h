/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISHIDDENCOLORS_H
#define KISHIDDENCOLORS_H

#include <QRect>

#include <kis_types.h>

class QImage;

/**
 * The colors clear pixels still hold. Erasing only lowers alpha, so what was erased stays in the pixels' colors, and
 * an image file that keeps them gives it back to anyone who makes them opaque again. Files for others are written
 * without them.
 */
namespace KisHiddenColors
{
/// \p device, or a copy of it where every clear pixel in \p rect holds no color
KisPaintDeviceSP cleared(KisPaintDeviceSP device, const QRect &rect);

/// \p image (ARGB32) with no color in its clear pixels
void clear(QImage &image);
}

#endif
