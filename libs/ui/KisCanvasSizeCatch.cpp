/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisCanvasSizeCatch.h"

#include <QVector>

#include <cmath>

namespace KisCanvasSizeCatch
{

namespace
{

/// Sides images are often made at: HD and 4K screens and their halves and doubles, square textures, A sheets at 300 dpi
const QVector<int> &usualSides()
{
    static const QVector<int> sides = {512, 720, 768, 1024, 1080, 1200, 1280, 1440, 1600, 1920, 2048, 2160, 2480, 2560, 3072, 3508, 3840, 4096, 4961, 5120, 7016, 7680};
    return sides;
}

/// Ratios of one side to the other, the sheet's taken as √2
const QVector<qreal> &usualRatios()
{
    static const QVector<qreal> ratios = {1.0, 4.0 / 3, 3.0 / 4, 3.0 / 2, 2.0 / 3, 16.0 / 9, 9.0 / 16, std::sqrt(2.0), 1 / std::sqrt(2.0), 2.0, 0.5};
    return ratios;
}

/// The nearest of \p candidates to \p side within \p reach, or \p side
int nearest(int side, const QVector<qreal> &candidates, qreal reach)
{
    qreal best = side;
    qreal bestDistance = reach;
    for (qreal candidate : candidates) {
        const qreal distance = std::abs(candidate - side);
        if (candidate >= 1 && distance <= bestDistance) {
            bestDistance = distance;
            best = candidate;
        }
    }
    return int(std::round(best));
}

/// \p rect resized to \p size along one axis, the moving edge going
QRect resized(QRect rect, int size, bool across, Qt::Edges moving)
{
    if (across) {
        if (moving & Qt::LeftEdge) rect.setLeft(rect.right() - size + 1);
        else rect.setRight(rect.left() + size - 1);
    } else {
        if (moving & Qt::TopEdge) rect.setTop(rect.bottom() - size + 1);
        else rect.setBottom(rect.top() + size - 1);
    }
    return rect;
}

} // namespace

QRect caught(const QRect &rect, const QRect &original, Qt::Edges moving, qreal sizeReach)
{
    QRect result = rect;
    const bool across = moving & (Qt::LeftEdge | Qt::RightEdge);
    const bool down = moving & (Qt::TopEdge | Qt::BottomEdge);
    // sizes first, a little more eagerly than ratios: the size it had matters most
    QVector<qreal> sizes = {qreal(original.width()), qreal(original.height())};
    for (int side : usualSides()) sizes << side;
    auto ratios = [&](int other) {
        QVector<qreal> candidates;
        for (qreal ratio : usualRatios()) candidates << other * ratio;
        return candidates;
    };
    const qreal ratioReach = sizeReach * 2 / 3;
    if (across) {
        int width = nearest(result.width(), sizes, sizeReach);
        // a ratio only by a side that stays, so the two don't chase each other
        if (width == result.width() && !down) width = nearest(result.width(), ratios(result.height()), ratioReach);
        result = resized(result, width, true, moving);
    }
    if (down) {
        int height = nearest(result.height(), sizes, sizeReach);
        if (height == result.height()) height = nearest(result.height(), ratios(result.width()), ratioReach);
        result = resized(result, height, false, moving);
    }
    return result;
}

} // namespace KisCanvasSizeCatch
