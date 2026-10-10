/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisOpenGLGlass.h"

#include <cmath>

#include <QHash>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QtMath>

#include "KisStudioGlass.h"
#include "kis_debug.h"
#include "opengl/kis_opengl.h"

namespace {

/// The most the blur reaches either way, in the small pixels it is made in
constexpr int maxTaps = 32;

/// A strip of four corners, of rect (x0, y0, x1, y1, in clip coordinates)
const char *const quadSource = R"(
uniform vec4 rect;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
}
)";

/**
 * Between the steps colors are kept as the square root of light: mixed as
 * light, squared back, and as fine in the darks as sRGB is, in eight bits.
 */
const char *const commonSource = R"(
vec3 fromSrgb(vec3 c)
{
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

vec3 toSrgb(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 light(vec4 kept)
{
    return kept.rgb * kept.rgb;
}

vec4 keep(vec3 light)
{
    return vec4(sqrt(max(light, 0.0)), 1.0);
}
)";

/// The canvas made smaller: each pixel the mean of the factor x factor it stands for
const char *const shrinkSource = R"(
uniform sampler2D source;
uniform int factor;
uniform bool decode;

void main()
{
    ivec2 last = textureSize(source, 0) - 1;
    ivec2 base = ivec2(gl_FragCoord.xy) * factor;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < factor; y++) {
        for (int x = 0; x < factor; x++) {
            vec3 c = texelFetch(source, min(base + ivec2(x, y), last), 0).rgb;
            sum += decode ? fromSrgb(c) : c;
        }
    }
    fragColor = keep(sum / float(factor * factor));
}
)";

/// Half of a gaussian: across, or down
const char *const blurSource = R"(
uniform sampler2D source;
uniform ivec2 direction;
uniform int taps;
uniform float weights[33];

vec3 at(ivec2 p)
{
    return light(texelFetch(source, clamp(p, ivec2(0), textureSize(source, 0) - 1), 0));
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 sum = at(p) * weights[0];
    for (int i = 1; i <= taps; i++) {
        sum += (at(p + direction * i) + at(p - direction * i)) * weights[i];
    }
    fragColor = keep(sum);
}
)";

/**
 * A glass panel's quad, and how strongly it is tinted: as much as what is
 * behind the whole panel needs, for what is on it to read (a bright canvas
 * more, a dark one less), the same all over it, so that what is behind
 * keeps its light and dark. Worked out at its corners, once.
 */
const char *const glassVertexSource = R"(
uniform vec4 rect;
uniform sampler2D blurred;
uniform float factor;
uniform vec4 shape;
uniform float tintLightness;
uniform vec2 tintStrength;
flat out float strength;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);

    // how bright it is behind, about as seen (the kept colors are about as light looks)
    ivec2 last = textureSize(blurred, 0) - 1;
    float sum = 0.0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            vec2 p = shape.xy + shape.zw * ((vec2(x, y) + 0.5) / 2.0 - 1.0);
            sum += dot(texelFetch(blurred, clamp(ivec2(p / factor), ivec2(0), last), 0).rgb, vec3(0.299, 0.587, 0.114));
        }
    }
    strength = mix(tintStrength.x, tintStrength.y, clamp(abs(sum / 16.0 - tintLightness), 0.0, 1.0));
}
)";

/// A glass panel: the blur scaled back up in its shape, more vibrant, tinted
const char *const glassSource = R"(
uniform sampler2D blurred;
uniform float factor;
uniform vec4 shape;
uniform vec2 corner;
uniform vec3 tint;
uniform float vibrance;
uniform float opacity;
flat in float strength;

// how far outside the shape p is, in pixels (inside, less than 0): its squircle corners
// are each a quarter of a superellipse, |x|^n + |y|^n = reach^n
float outside(vec2 p)
{
    vec2 q = abs(p - shape.xy) - (shape.zw - corner.x);
    if (q.x <= 0.0 || q.y <= 0.0) {
        return max(q.x, q.y) - corner.x;
    }
    float n = corner.y;
    vec2 a = pow(q, vec2(n));
    float f = pow(a.x + a.y, 1.0 / n);
    // the distance to f = reach, about: f over how fast it grows
    vec2 gradient = pow(q / f, vec2(n - 1.0));
    return (f - corner.x) / length(gradient);
}

vec3 at(ivec2 p)
{
    return light(texelFetch(blurred, clamp(p, ivec2(0), textureSize(blurred, 0) - 1), 0));
}

