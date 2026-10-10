/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef KISOPENGLGLASS_H
#define KISOPENGLGLASS_H

#include <QColor>
#include <QRectF>
#include <QScopedPointer>
#include <QSize>
#include <QVector>

/**
 * The frosted glass of what lies over the canvas (see KisStudioGlass),
 * drawn by the canvas itself, on the GPU: what it has drawn of the image,
 * blurred in linear light, tinted, in the shape of each glass panel. It
 * follows the canvas frame by frame, and costs the CPU nothing.
 *
 * The blur: the canvas a few times smaller (each pixel the mean of the
 * ones it stands for), then a gaussian across and down, then scaled back
 * up where the glass is. Colors are mixed as light, saturated ones counting
 * more (see KisStudioGlass::salience).
 *
 * The edge (see KisStudioGlass::Edge): a bevel, traced as glass, that bends
 * the canvas behind it, sharp, and reflects the room.
 */
class KisOpenGLGlass
{
public:
    /// A glass panel, in device pixels of the surface (origin bottom left)
    struct Surface {
        QRectF rect;
        /// Its squircle corners (see KisModernStyle::squircleCorner)
        qreal reach {0};
        qreal exponent {2};
        QColor tint;
        qreal opacity {1};
    };

    KisOpenGLGlass();
    ~KisOpenGLGlass();

    /// Compiles what it draws with; false if it cannot draw here (no OpenGL 3 or OpenGL ES 3)
    bool initialize();
    bool isValid() const;

    /**
     * Draws \p surfaces into the framebuffer bound, \p viewport device
     * pixels, from \p canvas (a texture of the same size, the canvas as
     * drawn, of \p canvasFormat: GL_RGBA16F is linear light, HDR; others
     * sRGB), only in \p clip (surface pixels) if it is not empty. \p
     * pixelRatio: device pixels to a widget's.
     */
    void render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                const QVector<Surface> &surfaces, const QRect &clip);

    /// How far around a glass panel the canvas shows in its blur, in widget pixels
    static qreal reach();

private:
    struct Private;
    QScopedPointer<Private> d;
};

#endif // KISOPENGLGLASS_H
