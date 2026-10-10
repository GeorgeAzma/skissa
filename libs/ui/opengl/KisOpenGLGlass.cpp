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
#include <QVector3D>
#include <QtMath>

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2 0x8059
#endif

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
 * The blur weighs colors (see KisStudioGlass::salience): kept is the light
 * times its weight, and the weight in alpha, both over the most a weight is,
 * so that a mean is the one over the other.
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

/// Light and its weight, as kept
vec4 weighted(vec4 kept)
{
    return vec4(kept.rgb * kept.rgb, kept.a);
}

vec4 keep(vec4 weighted)
{
    return vec4(sqrt(max(weighted.rgb, 0.0)), weighted.a);
}

/// Oklab (Bjoern Ottosson, 2020): lightness, and the two axes of color, as differences are seen
vec3 toOklab(vec3 c)
{
    vec3 lms = vec3(0.4122214708 * c.r + 0.5363325363 * c.g + 0.0514459929 * c.b,
                    0.2119034982 * c.r + 0.6806995451 * c.g + 0.1073969566 * c.b,
                    0.0883024619 * c.r + 0.2817188376 * c.g + 0.6299787005 * c.b);
    lms = pow(max(lms, 0.0), vec3(1.0 / 3.0));
    return vec3(0.2104542553 * lms.x + 0.7936177850 * lms.y - 0.0040720468 * lms.z,
                1.9779984951 * lms.x - 2.4285922050 * lms.y + 0.4505937099 * lms.z,
                0.0259040371 * lms.x + 0.7827717662 * lms.y - 0.8086757660 * lms.z);
}

vec3 fromOklab(vec3 c)
{
    vec3 lms = vec3(c.x + 0.3963377774 * c.y + 0.2158037573 * c.z,
                    c.x - 0.1055613458 * c.y - 0.0638541728 * c.z,
                    c.x - 0.0894841775 * c.y - 1.2914855480 * c.z);
    lms = lms * lms * lms;
    return vec3(4.0767416621 * lms.x - 3.3077115913 * lms.y + 0.2309699292 * lms.z,
                -1.2684380046 * lms.x + 2.6097574011 * lms.y - 0.3413193965 * lms.z,
                -0.0041960863 * lms.x - 0.7034186147 * lms.y + 1.7076147010 * lms.z);
}
)";

/**
 * The canvas made smaller: each pixel the mean of the factor x factor it
 * stands for, a saturated color counting more (its chroma in Oklab is about
 * 0.32 at most, in sRGB). Of light; or (statistics) of Oklab's two axes of
 * color, a and b (half added, to be kept as they are), and of chroma: the
 * mean color as seen, and how far its hues agree.
 */
const char *const shrinkSource = R"(
uniform sampler2D source;
uniform int factor;
uniform bool decode;
uniform float salience;
uniform bool statistics;

void main()
{
    ivec2 last = textureSize(source, 0) - 1;
    ivec2 base = ivec2(gl_FragCoord.xy) * factor;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < factor; y++) {
        for (int x = 0; x < factor; x++) {
            vec3 c = texelFetch(source, min(base + ivec2(x, y), last), 0).rgb;
            c = clamp(decode ? fromSrgb(c) : c, 0.0, 1.0);
            vec3 lab = toOklab(c);
            float chroma = length(lab.yz);
            float weight = 1.0 + salience * chroma;
            sum += statistics ? vec4(vec3(lab.yz + 0.5, chroma) * weight, weight) : vec4(c * weight, weight);
        }
    }
    fragColor = keep(sum / (float(factor * factor) * (1.0 + salience * 0.33)));
}
)";

/// Half of a gaussian: across, or down
const char *const blurSource = R"(
uniform sampler2D source;
uniform ivec2 direction;
uniform int taps;
uniform float weights[33];

