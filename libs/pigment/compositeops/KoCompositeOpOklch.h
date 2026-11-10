/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KOCOMPOSITEOPOKLCH_H
#define KOCOMPOSITEOPOKLCH_H

#include <QColor>
#include <QList>
#include <QString>
#include <QVector>

#include <algorithm>
#include <atomic>
#include <cmath>

#include "KoColorProfile.h"
#include "KoCompositeOpInLinear.h"
#include "KoID.h"
#include <klocalizedstring.h>

/**
 * The HSV, HSL and HSY blends' four (hue, saturation, color, lightness) in OKLCH, where lightness looks the same
 * across hues: a yellow and a blue of one OKLCH lightness look equally light, which they don't in HSL. A result
 * outside sRGB keeps its lightness and hue and gives up chroma until it fits.
 */
namespace KoOklch
{
enum Mode { Hue = 0, Chroma = 1, Color = 2, Lightness = 3 };

inline QString opId(int mode)
{
    static const char *names[] = {"hue", "chroma", "color", "lightness"};
    return QStringLiteral("skissa_oklch_%1").arg(QLatin1String(names[qBound(0, mode, 3)]));
}

inline KoID category()
{
    return KoID(QStringLiteral("oklch"), i18nc("Blending mode - category OKLCH", "OKLCH"));
}

/// The registry's names, as Krita names its HSL and HSV blends
inline QList<KoID> modes()
{
    return {KoID(opId(Color), i18nc("Blending mode - Color OKLCH", "Color OKLCH")),
            KoID(opId(Hue), i18nc("Blending mode - Hue OKLCH", "Hue OKLCH")),
            KoID(opId(Chroma), i18nc("Blending mode - Chroma OKLCH", "Chroma OKLCH")),
            KoID(opId(Lightness), i18nc("Blending mode - Lightness OKLCH", "Lightness OKLCH"))};
}

/// Adds the category, after HSV, and its blends to the registry's lists
template<class Categories, class Map>
void addTo(Categories &categories, Map &map)
{
    const int hsv = categories.indexOf(KoID(QStringLiteral("hsv")));
    categories.insert(hsv < 0 ? categories.size() : hsv + 1, category());
    Q_FOREACH (const KoID &mode, modes()) map.insert(category(), mode);
}

/**
 * The OKLCH hues of the pure colors around an HSV wheel, a quarter degree apart (1441 of them, the last a turn on),
 * unwrapped: the color picker's ring and the Color effect's hue axis are spaced by it. Around blue they stall and even
 * fall back a little (HSV 229° to 240° are all near 264°): where they rise slower than 0.15° a degree, they rise evenly
 * instead, so each hue has one place and none moves more than 2°.
 */
inline const QVector<qreal> &wheelHues()
{
    static const QVector<qreal> hues = []() {
        constexpr int count = 1441;
        constexpr qreal least = 0.15 * 0.25;
        constexpr qreal degrees = 180.0 / 3.14159265358979323846;
        auto linear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        QVector<qreal> hues(count);
        qreal turns = 0;
        for (int i = 0; i < count; i++) {
            const QColor pure = QColor::fromHsvF(std::fmod(i * 0.25 / 360.0, 1.0), 1.0, 1.0);
            const qreal r = linear(pure.redF()), g = linear(pure.greenF()), b = linear(pure.blueF());
            const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
            const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
            const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
            qreal h = std::atan2(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s,
                                 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s) * degrees;
            if (h < 0) h += 360.0;
            h += turns;
            if (i > 0 && h < hues[i - 1] - 180.0) {
                turns += 360.0;
                h += 360.0;
            }
            hues[i] = h;
        }
        for (int i = 1; i < count;) {
            if (hues[i] - hues[i - 1] >= least - 1e-9) {
                i++;
                continue;
            }
            int a = i - 1;
            int b = i;
            while (hues[b] - hues[a] < least * (b - a) && (a > 0 || b < count - 1)) {
                a = std::max(0, a - 1);
                b = std::min(count - 1, b + 1);
            }
            const qreal from = hues[a];
            const qreal to = hues[b];
            for (int k = a; k <= b; k++) hues[k] = from + (to - from) * (k - a) / (b - a);
            i = a + 1;
        }
        return hues;
    }();
    return hues;
}

/**
 * Whether colors in \p cs stop at sRGB's edge, so results past it give up chroma to fit (inGamut): integer values
 * with sRGB's primaries. Floats hold what is past sRGB, and a wider gamut holds more of it, which the conversion
 * into it keeps. Not from a color space's constructor: it asks the registry for sRGB.
 */
inline bool fitsInSrgb(const KoColorSpace *cs)
{
    if (!cs) return false;
    const KoID depth = cs->colorDepthId();
    if (depth == Float16BitsColorDepthID || depth == Float32BitsColorDepthID || depth == Float64BitsColorDepthID) return false;
    const KoColorProfile *profile = cs->profile();
    const KoColorProfile *srgb = KoColorSpaceRegistry::instance()->rgb8()->profile();
    if (!profile || !srgb) return false;
    if (profile == srgb) return true;
    if (!profile->hasColorants() || !srgb->hasColorants()) return false;
    // the same primaries as Krita's own sRGB, read alike (adapted to the profile's white), within what profiles round
    const QVector<KoColorimetryUtils::xyY> own = profile->getColorantsxyY();
    const QVector<KoColorimetryUtils::xyY> standard = srgb->getColorantsxyY();
    if (own.size() < 3 || standard.size() < 3) return false;
    for (int i = 0; i < 3; i++) {
        if (std::abs(own[i].x - standard[i].x) > 0.002 || std::abs(own[i].y - standard[i].y) > 0.002) return false;
    }
    return true;
}

/// Whether linear sRGB \p rgb is a color sRGB has; past white (lightness above 1) only its darks count
inline bool fits(const float rgb[3], float lightness)
{
    constexpr float slack = 1e-5f;
    const float low = std::min({rgb[0], rgb[1], rgb[2]});
    const float high = std::max({rgb[0], rgb[1], rgb[2]});
    return low >= -slack && (high <= 1 + slack || lightness >= 1);
}

/// \p lab in linear sRGB, with as much of its chroma as fits
inline void inGamut(const float lab[3], float rgb[3])
{
    KoMixing::oklabToLinear(lab, rgb);
    if (fits(rgb, lab[0])) return;
    float low = 0, high = 1;
    float tried[3] = {lab[0], 0, 0};
    for (int i = 0; i < 16; i++) {
        const float k = (low + high) / 2;
        tried[1] = lab[1] * k;
        tried[2] = lab[2] * k;
        float made[3];
        KoMixing::oklabToLinear(tried, made);
        (fits(made, lab[0]) ? low : high) = k;
    }
    tried[1] = lab[1] * low;
    tried[2] = lab[2] * low;
    KoMixing::oklabToLinear(tried, rgb);
    // gray past black or white: what the blends on integers would clip to
    if (lab[0] < 1) {
        for (int c = 0; c < 3; c++) rgb[c] = std::clamp(rgb[c], 0.0f, 1.0f);
    } else {
        for (int c = 0; c < 3; c++) rgb[c] = std::max(rgb[c], 0.0f);
    }
}

/// \p under (OKLab) with what \p mode takes from \p source (OKLab)
inline void blend(int mode, const float source[3], const float under[3], float out[3])
{
    // below this a color is gray and has no hue to give or keep
    constexpr float gray = 1e-4f;
    const float sourceChroma = std::hypot(source[1], source[2]);
    const float underChroma = std::hypot(under[1], under[2]);
    out[0] = mode == Lightness ? source[0] : under[0];
    switch (mode) {
    case Hue: {
        const float k = sourceChroma > gray ? underChroma / sourceChroma : 0;
        out[1] = source[1] * k;
        out[2] = source[2] * k;
        break;
    }
    case Chroma: {
        const float k = underChroma > gray ? sourceChroma / underChroma : 0;
        out[1] = under[1] * k;
        out[2] = under[2] * k;
        break;
    }
    case Color:
        out[1] = source[1];
        out[2] = source[2];
        break;
    default:
        out[1] = under[1];
        out[2] = under[2];
        break;
    }
}
} // namespace KoOklch

