/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KISPUPPET_H
#define KISPUPPET_H

#include <functional>

#include <QPointF>
#include <QRect>
#include <QSharedPointer>
#include <QVector>

#include "kis_types.h"
#include "kritaimage_export.h"

/**
 * A puppet for bending a picture by pins: a grid of cells over the picture,
 * stiff where there is paint and nearly free in the air between, posed as
 * rigidly as possible (Igarashi, Moscovich and Hughes 2005, "As-rigid-as-possible
 * shape manipulation": a scale-free pose, then each triangle fitted without
 * scaling). A pose depends only on the pins, so the same pins always give the
 * same picture. Pins are points anywhere on it. Each separate shape moves by its own
 * pins: without any it stays, with one it moves without turning, with more it bends.
 */
class KRITAIMAGE_EXPORT KisPuppet
{
public:
    /// \p stiff tells whether a cell of the grid holds paint
    KisPuppet(const QRect &bounds, int step, const std::function<bool(const QRect &cell)> &stiff);

    /// A puppet of \p device's paint within \p bounds, in cells sized for the bounds
    static KisPuppet ofDevice(KisPaintDeviceSP device, const QRect &bounds);

    /// The cell size for \p bounds: fine enough to bend smoothly, coarse enough to pose at once
    static int stepFor(const QRect &bounds);

    QRect bounds() const;
    int step() const;

    /// Adds a pin at \p at (where the picture is now) and returns its index
    int addPin(const QPointF &at);
    void removePin(int index);
    /// Puts pins back as they were, \p rest on the unbent picture and \p at where they are now
    void setPins(const QVector<QPointF> &rest, const QVector<QPointF> &at);
    void movePin(int index, const QPointF &to);
    int pinCount() const;
    /// Where the pin is on the unbent picture
    QPointF pinRest(int index) const;
    /// Where the pin is now
    QPointF pinAt(int index) const;
    /// The pin under \p point (in the bent picture) within \p radius, or -1
    int pinNear(const QPointF &point, qreal radius) const;

    /// Where \p rest, a point of the unbent picture, is in the bent one
    QPointF map(const QPointF &rest) const;

    /// The grid's corners, unbent and bent, row by row
    int columns() const;
    int rows() const;
    const QVector<QPointF> &restPoints() const;
    const QVector<QPointF> &bentPoints() const;
    /// Whether the cell at \p column, \p row holds paint
    bool isStiff(int column, int row) const;

private:
    struct Private;
    QSharedPointer<Private> d;
};

#endif
