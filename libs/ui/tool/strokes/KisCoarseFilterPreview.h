/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISCOARSEFILTERPREVIEW_H
#define KISCOARSEFILTERPREVIEW_H

#include <QRect>

#include <kis_default_bounds_base.h>
#include <kis_types.h>
#include <filter/kis_filter_configuration.h>

/// The coarse first pass of an effect's preview on a large image: worked out smaller, then scaled up
namespace KisCoarseFilterPreview
{
/// \p base's bounds a level of detail smaller: filters scale their sizes by it (KisLodTransformScalar)
KisDefaultBoundsBaseSP lodBounds(KisDefaultBoundsBaseSP base, int lod);
/// The level of detail of a coarse pass (about half a megapixel), or 0 when it isn't worth it
int level(KisFilterSP filter, const KisFilterConfigurationSP &config, const QRect &rect);
/// \p patch of \p target from \p small, \p lod levels smaller, scaled up bilinearly
void upscale(KisPaintDeviceSP small, KisPaintDeviceSP target, const QRect &patch, int lod);
}

#endif // KISCOARSEFILTERPREVIEW_H
