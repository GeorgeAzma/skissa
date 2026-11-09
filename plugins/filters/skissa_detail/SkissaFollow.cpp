/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "SkissaFollow.h"

#include <KisFilterGeometry.h>
#include <kis_properties_configuration.h>

#include <algorithm>
#include <cmath>

#include "SkissaDetail.h"
#include "SkissaNoise.h"

namespace SkissaFollow
{

namespace
{

using KisFilterGeometry::Change;

void scaled(KisPropertiesConfiguration &config, const Change &change, const char *key, bool whole = false)
{
    if (!config.hasProperty(key)) return;
    const qreal after = change.length(config.getDouble(key));
    if (whole) {
        config.setProperty(key, std::max(1, qRound(after)));
    } else {
        config.setProperty(key, after);
    }
}

void turned(KisPropertiesConfiguration &config, const Change &change, const char *key)
{
    if (config.hasProperty(key)) config.setProperty(key, change.direction(config.getDouble(key)));
}

/// A middle kept as a share of the image
void placed(KisPropertiesConfiguration &config, const Change &change)
{
    if (!config.hasProperty("x") && !config.hasProperty("y")) return;
    const QPointF at = change.share(QPointF(config.getDouble("x", 0.5), config.getDouble("y", 0.5)));
    config.setProperty("x", at.x());
    config.setProperty("y", at.y());
}

void curve(KisPropertiesConfiguration &config, const Change &change, const char *key, const KisStudioCurve &fallback)
{
    const KisStudioCurve before = KisStudioCurve::fromString(config.getString(key), fallback);
    const KisStudioCurve after = shifted(before, change.scale());
    if (after != before) config.setProperty(key, after.toString());
}

/// Blur, Sharpen, High pass and Smooth: their radius, Skissa's own name for it from before
void radius(KisPropertiesConfiguration &config, const Change &change)
{
    scaled(config, change, "radius");
}

} // namespace

KisStudioCurve shifted(const KisStudioCurve &curve, qreal scale)
{
    const qreal shift = std::log2(scale) / SkissaDetail::octaves;
    if (std::abs(shift) < 1e-9 || curve.points.isEmpty()) return curve;
    const QVector<float> values = curve.sampled(1025);
    auto at = [&](qreal x) {
        const qreal i = std::clamp(x, 0.0, 1.0) * (values.size() - 1);
        const int below = std::min(int(i), values.size() - 2);
        return values[below] + (values[below + 1] - values[below]) * (i - below);
    };
    // past the ends the curve goes on as it ends: features finer than a pixel were the finest, larger than the
    // largest the largest
    KisStudioCurve result;
    result.points << QPointF(0, at(-shift));
    for (const QPointF &point : curve.points) {
        const qreal x = point.x() + shift;
        if (x > 1e-3 && x < 1 - 1e-3) result.points << QPointF(x, point.y());
    }
    result.points << QPointF(1, at(1 - shift));
    return result;
}

void addAll()
{
    using KisFilterGeometry::add;
    add(QStringLiteral("skissa_blur"), radius);
    add(QStringLiteral("skissa_sharpen"), radius);
    add(QStringLiteral("skissa_high_pass"), radius);
    add(QStringLiteral("skissa_smooth"), radius);
    add(QStringLiteral("skissa_detail"), [](KisPropertiesConfiguration &config, const Change &change) {
        if (config.hasProperty("curve")) curve(config, change, "curve", SkissaDetail::flatCurve());
    });
    add(QStringLiteral("skissa_grain"), [](KisPropertiesConfiguration &config, const Change &change) {
        scaled(config, change, "size");
    });
    add(QStringLiteral("skissa_noise"), [](KisPropertiesConfiguration &config, const Change &change) {
        curve(config, change, "sizes", SkissaNoise::fineSizes());
        scaled(config, change, "displace");
    });
    add(QStringLiteral("skissa_lens"), placed);
    add(QStringLiteral("skissa_radial"), [](KisPropertiesConfiguration &config, const Change &change) {
        placed(config, change);
        if (change.mirrors() && config.hasProperty("spin")) config.setProperty("spin", -config.getDouble("spin"));
    });
    add(QStringLiteral("skissa_light"), [](KisPropertiesConfiguration &config, const Change &change) {
        for (const char *key : {"size", "length", "streak_length", "halation_size"}) scaled(config, change, key);
        for (const char *key : {"angle", "streak_angle"}) turned(config, change, key);
    });
    add(QStringLiteral("skissa_shape"), [](KisPropertiesConfiguration &config, const Change &change) {
        for (const char *key : {"grow", "outline", "glow", "shadow_softness", "bevel"}) scaled(config, change, key);
        // the shadow's fall is a length along its direction
        if (config.hasProperty("shadow") && config.hasProperty("shadow_angle")) {
            const qreal angle = config.getDouble("shadow_angle");
            config.setProperty("shadow", change.along(config.getDouble("shadow"), angle).x());
            config.setProperty("shadow_angle", change.direction(angle));
        }
    });
    add(QStringLiteral("skissa_ink"), [](KisPropertiesConfiguration &config, const Change &change) {
        for (const char *key : {"size", "spacing", "length", "dot"}) scaled(config, change, key);
        turned(config, change, "angle");
    });
    add(QStringLiteral("skissa_fewer_colors"), [](KisPropertiesConfiguration &config, const Change &change) {
        scaled(config, change, "pixel", true);
    });
    add(QStringLiteral("skissa_paint"), [](KisPropertiesConfiguration &config, const Change &change) {
        scaled(config, change, "size");
    });
    add(QStringLiteral("skissa_focus"), [](KisPropertiesConfiguration &config, const Change &change) {
        scaled(config, change, "size");
        placed(config, change);
        for (const char *key : {"angle", "band_angle"}) turned(config, change, key);
    });
}

} // namespace SkissaFollow
