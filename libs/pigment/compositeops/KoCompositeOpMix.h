/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The pigment mixing below takes Mixbox's shape without its table (Šárka Sochorová and Ondřej Jamriška,
 *  "Practical Pigment Mixing for Digital Painting", 2021); its numbers are fitted to Mixbox's own mixtures
 *  (pymixbox, Secret Weapons, CC BY-NC 4.0).
 */
#ifndef KOCOMPOSITEOPMIX_H
#define KOCOMPOSITEOPMIX_H

#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <vector>

#include "KoCompositeOpInLinear.h"
#include "KoCompositeOpOklch.h"
#include "KoCompositeOpRegistry.h"
#include "KoID.h"
#include <klocalizedstring.h>

/**
 * How a brush's paint mixes with what is under it (and, smudging, with what it carries): as Krita does, in the
 * image's own values; evenly, in OKLab; or as pigment, as Mixbox mixes, so blue and yellow make green. A layer can
 * mix into the layers under it as pigment too, as its blend mode.
 * The image's colors are brought to linear sRGB through its own profile first, so an sRGB image and a linear one
 * mix alike. Only the blending of paint over paint changes: a brush's look (its edges, its build-up) stays.
 */
namespace KoMixing
{
enum Mode { Normal = 0, Even = 1, Pigment = 2 };

/// The brush setting holding the mode (kept by Krita when it writes a brush's settings again)
inline QString settingName() { return QStringLiteral("SkissaMixing"); }

/// \p t of \p b (in OKLab) into \p a (linear sRGB)
inline void mixEven(const float a[3], const float bLab[3], float t, float out[3])
{
    float lab[3];
    linearToOklab(a, lab);
    for (int c = 0; c < 3; c++) lab[c] += (bLab[c] - lab[c]) * t;
    oklabToLinear(lab, out);
}

/// The sRGB curve, both ways
inline float srgbOf(float linear)
{
    return linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f;
}

inline float linearOf(float srgb)
{
    return srgb <= 0.04045f ? srgb / 12.92f : std::pow((srgb + 0.055f) / 1.055f, 2.4f);
}

/// The sRGB curve over 0 to 1 as tables, read between their steps: several times faster than the curve, and off it
/// by at most 1e-5
struct Curves {
    static constexpr int toSrgbSteps = 8192;
    static constexpr int toLinearSteps = 4096;
    float toSrgb[toSrgbSteps + 1];
    float toLinear[toLinearSteps + 1];
};

inline const Curves &curves()
{
    static Curves tables;
    static const bool made = []() {
        for (int i = 0; i <= Curves::toSrgbSteps; i++) tables.toSrgb[i] = srgbOf(float(i) / Curves::toSrgbSteps);
        for (int i = 0; i <= Curves::toLinearSteps; i++) tables.toLinear[i] = linearOf(float(i) / Curves::toLinearSteps);
        return true;
    }();
    Q_UNUSED(made);
    return tables;
}

template<int steps>
inline float readTable(const float *table, float x)
{
    const float at = std::clamp(x, 0.0f, 1.0f) * steps;
    const int i = std::min(int(at), steps - 1);
    return table[i] + (table[i + 1] - table[i]) * (at - i);
}

/**
 * A color as pigment: how much of four paints (blue, red, yellow, white) make it and what they miss of it, in sRGB
 * values, and what lies past sRGB (light beyond white, colors outside it), which mixes as light does. Weighed sums
 * of these mix the colors as paint, and every color comes back exactly.
 */
struct Paint {
    float amounts[4] {0, 0, 0, 1};
    float rest[3] {0, 0, 0};
    float past[3] {0, 0, 0};
};

/// The color of a mixture of the four paints (amounts adding up to 1) in sRGB values, and how it changes with each
/// amount: a cubic Bézier tetrahedron of 20 control points, its corners the paints, by de Casteljau; the four points
/// of its second to last level are the slope over 3. Colors are four wide (the last is 0), so the loops vectorize.
inline void colorOfPaints(const float a[4], float color[4], float slope[4][4])
{
    alignas(16) static constexpr float points[20][4] = {
        {0.1408276f, 0.09427677f, 0.2771976f, 0}, // blue
        {0.06372603f, -0.1513644f, 0.3334551f, 0},
        {0.002746013f, 0.2942008f, 0.2296794f, 0},
        {-0.1343184f, 0.5085892f, 1.202733f, 0},
        {0.2660939f, 0.2252081f, 0.3348524f, 0},
        {0.3220368f, 0.2857496f, -0.02211571f, 0},
        {0.6443775f, -0.1663641f, 0.4947264f, 0},
        {-0.2010434f, 0.4852608f, 0.2216749f, 0},
        {0.4305908f, 1.179865f, 0.04487347f, 0},
        {0.358638f, 0.6626511f, 0.9352975f, 0},
        {0.578337f, 0.0539748f, -0.002240727f, 0}, // red
        {0.758479f, 0.2433439f, -0.1130014f, 0},
        {0.8562825f, 0.1732998f, 0.5905524f, 0},
        {0.9534689f, 0.2119738f, 0.4188433f, 0},
        {0.9614567f, 0.4073448f, 0.2677804f, 0},
        {0.9368071f, 0.399673f, 0.6062817f, 0},
        {0.9676888f, 0.7450792f, 0.01652574f, 0}, // yellow
        {1.013269f, 0.8221754f, 0.155246f, 0},
        {0.9914707f, 0.9108174f, 0.3591574f, 0},
        {1.003855f, 1.005778f, 1.012156f, 0}, // white
    };
    // which four points of the level above each point of the next two levels mixes
    static constexpr int ten[10][4] = {{0, 1, 2, 3}, {1, 4, 5, 6}, {2, 5, 7, 8}, {3, 6, 8, 9}, {4, 10, 11, 12},
                                       {5, 11, 13, 14}, {6, 12, 14, 15}, {7, 13, 16, 17}, {8, 14, 17, 18}, {9, 15, 18, 19}};
    static constexpr int four[4][4] = {{0, 1, 2, 3}, {1, 4, 5, 6}, {2, 5, 7, 8}, {3, 6, 8, 9}};
    alignas(16) float level[10][4];
    for (int p = 0; p < 10; p++) {
        for (int c = 0; c < 4; c++) {
            level[p][c] = a[0] * points[ten[p][0]][c] + a[1] * points[ten[p][1]][c] + a[2] * points[ten[p][2]][c]
                + a[3] * points[ten[p][3]][c];
        }
    }
    for (int c = 0; c < 4; c++) color[c] = 0;
    for (int k = 0; k < 4; k++) {
        for (int c = 0; c < 4; c++) {
            const float q = a[0] * level[four[k][0]][c] + a[1] * level[four[k][1]][c] + a[2] * level[four[k][2]][c]
                + a[3] * level[four[k][3]][c];
            slope[k][c] = 3 * q;
            color[c] += a[k] * q;
        }
    }
}

/// The four amounts from three logits, white's being 0
inline void amountsOfLogits(const float l[3], float a[4])
{
    const float top = std::max({l[0], l[1], l[2], 0.0f});
    float sum = 0;
    for (int k = 0; k < 4; k++) {
        a[k] = std::exp((k < 3 ? l[k] : 0.0f) - top);
        sum += a[k];
    }
    for (int k = 0; k < 4; k++) a[k] /= sum;
}

/// How much of each paint makes \p color (sRGB values, 0 to 1): a first guess in closed form, then a Gauss-Newton
/// step toward the mixture nearest the color, taken in the logits so the amounts stay positive and add up to 1
inline void amountsOf(const float color[3], float a[4])
{
    // the logits of blue, red and yellow by 1, r, g, b, rr, rg, rb, gg, gb, bb, rrr, rrg, rrb, rgg, rgb, rbb, ggg,
    // ggb, gbb, bbb
    static constexpr float guess[20][3] = {
        {6.055599f, 3.940631f, 2.804857f},  {-1.999801f, 2.858038f, -2.904711f},  {-3.312849f, -3.237203f, 6.399474f},
        {-9.280106f, -9.154124f, -6.504448f},  {-7.280895f, -2.009969f, 0.5767507f},  {3.808748f, -0.9679496f, 2.832994f},
        {0.3423549f, 2.678068f, -5.11866f},  {1.844443f, -4.038572f, -5.247148f},  {-0.8235854f, 0.9314673f, -0.4751448f},
        {6.189008f, 4.46046f, -4.509639f},  {-2.17123f, -2.431664f, 0.3123298f},  {9.047656f, 2.75325f, -0.4298195f},
        {2.393048f, -3.960866f, -2.275873f},  {-5.335266f, 3.424643f, 0.03406293f},  {-7.262702f, -0.5038084f, 3.210757f},
        {5.905912f, 5.981244f, 4.364719f},  {-2.056164f, -2.219392f, -3.003615f},  {3.69256f, 0.4985808f, 1.871292f},
        {1.478988f, 1.26659f, 6.594266f},  {-3.860267f, -3.870278f, -1.435659f},
    };
    const float r = color[0], g = color[1], b = color[2];
    const float terms[20] = {1, r, g, b, r * r, r * g, r * b, g * g, g * b, b * b, r * r * r, r * r * g, r * r * b,
                             r * g * g, r * g * b, r * b * b, g * g * g, g * g * b, g * b * b, b * b * b};
    float l[3] = {0, 0, 0};
    for (int i = 0; i < 20; i++) {
        for (int k = 0; k < 3; k++) l[k] += terms[i] * guess[i][k];
    }
    amountsOfLogits(l, a);
    alignas(16) float made[4], slope[4][4];
    colorOfPaints(a, made, slope);
    // how the color follows each logit (the slope times the amounts is three times the color), and the damped step
    // (JᵀJ + 0.001)⁻¹ Jᵀ (made − color), by Cramer's rule
    float J[3][3];
    for (int c = 0; c < 3; c++) {
        for (int k = 0; k < 3; k++) J[c][k] = a[k] * (slope[k][c] - 3 * made[c]);
    }
    float H[3][3], toward[3];
    for (int i = 0; i < 3; i++) {
        toward[i] = J[0][i] * (made[0] - color[0]) + J[1][i] * (made[1] - color[1]) + J[2][i] * (made[2] - color[2]);
        for (int j = 0; j < 3; j++) {
            H[i][j] = J[0][i] * J[0][j] + J[1][i] * J[1][j] + J[2][i] * J[2][j] + (i == j ? 1e-3f : 0.0f);
        }
    }
    const float adj[3][3] = {
        {H[1][1] * H[2][2] - H[1][2] * H[1][2], H[0][2] * H[1][2] - H[0][1] * H[2][2], H[0][1] * H[1][2] - H[0][2] * H[1][1]},
        {H[0][2] * H[1][2] - H[0][1] * H[2][2], H[0][0] * H[2][2] - H[0][2] * H[0][2], H[0][1] * H[0][2] - H[0][0] * H[1][2]},
        {H[0][1] * H[1][2] - H[0][2] * H[1][1], H[0][1] * H[0][2] - H[0][0] * H[1][2], H[0][0] * H[1][1] - H[0][1] * H[0][1]},
    };
    const float det = H[0][0] * adj[0][0] + H[0][1] * adj[0][1] + H[0][2] * adj[0][2];
    for (int k = 0; k < 3; k++) l[k] -= (adj[k][0] * toward[0] + adj[k][1] * toward[1] + adj[k][2] * toward[2]) / det;
    amountsOfLogits(l, a);
}

/// amountsOf over a grid of sRGB values, made the first time it is needed, as Mixbox keeps its table: read between
/// its steps it is several times faster, and mixes no differently to the eye
struct AmountsGrid {
    static constexpr int steps = 32;
    std::vector<float> amounts;
};

inline const AmountsGrid &amountsGrid()
{
    static const AmountsGrid grid = []() {
        constexpr int side = AmountsGrid::steps + 1;
        AmountsGrid made;
        made.amounts.resize(size_t(side) * side * side * 4);
        for (int r = 0; r < side; r++) {
            for (int g = 0; g < side; g++) {
                for (int b = 0; b < side; b++) {
                    const float color[3] = {float(r) / AmountsGrid::steps, float(g) / AmountsGrid::steps, float(b) / AmountsGrid::steps};
                    amountsOf(color, &made.amounts[((size_t(r) * side + g) * side + b) * 4]);
                }
            }
        }
        return made;
    }();
    return grid;
}

/// The amounts of \p color (sRGB values, 0 to 1) from the grid
inline void amountsNear(const float color[3], float a[4])
{
    constexpr int side = AmountsGrid::steps + 1;
    const float *grid = amountsGrid().amounts.data();
    int at[3];
    float f[3];
    for (int c = 0; c < 3; c++) {
        const float x = std::clamp(color[c], 0.0f, 1.0f) * AmountsGrid::steps;
        at[c] = std::min(int(x), AmountsGrid::steps - 1);
        f[c] = x - at[c];
    }
    for (int k = 0; k < 4; k++) a[k] = 0;
    for (int corner = 0; corner < 8; corner++) {
        const int dr = corner >> 2, dg = (corner >> 1) & 1, db = corner & 1;
        const float w = (dr ? f[0] : 1 - f[0]) * (dg ? f[1] : 1 - f[1]) * (db ? f[2] : 1 - f[2]);
        const float *p = grid + ((size_t(at[0] + dr) * side + at[1] + dg) * side + at[2] + db) * 4;
        for (int k = 0; k < 4; k++) a[k] += w * p[k];
    }
}

/// \p color (linear sRGB) as paint
inline void paintOf(const float color[3], Paint &paint)
{
    const Curves &tables = curves();
    float srgb[3];
    for (int c = 0; c < 3; c++) {
        srgb[c] = readTable<Curves::toSrgbSteps>(tables.toSrgb, color[c]);
        // what the curve there and back doesn't give back, with what lies past sRGB
        paint.past[c] = color[c] - readTable<Curves::toLinearSteps>(tables.toLinear, srgb[c]);
    }
    amountsNear(srgb, paint.amounts);
    alignas(16) float made[4], slope[4][4];
    colorOfPaints(paint.amounts, made, slope);
    for (int c = 0; c < 3; c++) paint.rest[c] = srgb[c] - std::clamp(made[c], 0.0f, 1.0f);
}

/// \p t of \p b into \p a (linear sRGB), as paint: the color of the mixed amounts plus what they miss, clipped to
/// sRGB channel by channel as Mixbox does (a single color never needs it), and what lies past sRGB, mixed as light
inline void mixPaint(const Paint &a, const Paint &b, float t, float out[3])
{
    float amounts[4];
    for (int k = 0; k < 4; k++) amounts[k] = a.amounts[k] + (b.amounts[k] - a.amounts[k]) * t;
    alignas(16) float made[4], slope[4][4];
    colorOfPaints(amounts, made, slope);
    const Curves &tables = curves();
    for (int c = 0; c < 3; c++) {
        const float rest = a.rest[c] + (b.rest[c] - a.rest[c]) * t;
        out[c] = readTable<Curves::toLinearSteps>(tables.toLinear, std::clamp(made[c], 0.0f, 1.0f) + rest) + a.past[c]
            + (b.past[c] - a.past[c]) * t;
    }
}

/// \p t of \p b into \p a (linear sRGB), as \p mode mixes
inline void mix(int mode, const float a[3], const float b[3], float t, float out[3])
{
    if (mode == Even) {
        float lb[3];
        linearToOklab(b, lb);
        mixEven(a, lb, t, out);
    } else if (mode == Pigment) {
        Paint pa, pb;
        paintOf(a, pa);
        paintOf(b, pb);
        mixPaint(pa, pb, t, out);
    } else {
        for (int c = 0; c < 3; c++) out[c] = a[c] + (b[c] - a[c]) * t;
    }
}

/// The blend of \p mode for Krita's \p baseId (Normal or Copy); empty for others or for Normal mixing
inline QString opId(int mode, const QString &baseId)
{
    if (mode != Even && mode != Pigment) return QString();
    const QString space = mode == Even ? QStringLiteral("oklab") : QStringLiteral("pigment");
    if (baseId == COMPOSITE_OVER) return QStringLiteral("skissa_mix_%1_over").arg(space);
    if (baseId == COMPOSITE_COPY) return QStringLiteral("skissa_mix_%1_copy").arg(space);
    return QString();
}

/// The mode a blend of these mixes in (Normal for any other)
inline int modeOf(const QString &id)
{
    if (id.startsWith(QLatin1String("skissa_mix_oklab_"))) return Even;
    if (id.startsWith(QLatin1String("skissa_mix_pigment_"))) return Pigment;
    return Normal;
}

/// Pigment as a layer's blend mode: the layer's paint mixes into what is under it by how much of it there is
inline KoID layerMode()
{
    return KoID(opId(Pigment, COMPOSITE_OVER), i18nc("Blending mode - Pigment", "Pigment"));
}

/// Whether blend mode \p id is only for layers: a brush mixes as pigment by its own setting instead
inline bool isLayerOnly(const QString &id)
{
    return id == opId(Pigment, COMPOSITE_OVER);
}

} // namespace KoMixing

