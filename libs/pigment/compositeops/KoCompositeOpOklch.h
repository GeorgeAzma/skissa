/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KOCOMPOSITEOPOKLCH_H
#define KOCOMPOSITEOPOKLCH_H

#include <QList>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>

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
                KoOklch::inGamut(lab, blended);
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
};

#endif // KOCOMPOSITEOPOKLCH_H
