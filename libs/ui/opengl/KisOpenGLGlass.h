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

class QOpenGLFramebufferObject;
class QTransform;

/**
 * The frosted glass of what lies over the canvas (see KisStudioGlass),
 * drawn by the canvas itself, on the GPU: what it has drawn of the image,
 * blurred in linear light, tinted, in the shape of each glass panel. It
 * follows the canvas frame by frame, and costs the CPU nothing.
 *
 * The blur: levels of the canvas, each half as large as the one before and
 * twice as blurred, from which a blur of any width is read (the two levels
 * about as wide, mixed), smooth however magnified. Colors are mixed as they
 * are seen (Oklab), saturated ones counting more (see
 * KisStudioGlass::salience).
 *
 * The edge (see KisStudioGlass::Bevel): a bevel, traced as glass, that bends
 * the canvas behind it, clear at the rim and as blurred as the frost where
 * it meets the face, the blur growing between without a seam of any order.
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
     * pixelRatio: device pixels to a widget's. \p canvasChanged: what the
     * canvas shows changed since the last time; if not, and the surfaces are
     * where they were, the blur made then is used again.
     */
    void render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                const QVector<Surface> &surfaces, const QRect &clip, bool canvasChanged = true);

    /**
     * The ambient light of the image (see KisStudioGlass::ambientSurround) drawn
     * into \p canvas (the canvas as drawn, \p canvasFormat as for render())
     * around the image: \p surfaceToImage takes surface pixels to the image's
     * own coordinates, 0 to 1 across it; \p base the color around it.
     */
    void renderAmbient(QOpenGLFramebufferObject *canvas, uint canvasFormat, qreal pixelRatio, const QTransform &surfaceToImage,
                       const QColor &base);

    /// How far around a glass panel the canvas shows in its blur, in widget pixels
    static qreal reach();

private:
    struct Private;
    QScopedPointer<Private> d;
};

#endif // KISOPENGLGLASS_H
