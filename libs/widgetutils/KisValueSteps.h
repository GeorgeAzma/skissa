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

/*
 * One law for a value dragged or turned by the wheel, so the same move does as much everywhere: a size (a diameter, a
 * radius) moves by ratios, as sizes are seen, anything else evenly across its usual range. With Shift a drag is
 * finer by `fine`; the gains are the top bar's.
 */

/// Pixels of drag that double a size
constexpr double pixelsPerDoubling = 100;
/// Pixels of drag across a value's whole usual range
constexpr double pixelsPerRange = 200;
/// How much finer a drag is with Shift held
constexpr double fine = 0.25;
/// One notch of the wheel: a fifth of a doubling, or a twentieth of the range
constexpr double notchesPerDoubling = 5;
constexpr double notchesPerRange = 20;

/// \p start dragged by \p pixels (positive is more): a size by ratios (from \p least when it starts below), anything
/// else across \p range
inline double draggedBy(double start, double pixels, double range, bool size, double least = 0)
{
    if (size) return std::max(start, least) * std::exp2(pixels / pixelsPerDoubling);
    return start + pixels / pixelsPerRange * range;
}

/// \p value turned by \p notches of the wheel, by the same law
inline double turnedBy(double value, double notches, double range, bool size, double least = 0)
{
    if (size) return std::max(value, least) * std::exp2(notches / notchesPerDoubling);
    return value + notches / notchesPerRange * range;
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
