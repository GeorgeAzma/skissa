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
 * GPU renderer for the glass panels over the canvas (see KisStudioGlass). It downsamples the canvas
 * FBO, blurs it in Oklab under each panel, tints it and draws a refracting edge bevel.
 */
class KisOpenGLGlass
{
public:
    /// A glass panel in surface device pixels, origin at the bottom left.
    struct Surface {
        QRectF rect;
        /// Squircle corner shape, see KisModernStyle::squircleCorner().
        qreal reach {0};
        qreal exponent {2};
        QColor tint;
        qreal opacity {1};
    };

    KisOpenGLGlass();
    ~KisOpenGLGlass();

    /// Compiles the shaders, shared by all canvases. Returns false without OpenGL 3 or OpenGL ES 3.
    bool initialize();
    bool isValid() const;

    /**
     * Draws \p surfaces into the bound framebuffer from the \p canvas texture (GL_RGBA16F is linear HDR,
     * other formats sRGB), limited to \p clip if it is not empty. Call it every frame, even with no
     * surfaces; the previous blur is reused unless \p canvasChanged.
     */
    void render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                const QVector<Surface> &surfaces, const QRect &clip, bool canvasChanged = true);

    /// Draws the image's ambient glow into \p canvas around it; \p surfaceToImage maps to 0..1 across the image.
    void renderAmbient(QOpenGLFramebufferObject *canvas, uint canvasFormat, qreal pixelRatio, const QTransform &surfaceToImage,
                       const QColor &base);

    /// How far around a panel the canvas affects its glass, in widget pixels.
    static qreal reach();

private:
    struct Private;
    QScopedPointer<Private> d;
};

#endif // KISOPENGLGLASS_H
