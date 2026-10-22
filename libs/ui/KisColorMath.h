/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISCOLORMATH_H
#define KISCOLORMATH_H

#include <QColor>
#include <QtGlobal>

#include <cmath>

/// The sRGB curve and Oklab (Björn Ottosson's), for the colors the interface works out itself
namespace KisColorMath
{

struct Oklab {
    qreal l {0};
    qreal a {0};
    qreal b {0};
};

/// An sRGB channel (0 to 1) in linear light
inline qreal toLinear(qreal c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// A linear channel on the sRGB curve, kept within 0 to 1
inline qreal toSrgb(qreal c)
{
    c = qBound<qreal>(0, c, 1);
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
}

inline Oklab oklabFromLinear(qreal r, qreal g, qreal b)
{
    const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return {0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
            0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s};
}

inline Oklab oklabOf(const QColor &color)
{
    return oklabFromLinear(toLinear(color.redF()), toLinear(color.greenF()), toLinear(color.blueF()));
}

/// Linear sRGB, out of gamut as it is, so callers can tell
inline void linearFromOklab(const Oklab &c, qreal &r, qreal &g, qreal &b)
{
    const qreal l = std::pow(c.l + 0.3963377774 * c.a + 0.2158037573 * c.b, 3);
    const qreal m = std::pow(c.l - 0.1055613458 * c.a - 0.0638541728 * c.b, 3);
    const qreal s = std::pow(c.l - 0.0894841775 * c.a - 1.2914855480 * c.b, 3);
    r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
}

/// The sRGB color, each channel clipped to the gamut
inline QColor colorOf(const Oklab &c)
{
    qreal r, g, b;
    linearFromOklab(c, r, g, b);
    return QColor::fromRgbF(toSrgb(r), toSrgb(g), toSrgb(b));
}

} // namespace KisColorMath

#endif // KISCOLORMATH_H
