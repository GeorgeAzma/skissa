/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef SKISSAFOLLOW_H
#define SKISSAFOLLOW_H

#include <QtGlobal>

#include <widgets/KisStudioCurve.h>

/// How each of Skissa's effects keeps its look when its image is scaled, turned or mirrored (KisFilterGeometry)
namespace SkissaFollow
{

void addAll();

/// A curve over Detail's sizes for a picture \p scale times as large: the same features, now that many times as
/// many pixels across
KisStudioCurve shifted(const KisStudioCurve &curve, qreal scale);

} // namespace SkissaFollow

#endif
