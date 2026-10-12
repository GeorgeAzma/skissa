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
 * Frosted glass for what lies over the canvas: the artwork behind, blurred and tinted.
 * The OpenGL canvas draws it on the GPU (KisOpenGLGlass); elsewhere it is made here on the CPU.
 * Where there is no canvas behind, it is the plain panel color.
 */
class KRITAUI_EXPORT KisStudioGlass
{
public:
    /// Blur sigma over small glass (a pill), widget pixels, as frosted as the setting makes it
    static qreal blurSigma();
    /// Larger glass blurs up to this many times wider, so tall panels show no shapes
    static constexpr qreal maxBlurScale = 2.0;
    /// Blur multiplier for glass of \p size: grows with the square root of its area
    static qreal blurScale(const QSizeF &size);
    /// Saturated colors weigh up to 1 + salience times a gray in the (Oklab) blur
    static constexpr qreal salience = 1.0;
    /// Power mean of Oklab lightness in the blur: 1 averages as seen, ~3 as light
    static constexpr qreal lightnessPower = 1.5;
    /// Tint strength over a dark and over a bright canvas
    static qreal tintOverDark();
    static qreal tintOverBright();

    /// How frosted the glass is: 0.25 nearly clear to 2, 1 as Krita comes
    static qreal blur();
    static void setBlur(qreal blur);
    /// How strongly the panel color covers what is behind: 0 none to 1 solid, 0.7 as Krita comes
    static qreal opacity();
    static void setOpacity(qreal opacity);
    /**
     * The frost's color is the mean of Oklab's a and b, where hues that disagree cancel: of the chroma lost
     * so, this share is restored as far as they agree (never more than the colors behind have)
     */
    static constexpr qreal restore = 0.5;
    /// Share of the tint that veils what is behind (its chroma is otherwise kept)
    static constexpr qreal veil = 0.3;

    /// How much the edge bevel bends the canvas, 0 (a plain line) to 1
    static qreal refraction();
    static void setRefraction(qreal refraction);

    /// The edge bevel, widget pixels (all zero for a line)
    struct Bevel {
        qreal width {0};     ///< from the edge in to the flat face
        qreal thickness {0}; ///< of the pane at the flat face
        qreal lift {0};      ///< gap between the pane and the canvas
        qreal polish {0};    ///< 0 frosted, 1 clear at the rim
        qreal clear {0};     ///< share of the bevel that is fully clear
        qreal abbeNumber {40}; ///< dispersion: lower spreads colors more
    };
    static Bevel bevel();

    /// Whether the canvas around the image is lit by the image (only when the border color is black)
    static bool ambientSurround();
    static constexpr qreal ambientSigma = 110; ///< widget pixels
    static constexpr qreal ambientStrength = 0.5;
    /// Crown glass, at the sodium d line
    static constexpr qreal refractiveIndex = 1.52;

    /// Strokes \p shape as \p widget's border, except where the canvas draws a bevel instead
    static void paintBorder(QPainter &painter, QWidget *widget, const QPainterPath &shape, const QColor &color, qreal width = 1);

    /**
     * Paints \p widget's background in \p rect with squircle corners of \p radius and returns the shape.
     * Where a glass window covers \p widget, the painter is left clipped so nothing shows through it.
     */
    static QPainterPath paint(QPainter &painter, QWidget *widget, const QRectF &rect, qreal radius);

    /// A soft shadow around \p panel, outside it only: \p margin wide, \p darkness alpha, \p drop lower
    static void paintShadow(QPainter &painter, const QRectF &panel, qreal radius, int margin, int darkness, int drop);

    /// A glass panel over a canvas, in the canvas widget's coordinates
    struct Surface {
        QRectF rect;
        qreal radius;
        QColor tint;
        qreal opacity;
    };

    /// The glass panels over \p canvasWidget, for it to draw
    static QVector<Surface> surfacesOver(const QWidget *canvasWidget);

    /// Whether \p canvasWidget draws the glass of the panels over it (then they paint only their content)
    static void setDrawnByCanvas(QWidget *canvasWidget, bool on);

    /// \p widget (a glass panel, or what holds one) changed how it shows, e.g. faded: the canvas redraws
    static void changed(QWidget *widget);

    /**
     * Makes \p widget's surfaces see-through, its fields veils, for a glass background; or (\p on false)
     * restores them. Follows theme changes while on. Its menus and dropdowns are left alone.
     */
    static void setSeeThrough(QWidget *widget, bool on);

    /// Menus, tooltips and dropdowns become glass windows from now on
    static void installMenuGlass();

    /// \p panel, in the canvas' window, is over the other overlays there: it hides what it covers
    static void setOverOverlays(QWidget *panel);

    /// The setting; off, backgrounds are the plain panel color
    static bool isEnabled();
    static void setEnabled(bool on);

    /**
     * Whether windows of their own can be see-through: not on X11 without a compositor.
     * KRITA_STUDIO_SOLID_WINDOWS=1 forces solid windows.
     */
    static bool seeThroughWindows();

    /**
     * Window type of Studio popups over the main window: Qt::Tool on Windows (a see-through Qt::Popup
     * renders black there), Qt::Popup elsewhere (Wayland only lets popups be placed).
     * KRITA_STUDIO_POPUP_WINDOWS=1 uses popups on Windows too.
     */
    static Qt::WindowType popupWindowType();
};

#endif // KISSTUDIOGLASS_H
