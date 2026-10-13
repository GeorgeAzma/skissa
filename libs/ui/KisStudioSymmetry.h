/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOSYMMETRY_H
#define KISSTUDIOSYMMETRY_H

#include <QObject>
#include <QSize>

#include "kritaui_export.h"

class KisKActionCollection;

/**
 * Symmetry settings: painting each stroke several times at once.
 *
 *  - Mirror: Krita's canvas mirror axes (hmirror_action, vmirror_action),
 *    which work with every painting tool
 *  - Radial: copies rotated around a center, optionally mirrored
 *    (kaleidoscope); uses the Multibrush tool
 *  - Tile: the stroke repeated on a grid; uses the Multibrush tool
 *
 * Stored in the [Symmetry] group of kritarc; tools listen to changed().
 */
class KRITAUI_EXPORT KisStudioSymmetry : public QObject
{
    Q_OBJECT
public:
    enum Mode { Mirror = 0, Radial, Tile };

    static KisStudioSymmetry *instance();

    Mode mode() const;
    void setMode(Mode mode);
    /// Radial: number of copies, and whether every other one is mirrored
    int copies() const;
    void setCopies(int copies);
    bool mirrored() const;
    void setMirrored(bool mirrored);
    /// Radial: rotation of the axes, in radians clockwise from straight up
    qreal turn() const;
    void setTurn(qreal turn);
    /// Tile: distance between copies in image pixels (0 disables repeats in that direction)
    QSize spacing() const;
    void setSpacing(const QSize &spacing);
    /// Mirror: the axes to use when mirroring is turned on
    bool mirrorAcross() const;
    bool mirrorDown() const;
    void setMirrorAxes(bool across, bool down);

    /// Whether symmetry is currently on in the window that owns \p actions
    static bool isOn(KisKActionCollection *actions);
    /// Turns symmetry on (in the current mode) or off in the window that owns \p actions
    static void setOn(KisKActionCollection *actions, bool on);

    static constexpr int maximumCopies = 24;

Q_SIGNALS:
    void changed();

private:
    KisStudioSymmetry();
};

#endif // KISSTUDIOSYMMETRY_H
