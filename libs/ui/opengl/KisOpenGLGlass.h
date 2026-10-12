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
 * The glass of the panels over the canvas (see KisStudioGlass), drawn on the GPU from the canvas FBO.
 * The blur reads a pyramid of levels, each half the size and twice the blur of the last, mixed in Oklab;
 * the edge is a refracting bevel (KisStudioGlass::Bevel).
 */
class KisOpenGLGlass
{
public:
    /// A glass panel, in surface device pixels (origin bottom left)
    struct Surface {
        QRectF rect;
        /// Squircle corners (see KisModernStyle::squircleCorner)
        qreal reach {0};
        qreal exponent {2};
        QColor tint;
        qreal opacity {1};
    };

    KisOpenGLGlass();
    ~KisOpenGLGlass();

    /// Compiles the shaders; false without OpenGL 3 or OpenGL ES 3
    bool initialize();
    bool isValid() const;

    /**
     * Draws \p surfaces into the bound framebuffer from the \p canvas texture (GL_RGBA16F is linear HDR,
     * other formats sRGB), within \p clip if not empty. Call it every frame, with no surfaces too: unless
     * \p canvasChanged, the last blur is reused.
     */
    void render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                const QVector<Surface> &surfaces, const QRect &clip, bool canvasChanged = true);

    /// The image's ambient light drawn into \p canvas around it; \p surfaceToImage maps to 0..1 across the image
    void renderAmbient(QOpenGLFramebufferObject *canvas, uint canvasFormat, qreal pixelRatio, const QTransform &surfaceToImage,
                       const QColor &base);

    /// How far around a panel the canvas affects its glass, in widget pixels
    static qreal reach();

private:
    struct Private;
    QScopedPointer<Private> d;
};

#endif // KISOPENGLGLASS_H
