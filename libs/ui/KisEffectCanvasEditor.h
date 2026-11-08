/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISEFFECTCANVASEDITOR_H
#define KISEFFECTCANVASEDITOR_H

#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPolygonF>
#include <QString>

/**
 * An effect's settings widget that is also edited on the image: while the effect is open on the canvas, the
 * pointer over the image and drags on it go to the widget (found with qobject_cast) instead of its first numbers.
 * Pixels are in image coordinates.
 */
class KisEffectCanvasEditor
{
public:
    virtual ~KisEffectCanvasEditor() = default;

    /// The pointer is over \p pixel; at INT_MIN, INT_MIN it left the image
    virtual void canvasHovered(const QPoint &pixel) = 0;
    /// A press on \p pixel: whether a drag follows
    virtual bool canvasPressed(const QPoint &pixel) = 0;
    /// The drag moved \p moved screen pixels from where it started and is over \p at on the image (both slower with
    /// Shift); returns the value to show
    virtual QString canvasDragged(const QPointF &moved, const QPointF &at) = 0;
    virtual void canvasReleased() = 0;

    /// The point on the image the effect is about (a lens's or a radial blur's middle), drawn as a handle there;
    /// false if it has none. Its presses place that point and its drags move it, under a crosshair.
    virtual bool canvasMiddle(QPointF *at) const
    {
        Q_UNUSED(at);
        return false;
    }

    /**
     * A sample of what the effect does, drawn by its middle: a path on the image (in its pixels) ending in a knob
     * that drags; \p pixelsPerScreen of the image's pixels make a screen pixel, so the sample keeps its size on
     * screen. False if it has none.
     */
    virtual bool canvasStreak(qreal pixelsPerScreen, QPolygonF *path) const
    {
        Q_UNUSED(pixelsPerScreen);
        Q_UNUSED(path);
        return false;
    }
    /// The sample's knob was dragged to \p end (in the image's pixels); returns the value to show
    virtual QString canvasStreakDragged(const QPointF &end)
    {
        Q_UNUSED(end);
        return QString();
    }
};

#define KisEffectCanvasEditor_iid "org.skissa.KisEffectCanvasEditor"
Q_DECLARE_INTERFACE(KisEffectCanvasEditor, KisEffectCanvasEditor_iid)

#endif
