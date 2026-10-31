/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISVALUESTEPS_H
#define KISVALUESTEPS_H

#include <cmath>

#include <QString>

/// How a dragged decimal value lands and reads: decimals only where they matter
namespace KisValueSteps
{

/**
 * \p value as a drag should leave it: whole from 10 up, tenths from 1, hundredths
 * below, but always fine enough for at least 200 steps across \p range
 */
inline double dragged(double value, double range)
{
    const double magnitude = std::abs(value);
    double step = magnitude >= 10 ? 1.0 : magnitude >= 1 ? 0.1 : 0.01;
    if (range > 0) {
        step = std::min(step, std::pow(10.0, std::floor(std::log10(range / 200))));
    }
    return std::round(value / step) * step;
}

/// \p text without the zeros ending its decimals: "192" for "192.00", "0.5" for "0.50"
inline QString withoutTrailingZeros(QString text, QChar decimalPoint = QLatin1Char('.'))
{
    const int point = text.indexOf(decimalPoint);
    if (point < 0) return text;
    int end = text.size();
    while (end > point + 1 && text[end - 1] == QLatin1Char('0')) end--;
    text.truncate(end == point + 1 ? point : end);
    if (text == QLatin1String("-0")) text = QStringLiteral("0");
    return text;
}

}

#endif
