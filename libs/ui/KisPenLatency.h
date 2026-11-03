/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISPENLATENCY_H
#define KISPENLATENCY_H

#include <QPointF>
#include <QRect>
#include <QtGlobal>

/**
 * For development (KRITA_DEV_LATENCY=<file> with KRITA_DEV_PROFILE): how long a pen event takes to become pixels on
 * screen. Each event of a brush stroke is followed by its stroke time (KisPaintInformation::currentTime): arrived at
 * the brush, its dabs handed to the brush engine, merged into the image, the canvas given the change, and the frame
 * showing it handed to the screen. At the end of a stroke a line of the median, the 95th percentile and the worst of
 * each, in ms, is added to the file. Off, every call returns at once.
 */
namespace KisPenLatency
{
bool enabled();
void strokeStarted();
/// An event of the stroke at \p at (image pixels) reached the brush; \p eventTime is the system's stamp of it, ms
void arrived(qreal strokeTime, const QPointF &at, ulong eventTime);
/// Dabs up to \p strokeTime went to the brush engine (any thread)
void painted(qreal strokeTime);
/// The image's projection has the changes in \p imageRect (image pixels; any thread)
void merged(const QRect &imageRect);
/// The canvas took the image's changes in \p imageRect (image pixels)
void uploaded(const QRect &imageRect);
/// A frame of the canvas went to the screen
void presented();
void strokeEnded();
}

#endif
