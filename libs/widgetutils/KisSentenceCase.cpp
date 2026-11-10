/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisSentenceCase.h"

#include <QRegularExpression>
#include <QSet>
#include <QVector>

#include <klocalizedstring.h>

namespace
{

bool english()
{
    const QStringList languages = KLocalizedString::languages();
    return languages.isEmpty() || languages.first().startsWith(QLatin1String("en"));
}

/// Capitalized words that are names, not just the start of a Title Case word, and keys that are no other word
const QSet<QString> &names()
{
    static const QSet<QString> names = {
        QStringLiteral("Krita"), QStringLiteral("Skissa"), QStringLiteral("Windows"), QStringLiteral("Wacom"), QStringLiteral("Python"),
        QStringLiteral("Qt"), QStringLiteral("Photoshop"), QStringLiteral("Gimp"), QStringLiteral("Inkscape"), QStringLiteral("Adobe"),
        QStringLiteral("Android"), QStringLiteral("Linux"), QStringLiteral("Mac"), QStringLiteral("Lab"), QStringLiteral("Oklab"),
        QStringLiteral("Oklch"), QStringLiteral("Bezier"), QStringLiteral("Bézier"), QStringLiteral("Gaussian"), QStringLiteral("Fourier"),
        QStringLiteral("Perlin"), QStringLiteral("Voronoi"), QStringLiteral("Sobel"), QStringLiteral("Prewitt"), QStringLiteral("Laplacian"),
        QStringLiteral("Kuwahara"), QStringLiteral("Bayer"), QStringLiteral("Floyd"), QStringLiteral("Steinberg"), QStringLiteral("Lanczos"),
        QStringLiteral("Mitchell"), QStringLiteral("Hermite"), QStringLiteral("Catmull"), QStringLiteral("Rom"), QStringLiteral("Kubelka"),
        QStringLiteral("Munk"), QStringLiteral("Phong"), QStringLiteral("Blinn"), QStringLiteral("Euclidean"), QStringLiteral("Pegtop"),
        QStringLiteral("Delphi"), QStringLiteral("Ctrl"), QStringLiteral("Alt"), QStringLiteral("Esc"), QStringLiteral("Backspace"),
        QStringLiteral("I"),
    };
    return names;
}

/// Names of more than one word, each of which alone is a common word ("Clip to layer", "Black ink")
const QRegularExpression &phrases()
{
    static const QRegularExpression phrases(QStringLiteral("\\b(?:Clip Studio Paint|Clip Studio|Windows Ink)\\b"));
    return phrases;
}

/// Keys that are also words: capitals only where they name the key ("Shift+Tab", "press Enter", "the Space key")
const QSet<QString> &keyWords()
{
    static const QSet<QString> keys = {QStringLiteral("Shift"), QStringLiteral("Tab"), QStringLiteral("Enter"), QStringLiteral("Return"),
                                       QStringLiteral("Space"), QStringLiteral("Delete"), QStringLiteral("Home"), QStringLiteral("End"),
                                       QStringLiteral("Insert")};
    return keys;
}

struct Word {
    int start;
    int end;
    QString bare; ///< without an accelerator's "&"
};

bool namesKey(const QString &text, const QVector<Word> &words, int i)
{
    const Word &word = words[i];
    if (word.start > 0 && text.at(word.start - 1) == QLatin1Char('+')) return true;
    if (word.end < text.size() && text.at(word.end) == QLatin1Char('+')) return true;
    if (i + 1 < words.size()) {
        const QString next = words[i + 1].bare.toLower();
        if (next == QLatin1String("key") || next == QLatin1String("keys")) return true;
    }
    if (i > 0) {
        const QString before = words[i - 1].bare.toLower();
        if (before == QLatin1String("press") || before == QLatin1String("hold")) return true;
    }
    return false;
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
    // a name after " · " or " — " starts again too ("Select · Rectangular selection")
    static const QRegularExpression sentenceEnd(QStringLiteral("[.!?]\\s|\\n|<p>|<br|\\s[\\x{00B7}\\x{2014}]\\s"));

    QVector<Word> words;
    for (auto it = word.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        QString bare = match.captured();
        bare.remove(QLatin1Char('&'));
        words << Word{match.capturedStart(), match.capturedEnd(), bare};
    }
    QVector<QPair<int, int>> named;
    for (auto it = phrases().globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        named << qMakePair(match.capturedStart(), match.capturedEnd());
    }
    auto inName = [&named](int at) {
        for (const auto &range : named) {
            if (at >= range.first && at < range.second) return true;
        }
        return false;
    };

    QString out = text;
    for (int i = 0; i < words.size(); i++) {
        // the first word of the text, or of a sentence in it
        const int previousEnd = i > 0 ? words[i - 1].end : -1;
        if (previousEnd < 0 || text.mid(previousEnd, words[i].start - previousEnd).contains(sentenceEnd)) continue;
        const QString &bare = words[i].bare;
        if (!capitalized.match(bare).hasMatch() || names().contains(bare) || inName(words[i].start)) continue;
        if (keyWords().contains(bare) && namesKey(text, words, i)) continue;
        // the word's first letter, past an accelerator's "&"
        int at = words[i].start;
        while (out.at(at) == QLatin1Char('&')) at++;
        out[at] = out.at(at).toLower();
    }
    return out;
}
