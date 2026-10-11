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
#include <QTransform>
#include <QVector3D>
#include <QVector4D>
#include <QtMath>

#ifndef GL_RGBA16F
#define GL_RGBA16F 0x881A
#endif
#ifndef GL_RGB10_A2
#define GL_RGB10_A2 0x8059
#endif

#include "KisStudioGlass.h"
#include "KisOpenGLGpuTiming.h"
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
 * Between the steps values are kept as their square roots, as fine near 0
 * as sRGB is, in eight bits. The blur weighs colors (see
 * KisStudioGlass::salience): kept is a value times its weight, and the
 * weight in alpha, both over the most a weight is, so that a mean is the
 * one over the other.
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
 * 0.32 at most, in sRGB). Of Oklab: its lightness to the power (see
 * KisStudioGlass::lightnessPower) and its two axes of color, a and b (half
 * added, to be kept as they are); or (statistics) of chroma: how far the
 * hues agree.
 */
const char *const shrinkSource = R"(
uniform sampler2D source;
uniform ivec2 shift;
uniform int factor;
uniform bool decode;
uniform float salience;
uniform float power;
uniform bool statistics;

void main()
{
    ivec2 last = textureSize(source, 0) - 1;
    ivec2 base = (ivec2(gl_FragCoord.xy) - shift) * factor;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < factor; y++) {
        for (int x = 0; x < factor; x++) {
            vec3 c = texelFetch(source, clamp(base + ivec2(x, y), ivec2(0), last), 0).rgb;
            c = clamp(decode ? fromSrgb(c) : c, 0.0, 1.0);
            vec3 lab = toOklab(c);
            float chroma = length(lab.yz);
            float weight = 1.0 + salience * chroma;
            vec3 kept = statistics ? vec3(chroma, 0.0, 0.0) : vec3(pow(max(lab.x, 0.0), power), lab.yz + 0.5);
            sum += vec4(kept * weight, weight);
        }
    }
    fragColor = keep(sum / (float(factor * factor) * (1.0 + salience * 0.33)));
}
)";

/// Half of a gaussian: across, or down, within a surface's place (slot: its first and last texel)
const char *const blurSource = R"(
uniform sampler2D source;
uniform ivec4 slot;
uniform ivec2 direction;
uniform int taps;
uniform float weights[33];