/**
 * Paint over paint (Normal) or paint replacing paint by an amount (Copy, as smudging carries it), mixed in
 * OKLab or as pigment: the color of each pixel comes from mixing the two by how much of each there is, its
 * opacity as Krita's own blend has it.
 */
class KoCompositeOpMix : public KoCompositeOpInLinear
{
public:
    KoCompositeOpMix(const KoColorSpace *cs, int mode, bool copy)
        : KoCompositeOpInLinear(cs, KoMixing::opId(mode, copy ? COMPOSITE_COPY : COMPOSITE_OVER), KoCompositeOp::categoryMix())
        , m_mode(mode)
        , m_copy(copy)
    {
    }

    /// All of them for \p cs, with the OKLCH blends, for its constructor to add
    static QVector<KoCompositeOp *> all(const KoColorSpace *cs)
    {
        return QVector<KoCompositeOp *>{new KoCompositeOpMix(cs, KoMixing::Even, false), new KoCompositeOpMix(cs, KoMixing::Even, true),
                                        new KoCompositeOpMix(cs, KoMixing::Pigment, false), new KoCompositeOpMix(cs, KoMixing::Pigment, true)}
            + KoCompositeOpOklch::all(cs);
    }

    using KoCompositeOp::composite;

    void composite(const KoCompositeOp::ParameterInfo &params) const override
    {
        // a color seen again (one paint color, a flat area under it) isn't made into pigment or OKLab again
        KoMixing::Paint sourcePaint, underPaint;
        float sourceSeen[3] = {-1, -1, -1};
        float underSeen[3] = {-1, -1, -1};
        auto paintFor = [](const float *color, float seen[3], KoMixing::Paint &paint) {
            if (seen[0] != color[0] || seen[1] != color[1] || seen[2] != color[2]) {
                KoMixing::paintOf(color, paint);
                std::copy(color, color + 3, seen);
            }
        };
        float sourceLab[3];
        float sourceLabSeen[3] = {-1, -1, -1};

        auto pixel = [&](const float *s, float *d, float amount, bool alphaLocked) {
            // how much of each there is in the result: Normal is paint over paint, Copy replaces by the amount
            const float ofSource = amount * s[3];
            const float ofUnder = m_copy ? (1 - amount) * d[3] : d[3] * (1 - amount * s[3]);
            if (ofSource <= 0 && !(m_copy && amount > 0)) return false;
            const float total = ofSource + ofUnder;
            const float t = total > 0 ? ofSource / total : 1;
            float mixed[3];
            if (d[3] <= 0 || t >= 1) {
                std::copy(s, s + 3, mixed);
            } else if (t <= 0) {
                std::copy(d, d + 3, mixed);
            } else if (m_mode == KoMixing::Pigment) {
                paintFor(s, sourceSeen, sourcePaint);
                paintFor(d, underSeen, underPaint);
                KoMixing::mixPaint(underPaint, sourcePaint, t, mixed);
            } else {
                if (sourceLabSeen[0] != s[0] || sourceLabSeen[1] != s[1] || sourceLabSeen[2] != s[2]) {
                    KoMixing::linearToOklab(s, sourceLab);
                    std::copy(s, s + 3, sourceLabSeen);
                }
                KoMixing::mixEven(d, sourceLab, t, mixed);
            }
            std::copy(mixed, mixed + 3, d);
            if (!alphaLocked) d[3] = std::clamp(total, 0.0f, 1.0f);
            return true;
        };
        compositeRows(params, pixel);
    }

private:
    int m_mode;
    bool m_copy;
};

#endif // KOCOMPOSITEOPMIX_H
