/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISRETIREDEFFECTS_H
#define KISRETIREDEFFECTS_H

#include <QHash>
#include <QSet>
#include <QString>

/**
 * Krita's effects that one of Skissa's does all of, and those left out as clutter (dated, or for a use too
 * narrow): they aren't offered (Find, the effects picker; the shortcuts of the covered ones open what covers them),
 * but stay registered, so that files and filter masks using them still open and render.
 */
namespace KisRetiredEffects
{
/// The effect that does what a retired one did; empty: it isn't retired
inline QString coveredBy(const QString &filterId)
{
    static const QString color = QStringLiteral("skissa_color");
    static const QHash<QString, QString> by = {
        // Color: curves in OKLCH and RGB, white balance, tints
        {QStringLiteral("perchannel"), color}, {QStringLiteral("crosschannel"), color}, {QStringLiteral("hsvadjustment"), color},
        {QStringLiteral("colorbalance"), color}, {QStringLiteral("desaturate"), color}, {QStringLiteral("invert"), color},
        {QStringLiteral("levels"), color}, {QStringLiteral("asc-cdl"), color}, {QStringLiteral("autocontrast"), color},
        {QStringLiteral("normalize"), color}, {QStringLiteral("maximize"), color}, {QStringLiteral("minimize"), color},
        // Detail and the effects made of it; Blur, Sharpen and High pass have Krita's ids, and their own from before
        {QStringLiteral("blur"), QStringLiteral("gaussian blur")}, {QStringLiteral("skissa_blur"), QStringLiteral("gaussian blur")},
        {QStringLiteral("sharpen"), QStringLiteral("unsharp")}, {QStringLiteral("mean removal"), QStringLiteral("unsharp")},
        {QStringLiteral("skissa_sharpen"), QStringLiteral("unsharp")},
        {QStringLiteral("skissa_high_pass"), QStringLiteral("gaussianhighpass")},
        {QStringLiteral("gaussiannoisereducer"), QStringLiteral("skissa_smooth")},
        {QStringLiteral("waveletnoisereducer"), QStringLiteral("skissa_smooth")},
        // Noise
        {QStringLiteral("noise"), QStringLiteral("skissa_grain")}, {QStringLiteral("randompick"), QStringLiteral("skissa_noise")},
        // Fewer colors
        {QStringLiteral("posterize"), QStringLiteral("skissa_fewer_colors")}, {QStringLiteral("threshold"), QStringLiteral("skissa_fewer_colors")},
        {QStringLiteral("palettize"), QStringLiteral("skissa_fewer_colors")}, {QStringLiteral("indexcolors"), QStringLiteral("skissa_fewer_colors")},
    };
    return by.value(filterId);
}

/// Left out with nothing in their place
inline bool isLeftOut(const QString &filterId)
{
    static const QSet<QString> leftOut = {
        QStringLiteral("wave"), QStringLiteral("raindrops"), QStringLiteral("smalltiles"), QStringLiteral("roundcorners"),
        QStringLiteral("oilpaint"), QStringLiteral("emboss"), QStringLiteral("emboss laplascian"), QStringLiteral("emboss all directions"),
        QStringLiteral("emboss horizontal and vertical"), QStringLiteral("emboss vertical only"), QStringLiteral("emboss horizontal only"),
        QStringLiteral("emboss diagonal"), QStringLiteral("edge detection"), QStringLiteral("phongbumpmap"), QStringLiteral("dodge"),
        QStringLiteral("burn"), QStringLiteral("tone_gamut_map"),
        // a color over the layer in a blend mode (a fill layer does it); the colors of an image file
        QStringLiteral("fastcoloroverlay"), QStringLiteral("colortransfer"),
    };
    return leftOut.contains(filterId);
}

inline bool isRetired(const QString &filterId)
{
    return !coveredBy(filterId).isEmpty() || isLeftOut(filterId);
}

/// The effect opened in place of a retired one, as its keys (Ctrl+M, Ctrl+U, Ctrl+L, Ctrl+B) ask; empty: it runs itself
inline QString replacement(const QString &filterId)
{
    static const QSet<QString> runsItself = {
        QStringLiteral("desaturate"), QStringLiteral("invert"), QStringLiteral("autocontrast"),
        QStringLiteral("normalize"), QStringLiteral("maximize"), QStringLiteral("minimize"),
    };
    return runsItself.contains(filterId) ? QString() : coveredBy(filterId);
}
} // namespace KisRetiredEffects

#endif
