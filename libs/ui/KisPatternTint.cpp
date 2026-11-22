/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisPatternTint.h"

#include <QMutex>

#include <kis_config.h>

bool KisPatternTint::isOn()
{
    return KisConfig(true).readEntry<bool>("StudioPatternTint", false);
}

void KisPatternTint::setOn(bool on)
{
    KisConfig(false).writeEntry("StudioPatternTint", on);
}

KoPatternSP KisPatternTint::applied(KoPatternSP pattern, const KoColor &color)
{
    if (!pattern || !isOn()) return pattern;
    // made once per pattern and color: every stroke takes a snapshot of the resources
    static QMutex lock;
    static KoPatternSP lastFrom;
    static QRgb lastColor = 0;
    static KoPatternSP lastMade;
    const QColor ink = color.toQColor();
    QMutexLocker locker(&lock);
    if (pattern == lastFrom && ink.rgba() == lastColor && lastMade) return lastMade;

    QImage image = pattern->pattern().convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < image.height(); y++) {
        QRgb *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); x++) {
            const QRgb pixel = line[x];
            const int coverage = (255 - qGray(pixel)) * qAlpha(pixel) / 255;
            line[x] = qRgba(ink.red(), ink.green(), ink.blue(), coverage * ink.alpha() / 255);
        }
    }
    lastMade = KoPatternSP(new KoPattern(image, pattern->name(), pattern->filename()));
    lastFrom = pattern;
    lastColor = ink.rgba();
    return lastMade;
}
