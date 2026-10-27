/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KOCOMPOSITEOPGLOW_H
#define KOCOMPOSITEOPGLOW_H

#include <QString>

#include <algorithm>

#include <klocalizedstring.h>

#include "KoCompositeOpRegistry.h"
#include <KoID.h>

/**
 * The glow blends as painters of light know them from Clip Studio: the paint is scaled by how covering it is and
 * then used as light, so the see-through parts of a stroke (a soft airbrush's edge) count fully instead of being
 * faded back toward what is under them. Add (glow) is Krita's "Luminosity/Shine (SAI)", under the name artists look
 * for; Glow dodge is the color dodge of the same kind. Upstream Krita reads Glow dodge as Normal.
 */
namespace KoGlow
{
inline QString dodgeId()
{
    return QStringLiteral("skissa_glow_dodge");
}

/// What \p dst under paint \p src of coverage \p sa becomes, where \p da is how much is under it
template<typename T>
inline void cfGlowDodge(T src, T sa, T &dst, T &da)
{
    const T light = src * sa;
    const T glow = dst <= T(0) ? T(0) : light >= T(1) ? T(1) : std::min(T(1), dst / (T(1) - light));
    // where nothing is under it, the paint as it is
    dst = da * glow + (T(1) - da) * src;
}

/// Renames the SAI blend and adds Glow dodge beside it, in the registry's lists
template<class Categories, class Map>
void addTo(Categories &categories, Map &map)
{
    const KoID light = categories.value(categories.indexOf(KoID(QStringLiteral("light"))));
    if (light.id().isEmpty()) return;
    map.remove(light, KoID(COMPOSITE_LUMINOSITY_SAI));
    map.insert(light, KoID(COMPOSITE_LUMINOSITY_SAI, i18nc("Blending mode: Clip Studio's Add (Glow)", "Add (glow)")));
    map.insert(light, KoID(dodgeId(), i18nc("Blending mode: Clip Studio's Glow Dodge", "Glow dodge")));
}
} // namespace KoGlow

#endif // KOCOMPOSITEOPGLOW_H
