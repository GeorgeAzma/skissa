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
 * Frosted glass backgrounds for panels over the canvas: the artwork behind, blurred and tinted.
 * The OpenGL canvas draws it on the GPU (KisOpenGLGlass); otherwise it is rendered here on the CPU.
 * Where there is no canvas behind, the plain panel color is used.
 */
class KRITAUI_EXPORT KisStudioGlass
{
public:
    /// Blur sigma for small glass such as a pill, in widget pixels, scaled by the blur setting.
    static qreal blurSigma();
    /// Larger glass blurs up to this many times wider, so no shapes show through tall panels.
    static constexpr qreal maxBlurScale = 2.0;
    /// Blur multiplier for glass of \p size; grows with the square root of its area.
    static qreal blurScale(const QSizeF &size);
    /// In the Oklab blur, saturated colors weigh up to 1 + salience times as much as gray.
    static constexpr qreal salience = 1.0;
    /// Power mean exponent for Oklab lightness in the blur: 1 averages perceptually, ~3 like light.
    static constexpr qreal lightnessPower = 1.5;
    /// Tint strength over a dark and over a bright canvas.
    static qreal tintOverDark();
    static qreal tintOverBright();

    /// How frosted the glass is in the middle of a panel, from 0 (clear) to 2. Defaults to 1.
    static qreal blur();
    static void setBlur(qreal blur);
    /// How strongly the panel color covers what is behind, from 0 to 1 (solid). Defaults to 0.7.
    static qreal opacity();
    static void setOpacity(qreal opacity);
    /// How bright the line of light along a panel's rim is, from 0 (none) to 2. Defaults to 1.
    static qreal rimLight();
    static void setRimLight(qreal strength);
    /**
     * Averaging Oklab a and b lets opposing hues cancel out. This share of the lost chroma is restored,
     * never exceeding the chroma of the colors behind.
     */
    static constexpr qreal restore = 0.5;
    /// Share of the tint that veils the chroma of what is behind.
    static constexpr qreal veil = 0.3;

    /// The refracting edge, in widget pixels.
    struct Rim {
        qreal band {20}; ///< of the band along the edge that bends what is behind
        /// How far outside the panel its edge sees: the bend looks inward, so only the sampling's margin
        qreal reach() const { return 2; }
    };
    static Rim rim() { return {}; }

    /// Whether the image casts an ambient glow on the canvas around it (only with a black border color).
    static bool ambientSurround();
    static constexpr qreal ambientSigma = 110; ///< widget pixels
    static constexpr qreal ambientStrength = 0.5;
    /// The image's luminance where its glow is compressed to half (linear)
    static constexpr qreal ambientKnee = 0.25;
    /// The least light around the image (linear, where all of it is near): a dark image's edge still shows
    static constexpr qreal ambientBacklight = 0.02;
    /// Crown glass at the sodium d line.
    static constexpr qreal refractiveIndex = 1.52;

    /// Strokes \p shape as \p widget's border, except where the canvas draws a bevel instead.
    static void paintBorder(QPainter &painter, QWidget *widget, const QPainterPath &shape, const QColor &color, qreal width = 1);

    /**
     * Paints \p widget's background in \p rect with squircle corners of \p radius and returns the shape.
     * Where a glass window covers \p widget, the painter is left clipped so nothing shows through it.
     */
    static QPainterPath paint(QPainter &painter, QWidget *widget, const QRectF &rect, qreal radius);

    /// Paints a soft shadow outside \p panel, \p margin wide, with \p darkness alpha, offset down by \p drop.
    static void paintShadow(QPainter &painter, const QRectF &panel, qreal radius, int margin, int darkness, int drop);

    /// A glass panel over a canvas, in the canvas widget's coordinates.
    struct Surface {
        QRectF rect;
        qreal radius;
        QColor tint;
        qreal opacity;
    };

    /// The glass panels \p canvasWidget has to draw.
    static QVector<Surface> surfacesOver(const QWidget *canvasWidget);
    /// Reports that \p canvasWidget drew the glass in \p painted (empty means everywhere).
    static void drewGlassOver(QWidget *canvasWidget, const QRect &painted);

    /// Sets whether \p canvasWidget draws the glass of the panels over it, so they paint only their content.
    static void setDrawnByCanvas(QWidget *canvasWidget, bool on);

    /// Call when \p widget (a glass panel or its container) changes appearance, e.g. fades, so the canvas redraws.
    static void changed(QWidget *widget);

    /**
     * Makes \p widget's background see-through and its fields translucent veils for a glass background,
     * or restores them. Follows theme changes while on. Menus and dropdowns are not affected.
     */
    static void setSeeThrough(QWidget *widget, bool on);

    /// Lets \p window be dragged by its background, labels and plain frames
    static void setMovableByBackground(QWidget *window);

    /// Makes menus, tooltips and dropdowns created from now on glass windows.
    static void installMenuGlass();

    /// Marks \p panel, in the canvas' window, as above the other overlays there, hiding what it covers.
    static void setOverOverlays(QWidget *panel);

    /// The glass setting. When off, backgrounds use the plain panel color.
    static bool isEnabled();
    static void setEnabled(bool on);

    /**
     * Whether top-level windows can be see-through; not on X11 without a compositor.
     * KRITA_STUDIO_SOLID_WINDOWS=1 forces solid windows.
     */
    static bool seeThroughWindows();
    /// Room around a popup window for its drop shadow: \p margin when it is see-through, none when opaque
    static int shadowMargin(const QWidget *window, int margin);

    /**
     * Window type of Studio popups over the main window: Qt::Tool on Windows, where a translucent Qt::Popup
     * renders black, and Qt::Popup elsewhere, since Wayland only lets popups be positioned.
     * KRITA_STUDIO_POPUP_WINDOWS=1 uses Qt::Popup on Windows too.
     */
    static Qt::WindowType popupWindowType();
};

#endif // KISSTUDIOGLASS_H
