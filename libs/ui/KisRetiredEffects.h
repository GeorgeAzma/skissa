/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISRETIREDEFFECTS_H
#define KISRETIREDEFFECTS_H

#include <QSet>
#include <QString>

/**
 * Krita's effects that one of Skissa's does all of: they aren't offered (Find, the effects picker, their
 * shortcuts), but stay registered, so that files and filter masks using them still open and render.
 */
namespace KisRetiredEffects
{
inline bool isRetired(const QString &filterId)
{
    // all done by Color: curves in OKLCH and RGB, white balance, tints
    static const QSet<QString> retired = {
        QStringLiteral("perchannel"), QStringLiteral("crosschannel"), QStringLiteral("hsvadjustment"),
        QStringLiteral("colorbalance"), QStringLiteral("desaturate"), QStringLiteral("invert"),
        QStringLiteral("levels"), QStringLiteral("asc-cdl"), QStringLiteral("autocontrast"),
        QStringLiteral("normalize"), QStringLiteral("maximize"), QStringLiteral("minimize"),
    };
    return retired.contains(filterId);
}

/// The effect opened in place of a retired one, as its keys (Ctrl+M, Ctrl+U, Ctrl+L, Ctrl+B) ask; empty: it runs itself
inline QString replacement(const QString &filterId)
{
    static const QSet<QString> color = {
        QStringLiteral("perchannel"), QStringLiteral("crosschannel"), QStringLiteral("hsvadjustment"),
        QStringLiteral("colorbalance"), QStringLiteral("levels"), QStringLiteral("asc-cdl"),
    };
    return color.contains(filterId) ? QStringLiteral("skissa_color") : QString();
}
} // namespace KisRetiredEffects

#endif