vec4 at(ivec2 p)
{
    return weighted(texelFetch(source, clamp(p, ivec2(0), textureSize(source, 0) - 1), 0));
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec4 sum = at(p) * weights[0];
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
uniform float spacing;
flat out float strength;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);

    // how bright it is behind, about as seen (the root of light is about as light looks): a grid
    // as fine as the blur is wide (12 x 12 at most), so that the tint does not flicker as the
    // canvas slides under it
    ivec2 last = textureSize(blurred, 0) - 1;
    ivec2 count = clamp(ivec2(ceil(2.0 * shape.zw / spacing)), ivec2(1), ivec2(12));
    float sum = 0.0;
    for (int y = 0; y < count.y; y++) {
        for (int x = 0; x < count.x; x++) {
            vec2 p = shape.xy + shape.zw * (2.0 * (vec2(x, y) + 0.5) / vec2(count) - 1.0);
            vec4 kept = texelFetch(blurred, clamp(ivec2(p / factor), ivec2(0), last), 0);
            sum += dot(kept.rgb / sqrt(max(kept.a, 1e-4)), vec3(0.299, 0.587, 0.114));
        }
    }
    strength = mix(tintStrength.x, tintStrength.y, clamp(abs(sum / float(count.x * count.y) - tintLightness), 0.0, 1.0));
}
)";

/**
 * A glass panel: a pane of glass held a little over the canvas, its face
 * frosted, its edge a polished bevel rounding down to the rim. Each pixel
 * is the ray seen there, followed down through the glass to the canvas:
 *
 *  - where the face is flat it goes straight through, and the frost
 *    scatters it: the blur, tinted;
 *  - on the bevel it is bent by the slope it enters (Snell's law), again as
 *    it leaves the flat underside, and meets the canvas further in: what is
 *    behind the bevel is seen bent, magnified, through clear glass (each
 *    color by its own index of refraction: a faint fringe of color, as
 *    glass disperses light). Where the bevel is steep, the ray cannot leave
 *    the underside (total internal reflection): it stays in the glass, and
 *    shows the frosted glass.
 *  - where the bevel is steep the face reflects (Fresnel), down onto the
 *    canvas around it: the rim takes on the colors around the panel.
 *
 * No highlights, no lighting: the edge shows by what it bends. All of it
 * is worked out in light, what is behind tinted as tinted glass does (see
 * KisStudioGlass::veil).
 */
const char *const glassSource = R"(
uniform sampler2D blurred;
uniform sampler2D small;
uniform sampler2D sharp;
uniform sampler2D statistics;
uniform sampler2D narrow;
uniform float detail;
uniform float restore;
uniform float grain;
uniform bool decodeSharp;
uniform float factor;
uniform vec4 shape;
uniform vec2 corner;
uniform vec3 tint;
uniform float opacity;
uniform vec2 viewport;
uniform float sigma;
uniform vec4 beyond;
uniform float dither;
uniform float veil;
uniform vec3 bevel;
uniform vec3 ior;
flat in float strength;

float hash(uvec2 p)
{
    uint h = p.x * 1664525u + p.y * 1013904223u;
    h ^= h >> 16;
    h *= 0x7feb352du;
    h ^= h >> 15;
    h *= 0x846ca68bu;
    h ^= h >> 16;
    return float(h) * (1.0 / 4294967295.0);
}

