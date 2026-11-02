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
 * Krita's effects that one of Skissa's does all of: they aren't offered (Find, the effects picker, their
 * shortcuts), but stay registered, so that files and filter masks using them still open and render.
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
        // Detail and the effects made of it
        {QStringLiteral("gaussian blur"), QStringLiteral("skissa_blur")}, {QStringLiteral("blur"), QStringLiteral("skissa_blur")},
        {QStringLiteral("unsharp"), QStringLiteral("skissa_sharpen")}, {QStringLiteral("sharpen"), QStringLiteral("skissa_sharpen")},
        {QStringLiteral("mean removal"), QStringLiteral("skissa_sharpen")},
        {QStringLiteral("gaussianhighpass"), QStringLiteral("skissa_high_pass")},
        {QStringLiteral("gaussiannoisereducer"), QStringLiteral("skissa_smooth")},
        {QStringLiteral("waveletnoisereducer"), QStringLiteral("skissa_smooth")},
    };
    return by.value(filterId);
}

inline bool isRetired(const QString &filterId)
{
    return !coveredBy(filterId).isEmpty();
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
