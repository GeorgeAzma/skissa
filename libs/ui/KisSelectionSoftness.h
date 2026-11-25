/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KIS_SELECTION_SOFTNESS_H
#define KIS_SELECTION_SOFTNESS_H

#include <functional>

#include <QColor>
#include <QImage>
#include <QTransform>

#include <kis_types.h>

class QObject;

namespace KisSelectionSoftness
{
/**
 * Where \p selection is only partly selected, as a veil of \p color that deepens toward where
 * the selection ends, so a soft edge shows how far it reaches. Made on the image's threads;
 * \p done gets it on \p receiver's thread with where it goes in the image (a null image when
 * nothing is partly selected).
 */
void start(KisImageSP image, KisSelectionSP selection, const QColor &color, QObject *receiver,
           const std::function<void(const QImage &band, const QTransform &imageTransform)> &done);
}

#endif