vec4 at(ivec2 p)
{
    return weighted(texelFetch(source, clamp(p, slot.xy, slot.zw), 0));
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
uniform vec2 shift;
uniform ivec4 slot;
uniform float factor;
uniform vec4 shape;
uniform float tintLightness;
uniform vec2 tintStrength;
uniform float spacing;
uniform float power;
flat out float strength;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);

    // how light it is behind, as seen (Oklab): a grid
    // as fine as the blur is wide (12 x 12 at most), so that the tint does not flicker as the
    // canvas slides under it
    ivec2 count = clamp(ivec2(ceil(2.0 * shape.zw / spacing)), ivec2(1), ivec2(12));
    float sum = 0.0;
    for (int y = 0; y < count.y; y++) {
        for (int x = 0; x < count.x; x++) {
            vec2 p = shape.xy + shape.zw * (2.0 * (vec2(x, y) + 0.5) / vec2(count) - 1.0);
            vec4 mean = weighted(texelFetch(blurred, clamp(ivec2(p / factor + shift), slot.xy, slot.zw), 0));
            sum += pow(max(mean.x / max(mean.a, 1e-5), 0.0), 1.0 / power);
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
 *    it leaves the flat underside, and meets the frost further in: the same
 *    smooth frost, seen bent and magnified toward the rim, so the bevel is
 *    one with the face, without a seam (each color by its own index of
 *    refraction: a faint fringe of color, as glass disperses light). Where
 *    the bevel is steep, the ray cannot leave the underside (total internal
 *    reflection): it stays in the glass, and shows the frost there.
 *  - where the bevel is steep the face reflects (Fresnel), down onto the
 *    canvas around it: the rim takes on the colors around the panel.
 *
 * No highlights, no lighting: the edge shows by what it bends. All of it
 * is worked out in light, what is behind tinted as tinted glass does (see
 * KisStudioGlass::veil).
 */
const char *const glassSource = R"(
uniform sampler2D blurred;
uniform sampler2D statistics;
uniform vec2 shift;
uniform ivec4 slot;
uniform sampler2D under;
// the canvas made small (a few pixels made one), and blurred twice more, between that and the frost:
// how much each is blurred (surface pixels): made small, the two steps, the frost
uniform sampler2D small;
uniform sampler2D level1;
uniform sampler2D level2;
uniform vec4 levelSigmas;
uniform bool decode;
uniform float restore;
uniform float power;
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
uniform float polish;
uniform float clearing;
uniform float gloss;
uniform vec3 ior;
uniform vec4 reach;
// the glass under this one (a menu over a panel): its shape, its corners, its tint (Oklab), how strongly it tints
uniform int lowerCount;
uniform vec4 lowerShape[4];
uniform vec2 lowerCorner[4];
uniform vec3 lowerTint[4];
uniform float lowerStrength;
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

// how much of the blur at d pixels in from an edge of the canvas is of the canvas. Where the panel
// ends within the canvas, all of it: beyond the edge (the window's, in the studio layout) the blur
// goes on as the canvas there (its textures are clamped), never a haze of the panel color along the
// window's edges. Across the edge (crossing, where the panel goes on beyond the canvas, plain) none at
// the edge, for the glass to meet the plain panel there without a seam.
float within(float d, float crossing)
{
    return mix(1.0, max(erf(d / (sigma * 1.41421356)), 0.0), crossing);
}

// how far outside a shape (its middle and half its size, s; its corners, c) p is, in pixels (inside,
// less than 0), and which way is out there: its squircle corners are each a quarter of a
// superellipse, |x|^n + |y|^n = reach^n
float outsideOf(vec2 p, vec4 shape, vec2 corner, out vec2 normal)
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

// of this glass' own shape
float outside(vec2 p, out vec2 normal)
{
    return outsideOf(p, shape, corner, normal);
}

// 0 to 1 as t goes from 0 to 1, every derivative 0 at both ends (exp(-1/t) against exp(-1/(1-t))):
// what it eases between meet without a seam of any order, as nothing else smooth enough does
float seamless(float t)
{
    t = clamp(t, 0.0, 1.0);
    float a = t > 0.0 ? exp(-1.0 / t) : 0.0;
    float b = t < 1.0 ? exp(-1.0 / (1.0 - t)) : 0.0;
    return a / max(a + b, 1e-30);
}

// the bevel d pixels in from the rim, w wide, h thick: its height over the underside, and its
// slope. A quarter of a superellipse of exponent 4 (steep at the rim) of g = u exp(a^2 (1 - 1/u^2)),
// u from the face out: g is about u over most of the bevel (it bends the light as gradually across
// it), and every derivative of it is 0 at the face, so where the bevel turns into it there is no
// crease of any order
vec2 profile(float d, float w, float h)
{
    float u = 1.0 - clamp(d / w, 0.0, 1.0);
    if (u <= 0.0) return vec2(h, 0.0);
    const float a2 = 0.04;
    float e = exp(a2 * (1.0 - 1.0 / (u * u)));
    float g = u * e;
    // dg/du = e (1 + 2 a^2 / u^2)
    float dg = e * (1.0 + 2.0 * a2 / (u * u));
    float g3 = g * g * g;
    float k = max(1.0 - g3 * g, 1e-5);
    return vec2(h * pow(k, 0.25), h / w * g3 * pow(k, -0.75) * dg);
}

// the bevel over the pixel d in from the edge: its height there and its slope over all of the pixel
// (the part of it on the glass), not at its middle. Toward the edge the slope goes up without end;
// over a pixel it is what the face rises across it, which is finite: the rim antialiased, a line
// along the edge, not a pixel here and there that happened to fall where it is steepest.
vec2 bevelOver(float d, float w, float h)
{
    float a = max(d - 0.5, 0.0);
    float b = max(d + 0.5, a + 1e-3);
    return vec2(profile(0.5 * (a + b), w, h).x, (profile(b, w, h).x - profile(a, w, h).x) / (b - a));
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


// a small weighted texture (the blur, or its statistics) at p (surface pixels), in this surface's
// place of it: bilinear, the weights too
vec3 softAt(sampler2D source, vec2 p)
{
    vec2 t = p / factor - 0.5 + shift;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    vec4 a = weighted(texelFetch(source, clamp(i, slot.xy, slot.zw), 0));
    vec4 b = weighted(texelFetch(source, clamp(i + ivec2(1, 0), slot.xy, slot.zw), 0));
    vec4 c = weighted(texelFetch(source, clamp(i + ivec2(0, 1), slot.xy, slot.zw), 0));
    vec4 d = weighted(texelFetch(source, clamp(i + ivec2(1, 1), slot.xy, slot.zw), 0));
    vec4 m = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    return m.rgb / max(m.a, 1e-5);
}

vec3 inGamut(float l, vec2 ab);

// a mean kept in source (lightness to the power, a and b half added) at p, in Oklab
vec3 meanAt(sampler2D source, vec2 p)
{
    vec3 m = softAt(source, p);
    return vec3(pow(max(m.x, 0.0), 1.0 / power), m.yz - 0.5);
}

// and in light: as it comes (made a color of the screen once, at the end: see tintedLab)
vec3 meanLightAt(sampler2D source, vec2 p)
{
    return max(fromOklab(meanAt(source, p)), 0.0);
}

/**
 * The frost at p, in Oklab: the mean of the wide blur. Its lightness a power
 * mean (see KisStudioGlass::lightnessPower). Its color the mean of a and b:
 * a mean of the hues as strong as they are, never a hue that is not between
 * them (mixed as light, orange and blue make pink), and a stroke of color on
 * white paper keeps its chroma (mixed as light, it keeps half). What hues
 * cancel where they disagree is as much restored as they agree: never more
 * than the chroma behind, on average.
 */
vec3 frostedLab(vec2 p)
{
    vec3 wide = meanAt(blurred, p);
    float chroma = softAt(statistics, p).x;
    float agree = clamp(length(wide.yz) / max(chroma, 1e-4), 0.0, 1.0);
    return vec3(wide.x, wide.yz * (1.0 + restore * (1.0 - agree)));
}

// the canvas itself at q, in light, softened a little (five taps a pixel and a half apart): what a
// polished rim shows, clear glass, of the canvas it bends
vec3 canvasLightAt(vec2 q)
{
    vec3 c = texture(under, q / viewport).rgb;
    return decode ? fromSrgb(c) : c;
}

/**
 * The canvas at q, blurred by a box about s pixels each way and averaged
 * over what a pixel there sees of it (jx, jy: how far that moves a pixel
 * across and down), in light: where the bevel bends it little and s is
 * small, a pixel sees about a pixel, all its detail; where it squeezes
 * much of the canvas into a pixel, all of that (none of it skipped: no
 * shimmer, no stair steps). A grid of taps over the two, as many as they
 * are pixels long, a few at most each way.
 */
vec3 clearLab(vec2 q, vec2 jx, vec2 jy, float s)
{
    vec2 ax = 0.5 * jx;
    vec2 ay = 0.5 * jy;
    float lx = length(ax);
    float ly = length(ay);
    ax = (lx > 1e-4 ? ax / lx : vec2(1.0, 0.0)) * max(lx, 1.7 * s);
    ay = (ly > 1e-4 ? ay / ly : vec2(0.0, 1.0)) * max(ly, 1.7 * s);
    int nx = int(clamp(ceil(2.0 * length(ax)), 1.0, 6.0));
    int ny = int(clamp(ceil(2.0 * length(ay)), 1.0, 6.0));
    vec3 sum = vec3(0.0);
    for (int j = 0; j < 6; j++) {
        if (j >= ny) break;
        for (int i = 0; i < 6; i++) {
            if (i >= nx) break;
            vec2 o = ax * (2.0 * (float(i) + 0.5) / float(nx) - 1.0) + ay * (2.0 * (float(j) + 0.5) / float(ny) - 1.0);
            sum += canvasLightAt(q + o);
        }
    }
    return toOklab(sum / float(nx * ny));
}

/**
 * The canvas at q blurred as much as a gaussian of s pixels (and over all a
 * pixel sees, jx and jy, the least): the canvas itself while s is under a
 * pixel or two, then the steps of it made small and blurred, up to the
 * frost; s between two steps, the two mixed by where it lies between them
 * (by its log, as a mipmap's levels are). The blur grows as s does: never
 * a sharp picture faded into a blurred one, which shows the sharp one's
 * edges where it fades out.
 */
vec3 blurredLab(vec2 q, float s, vec2 jx, vec2 jy)
{
    s = max(s, 0.4 * sqrt(max(dot(jx, jx), dot(jy, jy))));
    if (s >= levelSigmas.w) return frostedLab(q);
    if (s <= levelSigmas.x) {
        // the canvas itself, blurred as much; as it nears the step made small, handed over to it (as blurred)
        return mix(clearLab(q, jx, jy, s), meanAt(small, q), seamless((s / levelSigmas.x - 0.6) / 0.4));
    }
    float at = log(s);
    if (s <= levelSigmas.y) {
        return mix(meanAt(small, q), meanAt(level1, q), (at - log(levelSigmas.x)) / (log(levelSigmas.y) - log(levelSigmas.x)));
    }
    if (s <= levelSigmas.z) {
        return mix(meanAt(level1, q), meanAt(level2, q), (at - log(levelSigmas.y)) / (log(levelSigmas.z) - log(levelSigmas.y)));
    }
    return mix(meanAt(level2, q), frostedLab(q), (at - log(levelSigmas.z)) / (log(levelSigmas.w) - log(levelSigmas.z)));
}

// a point the rim sees, kept within as far around the panel as the canvas is blurred (reach): a
// steep rim sees far, beyond that is nothing blurred
vec2 held(vec2 q)
{
    return clamp(q, reach.xy, reach.zw);
}

// the canvas the glass at p is held over, where the reflected ray d (z toward the viewer, down)
// meets it: blurred, as the glass is not a mirror
vec3 reflectedAt(vec3 d, vec2 p)
{
    return meanLightAt(blurred, held(p + d.xy / max(-d.z, 0.2) * bevel.z));
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
    for (int i = 0; i < 8; i++) {
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

// what is behind (Oklab) through glass tinted s strong, in light: as light as the tint says, as
// colorful as what is behind (its chroma kept), veiled a little (see KisStudioGlass::veil), and made
// a color of the screen, once
vec3 tintedLab(vec3 b, vec3 tintLab, float s)
{
    float l = clamp(mix(b.x, tintLab.x, s), 0.0, 1.0);
    float scattered = s * mix(veil, 1.0, smoothstep(-0.02, 0.06, l - b.x));
    return inGamut(l, mix(b.yz, tintLab.yz, scattered));
}

vec3 tinted(vec3 behind, vec3 tintLab, float s)
{
    return tintedLab(toOklab(behind), tintLab, s);
}

/**
 * What is seen at p (lab, Oklab) through the glass under this one, where
 * this one lies over it (a menu over a panel): tinted by each as its own
 * glass tints it, glass over glass, not the canvas as if they were not
 * there. Their edges as soft as this glass' frost makes what is under it.
 */
vec3 throughLower(vec3 lab, vec2 p)
{
    for (int i = 0; i < 4; i++) {
        if (i >= lowerCount) break;
        vec2 n;
        float d = outsideOf(p, lowerShape[i], lowerCorner[i], n);
        float cover = clamp(0.5 - d / max(2.0 * sigma, 1.0), 0.0, 1.0);
        if (cover > 0.0) lab = mix(lab, toOklab(tintedLab(lab, lowerTint[i], lowerStrength)), cover);
    }
    return lab;
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 normal2d;
    float d = -outside(p, normal2d);

    // the bevel: no wider than the corners are round, nor than the panel is
    float w = min(bevel.x, min(corner.x, min(shape.z, shape.w)));
    bool onBevel = w > 0.5 && d < w;
    float h = bevel.y * w / max(bevel.x, 1e-3);
    vec2 bend = vec2(0.0);
    vec3 normal = vec3(0.0, 0.0, 1.0);
    // where a pixel sees the canvas (the middle color's ray), and how far that moves a pixel across and
    // down: of every pixel, before any is left out (derivatives are of the four pixels together)
    vec2 seenAt = p;
    if (onBevel) {
        bend = bevelOver(d, w, h);
        normal = normalize(vec3(normal2d * bend.y, 1.0));
        vec3 back;
        seenAt = p + trace(normal, bend.x, ior.g, back).xy;
    }
    vec2 jx = dFdx(seenAt);
    vec2 jy = dFdy(seenAt);

    float coverage = clamp(d + 0.5, 0.0, 1.0) * opacity;
    if (coverage <= 0.0) {
        discard;
    }
    // of the canvas, not of the panel color beyond its edges
    float canvas = within(p.x, beyond.x) * within(viewport.x - p.x, beyond.y) * within(p.y, beyond.z) * within(viewport.y - p.y, beyond.w);
    vec3 tintLight = fromSrgb(tint);
    vec3 tintLab = toOklab(tintLight);

    /**
     * How blurred what the glass shows is, d in from the rim: a pixel's worth
     * at the rim (all polish) up to the frost's, from where it is all clear
     * on to a frost's width into the face (the frost is wider than the bevel:
     * grown over the bevel alone, the blur doubled from a pixel to the next,
     * an edge), by its log, eased without a seam of any order
     */
    float sharpest = exp(mix(log(sigma), log(0.35), polish));
    float rampStart = clearing * w;
    float rampEnd = w + sigma;
    float frosting = w > 0.5 ? seamless((d - rampStart) / max(rampEnd - rampStart, 1.0)) : 1.0;
    float blur = exp(mix(log(sharpest), log(sigma), frosting));

    vec3 color;
    if (onBevel) {
        // polished toward the rim, it shows the canvas itself, bent; into the face, the frost (see blur)

        vec3 light3 = vec3(0.0);
        for (int i = 0; i < 3; i++) {
            vec3 back;
            vec4 ray = trace(normal, bend.x, ior[i], back);
            // mixed with the panel color as the face is (in Oklab), so that where the bevel
            // flattens into the face the two are one, without a step
            vec3 seen;
            if (ray.z > 0.0) {
                vec2 q = held(p + ray.xy);
                seen = throughLower(blurredLab(q, blur, jx, jy), q);
            } else {
                // reflected inside, scattered by the frost: the glass itself
                seen = throughLower(frostedLab(p), p);
            }
            vec3 through = max(fromOklab(mix(tintLab, seen, canvas)), 0.0);
            light3[i] = through[i];
        }
        // thinner toward the rim, the glass tints what is behind it less (Beer-Lambert): a band along the edge, lighter
        // over a light canvas, deeper over a dark one, as a border would be
        color = tinted(light3, tintLab, strength * mix(0.3, 1.0, bend.x / h));
        // where it is steep the face reflects (Fresnel) down onto the canvas around it; up, the
        // room, which is in the tint already (no gloss: the bevel shows by what it bends)
        vec3 mirrored = reflect(vec3(0.0, 0.0, -1.0), normal);
        float faceOn = fresnel(1.0, ior.g);
        float reflected = (fresnel(normal.z, ior.g) - faceOn) / (1.0 - faceOn) * seamless(-mirrored.z / 0.25);
        color = mix(color, reflectedAt(mirrored, p), reflected);
        if (gloss > 0.0) {
            // the edge catches a light from the top left where it faces it, and faintly where it faces away
            // (lit through the glass): only where it is steep, a thin line along the rim
            float facing = dot(normal2d, normalize(vec2(-1.0, -1.0)));
            float lit = facing * facing * (facing > 0.0 ? 1.0 : 0.35);
            color += vec3(gloss * lit * pow(1.0 - normal.z, 1.5));
        }
    } else {
        // through the light of the screen as the bevel's is (a color beyond it held at its edge): the same where they meet
        // the frost, or near the bevel what the blur is there on its way to it
        vec3 seen = blur < sigma * 0.999 ? blurredLab(p, blur, jx, jy) : frostedLab(p);
        color = tinted(max(fromOklab(mix(tintLab, throughLower(seen, p), canvas)), 0.0), tintLab, strength);
    }

    /**
     * Dithered, nothing more: triangular noise of a step of the target either
     * way (the sum of two uniform ones), which turns the steps a smooth
     * gradient would be cut into (banding) into noise too fine to see, and
     * leaves no pattern and no noise that follows the picture: the textbook
     * dither. No grain: the frost is smooth.
     */
    uvec2 u = uvec2(p);
    float steps = hash(u) + hash(u + uvec2(7919u, 104729u)) - 1.0;
    if (coverage < 1.0) {
        /**
         * A pixel the edge goes across (or glass fading in or out): what it
         * shows is the light of both, each as much as it covers. Mixed here,
         * in light, from the canvas under it: blending would mix the encoded
         * values, which takes the edge too dark over a light canvas and too
         * light over a dark one.
         */
        vec3 behind = texture(under, p / viewport).rgb;
        color = mix(decode ? fromSrgb(behind) : behind, color, coverage);
    }
    fragColor = vec4(toSrgb(color) + steps * dither, 1.0);
}
)";

/**
 * The ambient light: the canvas made sixteen times smaller (every other pixel
 * of each 16 x 16), only where the image is (toImage takes a pixel to the
 * image's own coordinates, 0 to 1 across it): its light, in linear, and how
 * much of the pixel is image, kept as the blur keeps them
 */
const char *const ambientShrinkSource = R"(
uniform sampler2D source;
uniform int factor;
uniform bool decode;
uniform mat3 toImage;

void main()
{
    ivec2 last = textureSize(source, 0) - 1;
    ivec2 base = ivec2(gl_FragCoord.xy) * factor;
    vec4 sum = vec4(0.0);
    float count = 0.0;
    for (int y = 0; y < factor; y += 2) {
        for (int x = 0; x < factor; x += 2) {
            ivec2 p = min(base + ivec2(x, y), last);
            vec3 q = toImage * vec3(vec2(p) + 0.5, 1.0);
            vec2 uv = q.xy / q.z;
            float inside = (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) ? 1.0 : 0.0;
            vec3 c = texelFetch(source, p, 0).rgb;
            c = clamp(decode ? fromSrgb(c) : c, 0.0, 1.0);
            sum += vec4(c * inside, inside);
            count += 1.0;
        }
    }
    fragColor = keep(sum / count);
}
)";

/**
 * The ambient light, drawn around the image (nothing over it): the light of
 * the image nearby (the blurred light over how much of it is image), as
 * much as there is image near (that much of it: the light falls off with
 * the distance as the blur does), a little more colorful (a wall lit by a
 * screen is lit by its colors, not its grays), over the color around it
 */
const char *const ambientSource = R"(
uniform sampler2D ambient;
uniform float factor;
uniform mat3 toImage;
uniform vec3 base;
uniform float strength;
uniform bool encode;
uniform float dither;

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

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec3 q = toImage * vec3(p, 1.0);
    vec2 uv = q.xy / q.z;
    if (uv.x >= 0.0 && uv.y >= 0.0 && uv.x <= 1.0 && uv.y <= 1.0) {
        discard;
    }
    ivec2 last = textureSize(ambient, 0) - 1;
    vec2 t = p / factor - 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    vec4 a = weighted(texelFetch(ambient, clamp(i, ivec2(0), last), 0));
    vec4 b = weighted(texelFetch(ambient, clamp(i + ivec2(1, 0), ivec2(0), last), 0));
    vec4 c = weighted(texelFetch(ambient, clamp(i + ivec2(0, 1), ivec2(0), last), 0));
    vec4 d = weighted(texelFetch(ambient, clamp(i + ivec2(1, 1), ivec2(0), last), 0));
    vec4 m = mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
    vec3 light = m.rgb / max(m.a, 1e-5);
    vec3 lab = toOklab(light);
    light = max(fromOklab(vec3(lab.x, lab.yz * 1.25)), 0.0);
    vec3 color = base + light * strength * m.a;
    uvec2 u = uvec2(p);
    float steps = hash(u) + hash(u + uvec2(7919u, 104729u)) - 1.0;
    fragColor = vec4((encode ? toSrgb(color) : color) + steps * dither, 1.0);
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
QVector3D refractiveIndices(qreal abbeNumber)
{
    const qreal nd = KisStudioGlass::refractiveIndex;
    // the spread from blue (the F line, 486.1 nm) to red (the C line, 656.3 nm)
    const qreal spread = (nd - 1) / abbeNumber;
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
    Program ambientShrink;
    Program ambient;
    /// The ambient light: the canvas made small, blurred across, then down
    QScopedPointer<QOpenGLFramebufferObject> ambientSmall;
    QScopedPointer<QOpenGLFramebufferObject> ambientAcross;
    QScopedPointer<QOpenGLFramebufferObject> ambientBlurred;
    QOpenGLVertexArrayObject vao;
    /// The canvas made small; blurred across; then down; and its Oklab statistics, the same
    QScopedPointer<QOpenGLFramebufferObject> small;
    QScopedPointer<QOpenGLFramebufferObject> across;
    QScopedPointer<QOpenGLFramebufferObject> blurred;
    QScopedPointer<QOpenGLFramebufferObject> smallStatistics;
    QScopedPointer<QOpenGLFramebufferObject> acrossStatistics;
    QScopedPointer<QOpenGLFramebufferObject> statistics;
    /// The steps between the canvas made small and the frost: blurred across, then down
    QScopedPointer<QOpenGLFramebufferObject> level1Across;
    QScopedPointer<QOpenGLFramebufferObject> level1;
    QScopedPointer<QOpenGLFramebufferObject> level2Across;
    QScopedPointer<QOpenGLFramebufferObject> level2;
    /// Of what is kept between the steps: half floats where they can be drawn to, eight bits elsewhere
    GLenum keptFormat {GL_RGBA};
    /// Where the blurs were made last, for which surfaces: made again only when the canvas or they changed
    QVector<QRectF> blurredFor;
    KisOpenGLGpuTiming timing {QStringLiteral("glass gpu:"), {QStringLiteral("shrink"), QStringLiteral("blur"), QStringLiteral("draw")}};
    QSize blurredViewport;
    qreal blurredRatio {0};
    bool valid {false};

    /// At least \p size: grown only (as panels come and go, their places take more or less of it)
    void ensureSize(const QSize &size)
    {
        if (small && small->width() >= size.width() && small->height() >= size.height()) {
            return;
        }
        const QSize grown = small ? size.expandedTo(small->size()) : size;
        for (QScopedPointer<QOpenGLFramebufferObject> *fbo : {&small, &across, &blurred, &smallStatistics, &acrossStatistics, &statistics,
                                                               &level1Across, &level1, &level2Across, &level2}) {
            fbo->reset(new QOpenGLFramebufferObject(grown, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, keptFormat));
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
    d->valid = d->shrink.make(quadSource, shrinkSource, {"rect", "source", "shift", "factor", "decode", "salience", "power", "statistics"})
        && d->blur.make(quadSource, blurSource, {"rect", "source", "slot", "direction", "taps", "weights"})
        && d->glass.make(glassVertexSource, glassSource,
                         {"rect", "blurred", "statistics", "shift", "slot", "restore", "power",
                          "factor", "shape", "corner", "tint", "tintLightness", "tintStrength", "opacity", "viewport", "sigma",
                          "beyond", "spacing", "dither", "veil", "bevel", "polish", "clearing", "gloss", "ior", "under", "decode", "reach",
                          "lowerCount", "lowerShape", "lowerCorner", "lowerTint", "lowerStrength", "small", "level1", "level2", "levelSigmas"})
        && d->ambientShrink.make(quadSource, ambientShrinkSource, {"rect", "source", "factor", "decode", "toImage"})
        && d->ambient.make(quadSource, ambientSource, {"rect", "ambient", "factor", "toImage", "base", "strength", "encode", "dither"});
    if (d->valid && KisOpenGL::supportsVAO()) {
        d->vao.create();
    }
    return d->valid;
}

bool KisOpenGLGlass::isValid() const
{
    return d->valid;
}

namespace {
/// \p transform as a matrix of GLSL (columns)
/// \p color (sRGB) in Oklab
QVector3D oklabOf(const QColor &color)
{
    auto linear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    const qreal r = linear(color.redF()), g = linear(color.greenF()), b = linear(color.blueF());
    const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return QVector3D(float(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s), float(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
                     float(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s));
}

void setMatrix(QOpenGLFunctions *f, int location, const QTransform &transform)
{
    const float m[9] = {float(transform.m11()), float(transform.m12()), float(transform.m13()),
                        float(transform.m21()), float(transform.m22()), float(transform.m23()),
                        float(transform.m31()), float(transform.m32()), float(transform.m33())};
    f->glUniformMatrix3fv(location, 1, GL_FALSE, m);
}
} // namespace

void KisOpenGLGlass::renderAmbient(QOpenGLFramebufferObject *canvas, uint canvasFormat, qreal pixelRatio, const QTransform &surfaceToImage,
                                   const QColor &base)
{
    if (!d->valid || !canvas) {
        return;
    }
    const QSize viewport = canvas->size();
    const bool linearCanvas = canvasFormat == GL_RGBA16F;
    // sixteen times smaller: the light reaches far, nothing in it is fine
    const int factor = qMax(1, qRound(16 * pixelRatio));
    const QSize smallSize((viewport.width() + factor - 1) / factor, (viewport.height() + factor - 1) / factor);
    if (!d->ambientSmall || d->ambientSmall->size() != smallSize) {
        for (QScopedPointer<QOpenGLFramebufferObject> *fbo : {&d->ambientSmall, &d->ambientAcross, &d->ambientBlurred}) {
            fbo->reset(new QOpenGLFramebufferObject(smallSize, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, d->keptFormat));
        }
    }
    const qreal sigma = KisStudioGlass::ambientSigma * pixelRatio / factor;
    const int taps = qBound(1, qCeil(3 * sigma), maxTaps);
    float weights[maxTaps + 1] = {};
    float sum = 0;
    for (int i = 0; i <= taps; i++) {
        weights[i] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
        sum += i ? 2 * weights[i] : weights[i];
    }
    for (int i = 0; i <= taps; i++) weights[i] /= sum;

    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    GLint target = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    if (d->vao.isCreated()) d->vao.bind();
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    f->glActiveTexture(GL_TEXTURE0);
    f->glViewport(0, 0, smallSize.width(), smallSize.height());

    // the image's light, made small
    d->ambientShrink->bind();
    f->glUniform4f(d->ambientShrink["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->ambientShrink["source"], 0);
    f->glUniform1i(d->ambientShrink["factor"], factor);
    f->glUniform1i(d->ambientShrink["decode"], !linearCanvas);
    setMatrix(f, d->ambientShrink["toImage"], surfaceToImage);
    d->ambientSmall->bind();
    f->glBindTexture(GL_TEXTURE_2D, canvas->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // spread as light falls off
    d->blur->bind();
    f->glUniform4f(d->blur["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->blur["source"], 0);
    f->glUniform1i(d->blur["taps"], taps);
    f->glUniform1fv(d->blur["weights"], maxTaps + 1, weights);
    // all of it is the ambient's (the panels' blurs each keep to a place of their own)
    f->glUniform4i(d->blur["slot"], 0, 0, smallSize.width() - 1, smallSize.height() - 1);
    d->ambientAcross->bind();
    f->glUniform2i(d->blur["direction"], 1, 0);
    f->glBindTexture(GL_TEXTURE_2D, d->ambientSmall->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    d->ambientBlurred->bind();
    f->glUniform2i(d->blur["direction"], 0, 1);
    f->glBindTexture(GL_TEXTURE_2D, d->ambientAcross->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // around the image, on the canvas
    canvas->bind();
    f->glViewport(0, 0, viewport.width(), viewport.height());
    d->ambient->bind();
    f->glUniform4f(d->ambient["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->ambient["ambient"], 0);
    f->glUniform1f(d->ambient["factor"], factor);
    setMatrix(f, d->ambient["toImage"], surfaceToImage);
    const QColor linearBase = base;
    auto toLinear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    f->glUniform3f(d->ambient["base"], float(toLinear(linearBase.redF())), float(toLinear(linearBase.greenF())), float(toLinear(linearBase.blueF())));
    f->glUniform1f(d->ambient["strength"], float(KisStudioGlass::ambientStrength));
    f->glUniform1i(d->ambient["encode"], !linearCanvas);
    f->glUniform1f(d->ambient["dither"], linearCanvas ? 0.0f : canvasFormat == GL_RGB10_A2 ? 1.0f / 1023 : 1.0f / 255);
    f->glBindTexture(GL_TEXTURE_2D, d->ambientBlurred->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    f->glBindTexture(GL_TEXTURE_2D, 0);
    d->ambient->release();
    if (d->vao.isCreated()) d->vao.release();
    f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(target));
}

qreal KisOpenGLGlass::reach()
{
    // the widest gaussian, and the pixels made one in the smaller canvas
    return 3 * KisStudioGlass::blurSigma * KisStudioGlass::maxBlurScale + 8;
}

void KisOpenGLGlass::render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                            const QVector<Surface> &surfaces, const QRect &clip, bool canvasChanged)
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
    // over each panel as wide as it is large (see KisStudioGlass::blurScale)
    auto scaleOf = [pixelRatio](const Surface &surface) { return KisStudioGlass::blurScale(surface.rect.size() / pixelRatio); };

    const QSize smallSize((viewport.width() + factor - 1) / factor, (viewport.height() + factor - 1) / factor);
    const QRect smallWhole(QPoint(), smallSize);

    /**
     * Each panel blurred under it (whole: how bright it is behind all of it
     * sets its tint), in a place of its own in the small textures: its small
     * pixels (one more for scaling back up), as far as the rim sees around it
     * (its reflections, what it bends in from beside it), and as far around
     * that as its blur takes in. Of a place of their own: panels side by side
     * are each blurred as wide as they are large (a pill less than a column),
     * and in one place the blur of one would show in the other where they are
     * near (a block of it, with straight edges).
     */
    struct Region {
        QRect area;
        QRect around;
        /// From the small canvas to its place
        QPoint shift;
        Kernel wide;
        /// The steps between the canvas made small and the frost: how much each is blurred (surface pixels), and their kernels
        QVector4D sigmas;
        Kernel step1;
        Kernel step2;
    };
    QVector<Region> regions(surfaces.size());
    const int rimReach = qCeil(KisStudioGlass::bevel().lift * pixelRatio);
    {
        // in rows, left to right
        QPoint next;
        int rowHeight = 0;
        for (int i = 0; i < surfaces.size(); i++) {
            const QRect r = surfaces[i].rect.toAlignedRect().adjusted(-rimReach, -rimReach, rimReach, rimReach) & whole;
            if (r.isEmpty()) continue;
            Region &region = regions[i];
            region.area = QRect(QPoint(r.left() / factor, r.top() / factor), QPoint(r.right() / factor + 1, r.bottom() / factor + 1)) & smallWhole;
            region.wide = kernel(KisStudioGlass::blurSigma * scaleOf(surfaces[i]));
            {
                // made small, a few pixels are one (a box, and scaled back up between them): about this blurred
                const qreal madeSmall = 0.6 * factor;
                const qreal frost = KisStudioGlass::blurSigma * scaleOf(surfaces[i]) * pixelRatio;
                // the two steps evenly between them (by their log)
                const qreal ratio = std::pow(qMax(frost / madeSmall, 1.0001), 1.0 / 3);
                const qreal sigma1 = madeSmall * ratio;
                const qreal sigma2 = madeSmall * ratio * ratio;
                region.sigmas = QVector4D(float(madeSmall), float(sigma1), float(sigma2), float(qMax(frost, sigma2 * 1.0001)));
                // blurred from the canvas made small, which is that blurred already
                auto from = [&](qreal sigma) { return std::sqrt(qMax(sigma * sigma - madeSmall * madeSmall, 0.25)) / pixelRatio; };
                region.step1 = kernel(from(sigma1));
                region.step2 = kernel(from(sigma2));
            }
            region.around = region.area.adjusted(-region.wide.taps, -region.wide.taps, region.wide.taps, region.wide.taps) & smallWhole;
            if (next.x() + region.around.width() > smallSize.width()) {
                next = QPoint(0, next.y() + rowHeight);
                rowHeight = 0;
            }
            region.shift = next - region.around.topLeft();
            next.rx() += region.around.width();
            rowHeight = qMax(rowHeight, region.around.height());
        }
        d->ensureSize(QSize(smallSize.width(), qMax(1, next.y() + rowHeight)));
    }
    const QSize placesSize = d->small->size();

    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    GLint target = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    if (d->vao.isCreated()) d->vao.bind();
    f->glDisable(GL_BLEND);
    f->glEnable(GL_SCISSOR_TEST);
    f->glActiveTexture(GL_TEXTURE0);
    f->glViewport(0, 0, placesSize.width(), placesSize.height());
    auto scissor = [f](const QRect &r) { f->glScissor(r.x(), r.y(), r.width(), r.height()); };

    d->shrink->bind();
    f->glUniform4f(d->shrink["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->shrink["source"], 0);
    f->glUniform1i(d->shrink["factor"], factor);
    f->glUniform1i(d->shrink["decode"], !linearCanvas);
    f->glUniform1f(d->shrink["salience"], KisStudioGlass::salience);
    f->glUniform1f(d->shrink["power"], KisStudioGlass::lightnessPower);
    d->blur->bind();
    f->glUniform4f(d->blur["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->blur["source"], 0);


    d->timing.begin();
    // only the brush outline moved, say: what is behind the glass is what was blurred last time
    QVector<QRectF> rects;
    for (const Surface &surface : surfaces) rects << surface.rect;
    const bool blurAgain = canvasChanged || rects != d->blurredFor || viewport != d->blurredViewport || pixelRatio != d->blurredRatio;
    d->blurredFor = rects;
    d->blurredViewport = viewport;
    d->blurredRatio = pixelRatio;

    // only those that are on the canvas, when what is behind them changed
    QVector<Region> blurring;
    if (blurAgain) {
        for (const Region &region : qAsConst(regions)) {
            if (!region.around.isEmpty()) blurring << region;
        }
    }
    // the canvas made small, as colors and as their statistics
    d->shrink->bind();
    f->glBindTexture(GL_TEXTURE_2D, canvas);
    for (int statistics = 0; statistics < 2; statistics++) {
        (statistics ? d->smallStatistics : d->small)->bind();
        f->glUniform1i(d->shrink["statistics"], statistics);
        for (const Region &region : qAsConst(blurring)) {
            f->glUniform2i(d->shrink["shift"], region.shift.x(), region.shift.y());
            scissor(region.around.translated(region.shift));
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    d->timing.mark();
    // blurred: each pass for all the panels at once, the target switched (which makes the GPU wait for what
    // was drawn into it) four times, not four times a panel
    d->blur->bind();
    struct Pass {
        QOpenGLFramebufferObject *source;
        QOpenGLFramebufferObject *target;
        bool across;
    };
    const Pass passes[] = {
        {d->small.data(), d->across.data(), true},
        {d->smallStatistics.data(), d->acrossStatistics.data(), true},
        {d->across.data(), d->blurred.data(), false},
        {d->acrossStatistics.data(), d->statistics.data(), false},
    };
    for (const Pass &pass : passes) {
        if (blurring.isEmpty()) break;
        pass.target->bind();
        f->glBindTexture(GL_TEXTURE_2D, pass.source->texture());
        f->glUniform2i(d->blur["direction"], pass.across ? 1 : 0, pass.across ? 0 : 1);
        for (const Region &region : qAsConst(blurring)) {
            const Kernel &k = region.wide;
            f->glUniform1i(d->blur["taps"], k.taps);
            f->glUniform1fv(d->blur["weights"], maxTaps + 1, k.weights);
            const QRect place = region.around.translated(region.shift);
            f->glUniform4i(d->blur["slot"], place.left(), place.top(), place.right(), place.bottom());
            // across: as far up and down as the pass down takes in
            scissor((pass.across ? region.area.adjusted(0, -k.taps, 0, k.taps) & region.around : region.area).translated(region.shift));
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }
    // the steps between, from the canvas made small: across, then down, each
    struct StepPass {
        QOpenGLFramebufferObject *source;
        QOpenGLFramebufferObject *target;
        bool across;
        int step;
    };
    const StepPass stepPasses[] = {
        {d->small.data(), d->level1Across.data(), true, 1},
        {d->small.data(), d->level2Across.data(), true, 2},
        {d->level1Across.data(), d->level1.data(), false, 1},
        {d->level2Across.data(), d->level2.data(), false, 2},
    };
    for (const StepPass &pass : stepPasses) {
        if (blurring.isEmpty()) break;
        pass.target->bind();
        f->glBindTexture(GL_TEXTURE_2D, pass.source->texture());
        f->glUniform2i(d->blur["direction"], pass.across ? 1 : 0, pass.across ? 0 : 1);
        for (const Region &region : qAsConst(blurring)) {
            const Kernel &k = pass.step == 1 ? region.step1 : region.step2;
            f->glUniform1i(d->blur["taps"], k.taps);
            f->glUniform1fv(d->blur["weights"], maxTaps + 1, k.weights);
            const QRect place = region.around.translated(region.shift);
            f->glUniform4i(d->blur["slot"], place.left(), place.top(), place.right(), place.bottom());
            scissor((pass.across ? region.area.adjusted(0, -k.taps, 0, k.taps) & region.around : region.area).translated(region.shift));
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    d->timing.mark();
    // the glass, over what is drawn
    f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(target));
    f->glViewport(0, 0, viewport.width(), viewport.height());
    scissor(drawn);
    f->glEnable(GL_BLEND);
    f->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    d->glass->bind();
    f->glUniform1i(d->glass["blurred"], 0);
    f->glUniform1i(d->glass["statistics"], 1);
    f->glUniform1i(d->glass["under"], 2);
    f->glUniform1i(d->glass["small"], 3);
    f->glUniform1i(d->glass["level1"], 4);
    f->glUniform1i(d->glass["level2"], 5);
    f->glUniform1i(d->glass["decode"], !linearCanvas);
    f->glUniform1f(d->glass["restore"], KisStudioGlass::restore);
    f->glUniform1f(d->glass["power"], KisStudioGlass::lightnessPower);
    f->glUniform1f(d->glass["factor"], factor);
    f->glUniform2f(d->glass["tintStrength"], KisStudioGlass::tintOverDark, KisStudioGlass::tintOverBright);
    f->glUniform1f(d->glass["veil"], KisStudioGlass::veil);
    f->glUniform2f(d->glass["viewport"], viewport.width(), viewport.height());
    f->glUniform1f(d->glass["dither"], dither);
    const KisStudioGlass::Bevel bevel = KisStudioGlass::bevel();
    f->glUniform3f(d->glass["bevel"], bevel.width * pixelRatio, bevel.thickness * pixelRatio, bevel.lift * pixelRatio);
    f->glUniform1f(d->glass["polish"], bevel.polish);
    f->glUniform1f(d->glass["clearing"], bevel.clear);
    f->glUniform1f(d->glass["gloss"], bevel.gloss);
    const QVector3D ior = refractiveIndices(bevel.abbeNumber);
    f->glUniform3f(d->glass["ior"], ior.x(), ior.y(), ior.z());
    f->glUniform1f(d->glass["lowerStrength"], float(0.5 * (KisStudioGlass::tintOverDark + KisStudioGlass::tintOverBright)));
    f->glActiveTexture(GL_TEXTURE5);
    f->glBindTexture(GL_TEXTURE_2D, d->level2->texture());
    f->glActiveTexture(GL_TEXTURE4);
    f->glBindTexture(GL_TEXTURE_2D, d->level1->texture());
    f->glActiveTexture(GL_TEXTURE3);
    f->glBindTexture(GL_TEXTURE_2D, d->small->texture());
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, canvas);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, d->statistics->texture());
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, d->blurred->texture());
    for (int i = 0; i < surfaces.size(); i++) {
        const Surface &surface = surfaces[i];
        if (regions[i].around.isEmpty()) continue;
        const QRect place = regions[i].around.translated(regions[i].shift);
        f->glUniform2f(d->glass["shift"], regions[i].shift.x(), regions[i].shift.y());
        f->glUniform4i(d->glass["slot"], place.left(), place.top(), place.right(), place.bottom());
        f->glUniform4f(d->glass["levelSigmas"], regions[i].sigmas.x(), regions[i].sigmas.y(), regions[i].sigmas.z(), regions[i].sigmas.w());
        const QRectF r = surface.rect;
        const QRectF quad = r.adjusted(-1, -1, 1, 1);
        f->glUniform4f(d->glass["rect"], quad.left() / viewport.width() * 2 - 1, quad.top() / viewport.height() * 2 - 1,
                       quad.right() / viewport.width() * 2 - 1, quad.bottom() / viewport.height() * 2 - 1);
        f->glUniform4f(d->glass["shape"], r.center().x(), r.center().y(), r.width() / 2, r.height() / 2);
        f->glUniform2f(d->glass["corner"], surface.reach, surface.exponent);
        f->glUniform3f(d->glass["tint"], surface.tint.redF(), surface.tint.greenF(), surface.tint.blueF());
        // as light as it looks (Oklab, about the cube root of its luminance), as what is behind is measured
        auto linear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        const qreal luminance = 0.2126 * linear(surface.tint.redF()) + 0.7152 * linear(surface.tint.greenF()) + 0.0722 * linear(surface.tint.blueF());
        f->glUniform1f(d->glass["tintLightness"], float(std::cbrt(luminance)));
        f->glUniform1f(d->glass["opacity"], surface.opacity);
        const qreal scale = scaleOf(surface);
        f->glUniform1f(d->glass["sigma"], KisStudioGlass::blurSigma * scale * pixelRatio);
        f->glUniform1f(d->glass["spacing"], 2 * KisStudioGlass::blurSigma * scale * pixelRatio);
        // the sides it goes on beyond the canvas on: left, right, bottom, top
        f->glUniform4f(d->glass["beyond"], r.left() < 0, r.right() > viewport.width(), r.top() < 0, r.bottom() > viewport.height());
        const QRectF reach = r.adjusted(-rimReach, -rimReach, rimReach, rimReach) & QRectF(whole).adjusted(0.5, 0.5, -0.5, -0.5);
        f->glUniform4f(d->glass["reach"], reach.left(), reach.top(), reach.right(), reach.bottom());
        // the glass under it, where it lies over that (the nearest four)
        GLfloat lowerShape[4 * 4] = {};
        GLfloat lowerCorner[4 * 2] = {};
        GLfloat lowerTint[4 * 3] = {};
        int lowerCount = 0;
        for (int j = i - 1; j >= 0 && lowerCount < 4; j--) {
            const Surface &lower = surfaces[j];
            if (lower.opacity <= 0 || regions[j].around.isEmpty() || !lower.rect.intersects(r)) continue;
            const QRectF l = lower.rect;
            const GLfloat shape[4] = {GLfloat(l.center().x()), GLfloat(l.center().y()), GLfloat(l.width() / 2), GLfloat(l.height() / 2)};
            std::copy(shape, shape + 4, lowerShape + 4 * lowerCount);
            lowerCorner[2 * lowerCount] = GLfloat(lower.reach);
            lowerCorner[2 * lowerCount + 1] = GLfloat(lower.exponent);
            const QVector3D lab = oklabOf(lower.tint);
            lowerTint[3 * lowerCount] = lab.x();
            lowerTint[3 * lowerCount + 1] = lab.y();
            lowerTint[3 * lowerCount + 2] = lab.z();
            lowerCount++;
        }
        f->glUniform1i(d->glass["lowerCount"], lowerCount);
        f->glUniform4fv(d->glass["lowerShape"], 4, lowerShape);
        f->glUniform2fv(d->glass["lowerCorner"], 4, lowerCorner);
        f->glUniform3fv(d->glass["lowerTint"], 4, lowerTint);
        f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }

    d->timing.mark();
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    for (GLenum unit : {GL_TEXTURE5, GL_TEXTURE4, GL_TEXTURE3}) {
        f->glActiveTexture(unit);
        f->glBindTexture(GL_TEXTURE_2D, 0);
    }
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    d->glass->release();
    if (d->vao.isCreated()) d->vao.release();
}
