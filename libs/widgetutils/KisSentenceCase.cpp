/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSentenceCase.h"

#include <QRegularExpression>
#include <QSet>

#include <klocalizedstring.h>

namespace
{

bool english()
{
    const QStringList languages = KLocalizedString::languages();
    return languages.isEmpty() || languages.first().startsWith(QLatin1String("en"));
}

/// Capitalized words that are names, not just the start of a Title Case word
const QSet<QString> &names()
{
    static const QSet<QString> names = {
        QStringLiteral("Krita"), QStringLiteral("Skissa"), QStringLiteral("Windows"), QStringLiteral("Ink"), QStringLiteral("Wacom"),
        QStringLiteral("Python"), QStringLiteral("Qt"), QStringLiteral("Photoshop"), QStringLiteral("Gimp"), QStringLiteral("Inkscape"),
        QStringLiteral("Android"), QStringLiteral("Linux"), QStringLiteral("Mac"), QStringLiteral("Lab"), QStringLiteral("Bezier"),
        QStringLiteral("Bézier"), QStringLiteral("Gaussian"), QStringLiteral("Fourier"), QStringLiteral("Perlin"), QStringLiteral("Voronoi"),
        QStringLiteral("Sobel"), QStringLiteral("Prewitt"), QStringLiteral("Laplacian"), QStringLiteral("Kuwahara"), QStringLiteral("Bayer"),
        QStringLiteral("Floyd"), QStringLiteral("Steinberg"), QStringLiteral("Lanczos"), QStringLiteral("Mitchell"), QStringLiteral("Hermite"),
        QStringLiteral("Catmull"), QStringLiteral("Rom"), QStringLiteral("Kubelka"), QStringLiteral("Munk"), QStringLiteral("Ctrl"),
        QStringLiteral("Shift"), QStringLiteral("Alt"), QStringLiteral("Tab"), QStringLiteral("Esc"), QStringLiteral("Enter"),
        QStringLiteral("I"),
    };
    return names;
}

} // namespace

QString KisSentenceCase::of(const QString &text)
{
    static const bool inEnglish = english();
    if (!inEnglish || text.isEmpty()) return text;

    // a word, with the "&" of a keyboard accelerator anywhere in it
    static const QRegularExpression word(QStringLiteral("[&\\p{L}][\\p{L}\\p{N}'&]*"));
    // only words that are capitalized and otherwise lower case: acronyms (RGB) and names written so (OpenGL) stay
    static const QRegularExpression capitalized(QStringLiteral("^\\p{Lu}\\p{Ll}+(?:'\\p{Ll}+)?$"));
    static const QRegularExpression sentenceEnd(QStringLiteral("[.!?]\\s|\\n|<p>|<br"));
    QString out = text;
    int previousEnd = -1;
    auto it = word.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        // the first word of the text, or of a sentence in it
        const bool first = previousEnd < 0 || text.mid(previousEnd, match.capturedStart() - previousEnd).contains(sentenceEnd);
        previousEnd = match.capturedEnd();
        if (first) continue;
        QString bare = match.captured();
        bare.remove(QLatin1Char('&'));
        if (!capitalized.match(bare).hasMatch() || names().contains(bare)) continue;
        // the word's first letter, past an accelerator's "&"
        int at = match.capturedStart();
        while (out.at(at) == QLatin1Char('&')) at++;
        out[at] = out.at(at).toLower();
    }
    return out;
}
