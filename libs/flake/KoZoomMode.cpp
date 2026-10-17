/* This file is part of the KDE project
   SPDX-FileCopyrightText: 2005 Johannes Schaub <litb_devel@web.de>
   SPDX-FileCopyrightText: 2011 Arjen Hiemstra <ahiemstra@heimr.nl>

   SPDX-License-Identifier: LGPL-2.0-or-later
*/

#include "KoZoomMode.h"

#include <QDebug>

#include <klocalizedstring.h>
#include <kconfiggroup.h>
#include <ksharedconfig.h>

#include <cmath>
#include "kis_assert.h"

QString KoZoomMode::toString(Mode mode)
{
    switch (mode) {
    case KoZoomMode::ZOOM_CONSTANT:
        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(0 && "constant zoom has no user-friendly representation", "XXX");
    case KoZoomMode::ZOOM_PAGE:
        return i18n("Fit View");
    case KoZoomMode::ZOOM_WIDTH:
        return i18n("Fit Width");
    case KoZoomMode::ZOOM_HEIGHT:
        return i18n("Fit Height");
    }
    return "";
}

namespace {

/**
 * The simplest ratio near \p zoom (5/4, 3/2, 5/2...; its inverse below 100%), within \p tolerance as a log
 * ratio: the pixels land on the screen most evenly there. Smaller denominators win unless much further.
 */
qreal sharpened(qreal zoom, qreal tolerance)
{
    const bool down = zoom < 1.0;
    const qreal up = down ? 1.0 / zoom : zoom;
    qreal best = up;
    qreal bestScore = tolerance + 1.0;
    // halves and quarters only near 100%: further out, whole steps are the even ones
    const int finest = up < 2.0 ? 4 : up < 4.0 ? 2 : 1;
    for (int denominator = 1; denominator <= finest; denominator++) {
        const qreal ratio = qMax(1.0, std::round(up * denominator)) / denominator;
        const qreal distance = std::abs(std::log(ratio / up));
        const qreal score = distance + 0.02 * denominator;
        if (distance <= tolerance && score < bestScore) {
            best = ratio;
            bestScore = score;
        }
    }
    return down ? 1.0 / best : best;
}

}

QVector<qreal> KoZoomMode::generateStandardZoomLevels(qreal minZoom, qreal maxZoom)
{
    KConfigGroup config = KSharedConfig::openConfig()->group("");
    const int steps = qMax(1, config.readEntry("zoomSteps", 3));

    // evenly spaced by ratio, each level then caught by the simplest ratio near it
    const int first = std::ceil(std::log2(minZoom) * steps);
    const int last = std::floor(std::log2(maxZoom) * steps);
    const qreal tolerance = 0.4 * M_LN2 / steps;
    QVector<qreal> zoomLevels;
    for (int i = first; i <= last; i++) {
        const qreal zoom = qBound(minZoom, sharpened(std::exp2(qreal(i) / steps), tolerance), maxZoom);
        if (zoomLevels.isEmpty() || zoom > zoomLevels.last() * (1.0 + 1e-6)) zoomLevels << zoom;
    }
    return zoomLevels;
}

qreal KoZoomMode::findNextZoom(qreal currentZoom, const QVector<qreal> &zoomLevels)
{
    const qreal eps = 1e-5;
    int i = 0;
    while (i < zoomLevels.size() - 1 && currentZoom > zoomLevels[i] - eps) {
        i++;
    }

    return qMax(currentZoom, zoomLevels[i]);
}

qreal KoZoomMode::findPrevZoom(qreal currentZoom, const QVector<qreal> &zoomLevels)
{
    const qreal eps = 1e-5;
    int i = zoomLevels.size() - 1;
    while (i > 0 && currentZoom < zoomLevels[i] + eps) i--;

    return qMin(currentZoom, zoomLevels[i]);
}

QDebug operator<<(QDebug dbg, const KoZoomMode::Mode &mode)
{
    dbg.nospace() << "KoZoomMode::Mode(";

    switch (mode) {
    case KoZoomMode::ZOOM_CONSTANT:
        dbg << "ZOOM_CONSTANT";
        break;
    case KoZoomMode::ZOOM_PAGE:
        dbg << "ZOOM_PAGE";
        break;
    case KoZoomMode::ZOOM_WIDTH:
        dbg << "ZOOM_WIDTH";
        break;
    case KoZoomMode::ZOOM_HEIGHT:
        dbg << "ZOOM_HEIGHT";
        break;
    default:
        dbg << "UNKNOWN";
        break;
    }

    dbg << ")";
    return dbg.space();
}
