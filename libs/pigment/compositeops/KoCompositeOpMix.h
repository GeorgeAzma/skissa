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

#include <QMutex>
#include <QMutexLocker>
#include <QString>
#include <QVector>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

#include "KoChannelInfo.h"
#include "KoColorModelStandardIds.h"
#include "KoColorSpace.h"
#include "KoColorSpaceRegistry.h"
#include "KoCompositeOp.h"
#include "KoCompositeOpRegistry.h"

/**
 * How a brush's paint mixes with what is under it (and, smudging, with what it carries): as Krita does, in the
 * image's own values; evenly, in OKLab; or as pigment, as Mixbox mixes, so blue and yellow make green.
 * The image's colors are brought to linear sRGB through its own profile first, so an sRGB image and a linear one
 * mix alike. Only the blending of paint over paint changes: a brush's look (its edges, its build-up) stays.
 */
namespace KoMixing
{
enum Mode { Normal = 0, Even = 1, Pigment = 2 };

/// The brush setting holding the mode (kept by Krita when it writes a brush's settings again)
inline QString settingName() { return QStringLiteral("SkissaMixing"); }

inline void linearToOklab(const float rgb[3], float lab[3])
{
    const float l = std::cbrt(0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2]);
    const float m = std::cbrt(0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2]);
    const float s = std::cbrt(0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2]);
    lab[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
    lab[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
    lab[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

inline void oklabToLinear(const float lab[3], float rgb[3])
{
    const float l1 = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
    const float m1 = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
    const float s1 = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
    const float l = l1 * l1 * l1, m = m1 * m1 * m1, s = s1 * s1 * s1;
    rgb[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
    rgb[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
    rgb[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

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
        {0.1406857f, 0.09147363f, 0.2775293f, 0}, // blue
        {0.06543413f, -0.1580722f, 0.3274543f, 0},
        {0.01112019f, 0.2907245f, 0.2329601f, 0},
        {-0.1444913f, 0.5028814f, 1.20677f, 0},
        {0.2672834f, 0.2147136f, 0.3364322f, 0},
        {0.2985015f, 0.2963663f, -0.03261238f, 0},
        {0.6341857f, -0.1682934f, 0.4952036f, 0},
        {-0.2124452f, 0.4912994f, 0.2359678f, 0},
        {0.4397291f, 1.187265f, 0.05227801f, 0},
        {0.3563644f, 0.6582994f, 0.9308499f, 0},
        {0.566494f, 0.05742752f, -0.003538958f, 0}, // red
        {0.748164f, 0.2255562f, -0.09128919f, 0},
        {0.860177f, 0.1703138f, 0.5918832f, 0},
        {0.9801208f, 0.2192346f, 0.4190495f, 0},
        {0.9594298f, 0.4192392f, 0.2492729f, 0},
        {0.9260871f, 0.4008786f, 0.6109692f, 0},
        {0.9935196f, 0.7527797f, 0.01911602f, 0}, // yellow
        {1.011694f, 0.8214535f, 0.1651485f, 0},
        {0.9954063f, 0.9112986f, 0.3545238f, 0},
        {1.003194f, 1.006116f, 1.012645f, 0}, // white
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
        {6.012021f, 4.031786f, 2.753009f},    {-1.951351f, 2.541829f, -2.943336f},  {-3.538982f, -3.401223f, 6.261918f},
        {-9.158942f, -9.43565f, -6.419333f},  {-6.943133f, -1.841434f, 0.6081696f}, {3.486235f, -1.178201f, 2.700376f},
        {-0.2614991f, 2.874192f, -5.495195f}, {1.896523f, -4.150289f, -5.147171f},  {-1.033177f, 1.050958f, -0.5081406f},
        {6.352015f, 4.350923f, -4.255878f},   {-1.58086f, -2.425195f, 0.5121909f},  {9.486713f, 2.580155f, -0.349689f},
        {2.617461f, -3.75726f, -2.544169f},   {-5.539069f, 3.371077f, 0.04874995f}, {-7.649244f, -0.4619463f, 2.920352f},
        {5.274422f, 6.279873f, 4.325353f},    {-1.929021f, -2.138671f, -2.761521f}, {3.902634f, 0.6664852f, 1.838669f},
        {1.689628f, 1.487318f, 6.79435f},     {-3.703003f, -3.764152f, -1.039664f},
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

/// \p t of \p b into \p a (linear sRGB), as paint: the color of the mixed amounts plus what they miss, that
/// shortened as a whole where it would leave sRGB (so no channel is cut), and what lies past sRGB, mixed as light
inline void mixPaint(const Paint &a, const Paint &b, float t, float out[3])
{
    float amounts[4], rest[3];
    for (int k = 0; k < 4; k++) amounts[k] = a.amounts[k] + (b.amounts[k] - a.amounts[k]) * t;
    for (int c = 0; c < 3; c++) rest[c] = a.rest[c] + (b.rest[c] - a.rest[c]) * t;
    alignas(16) float made[4], slope[4][4];
    colorOfPaints(amounts, made, slope);
    float room = 1;
    for (int c = 0; c < 3; c++) {
        made[c] = std::clamp(made[c], 0.0f, 1.0f);
        if (rest[c] > 0) room = std::min(room, (1 - made[c]) / rest[c]);
        else if (rest[c] < 0) room = std::min(room, made[c] / -rest[c]);
    }
    const Curves &tables = curves();
    for (int c = 0; c < 3; c++) {
        out[c] = readTable<Curves::toLinearSteps>(tables.toLinear, made[c] + room * rest[c]) + a.past[c]
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

/// Linear sRGB with alpha, in floats: where the mixing happens
inline const KoColorSpace *linearSpace()
{
    static const KoColorSpace *space = KoColorSpaceRegistry::instance()->colorSpace(
        RGBAColorModelID.id(), Float32BitsColorDepthID.id(), KoColorSpaceRegistry::instance()->p709G10Profile());
    return space;
}

} // namespace KoMixing

/**
 * Paint over paint (Normal) or paint replacing paint by an amount (Copy, as smudging carries it), mixed in
 * OKLab or as pigment: the color of each pixel comes from mixing the two by how much of each there is, its
 * opacity as Krita's own blend has it.
 */
class KoCompositeOpMix : public KoCompositeOp
{
public:
    KoCompositeOpMix(const KoColorSpace *cs, int mode, bool copy)
        : KoCompositeOp(cs, KoMixing::opId(mode, copy ? COMPOSITE_COPY : COMPOSITE_OVER), KoCompositeOp::categoryMix())
        , m_mode(mode)
        , m_copy(copy)
    {
    }

    /// All of them for \p cs, for its constructor to add
    static QVector<KoCompositeOp *> all(const KoColorSpace *cs)
    {
        return {new KoCompositeOpMix(cs, KoMixing::Even, false), new KoCompositeOpMix(cs, KoMixing::Even, true),
                new KoCompositeOpMix(cs, KoMixing::Pigment, false), new KoCompositeOpMix(cs, KoMixing::Pigment, true)};
    }

    using KoCompositeOp::composite;

    void composite(const KoCompositeOp::ParameterInfo &params) const override
    {
        const KoColorSpace *own = colorSpace();
        const KoColorSpace *linear = KoMixing::linearSpace();
        if (!linear || params.rows <= 0 || params.cols <= 0) return;
        const int pixelSize = own->pixelSize();
        const int cols = params.cols;

        // channels switched off keep their values (the alpha channel off is the layer's alpha lock), but where
        // nothing was painted they are cleared, as Krita's own blends do
        const QList<KoChannelInfo *> channels = own->channels();
        bool alphaLocked = false;
        bool someLocked = false;
        if (!params.channelFlags.isEmpty()) {
            for (int i = 0; i < channels.size() && i < params.channelFlags.size(); i++) {
                if (params.channelFlags.testBit(i)) continue;
                someLocked = true;
                if (channels[i]->channelType() == KoChannelInfo::ALPHA) alphaLocked = true;
            }
        }

        const bool oneSource = params.srcRowStride == 0;
        std::unique_ptr<Workspace> workspace = take(linear);
        QVector<float> &source = workspace->source;
        QVector<float> &under = workspace->under;
        QVector<quint8> &back = workspace->back;
        QVector<bool> &changed = workspace->changed;
        QVector<bool> &wasClear = workspace->wasClear;
        source.resize(oneSource ? 4 : cols * 4);
        under.resize(cols * 4);
        back.resize(cols * pixelSize);
        changed.resize(cols);
        wasClear.resize(cols);
        // an image already in linear sRGB floats needs no conversion
        const bool same = !workspace->toLinear;
        auto toLinear = [&](const quint8 *from, float *to, int count) {
            if (same) std::memcpy(to, from, size_t(count) * pixelSize);
            else workspace->toLinear->transform(from, reinterpret_cast<quint8 *>(to), count);
        };
        if (oneSource) toLinear(params.srcRowStart, source.data(), 1);

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

        for (int row = 0; row < params.rows; row++) {
            quint8 *dst = params.dstRowStart + row * params.dstRowStride;
            const quint8 *src = params.srcRowStart + row * params.srcRowStride;
            const quint8 *mask = params.maskRowStart ? params.maskRowStart + row * params.maskRowStride : nullptr;
            toLinear(dst, under.data(), cols);
            if (!oneSource) toLinear(src, source.data(), cols);
            bool any = false;
            for (int x = 0; x < cols; x++) {
                changed[x] = false;
                const float *s = oneSource ? source.data() : source.data() + 4 * x;
                float *d = under.data() + 4 * x;
                wasClear[x] = d[3] <= 0;
                const float amount = params.opacity * (mask ? mask[x] / 255.0f : 1.0f);
                // how much of each there is in the result: Normal is paint over paint, Copy replaces by the amount
                const float ofSource = amount * s[3];
                const float ofUnder = m_copy ? (1 - amount) * d[3] : d[3] * (1 - amount * s[3]);
                if (ofSource <= 0 && !(m_copy && amount > 0)) continue;
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
                changed[x] = any = true;
            }
            if (!any) continue;
            // only the pixels painted on are written back, so the rest keep their exact values
            if (same) std::memcpy(back.data(), under.data(), size_t(cols) * pixelSize);
            else workspace->toOwn->transform(reinterpret_cast<const quint8 *>(under.data()), back.data(), cols);
            for (int x = 0; x < cols; x++) {
                if (!changed[x]) continue;
                const quint8 *mixed = back.data() + x * pixelSize;
                quint8 *out = dst + x * pixelSize;
                if (!someLocked) {
                    std::copy(mixed, mixed + pixelSize, out);
                    continue;
                }
                for (int i = 0; i < channels.size(); i++) {
                    const int at = channels[i]->pos();
                    const int size = channels[i]->size();
                    if (i < params.channelFlags.size() && !params.channelFlags.testBit(i)) {
                        if (wasClear[x]) std::fill(out + at, out + at + size, quint8(0));
                    } else {
                        std::copy(mixed + at, mixed + at + size, out + at);
                    }
                }
            }
        }
        giveBack(std::move(workspace));
    }

private:
    /**
     * What one composite() works with: the color conversions, which can't be shared between threads and are
     * costly to make (through the color spaces each row looked them up in a cache under a lock), and its rows
     */
    struct Workspace {
        std::unique_ptr<KoColorConversionTransformation> toLinear;
        std::unique_ptr<KoColorConversionTransformation> toOwn;
        QVector<float> source;
        QVector<float> under;
        QVector<quint8> back;
        QVector<bool> changed;
        QVector<bool> wasClear;
    };

    /// A workspace no other thread is using
    std::unique_ptr<Workspace> take(const KoColorSpace *linear) const
    {
        {
            QMutexLocker lock(&m_spareMutex);
            if (!m_spare.empty()) {
                std::unique_ptr<Workspace> spare = std::move(m_spare.back());
                m_spare.pop_back();
                return spare;
            }
        }
        auto made = std::make_unique<Workspace>();
        const KoColorSpace *own = colorSpace();
        if (!(*own == *linear)) {
            const auto intent = KoColorConversionTransformation::internalRenderingIntent();
            const auto flags = KoColorConversionTransformation::internalConversionFlags();
            made->toLinear.reset(own->createColorConverter(linear, intent, flags));
            made->toOwn.reset(linear->createColorConverter(own, intent, flags));
        }
        return made;
    }

    void giveBack(std::unique_ptr<Workspace> workspace) const
    {
        QMutexLocker lock(&m_spareMutex);
        m_spare.push_back(std::move(workspace));
    }

    int m_mode;
    bool m_copy;
    mutable QMutex m_spareMutex;
    mutable std::vector<std::unique_ptr<Workspace>> m_spare;
};

#endif // KOCOMPOSITEOPMIX_H