/**
 * A blend of KoOklch: the color the mode makes is laid over the color under it as Krita's own blends lay theirs,
 * by how much of each there is.
 */
class KoCompositeOpOklch : public KoCompositeOpInLinear
{
public:
    KoCompositeOpOklch(const KoColorSpace *cs, int mode)
        : KoCompositeOpInLinear(cs, KoOklch::opId(mode), KoOklch::category().id())
        , m_mode(mode)
    {
    }

    static QVector<KoCompositeOp *> all(const KoColorSpace *cs)
    {
        return {new KoCompositeOpOklch(cs, KoOklch::Hue), new KoCompositeOpOklch(cs, KoOklch::Chroma),
                new KoCompositeOpOklch(cs, KoOklch::Color), new KoCompositeOpOklch(cs, KoOklch::Lightness)};
    }

    using KoCompositeOp::composite;

    void composite(const KoCompositeOp::ParameterInfo &params) const override
    {
        // found at the first use: the color space is still being made when its blends are
        if (m_fit < 0) m_fit = KoOklch::fitsInSrgb(colorSpace()) ? 1 : 0;
        const bool fit = m_fit > 0;
        // a color seen again (one paint color, a flat area under it) isn't blended again
        float sourceSeen[3] = {-1, -1, -1};
        float underSeen[3] = {-1, -1, -1};
        float sourceLab[3], underLab[3], blended[3];
        bool blendedFresh = false;
        auto blendOf = [&](const float *s, const float *d) {
            if (!std::equal(s, s + 3, sourceSeen)) {
                KoMixing::linearToOklab(s, sourceLab);
                std::copy(s, s + 3, sourceSeen);
                blendedFresh = false;
            }
            if (!std::equal(d, d + 3, underSeen)) {
                KoMixing::linearToOklab(d, underLab);
                std::copy(d, d + 3, underSeen);
                blendedFresh = false;
            }
            if (!blendedFresh) {
                float lab[3];
                KoOklch::blend(m_mode, sourceLab, underLab, lab);
                if (fit) {
                    KoOklch::inGamut(lab, blended);
                } else {
                    KoMixing::oklabToLinear(lab, blended);
                }
                blendedFresh = true;
            }
        };

        auto pixel = [&](const float *s, float *d, float amount, bool alphaLocked) {
            const float sa = std::clamp(amount * s[3], 0.0f, 1.0f);
            if (sa <= 0) return false;
            const float da = d[3];
            if (da <= 0) {
                if (alphaLocked) return false;
                std::copy(s, s + 3, d);
                d[3] = sa;
                return true;
            }
            blendOf(s, d);
            if (alphaLocked) {
                for (int c = 0; c < 3; c++) d[c] += (blended[c] - d[c]) * sa;
                return true;
            }
            // over what is there, as Krita's blends composite: each alone where the other isn't, blended where both are
            const float total = sa + da - sa * da;
            for (int c = 0; c < 3; c++) {
                d[c] = ((1 - sa) * da * d[c] + (1 - da) * sa * s[c] + sa * da * blended[c]) / total;
            }
            d[3] = total;
            return true;
        };
        compositeRows(params, pixel);
    }

private:
    int m_mode;
    /// Whether results are fitted into sRGB (KoOklch::fitsInSrgb); -1 until known
    mutable std::atomic<int> m_fit {-1};
};

#endif // KOCOMPOSITEOPOKLCH_H
