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
    static constexpr qreal blurSigma = 18; ///< in widget pixels, over small glass (a pill)
    /// Over larger glass the blur is wider, up to this many times: over a tall panel what is behind would show its shapes
    static constexpr qreal maxBlurScale = 2.0;
    /**
     * How many times blurSigma the blur over glass of \p size (widget pixels)
     * is: as wide as the panel is large (the square root of its area), from a
     * pill's to maxBlurScale times
     */
    static qreal blurScale(const QSizeF &size);
    /**
     * The blur averages what is behind as it is seen (in Oklab), a saturated
     * color counting up to 1 + salience times as much as a gray: a stroke of
     * color stands out of a canvas.
     */
    static constexpr qreal salience = 1.0;
    /**
     * Lightness is averaged as a power mean of Oklab's lightness: 1 averages
     * it as seen (darks keep their depth, but small highlights vanish), about
     * 3 as light (as frosted glass does: its scattered light lifts the darks
     * into a milky haze); between, darks stay dark and highlights still glow
     */
    static constexpr qreal lightnessPower = 1.5;
    /**
     * The frost's color is the mean in Oklab: mixed as light, a stroke of
     * color on white would keep only half of its chroma, and orange and blue
     * would make pink. Hues that disagree cancel out: of the chroma lost so,
     * this share is restored as far as they agree (never more than the
     * colors behind have, on average).
     */
    static constexpr qreal restore = 0.5;
    /// The tint, over a dark and over a bright canvas: text on it reads as well over either
    static constexpr qreal tintOverDark = 0.56;
    static constexpr qreal tintOverBright = 0.84;
    /**
     * The tint sets how light the glass is, for text on it to read; what is
     * behind keeps its chroma (Oklab), as colorful as on the canvas, and only
     * the light the glass scatters veils it, this share of the tint (all of
     * it where the glass is lighter than what is behind: lightening is all
     * scattered light). Mixing with a gray instead muddies colors; keeping
     * the saturation (chroma over lightness) drains them, as dark glass over
     * a light canvas is a third as light.
     */
    static constexpr qreal veil = 0.3;

    /**
     * The glass over the canvas ends in a polished bevel, the face of the
     * pane rounding down to its edge, that bends what is behind as glass
     * does (Snell's law, with the dispersion of the glass), reflects the
     * canvas around it where it is steep (Fresnel) and, steepest, keeps the
     * light inside it (total internal reflection).
     *
     * Frosted: the rim is clear only at the very edge and shows by what it
     * bends, no gloss. Liquid: a thicker pane, clear far into the bevel,
     * which bends the canvas more and spreads its colors further, and its
     * edge catches the light (from the top left, and faintly opposite).
     */
    enum class Style {
        Frosted = 0,
        Liquid = 1,
    };
    static Style style();
    static void setStyle(Style style);

    /// How much the bevel bends the canvas, 0 to 1; at 0 there is no bevel, the glass ends in a line
    static qreal refraction();
    static void setRefraction(qreal refraction);

    /// The bevel of the edge (all zero for a line), in widget pixels
    struct Bevel {
        qreal width {0};     ///< from the edge in to where the face is flat
        qreal thickness {0}; ///< of the pane where its face is flat
        qreal lift {0};      ///< how far the pane is over the canvas
        /// How clear the rim is, 0 to 1: polished, it shows the canvas itself bent, frosted the blur
        qreal polish {0};
        /// How far into the bevel it is all clear (a share of its width), from where it turns to frost
        qreal clear {0};
        /// How bright the light the edge catches is (0, none)
        qreal gloss {0};
        /// The glass's Abbe number: the lower, the more it spreads the colors
        qreal abbeNumber {40};
    };
    static Bevel bevel();

    /**
     * What is around the image on the canvas: black lit by the image, as a
     * screen lights the wall behind it (its colors spill out around it and
     * fade with the distance, ambient light), unless a color other than
     * black is chosen for it (KisConfig::canvasBorderColor)
     */
    static bool ambientSurround();
    /// How far the ambient light reaches (a gaussian's sigma, widget pixels), and how strong it is next to the image
    static constexpr qreal ambientSigma = 110;
    static constexpr qreal ambientStrength = 0.5;
    /// The glass: crown glass, its refractive index for yellow light (the sodium d line)
    static constexpr qreal refractiveIndex = 1.52;

    /**
     * Strokes \p shape (what paint() returned) in \p color, \p width wide:
     * \p widget's border, where the canvas does not draw a bevel instead
     * (beyond the canvas, without OpenGL, with no refraction).
     */
    static void paintBorder(QPainter &painter, QWidget *widget, const QPainterPath &shape, const QColor &color, qreal width = 1);

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

    /**
     * Menus (context menus, a window's menus) of glass from now on, as the
     * panels are: see-through windows the canvas draws the glass of
     */
    static void installMenuGlass();

    /**
     * \p panel, glass in the canvas' window, is over the rest there (a
     * flyout over the panels and pills): what of them it covers is hidden,
     * as under a glass window, not seen through its glass
     */
    static void setOverOverlays(QWidget *panel);

    /// Whether glass is on (the setting); off, backgrounds are the plain panel color
    static bool isEnabled();
    static void setEnabled(bool on);

    /**
     * Whether windows of their own (menus, pickers, Find, cards, canvas
     * messages) can be see-through. Not on X11 without a compositing manager,
     * where what a window leaves unpainted is black: they are solid there,
     * square and without a shadow. KRITA_STUDIO_SOLID_WINDOWS=1 makes them
     * solid anywhere, to try that.
     */
    static bool seeThroughWindows();

    /**
     * The window type of a Studio window over the main window (a picker,
     * Find, a card). Qt::Tool on Windows, where a see-through Qt::Popup
     * renders black; elsewhere Qt::Popup, the one Wayland lets Krita place
     * (a Qt::Tool is a toplevel there, put where the compositor likes).
     * KRITA_STUDIO_POPUP_WINDOWS=1 makes them popups on Windows too, to try
     * that with solid windows.
     */
    static Qt::WindowType popupWindowType();
};

#endif // KISSTUDIOGLASS_H
