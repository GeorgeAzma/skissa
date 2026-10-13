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
 * Symmetry: painting a stroke many times at once, one way of three.
 *
 *  - Mirror: Krita's mirror axes (hmirror_action, vmirror_action), with
 *    every tool that paints
 *  - Radial: copies turned around a center, maybe mirrored too (a
 *    kaleidoscope), with the brush (the Multibrush tool)
 *  - Tile: the stroke repeated on a grid, with the brush
 *
 * Its settings, kept in kritarc's [Symmetry]; the tools follow changed().
 */
class KRITAUI_EXPORT KisStudioSymmetry : public QObject
{
    Q_OBJECT
public:
    enum Mode { Mirror = 0, Radial, Tile };

    static KisStudioSymmetry *instance();

    Mode mode() const;
    void setMode(Mode mode);
    /// Radial: how many copies, and whether every other one is mirrored
    int copies() const;
    void setCopies(int copies);
    bool mirrored() const;
    void setMirrored(bool mirrored);
    /// Radial: how its axes are turned, in radians from straight up, clockwise
    qreal turn() const;
    void setTurn(qreal turn);
    /// Tile: how far apart the copies are, in image pixels (0: no repeats that way)
    QSize spacing() const;
    void setSpacing(const QSize &spacing);
    /// Mirror: which axes, as they were last on
    bool mirrorAcross() const;
    bool mirrorDown() const;
    void setMirrorAxes(bool across, bool down);

    /// Whether the window painting through \p actions paints with symmetry now
    static bool isOn(KisKActionCollection *actions);
    /// Turns it on (in the chosen mode) or off for the window painting through \p actions
    static void setOn(KisKActionCollection *actions, bool on);

    static constexpr int maximumCopies = 24;

Q_SIGNALS:
    void changed();

private:
    KisStudioSymmetry();
};

#endif // KISSTUDIOSYMMETRY_H