float erf(float x)
{
    // Abramowitz and Stegun 7.1.26: within 1.5e-7
    float t = 1.0 / (1.0 + 0.3275911 * abs(x));
    float y = 1.0 - (((((1.061405429 * t - 1.453152027) * t) + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t * exp(-x * x);
    return sign(x) * y;
}

// how much of the blur at d pixels in from an edge of the canvas is of the canvas: beyond the edge
// is the panel color. Across the edge (crossing, where the panel goes on beyond the canvas, plain)
// none at the edge, for the glass to meet the plain panel there without a seam.
float within(float d, float crossing)
{
    float e = erf(d / (sigma * 1.41421356));
    return mix(0.5 + 0.5 * e, max(e, 0.0), crossing);
}

// how far outside the shape p is, in pixels (inside, less than 0), and which way is out there:
// its squircle corners are each a quarter of a superellipse, |x|^n + |y|^n = reach^n
float outside(vec2 p, out vec2 normal)
{
    vec2 from = p - shape.xy;
    vec2 side = vec2(from.x < 0.0 ? -1.0 : 1.0, from.y < 0.0 ? -1.0 : 1.0);
    vec2 q = abs(from) - (shape.zw - corner.x);
    if (q.x <= 0.0 || q.y <= 0.0) {
        normal = q.x > q.y ? vec2(side.x, 0.0) : vec2(0.0, side.y);
        return max(q.x, q.y) - corner.x;
    }
    float n = corner.y;
    vec2 a = pow(q, vec2(n));
    float f = pow(a.x + a.y, 1.0 / n);
    // the distance to f = reach, about: f over how fast it grows
    vec2 gradient = pow(q / f, vec2(n - 1.0));
    normal = normalize(gradient) * side;
    return (f - corner.x) / length(gradient);
}

// the bevel d pixels in from the rim, w wide, h thick: its height over the underside, and its
// slope. A quarter of a superellipse of exponent 4: steep at the rim, flat where the face is,
// without a crease there
vec2 profile(float d, float w, float h)
{
    float u = 1.0 - clamp(d / w, 0.0, 1.0);
    float u3 = u * u * u;
    float k = max(1.0 - u3 * u, 1e-5);
    return vec2(h * pow(k, 0.25), h / w * u3 * pow(k, -0.75));
}

// the ray seen at a point of the bevel with the surface normal there, in glass of index n, whose
// face is height over the underside: where it meets the canvas, from there (xy); z 1 if it gets
// there, 0 if it is reflected back up inside the glass, and then the way it leaves the face (w,
// then 0 if it does not)
vec4 trace(vec3 normal, float height, float n, out vec3 back)
{
    // in, at the face
    vec3 t = refract(vec3(0.0, 0.0, -1.0), normal, 1.0 / n);
    vec2 offset = t.xy / max(-t.z, 1e-3) * height;
    // out of the flat underside, and on across the gap down to the canvas
    float sinGlass = length(t.xy);
    float sinAir = n * sinGlass;
    if (sinAir >= 1.0) {
        // reflected by the underside, and up out of the face (or not)
        back = refract(vec3(t.xy, -t.z), -normal, n);
        return vec4(offset, 0.0, dot(back, back) > 0.0 ? 1.0 : 0.0);
    }
    back = vec3(0.0);
    offset += t.xy / max(sinGlass, 1e-6) * (sinAir / sqrt(1.0 - sinAir * sinAir)) * bevel.z;
    return vec4(offset, 1.0, 0.0);
}

// how much light the glass reflects, seen at an angle of acos(c) to its face (Schlick)
float fresnel(float c, float n)
{
    float r = (n - 1.0) / (n + 1.0);
    r *= r;
    return r + (1.0 - r) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0);
}


// the canvas at p (surface pixels), in light: bilinear
vec3 sharpAt(vec2 p)
{
    ivec2 last = textureSize(sharp, 0) - 1;
    vec2 t = p - 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    vec3 a = texelFetch(sharp, clamp(i, ivec2(0), last), 0).rgb;
    vec3 b = texelFetch(sharp, clamp(i + ivec2(1, 0), ivec2(0), last), 0).rgb;
    vec3 c = texelFetch(sharp, clamp(i + ivec2(0, 1), ivec2(0), last), 0).rgb;
    vec3 d = texelFetch(sharp, clamp(i + ivec2(1, 1), ivec2(0), last), 0).rgb;
    if (decodeSharp) {
        a = fromSrgb(a);
        b = fromSrgb(b);
        c = fromSrgb(c);
        d = fromSrgb(d);
    }
    return max(mix(mix(a, b, f.x), mix(c, d, f.x), f.y), 0.0);
}

// a small weighted texture (the blur, or the canvas made small) at p (surface pixels), in light:
// bilinear, the weights too
vec3 softAt(sampler2D source, vec2 p)
{
    ivec2 last = textureSize(source, 0) - 1;
    vec2 t = p / factor - 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    vec4 a = weighted(texelFetch(source, clamp(i, ivec2(0), last), 0));
    vec4 b = weighted(texelFetch(source, clamp(i + ivec2(1, 0), ivec2(0), last), 0));
    vec4 c = weighted(texelFetch(source, clamp(i + ivec2(0, 1), ivec2(0), last), 0));
    vec4 d = weighted(texelFetch(source, clamp(i + ivec2(1, 1), ivec2(0), last), 0));
    vec4 m = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    return m.rgb / max(m.a, 1e-5);
}

vec3 inGamut(float l, vec2 ab);

/**
 * The frost at p, in light. How light: from light mixed (as it mixes, on the
 * eye too), mostly the wide blur, a little of a narrow one: the eye sees
 * structure by lightness, finer than by color. Its color: the mean in Oklab
 * (mixed as light, a stroke of color on white paper keeps only half of its
 * chroma: brightness drowns it), and of what hues cancel out where they
 * disagree, as much restored as they agree: never more than the chroma of
 * the colors behind, on average.
 */
vec3 frostedAt(vec2 p)
{
    float l = mix(toOklab(softAt(blurred, p)).x, toOklab(softAt(narrow, p)).x, detail);
    vec3 seen = softAt(statistics, p);
    vec2 ab = seen.xy - 0.5;
    float agree = clamp(length(ab) / max(seen.z, 1e-4), 0.0, 1.0);
    return inGamut(clamp(l, 0.0, 1.0), ab * (1.0 + restore * (1.0 - agree)));
}

// what is behind at p, seen through the glass: clear (as sharp as it is spread over the screen:
// magnified, the canvas; squeezed, the canvas made small), or frosted
vec3 behindAt(vec2 p, float frost, float spread)
{
    vec3 clear = sharpAt(p);
    if (spread > 1.0) {
        clear = mix(clear, softAt(small, p), clamp((spread - 1.0) / (factor - 1.0), 0.0, 1.0));
    }
    return mix(clear, frostedAt(p), frost);
}

// the canvas the glass at p is held over, where the reflected ray d (z toward the viewer, down)
// meets it: blurred, as the glass is not a mirror
vec3 reflectedAt(vec3 d, vec2 p)
{
    return softAt(blurred, p + d.xy / max(-d.z, 0.2) * bevel.z);
}

// the largest chroma of hue ab at lightness l that is a color of the screen, scaled: ab times it
vec3 inGamut(float l, vec2 ab)
{
    vec3 c = fromOklab(vec3(l, ab));
    if (all(greaterThanEqual(c, vec3(-1e-4))) && all(lessThanEqual(c, vec3(1.0 + 1e-4)))) {
        return c;
    }
    float lo = 0.0;
    float hi = 1.0;
    for (int i = 0; i < 10; i++) {
        float m = 0.5 * (lo + hi);
        c = fromOklab(vec3(l, ab * m));
        if (all(greaterThanEqual(c, vec3(-1e-4))) && all(lessThanEqual(c, vec3(1.0 + 1e-4)))) {
            lo = m;
        } else {
            hi = m;
        }
    }
    return fromOklab(vec3(l, ab * lo));
}

// what is behind, in light, through glass tinted s strong: as light as the tint says, as colorful
// as what is behind (its chroma kept), veiled a little (see KisStudioGlass::veil)
vec3 tinted(vec3 behind, vec3 tintLab, float s)
{
    vec3 b = toOklab(behind);
    float l = clamp(mix(b.x, tintLab.x, s), 0.0, 1.0);
    float scattered = s * mix(veil, 1.0, smoothstep(-0.02, 0.06, l - b.x));
    return inGamut(l, mix(b.yz, tintLab.yz, scattered));
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 normal2d;
    float d = -outside(p, normal2d);
    float coverage = clamp(d + 0.5, 0.0, 1.0) * opacity;
    if (coverage <= 0.0) {
        discard;
    }
    // of the canvas, not of the panel color beyond its edges
    float canvas = within(p.x, beyond.x) * within(viewport.x - p.x, beyond.y) * within(p.y, beyond.z) * within(viewport.y - p.y, beyond.w);
    vec3 tintLight = fromSrgb(tint);
    vec3 tintLab = toOklab(tintLight);

    // the bevel: no wider than the corners are round, nor than the panel is
    float w = min(bevel.x, min(corner.x, min(shape.z, shape.w)));
    vec3 color;
    // the grain of the frost: none on the clear bevel
    float grained = 1.0;
    if (w > 0.5 && d < w) {
        float h = bevel.y * w / bevel.x;
        vec2 bend = profile(d, w, h);
        vec3 normal = normalize(vec3(normal2d * bend.y, 1.0));
        float frost = smoothstep(0.0, 1.0, d / w);
        grained = frost;
        // how far apart neighbours on the screen see the canvas: more than a pixel, it is squeezed
        vec3 unused;
        float before = dot(trace(normalize(vec3(normal2d * profile(d - 0.5, w, h).y, 1.0)), profile(d - 0.5, w, h).x, ior.g, unused).xy, -normal2d);
        float after = dot(trace(normalize(vec3(normal2d * profile(d + 0.5, w, h).y, 1.0)), profile(d + 0.5, w, h).x, ior.g, unused).xy, -normal2d);
        float spread = abs(1.0 + after - before);

        vec3 light3 = vec3(0.0);
        for (int i = 0; i < 3; i++) {
            vec3 back;
            vec4 ray = trace(normal, bend.x, ior[i], back);
            vec3 seen;
            if (ray.z > 0.0) {
                seen = mix(tintLight, behindAt(p + ray.xy, frost, spread), canvas);
            } else {
                // reflected inside, scattered by the frost: the glass itself
                seen = mix(tintLight, frostedAt(p), canvas);
            }
            light3[i] = seen[i];
        }
        // thinner, the glass tints what is behind it a little less
        color = tinted(light3, tintLab, strength * mix(0.85, 1.0, bend.x / h));
        // where it is steep the face reflects (Fresnel) down onto the canvas around it; up, the
        // room, which is in the tint already (no gloss: the bevel shows by what it bends)
        vec3 mirrored = reflect(vec3(0.0, 0.0, -1.0), normal);
        float faceOn = fresnel(1.0, ior.g);
        float reflected = (fresnel(normal.z, ior.g) - faceOn) / (1.0 - faceOn) * smoothstep(0.0, 0.25, -mirrored.z);
        color = mix(color, reflectedAt(mirrored, p), reflected);
    } else {
        color = tinted(mix(tintLight, frostedAt(p), canvas), tintLab, strength);
    }

    // dithered (triangular, a step of the target either way), for its slow gradients not to band;
    // and grained, as frosted glass is (a finer texture than it can blur): the same on all three,
    // light and dark, as much either way as seen
    uvec2 u = uvec2(p);
    float noise = hash(u) + hash(u + uvec2(7919u, 104729u)) - 1.0;
    float speck = hash(u + uvec2(3571u, 6007u)) + hash(u + uvec2(15485u, 2741u)) - 1.0;
    fragColor = vec4(toSrgb(color) + noise * dither + speck * grain * grained, coverage);
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

/**
 * The refractive index of the glass for red, green and blue light (610,
 * 550 and 465 nm), from its index for yellow light and its Abbe number,
 * by Cauchy's equation, n = A + B / wavelength^2
 */
QVector3D refractiveIndices()
{
    const qreal nd = KisStudioGlass::refractiveIndex;
    // the spread from blue (the F line, 486.1 nm) to red (the C line, 656.3 nm)
    const qreal spread = (nd - 1) / KisStudioGlass::abbeNumber;
    const qreal b = spread / (1 / (0.4861 * 0.4861) - 1 / (0.6563 * 0.6563));
    const qreal a = nd - b / (0.5876 * 0.5876);
    auto at = [a, b](qreal micrometers) { return float(a + b / (micrometers * micrometers)); };
    return QVector3D(at(0.610), at(0.550), at(0.465));
}

} // namespace

struct KisOpenGLGlass::Private
{
    Program shrink;
    Program blur;
    Program glass;
    QOpenGLVertexArrayObject vao;
    /// The canvas made small; blurred across; then down: widely, a little (narrow), and its Oklab statistics, widely
    QScopedPointer<QOpenGLFramebufferObject> small;
    QScopedPointer<QOpenGLFramebufferObject> across;
    QScopedPointer<QOpenGLFramebufferObject> blurred;
    QScopedPointer<QOpenGLFramebufferObject> acrossNarrow;
    QScopedPointer<QOpenGLFramebufferObject> narrow;
    QScopedPointer<QOpenGLFramebufferObject> smallStatistics;
    QScopedPointer<QOpenGLFramebufferObject> acrossStatistics;
    QScopedPointer<QOpenGLFramebufferObject> statistics;
    /// Of what is kept between the steps: half floats where they can be drawn to, eight bits elsewhere
    GLenum keptFormat {GL_RGBA};
    bool valid {false};

    void ensureSize(const QSize &size)
    {
        if (small && small->size() == size) {
            return;
        }
        for (QScopedPointer<QOpenGLFramebufferObject> *fbo : {&small, &across, &blurred, &acrossNarrow, &narrow,
                                                               &smallStatistics, &acrossStatistics, &statistics}) {
            fbo->reset(new QOpenGLFramebufferObject(size, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, keptFormat));
        }
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
    // drawn to: OpenGL 3 can, OpenGL ES 3 with an extension
    const bool halfFloats = !context->isOpenGLES() || context->hasExtension("GL_EXT_color_buffer_half_float")
        || context->hasExtension("GL_EXT_color_buffer_float");
    d->keptFormat = halfFloats ? GL_RGBA16F : GL_RGBA;
    d->valid = d->shrink.make(quadSource, shrinkSource, {"rect", "source", "factor", "decode", "salience", "statistics"})
        && d->blur.make(quadSource, blurSource, {"rect", "source", "direction", "taps", "weights"})
        && d->glass.make(glassVertexSource, glassSource,
                         {"rect", "blurred", "small", "sharp", "statistics", "narrow", "detail", "restore", "grain", "decodeSharp",
                          "factor", "shape", "corner", "tint", "tintLightness", "tintStrength", "opacity", "viewport", "sigma",
                          "beyond", "spacing", "dither", "veil", "bevel", "ior"});
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
    // the gaussian, and the pixels made one in the smaller canvas
    return 3 * KisStudioGlass::blurSigma + 8;
}

void KisOpenGLGlass::render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                            const QVector<Surface> &surfaces, const QRect &clip)
{
    // a floating point (HDR) canvas is in linear light already, and has no steps to dither
    const bool linearCanvas = canvasFormat == GL_RGBA16F;
    const float dither = linearCanvas ? 0.0f : canvasFormat == GL_RGB10_A2 ? 1.0f / 1023 : 1.0f / 255;
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

    // made a few times smaller, the blurs as wide in widget pixels whatever the screen's scale
    const int factor = qMax(1, qRound(4 * pixelRatio));
    // half of a gaussian of sigma (widget pixels): its taps either way, and their weights
    struct Kernel {
        int taps {0};
        float weights[maxTaps + 1] = {};
    };
    auto kernel = [&](qreal widgetSigma) {
        Kernel k;
        const qreal sigma = widgetSigma * pixelRatio / factor;
        k.taps = qBound(1, qCeil(3 * sigma), maxTaps);
        float sum = 0;
        for (int i = 0; i <= k.taps; i++) {
            k.weights[i] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
            sum += i ? 2 * k.weights[i] : k.weights[i];
        }
        for (int i = 0; i <= k.taps; i++) k.weights[i] /= sum;
        return k;
    };
    const Kernel wide = kernel(KisStudioGlass::blurSigma);
    const Kernel fine = kernel(KisStudioGlass::detailSigma);
    const int taps = wide.taps;

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
    f->glUniform1f(d->shrink["salience"], KisStudioGlass::salience);
    d->blur->bind();
    f->glUniform4f(d->blur["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->blur["source"], 0);

    // source blurred across into across, then down into result, with kernel k, in area (and as far
    // around as it takes in)
    auto blur = [&](QOpenGLFramebufferObject *source, QOpenGLFramebufferObject *across, QOpenGLFramebufferObject *result,
                    const Kernel &k, const QRect &area) {
        f->glUniform1i(d->blur["taps"], k.taps);
        f->glUniform1fv(d->blur["weights"], maxTaps + 1, k.weights);
        across->bind();
        scissor(area.adjusted(0, -k.taps, 0, k.taps) & smallWhole);
        f->glUniform2i(d->blur["direction"], 1, 0);
        f->glBindTexture(GL_TEXTURE_2D, source->texture());
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        result->bind();
        scissor(area);
        f->glUniform2i(d->blur["direction"], 0, 1);
        f->glBindTexture(GL_TEXTURE_2D, across->texture());
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    // blurred under each panel only, whole (how bright it is behind all of it sets its tint)
    for (const Surface &surface : surfaces) {
        const QRect r = surface.rect.toAlignedRect() & whole;
        if (r.isEmpty()) continue;
        // its small pixels (one more for scaling back up), and as far as the blur takes in around them
        const QRect area = QRect(QPoint(r.left() / factor, r.top() / factor), QPoint(r.right() / factor + 1, r.bottom() / factor + 1)) & smallWhole;
        const QRect around = area.adjusted(-taps, -taps, taps, taps) & smallWhole;

        d->shrink->bind();
        f->glBindTexture(GL_TEXTURE_2D, canvas);
        scissor(around);
        d->small->bind();
        f->glUniform1i(d->shrink["statistics"], 0);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        d->smallStatistics->bind();
        f->glUniform1i(d->shrink["statistics"], 1);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        d->blur->bind();
        blur(d->small.data(), d->across.data(), d->blurred.data(), wide, area);
        blur(d->small.data(), d->acrossNarrow.data(), d->narrow.data(), fine, area);
        blur(d->smallStatistics.data(), d->acrossStatistics.data(), d->statistics.data(), wide, area);
    }

    // the glass, over what is drawn
    f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(target));
    f->glViewport(0, 0, viewport.width(), viewport.height());
    scissor(drawn);
    f->glEnable(GL_BLEND);
    f->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    d->glass->bind();
    f->glUniform1i(d->glass["blurred"], 0);
    f->glUniform1i(d->glass["small"], 1);
    f->glUniform1i(d->glass["sharp"], 2);
    f->glUniform1i(d->glass["statistics"], 3);
    f->glUniform1i(d->glass["narrow"], 4);
    f->glUniform1f(d->glass["detail"], KisStudioGlass::detail);
    f->glUniform1f(d->glass["restore"], KisStudioGlass::restore);
    // as much of a step either way in the target's steps, at least those of eight bits
    f->glUniform1f(d->glass["grain"], KisStudioGlass::grain);
    f->glUniform1i(d->glass["decodeSharp"], !linearCanvas);
    f->glUniform1f(d->glass["factor"], factor);
    f->glUniform2f(d->glass["tintStrength"], KisStudioGlass::tintOverDark, KisStudioGlass::tintOverBright);
    f->glUniform1f(d->glass["veil"], KisStudioGlass::veil);
    f->glUniform2f(d->glass["viewport"], viewport.width(), viewport.height());
    f->glUniform1f(d->glass["sigma"], KisStudioGlass::blurSigma * pixelRatio);
    f->glUniform1f(d->glass["spacing"], 2 * KisStudioGlass::blurSigma * pixelRatio);
    f->glUniform1f(d->glass["dither"], dither);
    const KisStudioGlass::Bevel bevel = KisStudioGlass::bevel();
    f->glUniform3f(d->glass["bevel"], bevel.width * pixelRatio, bevel.thickness * pixelRatio, bevel.lift * pixelRatio);
    const QVector3D ior = refractiveIndices();
    f->glUniform3f(d->glass["ior"], ior.x(), ior.y(), ior.z());
    f->glActiveTexture(GL_TEXTURE4);
    f->glBindTexture(GL_TEXTURE_2D, d->narrow->texture());
    f->glActiveTexture(GL_TEXTURE3);
    f->glBindTexture(GL_TEXTURE_2D, d->statistics->texture());
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, canvas);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, d->small->texture());
    f->glActiveTexture(GL_TEXTURE0);
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
        // the sides it goes on beyond the canvas on: left, right, bottom, top
        f->glUniform4f(d->glass["beyond"], r.left() < 0, r.right() > viewport.width(), r.top() < 0, r.bottom() > viewport.height());
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    f->glActiveTexture(GL_TEXTURE4);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE3);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    d->glass->release();
    if (d->vao.isCreated()) d->vao.release();
}