void main()
{
    float coverage = clamp(0.5 - outside(gl_FragCoord.xy), 0.0, 1.0) * opacity;
    if (coverage <= 0.0) {
        discard;
    }
    // bilinear, in light
    vec2 t = gl_FragCoord.xy / factor - 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    vec3 c = mix(mix(at(i), at(i + ivec2(1, 0)), f.x), mix(at(i + ivec2(0, 1)), at(i + ivec2(1, 1)), f.x), f.y);

    // vibrance: away from the gray of the same luminance, as light through glass looks
    float luminance = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c = max(vec3(0.0), luminance + (c - luminance) * vibrance);
    fragColor = vec4(mix(toSrgb(c), tint, strength), coverage);
}
)";

QByteArray header()
{
    if (KisOpenGL::hasOpenGLES()) {
        return "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
    }
#ifdef Q_OS_MACOS
    return "#version 150 core\n";
#else
    return "#version 130\n";
#endif
}

QOpenGLShaderProgram *compile(const char *vertex, const char *fragment)
{
    QScopedPointer<QOpenGLShaderProgram> result(new QOpenGLShaderProgram());
    if (!result->addShaderFromSourceCode(QOpenGLShader::Vertex, header() + commonSource + vertex)
        || !result->addShaderFromSourceCode(QOpenGLShader::Fragment, header() + "out vec4 fragColor;\n" + commonSource + fragment)
        || !result->link()) {
        warnOpenGL << "Glass is drawn without OpenGL:" << result->log();
        return nullptr;
    }
    return result.take();
}

/// A program and where its uniforms are
struct Program {
    QScopedPointer<QOpenGLShaderProgram> program;
    QHash<QByteArray, int> locations;

    bool make(const char *vertex, const char *fragment, const QList<QByteArray> &uniforms)
    {
        program.reset(compile(vertex, fragment));
        if (!program) return false;
        for (const QByteArray &uniform : uniforms) locations.insert(uniform, program->uniformLocation(uniform.constData()));
        return true;
    }

    int operator[](const char *uniform) const { return locations.value(QByteArray(uniform), -1); }
    QOpenGLShaderProgram *operator->() const { return program.data(); }
};

} // namespace

struct KisOpenGLGlass::Private
{
    Program shrink;
    Program blur;
    Program glass;
    QOpenGLVertexArrayObject vao;
    /// The canvas made small; blurred across; then down
    QScopedPointer<QOpenGLFramebufferObject> small;
    QScopedPointer<QOpenGLFramebufferObject> across;
    QScopedPointer<QOpenGLFramebufferObject> blurred;
    bool valid {false};

    void ensureSize(const QSize &size)
    {
        if (small && small->size() == size) {
            return;
        }
        small.reset(new QOpenGLFramebufferObject(size));
        across.reset(new QOpenGLFramebufferObject(size));
        blurred.reset(new QOpenGLFramebufferObject(size));
    }
};

KisOpenGLGlass::KisOpenGLGlass()
    : d(new Private())
{
}

KisOpenGLGlass::~KisOpenGLGlass() = default;

bool KisOpenGLGlass::initialize()
{
    QOpenGLContext *context = QOpenGLContext::currentContext();
    // texelFetch, gl_VertexID, flat: GLSL 1.30, or ES 3.00
    if (!context || context->format().majorVersion() < 3) {
        return false;
    }
    d->valid = d->shrink.make(quadSource, shrinkSource, {"rect", "source", "factor", "decode"})
        && d->blur.make(quadSource, blurSource, {"rect", "source", "direction", "taps", "weights"})
        && d->glass.make(glassVertexSource, glassSource,
                         {"rect", "blurred", "factor", "shape", "corner", "tint", "tintLightness", "tintStrength", "vibrance", "opacity"});
    if (d->valid && KisOpenGL::supportsVAO()) {
        d->vao.create();
    }
    return d->valid;
}

bool KisOpenGLGlass::isValid() const
{
    return d->valid;
}

qreal KisOpenGLGlass::reach()
{
    return 3 * KisStudioGlass::blurSigma;
}

