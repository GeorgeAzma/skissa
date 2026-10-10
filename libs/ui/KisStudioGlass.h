/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISSTUDIOGLASS_H
#define KISSTUDIOGLASS_H

#include <QColor>
#include <QPainterPath>
#include <QRectF>
#include <QVector>

#include "kritaui_export.h"

class QPainter;
class QWidget;

/**
 * Frosted glass for what lies over the canvas (the tool rail, the zoom
 * pill, panels opened over it, pickers, cards, messages): the artwork
 * behind, blurred, under a tint, so that what is covered still shows its
 * colors and the canvas is not cut off by solid blocks.
 *
 * The OpenGL canvas draws it itself, frame by frame, on the GPU (see
 * KisOpenGLGlass): what it shows of the image, blurred in linear light, as
 * light mixes: a bright stroke glows, it does not turn into grey mud. Over
 * another canvas it is made here, from the image drawn as the canvas shows
 * it, in the background, following the canvas about every frame.
 *
 * Where there is no canvas behind (over a panel, or with no image open)
 * it is the plain panel color, as before.
 */
class KRITAUI_EXPORT KisStudioGlass
{
public:
    /// How the glass looks, drawn here or by the canvas alike: soft enough that nothing in it reads as a shape
    static constexpr qreal blurSigma = 18; ///< in widget pixels
    /// A little more saturated, as light through glass looks
    static constexpr qreal vibrance = 1.18;
    /// The tint, over a dark and over a bright canvas: text on it reads as well over either
    static constexpr qreal tintOverDark = 0.56;
    static constexpr qreal tintOverBright = 0.84;

    /**
     * Paints \p widget's background, \p rect (widget coordinates) with
     * squircle corners of \p radius: glass over the canvas, the panel color
     * elsewhere. Returns the shape, for a border. Where a glass window over
     * \p widget covers it, the painter is left clipped: nothing painted
     * after shows through that window's glass either.
     */
    static QPainterPath paint(QPainter &painter, QWidget *widget, const QRectF &rect, qreal radius);

    /**
     * A soft shadow around \p panel (squircle corners of \p radius): \p
     * margin wide, as dark as \p darkness (alpha) next to the panel, \p drop
     * lower than it. Only outside the panel: the glass shows what is under it.
     */
    static void paintShadow(QPainter &painter, const QRectF &panel, qreal radius, int margin, int darkness, int drop);

    /// A glass panel over a canvas, in the canvas widget's coordinates
    struct Surface {
        QRectF rect;
        qreal radius;
        QColor tint;
        qreal opacity;
    };

    /// The glass panels shown over \p canvasWidget, for it to draw their glass itself
    static QVector<Surface> surfacesOver(const QWidget *canvasWidget);

    /**
     * \p canvasWidget draws the glass of the panels over it itself (on the
     * GPU, frame by frame), from now on; or, \p on false, no longer. The
     * panels then paint only what is on the glass.
     */
    static void setDrawnByCanvas(QWidget *canvasWidget, bool on);

    /// \p widget (a glass panel, or what holds one) is shown otherwise, e.g. faded: the canvas draws its glass again
    static void changed(QWidget *widget);

    /**
     * Makes \p widget's surfaces see-through (and its fields a darker
     * glass), for it to be shown on a glass background; or (\p on false) as
     * it was. Its menus and dropdowns, windows of their own, stay as they are.
     */
    static void setSeeThrough(QWidget *widget, bool on);

    /// The color a glass background is tinted with: what text is read against
    static QColor tint(const QWidget *widget);

    /// Whether glass is on (the setting); off, backgrounds are the plain panel color
    static bool isEnabled();
    static void setEnabled(bool on);
};

#endif // KISSTUDIOGLASS_H
