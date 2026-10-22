/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *
 *  The spectral tables and the pigment mixing below are from spectral.js:
 *
 *  MIT License
 *
 *  Copyright (c) 2025 Ronald van Wijnen
 *
 *  Permission is hereby granted, free of charge, to any person obtaining a
 *  copy of this software and associated documentation files (the "Software"),
 *  to deal in the Software without restriction, including without limitation
 *  the rights to use, copy, modify, merge, publish, distribute, sublicense,
 *  and/or sell copies of the Software, and to permit persons to whom the
 *  Software is furnished to do so, subject to the following conditions:
 *
 *  The above copyright notice and this permission notice shall be included in
 *  all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 *  FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 *  DEALINGS IN THE SOFTWARE.
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
 * image's own values; evenly, in OKLab; or as pigment, by Kubelka-Munk over spectra, so blue and yellow make green.
 * The image's colors are brought to linear sRGB through its own profile first, so an sRGB image and a linear one
 * mix alike. Only the blending of paint over paint changes: a brush's look (its edges, its build-up) stays.
 */
namespace KoMixing
{
enum Mode { Normal = 0, Even = 1, Pigment = 2 };

/// The brush setting holding the mode (kept by Krita when it writes a brush's settings again)
inline QString settingName() { return QStringLiteral("SkissaMixing"); }

// From spectral.js, MIT License, Copyright (c) 2025 Ronald van Wijnen: seven reflectance curves (white, cyan,
// magenta, yellow, red, green, blue) over 38 wavelengths that linear sRGB is made of, the CIE 1931 observer
// under D65 to bring a reflectance back to XYZ, and XYZ to linear sRGB.
constexpr int spectralSize = 38;
constexpr double spectralBasis[7][spectralSize] = {
    {1.0011607271876400, 1.0011606515972800, 1.0011603192274700, 1.0011586727078900, 1.0011525984455200, 1.0011325252899800, 1.0010850066332700, 1.0009968788945300, 1.0008652515227400, 1.0006962900094000, 1.0005049611488800, 1.0003080818799200, 1.0001196660201300, 0.9999527659684070, 0.9998218368992970, 0.9997386095575930, 0.9997095516396120, 0.9997319302106270, 0.9997994363461950, 0.9999003303166710, 1.0000204065261100, 1.0001447879365800, 1.0002599790341200, 1.0003557969708900, 1.0004275378026900, 1.0004762334488800, 1.0005072096750800, 1.0005251915637300, 1.0005350960689600, 1.0005402209748200, 1.0005427281678400, 1.0005438956908700, 1.0005444821215100, 1.0005447695999200, 1.0005448988776200, 1.0005449625468900, 1.0005449892705800, 1.0005449969930000}, // white
    {0.9705850013229620, 0.9705924981434250, 0.9706253487298910, 0.9707868061190170, 0.9713686732282480, 0.9731632306212520, 0.9767402231587650, 0.9815876054913770, 0.9862802656529490, 0.9899491476891340, 0.9924927015384200, 0.9941456804052560, 0.9951839750332120, 0.9957567501108180, 0.9959128182867100, 0.9956061578345280, 0.9945976009618540, 0.9922157154923700, 0.9862364527832490, 0.9679433372645410, 0.8912850042449430, 0.5362024778620530, 0.1541081190018780, 0.0574575093228929, 0.0315349873107007, 0.0222633920086335, 0.0182022841492439, 0.0162990559732640, 0.0153656239334613, 0.0149111568733976, 0.0146954339898235, 0.0145964146717719, 0.0145470156699655, 0.0145228771899495, 0.0145120341118965, 0.0145066940939832, 0.0145044507314479, 0.0145038009464639}, // cyan
    {0.9906735573199880, 0.9906715249619790, 0.9906625823534210, 0.9906181076447950, 0.9904514808787100, 0.9898710814002040, 0.9882866087596400, 0.9842906927975040, 0.9739349056253060, 0.9418178384601450, 0.8173903261951560, 0.4324728050657290, 0.1384539782588700, 0.0537347216940033, 0.0292174996673231, 0.0213136517508590, 0.0201349530181136, 0.0241323096280662, 0.0372236145223627, 0.0760506552706601, 0.2053754719423990, 0.5412689034604390, 0.8158416850864860, 0.9128177041239760, 0.9463398301669620, 0.9599276963319910, 0.9662605952303120, 0.9693259700584240, 0.9708545367213990, 0.9716050665281280, 0.9719627697573920, 0.9721272722745090, 0.9722094177458120, 0.9722495776784240, 0.9722676219987420, 0.9722765094621500, 0.9722802433068740, 0.9722813248265600}, // magenta
    {0.0210523371789306, 0.0210564627517414, 0.0210746178695038, 0.0211649058448753, 0.0215027957272504, 0.0226738799041561, 0.0258235649693629, 0.0334879385639851, 0.0519069663740307, 0.1007490148334730, 0.2391298997068470, 0.5348043122727480, 0.7978075786430300, 0.9114498940673840, 0.9537979630045070, 0.9712416154654290, 0.9793031238075880, 0.9833801195075750, 0.9854612465677550, 0.9864350469766050, 0.9867382506701410, 0.9866178824450320, 0.9862777767586430, 0.9858605924440560, 0.9854749276762100, 0.9851769347655580, 0.9849715740141810, 0.9848463034157120, 0.9847753518111990, 0.9847380666252650, 0.9847196483117650, 0.9847110233919390, 0.9847066833006760, 0.9847045543930910, 0.9847035963093700, 0.9847031240775520, 0.9847029256150900, 0.9847028681227950}, // yellow
    {0.0315605737777207, 0.0315520718330149, 0.0315148215513658, 0.0313318044982702, 0.0306729857725527, 0.0286480476989607, 0.0246450407045709, 0.0192960753663651, 0.0142066612220556, 0.0102942608878609, 0.0076191460521811, 0.0058980410835420, 0.0048233247781713, 0.0042298748350633, 0.0040599171299341, 0.0043533695594676, 0.0053434425970201, 0.0076917201010463, 0.0135969795736536, 0.0316975442661115, 0.1078611963552490, 0.4638126031687040, 0.8470554052720110, 0.9431854093939180, 0.9688621506965580, 0.9780306674736030, 0.9820436438543060, 0.9839236237187070, 0.9848454841543820, 0.9852942758145960, 0.9855072952198250, 0.9856050715398370, 0.9856538499335780, 0.9856776850338830, 0.9856883918061220, 0.9856936646900310, 0.9856958798482050, 0.9856965214637620}, // red
    {0.0095560747554212, 0.0095581580120851, 0.0095673245444588, 0.0096129126297349, 0.0097837090401843, 0.0103786227058710, 0.0120026452378567, 0.0160977721473922, 0.0267061902231680, 0.0595555440185881, 0.1860398265328260, 0.5705798201161590, 0.8614677684002920, 0.9458790897676580, 0.9704654864743050, 0.9784136302844500, 0.9795890314112240, 0.9755335369086320, 0.9622887553978130, 0.9231215745131200, 0.7934340189431110, 0.4592701359024290, 0.1855741036663030, 0.0881774959955372, 0.0543630228766700, 0.0406288447060719, 0.0342215204316970, 0.0311185790956966, 0.0295708898336134, 0.0288108739348928, 0.0284486271324597, 0.0282820301724731, 0.0281988376490237, 0.0281581655342037, 0.0281398910216386, 0.0281308901665811, 0.0281271086805816, 0.0281260133612096}, // green
    {0.9794047525020140, 0.9794007068431300, 0.9793829034702610, 0.9792943649455940, 0.9789630146085700, 0.9778144666940430, 0.9747243211338360, 0.9671984823439730, 0.9490796575305750, 0.9008501289409770, 0.7631504454622400, 0.4659221716493190, 0.2012632804510050, 0.0877524413419623, 0.0457176793291679, 0.0284706050521843, 0.0205271767569850, 0.0165302792310211, 0.0145135107212858, 0.0136003508637687, 0.0133604258769571, 0.0135488943145680, 0.0139594356366992, 0.0144434255753570, 0.0148854440621406, 0.0152254296999746, 0.0154592848180209, 0.0156018026485961, 0.0156824871281936, 0.0157248764360615, 0.0157458108784121, 0.0157556123350225, 0.0157605443964911, 0.0157629637515278, 0.0157640525629106, 0.0157645892329510, 0.0157648147772649, 0.0157648801149616}, // blue
};
constexpr double spectralObserver[spectralSize][3] = {
    {0.0000646919989576, 0.0000018442894440, 0.0003050171476380},
    {0.0002194098998132, 0.0000062053235865, 0.0010368066663574},
    {0.0011205743509343, 0.0000310096046799, 0.0053131363323992},
    {0.0037666134117111, 0.0001047483849269, 0.0179543925899536},
    {0.0118805536037990, 0.0003536405299538, 0.0570775815345485},
    {0.0232864424191771, 0.0009514714056444, 0.1136516189362870},
    {0.0345594181969747, 0.0022822631748318, 0.1733587261835500},
    {0.0372237901162006, 0.0042073290434730, 0.1962065755586570},
    {0.0324183761091486, 0.0066887983719014, 0.1860823707062960},
    {0.0212332056093810, 0.0098883960193565, 0.1399504753832070},
    {0.0104909907685421, 0.0152494514496311, 0.0891745294268649},
    {0.0032958375797931, 0.0214183109449723, 0.0478962113517075},
    {0.0005070351633801, 0.0334229301575068, 0.0281456253957952},
    {0.0009486742057141, 0.0513100134918512, 0.0161376622950514},
    {0.0062737180998318, 0.0704020839399490, 0.0077591019215214},
    {0.0168646241897775, 0.0878387072603517, 0.0042961483736618},
    {0.0286896490259810, 0.0942490536184085, 0.0020055092122156},
    {0.0426748124691731, 0.0979566702718931, 0.0008614711098802},
    {0.0562547481311377, 0.0941521856862608, 0.0003690387177652},
    {0.0694703972677158, 0.0867810237486753, 0.0001914287288574},
    {0.0830531516998291, 0.0788565338632013, 0.0001495555858975},
    {0.0861260963002257, 0.0635267026203555, 0.0000923109285104},
    {0.0904661376847769, 0.0537414167568200, 0.0000681349182337},
    {0.0850038650591277, 0.0426460643574120, 0.0000288263655696},
    {0.0709066691074488, 0.0316173492792708, 0.0000157671820553},
    {0.0506288916373645, 0.0208852059213910, 0.0000039406041027},
    {0.0354739618852640, 0.0138601101360152, 0.0000015840125870},
    {0.0214682102597065, 0.0081026402038399, 0.0000000000000000},
    {0.0125164567619117, 0.0046301022588030, 0.0000000000000000},
    {0.0068045816390165, 0.0024913800051319, 0.0000000000000000},
    {0.0034645657946526, 0.0012593033677378, 0.0000000000000000},
    {0.0014976097506959, 0.0005416465221680, 0.0000000000000000},
    {0.0007697004809280, 0.0002779528920067, 0.0000000000000000},
    {0.0004073680581315, 0.0001471080673854, 0.0000000000000000},
    {0.0001690104031614, 0.0000610327472927, 0.0000000000000000},
    {0.0000952245150365, 0.0000343873229523, 0.0000000000000000},
    {0.0000490309872958, 0.0000177059860053, 0.0000000000000000},
    {0.0000199961492222, 0.0000072209749130, 0.0000000000000000},
};
constexpr double spectralXyzToRgb[3][3] = {
    {3.2409699419045200, -1.537383177570090, -0.4986107602930030},
    {-0.9692436362808790, 1.875967501507720, 0.0415550574071756},
    {0.0556300796969936, -0.203976958888976, 1.0569715142428700},
};

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

/// The wavelengths padded to a multiple of four, with no light in the padding, so the loops over them vectorize
constexpr int spectralLanes = 40;

/// The tables above in floats, padded
struct SpectralTables {
    alignas(16) float basis[7][spectralLanes];
    alignas(16) float observer[3][spectralLanes];
};

inline const SpectralTables &spectralTables()
{
    static const SpectralTables tables = []() {
        SpectralTables t {};
        for (int i = 0; i < spectralSize; i++) {
            for (int k = 0; k < 7; k++) t.basis[k][i] = float(spectralBasis[k][i]);
            for (int c = 0; c < 3; c++) t.observer[c][i] = float(spectralObserver[i][c]);
        }
        return t;
    }();
    return tables;
}

/// A color as pigment: its absorption over scattering per wavelength, how much light it gives back, and the color
/// its spectrum makes again (a little off the color itself: the mix is corrected by it, so a color mixed with
/// itself stays exactly itself)
struct Paint {
    alignas(16) float ks[spectralLanes];
    float luminance {0};
    float again[3] {0, 0, 0};
};

inline void colorOfReflectance(const float reflectance[spectralLanes], float rgb[3])
{
    const SpectralTables &tables = spectralTables();
    // four sums each, so the loop vectorizes without reordering a sum
    float x[4] = {0, 0, 0, 0}, y[4] = {0, 0, 0, 0}, z[4] = {0, 0, 0, 0};
    for (int i = 0; i < spectralLanes; i += 4) {
        for (int j = 0; j < 4; j++) {
            x[j] += reflectance[i + j] * tables.observer[0][i + j];
            y[j] += reflectance[i + j] * tables.observer[1][i + j];
            z[j] += reflectance[i + j] * tables.observer[2][i + j];
        }
    }
    const double xyz[3] = {double(x[0] + x[1] + x[2] + x[3]), double(y[0] + y[1] + y[2] + y[3]), double(z[0] + z[1] + z[2] + z[3])};
    for (int c = 0; c < 3; c++) {
        rgb[c] = float(spectralXyzToRgb[c][0] * xyz[0] + spectralXyzToRgb[c][1] * xyz[1] + spectralXyzToRgb[c][2] * xyz[2]);
    }
}

inline void paintOf(const float color[3], Paint &paint)
{
    // pigment has no light beyond white
    float rgb[3];
    for (int c = 0; c < 3; c++) rgb[c] = std::clamp(color[c], 0.0f, 1.0f);
    const float w = std::min({rgb[0], rgb[1], rgb[2]});
    const float r0 = rgb[0] - w, g0 = rgb[1] - w, b0 = rgb[2] - w;
    const float cyan = std::min(g0, b0), magenta = std::min(r0, b0), yellow = std::min(r0, g0);
    const float red = std::min(std::max(0.0f, r0 - b0), std::max(0.0f, r0 - g0));
    const float green = std::min(std::max(0.0f, g0 - b0), std::max(0.0f, g0 - r0));
    const float blue = std::min(std::max(0.0f, b0 - g0), std::max(0.0f, b0 - r0));
    const float weights[7] = {w, cyan, magenta, yellow, red, green, blue};
    const SpectralTables &tables = spectralTables();
    // at most three of the curves are in a color
    alignas(16) float reflectance[spectralLanes] = {};
    for (int k = 0; k < 7; k++) {
        if (weights[k] <= 0) continue;
        for (int i = 0; i < spectralLanes; i++) reflectance[i] += weights[k] * tables.basis[k][i];
    }
    float luminance[4] = {0, 0, 0, 0};
    for (int i = 0; i < spectralLanes; i += 4) {
        for (int j = 0; j < 4; j++) {
            const float r = std::max(1e-16f, reflectance[i + j]);
            reflectance[i + j] = r;
            luminance[j] += r * tables.observer[1][i + j];
            paint.ks[i + j] = (1 - r) * (1 - r) / (2 * r);
        }
    }
    paint.luminance = luminance[0] + luminance[1] + luminance[2] + luminance[3];
    colorOfReflectance(reflectance, paint.again);
}

/// \p t of \p b into \p a, as paint: by concentration, each weighed by how much light it gives back
inline void mixPaint(const Paint &a, const float aColor[3], const Paint &b, const float bColor[3], float t, float out[3])
{
    const float ca = (1 - t) * (1 - t) * a.luminance;
    const float cb = t * t * b.luminance;
    if (!(ca + cb > 0)) {
        for (int c = 0; c < 3; c++) out[c] = aColor[c] + (bColor[c] - aColor[c]) * t;
        return;
    }
    const float wa = ca / (ca + cb), wb = cb / (ca + cb);
    alignas(16) float reflectance[spectralLanes];
    for (int i = 0; i < spectralLanes; i++) {
        const float ks = a.ks[i] * wa + b.ks[i] * wb;
        // 1 + ks - sqrt(ks² + 2ks) without the cancellation (dark paint's ks is in the millions); fmax tells the
        // compiler the root is of no negative number, so it needs no errno and vectorizes
        reflectance[i] = 1 / (1 + ks + std::sqrt(std::fmax(ks * (ks + 2), 0.0f)));
    }
    colorOfReflectance(reflectance, out);
    for (int c = 0; c < 3; c++) {
        out[c] += (1 - t) * (aColor[c] - a.again[c]) + t * (bColor[c] - b.again[c]);
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
        mixPaint(pa, a, pb, b, t, out);
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
                    KoMixing::mixPaint(underPaint, d, sourcePaint, s, t, mixed);
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