void KisOpenGLGlass::render(uint canvas, bool linearCanvas, const QSize &viewport, qreal pixelRatio,
                            const QVector<Surface> &surfaces, const QRect &clip)
{
    if (!d->valid || surfaces.isEmpty() || viewport.isEmpty()) {
        return;
    }
    const QRect whole(QPoint(), viewport);
    QRect drawn;
    for (const Surface &surface : surfaces) {
        drawn |= surface.rect.toAlignedRect();
    }
    drawn &= clip.isEmpty() ? whole : clip & whole;
    if (drawn.isEmpty()) {
        return;
    }

    // made a few times smaller, the blur as wide in widget pixels whatever the screen's scale
    const int factor = qMax(1, qRound(4 * pixelRatio));
    const qreal sigma = KisStudioGlass::blurSigma * pixelRatio / factor;
    const int taps = qBound(1, qCeil(3 * sigma), maxTaps);
    float weights[maxTaps + 1] = {};
    float sum = 0;
    for (int i = 0; i <= taps; i++) {
        weights[i] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
        sum += i ? 2 * weights[i] : weights[i];
    }
    for (int i = 0; i <= taps; i++) weights[i] /= sum;

    const QSize smallSize((viewport.width() + factor - 1) / factor, (viewport.height() + factor - 1) / factor);
    d->ensureSize(smallSize);
    const QRect smallWhole(QPoint(), smallSize);

    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    GLint target = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    if (d->vao.isCreated()) d->vao.bind();
    f->glDisable(GL_BLEND);
    f->glEnable(GL_SCISSOR_TEST);
    f->glActiveTexture(GL_TEXTURE0);
    f->glViewport(0, 0, smallSize.width(), smallSize.height());
    auto scissor = [f](const QRect &r) { f->glScissor(r.x(), r.y(), r.width(), r.height()); };

    d->shrink->bind();
    f->glUniform4f(d->shrink["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->shrink["source"], 0);
    f->glUniform1i(d->shrink["factor"], factor);
    f->glUniform1i(d->shrink["decode"], !linearCanvas);
    d->blur->bind();
    f->glUniform4f(d->blur["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->blur["source"], 0);
    f->glUniform1i(d->blur["taps"], taps);
    f->glUniform1fv(d->blur["weights"], maxTaps + 1, weights);

    // blurred under each panel only, whole (how bright it is behind all of it sets its tint)
    for (const Surface &surface : surfaces) {
        const QRect r = surface.rect.toAlignedRect() & whole;
        if (r.isEmpty()) continue;
        // its small pixels (one more for scaling back up), and as far as the blur takes in around them
        const QRect area = QRect(QPoint(r.left() / factor, r.top() / factor), QPoint(r.right() / factor + 1, r.bottom() / factor + 1)) & smallWhole;
        const QRect around = area.adjusted(-taps, -taps, taps, taps) & smallWhole;
        const QRect down = area.adjusted(0, -taps, 0, taps) & smallWhole;

        d->shrink->bind();
        d->small->bind();
        scissor(around);
        f->glBindTexture(GL_TEXTURE_2D, canvas);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        d->blur->bind();
        d->across->bind();
        scissor(down);
        f->glUniform2i(d->blur["direction"], 1, 0);
        f->glBindTexture(GL_TEXTURE_2D, d->small->texture());
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        d->blurred->bind();
        scissor(area);
        f->glUniform2i(d->blur["direction"], 0, 1);
        f->glBindTexture(GL_TEXTURE_2D, d->across->texture());
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    // the glass, over what is drawn
    f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(target));
    f->glViewport(0, 0, viewport.width(), viewport.height());
    scissor(drawn);
    f->glEnable(GL_BLEND);
    f->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    d->glass->bind();
    f->glUniform1i(d->glass["blurred"], 0);
    f->glUniform1f(d->glass["factor"], factor);
    f->glUniform1f(d->glass["vibrance"], KisStudioGlass::vibrance);
    f->glUniform2f(d->glass["tintStrength"], KisStudioGlass::tintOverDark, KisStudioGlass::tintOverBright);
    f->glBindTexture(GL_TEXTURE_2D, d->blurred->texture());
    for (const Surface &surface : surfaces) {
        const QRectF r = surface.rect;
        const QRectF quad = r.adjusted(-1, -1, 1, 1);
        f->glUniform4f(d->glass["rect"], quad.left() / viewport.width() * 2 - 1, quad.top() / viewport.height() * 2 - 1,
                       quad.right() / viewport.width() * 2 - 1, quad.bottom() / viewport.height() * 2 - 1);
        f->glUniform4f(d->glass["shape"], r.center().x(), r.center().y(), r.width() / 2, r.height() / 2);
        f->glUniform2f(d->glass["corner"], surface.reach, surface.exponent);
        f->glUniform3f(d->glass["tint"], surface.tint.redF(), surface.tint.greenF(), surface.tint.blueF());
        f->glUniform1f(d->glass["tintLightness"], qGray(surface.tint.rgb()) / 255.0f);
        f->glUniform1f(d->glass["opacity"], surface.opacity);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    d->glass->release();
    if (d->vao.isCreated()) d->vao.release();
}
