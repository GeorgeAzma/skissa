/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisOpenGLGlass.h"

#include <algorithm>
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

/// The most the ambient blur reaches either way, in the small pixels it is made in
constexpr int maxTaps = 32;

/// The levels of the blur: the canvas, then each half as large as the one before
constexpr int maxLevels = 8;
/// How much a level blurs (beyond the first, the canvas): its variance, in its own pixels. Enough that it does not shimmer as the canvas moves under it
constexpr qreal levelVariance = 0.5;
/// How much reading a level through a cubic B-spline blurs it: its variance, in the level's pixels
constexpr qreal bsplineVariance = 1.0 / 3.0;
/**
 * The fine levels (the canvas, a half, a quarter), made only around the
 * rims, where the blur is narrow; the coarse ones beyond them from the
 * canvas shrunk by a box, cheaply, all over the panels
 */
constexpr int fineLevels = 3;
/// What a box of 4 x 4 blurs, its variance in its own (quarter) pixels
constexpr qreal boxVariance = (16.0 - 1) / 12 / 16;

/// How blurred level \p k is as it is read: the sigma of a gaussian as wide, in pixels of the canvas
qreal levelSigma(int k)
{
    return k ? std::ldexp(std::sqrt(levelVariance + bsplineVariance), k) : std::sqrt(bsplineVariance);
}

/// A strip of four corners, of rect (x0, y0, x1, y1, in clip coordinates)
const char *const quadSource = R"(
uniform vec4 rect;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
}
)";

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

/**
 * A step from 0 (at x = 0 and before) to 1 (at x = 1 and after), about
 * straight between, every derivative 0 at both ends: what changes along it
 * changes without a seam of any order
 */
float rise(float x)
{
    x = clamp(x, 1e-3, 1.0 - 1e-3);
    return 1.0 / (1.0 + exp(clamp(0.5 * (1.0 / x - 1.0 / (1.0 - x)), -80.0, 80.0)));
}

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
)";

/**
 * A pixel of the canvas as the blur mixes it: colors as seen (Oklab), a
 * saturated one counting more (see KisStudioGlass::salience; its chroma is
 * about 0.32 at most, in sRGB). Kept: its lightness to the power (see
 * KisStudioGlass::lightnessPower) and its two axes of color, a and b (half
 * added), times its weight, and the weight: a mean is the one over the other.
 */
const char *const weighingSource = R"(
uniform sampler2D source;
uniform bool decode;
uniform float salience;
uniform float power;

vec4 weighed(ivec2 p)
{
    vec3 c = texelFetch(source, min(p, textureSize(source, 0) - 1), 0).rgb;
    vec3 lab = toOklab(clamp(decode ? fromSrgb(c) : c, 0.0, 1.0));
    float weight = 1.0 + salience * length(lab.yz);
    return vec4(vec3(pow(max(lab.x, 0.0), power), lab.yz + 0.5) * weight, weight);
}
)";

/// The first level, the canvas itself
const char *const weighSource = R"(
void main()
{
    fragColor = weighed(ivec2(gl_FragCoord.xy));
}
)";

/**
 * The canvas four times smaller, each pixel the mean of the 4 x 4 it stands
 * for: what the coarse levels are made from (never read itself, it would
 * shimmer as the canvas moves under it)
 */
const char *const shrinkSource = R"(
void main()
{
    ivec2 base = ivec2(gl_FragCoord.xy) * 4;
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) sum += weighed(base + ivec2(x, y));
    }
    fragColor = sum / 16.0;
}
)";

/**
 * A level of the blur from the one before, half as large: each pixel a
 * gaussian of the 8 x 8 of the one before around it, read four at a time
 * (bilinear): offsets of the two reads either way (pixels of the one
 * before), and their weights.
 */
const char *const halveSource = R"(
uniform sampler2D source;
uniform vec4 taps;

void main()
{
    vec2 size = vec2(textureSize(source, 0));
    vec2 middle = 2.0 * floor(gl_FragCoord.xy) + 1.0;
    vec4 offsets = vec4(-taps.y, -taps.x, taps.x, taps.y);
    vec4 weights = vec4(taps.w, taps.z, taps.z, taps.w);
    vec4 sum = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            sum += weights[x] * weights[y] * textureLod(source, (middle + vec2(offsets[x], offsets[y])) / size, 0.0);
        }
    }
    fragColor = sum;
}
)";

/**
 * Reading the levels: one, through a cubic B-spline (four bilinear reads),
 * smooth to the second order between its pixels however magnified; and the
 * blur of any width, the two levels about as wide on either side of it,
 * mixed by rise(), so that a blur growing smoothly over the glass grows as
 * smoothly in what it shows (each level about twice as wide as the last).
 */
const char *const levelsSource = R"(
uniform sampler2D level0;
uniform sampler2D level1;
uniform sampler2D level2;
uniform sampler2D level3;
uniform sampler2D level4;
uniform sampler2D level5;
uniform sampler2D level6;
uniform sampler2D level7;
uniform float levelLambda[8];
uniform int levelCount;
uniform float power;

vec4 bspline(sampler2D level, vec2 q, float scale)
{
    vec2 size = vec2(textureSize(level, 0));
    vec2 t = q * scale - 0.5;
    vec2 i = floor(t);
    vec2 f = t - i;
    vec2 g = 1.0 - f;
    vec2 w0 = g * g * g / 6.0;
    vec2 w1 = (4.0 - 6.0 * f * f + 3.0 * f * f * f) / 6.0;
    vec2 w3 = f * f * f / 6.0;
    vec2 a = w0 + w1;
    vec2 b = 1.0 - a;
    vec2 at0 = (i - 0.5 + w1 / a) / size;
    vec2 at1 = (i + 1.5 + w3 / max(b, 1e-6)) / size;
    return a.y * (a.x * textureLod(level, at0, 0.0) + b.x * textureLod(level, vec2(at1.x, at0.y), 0.0))
        + b.y * (a.x * textureLod(level, vec2(at0.x, at1.y), 0.0) + b.x * textureLod(level, at1, 0.0));
}

vec4 levelAt(int k, vec2 q)
{
    if (k <= 0) return bspline(level0, q, 1.0);
    if (k == 1) return bspline(level1, q, 0.5);
    if (k == 2) return bspline(level2, q, 0.25);
    if (k == 3) return bspline(level3, q, 0.125);
    if (k == 4) return bspline(level4, q, 0.0625);
    if (k == 5) return bspline(level5, q, 0.03125);
    if (k == 6) return bspline(level6, q, 0.015625);
    return bspline(level7, q, 0.0078125);
}

// the canvas at q (surface pixels) blurred as a gaussian of sigma 2^lambda (pixels) blurs it, in Oklab
vec3 seenAt(vec2 q, float lambda)
{
    lambda = clamp(lambda, levelLambda[0], levelLambda[levelCount - 1]);
    int k = 0;
    for (int i = 1; i < 7; i++) {
        if (i < levelCount - 1 && lambda >= levelLambda[i]) k = i;
    }
    float s = rise((lambda - levelLambda[k]) / (levelLambda[k + 1] - levelLambda[k]));
    vec4 m = s < 1e-6 ? levelAt(k, q) : s > 1.0 - 1e-6 ? levelAt(k + 1, q) : mix(levelAt(k, q), levelAt(k + 1, q), s);
    return vec3(pow(max(m.x / max(m.a, 1e-5), 0.0), 1.0 / power), m.yz / max(m.a, 1e-5) - 0.5);
}
)";

/// For the ambient light: half of a gaussian, across or down
const char *const blurSource = R"(
uniform sampler2D source;
uniform ivec2 direction;
uniform int taps;
uniform float weights[33];

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    ivec2 last = textureSize(source, 0) - 1;
    vec4 sum = texelFetch(source, p, 0) * weights[0];
    for (int i = 1; i <= taps; i++) {
        sum += (texelFetch(source, clamp(p + direction * i, ivec2(0), last), 0) + texelFetch(source, clamp(p - direction * i, ivec2(0), last), 0)) * weights[i];
    }
    fragColor = sum;
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
uniform vec4 shape;
uniform float tintLightness;
uniform vec2 tintStrength;
uniform float spacing;
uniform float frostLambda;
flat out float strength;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);

    // how light it is behind, as seen (Oklab): a grid as fine as the blur is wide (12 x 12 at most), of the
    // widest level (at least as blurred as the frost), so that the tint does not flicker as the canvas slides under it
    ivec2 count = clamp(ivec2(ceil(2.0 * shape.zw / spacing)), ivec2(1), ivec2(12));
    float scale = exp2(-float(levelCount - 1));
    vec4 sum = vec4(0.0);
    for (int y = 0; y < count.y; y++) {
        for (int x = 0; x < count.x; x++) {
            vec2 q = shape.xy + shape.zw * (2.0 * (vec2(x, y) + 0.5) / vec2(count) - 1.0);
            vec4 m = levelCount > 7 ? textureLod(level7, q * scale / vec2(textureSize(level7, 0)), 0.0)
                : levelCount > 6 ? textureLod(level6, q * scale / vec2(textureSize(level6, 0)), 0.0)
                : levelCount > 5 ? textureLod(level5, q * scale / vec2(textureSize(level5, 0)), 0.0)
                : textureLod(level4, q * scale / vec2(textureSize(level4, 0)), 0.0);
            sum += vec4(pow(max(m.x / max(m.a, 1e-5), 0.0), 1.0 / power), 0.0, 0.0, 1.0);
        }
    }
    strength = mix(tintStrength.x, tintStrength.y, clamp(abs(sum.x / sum.a - tintLightness), 0.0, 1.0));
}
)";

/**
 * A glass panel: a pane of glass held a little over the canvas, its face
 * frosted, its edge a polished bevel rounding down to the rim. Each pixel
 * is the ray seen there, followed down through the glass to the canvas, and
 * blurred there as much as the glass scatters it:
 *
 *  - where the face is flat it goes straight through, blurred as the frost;
 *  - on the bevel it is bent by the slope it enters (Snell's law), again as
 *    it leaves the flat underside: what is further in, squeezed toward the
 *    rim (each color by its own index of refraction: a faint fringe of
 *    color, as glass disperses light);
 *  - where the bevel is steep the face reflects (Fresnel) the canvas around
 *    the panel, as blurred as the frost.
 *
 * The blur is one, of any width (see seenAt()): polished at the rim, and
 * growing smoothly into the frost, so that what the rim shows clear is the
 * frost's own picture, sharper, and never a picture fading over another.
 * Everything here is smooth to every order over the glass (but the very
 * edge, antialiased), the shape of the bevel too: it meets the face flat
 * to every order. No highlights, no lighting (but Liquid's gloss): the
 * edge shows by what it bends. All of it is worked out in light, what is
 * behind tinted as tinted glass does (see KisStudioGlass::veil).
 */
const char *const glassSource = R"(
uniform sampler2D under;
uniform bool decode;
uniform vec4 shape;
uniform vec2 corner;
uniform vec3 tintLab;
uniform float opacity;
uniform vec2 viewport;
uniform vec4 beyond;
uniform float dither;
uniform float veil;
uniform vec3 bevel;
uniform float polish;
uniform float clearing;
uniform float gloss;
uniform vec3 ior;
uniform float frostLambda;
uniform float frostSigma;
// the glass under this one (a menu over a panel): its shape, its corners, its tint (Oklab), how strongly it tints
uniform int lowerCount;
uniform vec4 lowerShape[4];
uniform vec2 lowerCorner[4];
uniform vec3 lowerTint[4];
uniform float lowerStrength;
flat in float strength;

// how much of the blur at d pixels in from an edge of the canvas (blurred sigma wide there) is of the canvas.
// Where the panel ends within the canvas, all of it: beyond the edge (the window's, in the studio layout) the
// blur goes on as the canvas there (its textures are clamped), never a haze of the panel color along the
// window's edges. Across the edge (crossing, where the panel goes on beyond the canvas, plain) none at the
// edge, for the glass to meet the plain panel there without a seam (about erf(d / (sigma sqrt 2))).
float within(float d, float crossing, float sigma)
{
    return mix(1.0, max(tanh(0.85 * d / sigma), 0.0), crossing);
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

// the bevel d pixels in from the rim, w wide, h thick: its height over the underside, and its slope. Near the
// rim a quarter of a superellipse of exponent 4, h (1 - u^4)^(1/4) (steep at the rim), with exp(4 (1 - 1/u))
// for u^4 (the same at the rim, and flat to every order where the bevel meets the face)
vec2 profile(float d, float w, float h)
{
    float u = clamp(1.0 - d / w, 1e-3, 1.0);
    float g = exp(4.0 - 4.0 / u);
    float k = max(1.0 - g, 1e-6);
    return vec2(h * pow(k, 0.25), h / w * pow(k, -0.75) * g / (u * u));
}

// how far in (along the inward normal) the ray seen at a point of the bevel of slope k, its face height over
// the underside, meets the canvas, through glass of index n: bent as it enters the face, again as it leaves
// the flat underside, then on across the gap. Toward the rim the ray would leave the underside grazing it,
// then not at all (total internal reflection): the sine it leaves at is held under 0.9 there, smoothly, and
// the rim shows the canvas far in, squeezed, never a pixel that jumps
float bend(float k, float height, float n)
{
    float c = inversesqrt(1.0 + k * k);
    float s = k * c;
    float st = s / n;
    float ct = sqrt(1.0 - st * st);
    // in the glass, at an angle of sin sa from straight down; out of it at x
    float sa = s * ct - c * st;
    float ca = c * ct + s * st;
    float x = n * sa;
    float r = x / 0.9;
    r *= r;
    r *= r;
    x *= pow(1.0 + r * r, -0.125);
    return height * sa / ca + bevel.z * x * inversesqrt(1.0 - x * x);
}

// how much light the glass reflects, seen at an angle of acos(c) to its face (Schlick)
float fresnel(float c, float n)
{
    float r = (n - 1.0) / (n + 1.0);
    r *= r;
    return r + (1.0 - r) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0);
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
    float scattered = s * mix(veil, 1.0, rise((l - b.x + 0.02) / 0.08));
    return inGamut(l, mix(b.yz, tintLab.yz, scattered));
}

/**
 * What is seen at p (lab, Oklab) through the glass under this one, where
 * this one lies over it (a menu over a panel): tinted by each as its own
 * glass tints it, glass over glass, not the canvas as if they were not
 * there. Their edges as soft as this glass makes what is under it there.
 */
vec3 throughLower(vec3 lab, vec2 p, float sigma)
{
    for (int i = 0; i < 4; i++) {
        if (i >= lowerCount) break;
        vec2 n;
        float d = outsideOf(p, lowerShape[i], lowerCorner[i], n);
        float cover = 1.0 - rise(0.5 + d / max(2.0 * sigma, 1.0));
        if (cover > 0.0) lab = mix(lab, toOklab(tintedLab(lab, lowerTint[i], lowerStrength)), cover);
    }
    return lab;
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 normal2d;
    float d = -outsideOf(p, shape, corner, normal2d);
    float coverage = clamp(d + 0.5, 0.0, 1.0) * opacity;
    if (coverage <= 0.0) {
        discard;
    }

    // the bevel: no wider than the corners are round, nor than the panel is
    float w = min(bevel.x, min(corner.x, min(shape.z, shape.w)));
    bool bevelled = w > 0.5;
    float h = bevel.y * w / max(bevel.x, 1e-3);
    // how much the glass blurs (lambda, the log of the gaussian's sigma): polished toward the rim, clear over
    // its outer part, then more and more frosted on into the face, as the frost from as far in as a frost's
    // width or as its corners reach (where the distance from the edge is smooth)
    float clearTo = clearing * w;
    float frostFrom = max(min(w + 0.75 * frostSigma, corner.x), clearTo + 1.0);
    float rimLambda = mix(frostLambda, levelLambda[0], bevelled ? polish : 0.0);
    float lambda = mix(rimLambda, frostLambda, rise((d - clearTo) / (frostFrom - clearTo)));
    float sigma2 = exp2(2.0 * lambda);
    float sigma = sqrt(sigma2);
    // of the canvas, not of the panel color beyond its edges
    float canvas = within(p.x, beyond.x, sigma) * within(viewport.x - p.x, beyond.y, sigma) * within(p.y, beyond.z, sigma)
        * within(viewport.y - p.y, beyond.w, sigma);

    // the bevel's height and slope here (on the face: all of it, flat)
    vec2 b = bevelled && d < w ? profile(max(d, 0.0), w, h) : vec2(h, 0.0);
    // how much a pixel's ray moves across the canvas from one pixel in to the next: where the bevel squeezes much
    // of the canvas into a pixel, that much more blurred (never any of it skipped: no shimmer, no stair steps)
    float squeeze = 1.0;
    if (b.y > 0.0) {
        float at = max(d, 0.5);
        vec2 b0 = profile(at - 0.5, w, h);
        vec2 b1 = profile(at + 0.5, w, h);
        squeeze += bend(b1.y, b1.x, ior.g) - bend(b0.y, b0.x, ior.g);
    }
    float seenLambda = 0.5 * log2(sigma2 + 0.25 * squeeze * squeeze);

    vec3 light;
    if (b.y > 0.0) {
        for (int i = 0; i < 3; i++) {
            vec2 q = p - normal2d * bend(b.y, b.x, ior[i]);
            light[i] = max(fromOklab(mix(tintLab, throughLower(seenAt(q, seenLambda), q, sigma), canvas)), 0.0)[i];
        }
    } else {
        light = max(fromOklab(mix(tintLab, throughLower(seenAt(p, seenLambda), p, sigma), canvas)), 0.0);
    }
    // thinner toward the rim, the glass tints what is behind it less (Beer-Lambert): a band along the edge, lighter
    // over a light canvas, deeper over a dark one, as a border would be
    vec3 color = tintedLab(toOklab(light), tintLab, strength * mix(0.3, 1.0, bevelled ? b.x / h : 1.0));
    if (b.y > 0.0) {
        // where it is steep the face reflects (Fresnel) down onto the canvas around it: as frosted as the face,
        // the glass is not a mirror (no gloss: the bevel shows by what it bends); up, the room, which is in the
        // tint already
        float c = inversesqrt(1.0 + b.y * b.y);
        float s = b.y * c;
        float down = 1.0 - 2.0 * c * c;
        float faceOn = fresnel(1.0, ior.g);
        float reflected = (fresnel(c, ior.g) - faceOn) / (1.0 - faceOn) * rise(4.0 * down);
        vec2 r = p + normal2d * (2.0 * c * s) * (b.x + bevel.z) * inversesqrt(down * down + 0.25);
        color = mix(color, max(fromOklab(seenAt(r, frostLambda)), 0.0), reflected);
        if (gloss > 0.0) {
            // the edge catches a light from the top left where it faces it, and faintly where it faces away
            // (lit through the glass): only where it is steep, a thin line along the rim
            float facing = dot(normal2d, normalize(vec2(-1.0, -1.0)));
            float lit = facing * facing * (0.675 + 0.325 * tanh(4.0 * facing));
            color += vec3(gloss * lit * pow(1.0 - c, 1.5));
        }
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
        vec3 behind = texelFetch(under, ivec2(p), 0).rgb;
        color = mix(decode ? fromSrgb(behind) : behind, color, coverage);
    }
    fragColor = vec4(toSrgb(color) + steps * dither, 1.0);
}
)";

/**
 * The ambient light: the canvas made sixteen times smaller (every other pixel
 * of each 16 x 16), only where the image is (toImage takes a pixel to the
 * image's own coordinates, 0 to 1 across it): its light, in linear, and how
 * much of the pixel is image
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
    fragColor = sum / count;
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
    vec4 a = texelFetch(ambient, clamp(i, ivec2(0), last), 0);
    vec4 b = texelFetch(ambient, clamp(i + ivec2(1, 0), ivec2(0), last), 0);
    vec4 c = texelFetch(ambient, clamp(i + ivec2(0, 1), ivec2(0), last), 0);
    vec4 d = texelFetch(ambient, clamp(i + ivec2(1, 1), ivec2(0), last), 0);
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

/// \p shared (the levels, for the glass) goes in both stages, after commonSource
QOpenGLShaderProgram *compile(const char *vertex, const char *fragment, const char *shared)
{
    QScopedPointer<QOpenGLShaderProgram> result(new QOpenGLShaderProgram());
    if (!result->addShaderFromSourceCode(QOpenGLShader::Vertex, header() + commonSource + shared + vertex)
        || !result->addShaderFromSourceCode(QOpenGLShader::Fragment, header() + "out vec4 fragColor;\n" + commonSource + shared + fragment)
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

    bool make(const char *vertex, const char *fragment, const QList<QByteArray> &uniforms, const char *shared = "")
    {
        program.reset(compile(vertex, fragment, shared));
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

/**
 * The gaussian a level is made with from the one before (see halveSource),
 * for it to blur \p variance more (pixels of the one before): its 8 taps
 * either way of the middle, at 0.5 to 3.5, two to a bilinear read
 */
QVector4D halvingTaps(qreal variance)
{
    auto weightsOf = [](qreal sigma, qreal *w) {
        for (int i = 0; i < 4; i++) w[i] = std::exp(-0.5 * std::pow((i + 0.5) / sigma, 2));
    };
    // the sigma whose taps blur that much (the taps cut it off: more than the variance of their gaussian)
    qreal lo = 0.05, hi = 10;
    qreal w[4];
    for (int step = 0; step < 50; step++) {
        const qreal sigma = 0.5 * (lo + hi);
        weightsOf(sigma, w);
        qreal sum = 0, moment = 0;
        for (int i = 0; i < 4; i++) {
            sum += w[i];
            moment += w[i] * (i + 0.5) * (i + 0.5);
        }
        (moment / sum < variance ? lo : hi) = sigma;
    }
    weightsOf(0.5 * (lo + hi), w);
    const qreal sum = 2 * (w[0] + w[1] + w[2] + w[3]);
    const qreal nearWeight = (w[0] + w[1]) / sum;
    const qreal farWeight = (w[2] + w[3]) / sum;
    return QVector4D(float(0.5 + w[1] / (w[0] + w[1])), float(2.5 + w[3] / (w[2] + w[3])), float(nearWeight), float(farWeight));
}

/// Read bilinearly, clamped to its edges
void filterLinearly(QOpenGLFunctions *f, GLuint texture)
{
    f->glBindTexture(GL_TEXTURE_2D, texture);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    f->glBindTexture(GL_TEXTURE_2D, 0);
}

/// How far from a glass panel what it shows can be (device pixels): the canvas its bevel bends in, and what it reflects (see glassSource)
qreal seenAround(const KisStudioGlass::Bevel &bevel, qreal pixelRatio)
{
    return 2.1 * (bevel.thickness + bevel.lift) * pixelRatio;
}

/// \p rects, those that overlap made one (their bounds): each part of what they cover once
QVector<QRect> merged(QVector<QRect> rects)
{
    for (bool again = true; again;) {
        again = false;
        for (int i = 0; i < rects.size() && !again; i++) {
            for (int j = i + 1; j < rects.size() && !again; j++) {
                if (rects[i].intersects(rects[j])) {
                    rects[i] |= rects.takeAt(j);
                    again = true;
                }
            }
        }
    }
    return rects;
}

} // namespace

struct KisOpenGLGlass::Private
{
    Program weigh;
    Program shrink;
    Program halve;
    Program glass;
    Program blur;
    Program ambientShrink;
    Program ambient;
    /// The ambient light: the canvas made small, blurred across, then down
    QScopedPointer<QOpenGLFramebufferObject> ambientSmall;
    QScopedPointer<QOpenGLFramebufferObject> ambientAcross;
    QScopedPointer<QOpenGLFramebufferObject> ambientBlurred;
    QOpenGLVertexArrayObject vao;
    /// The levels of the blur (the first the canvas, as it is mixed), and what of each was made last: made again
    /// when the canvas changed, or where more of them is needed
    QScopedPointer<QOpenGLFramebufferObject> levels[maxLevels];
    /// The canvas shrunk, for the coarse levels
    QScopedPointer<QOpenGLFramebufferObject> shrunk;
    QVector<QRect> fineMade;
    QVector<QRect> coarseMade;
    int madeCount {0};
    KisOpenGLGpuTiming timing {QStringLiteral("glass gpu:"), {QStringLiteral("levels"), QStringLiteral("draw")}};
    bool valid {false};

    /// Its levels for a canvas of \p size
    void ensureLevels(QOpenGLFunctions *f, const QSize &size)
    {
        if (levels[0] && levels[0]->size() == size) {
            return;
        }
        for (int k = 0; k < maxLevels; k++) {
            const QSize levelSize(qMax(1, (size.width() + (1 << k) - 1) >> k), qMax(1, (size.height() + (1 << k) - 1) >> k));
            levels[k].reset(new QOpenGLFramebufferObject(levelSize, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, GL_RGBA16F));
            filterLinearly(f, levels[k]->texture());
        }
        shrunk.reset(new QOpenGLFramebufferObject(levels[fineLevels - 1]->size(), QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, GL_RGBA16F));
        filterLinearly(f, shrunk->texture());
        fineMade.clear();
        coarseMade.clear();
        madeCount = 0;
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
    // texelFetch, textureLod, gl_VertexID, flat: GLSL 1.30, or ES 3.00
    if (!context || context->format().majorVersion() < 3) {
        return false;
    }
    // the blur is kept in half floats, drawn to: OpenGL 3 can, OpenGL ES 3 with an extension
    if (context->isOpenGLES() && !context->hasExtension("GL_EXT_color_buffer_half_float") && !context->hasExtension("GL_EXT_color_buffer_float")) {
        return false;
    }
    QList<QByteArray> levelUniforms {"levelLambda", "levelCount", "power"};
    for (int k = 0; k < maxLevels; k++) levelUniforms << QByteArray("level") + QByteArray::number(k);
    d->valid = d->weigh.make(quadSource, weighSource, {"rect", "source", "decode", "salience", "power"}, weighingSource)
        && d->shrink.make(quadSource, shrinkSource, {"rect", "source", "decode", "salience", "power"}, weighingSource)
        && d->halve.make(quadSource, halveSource, {"rect", "source", "taps"})
        && d->glass.make(glassVertexSource, glassSource,
                         levelUniforms
                             + QList<QByteArray>{"rect", "shape", "corner", "tintLab", "tintLightness", "tintStrength", "opacity", "viewport",
                                                 "beyond", "spacing", "dither", "veil", "bevel", "polish", "clearing", "gloss", "ior", "under",
                                                 "decode", "frostLambda", "frostSigma", "lowerCount", "lowerShape", "lowerCorner", "lowerTint",
                                                 "lowerStrength"},
                         levelsSource)
        && d->blur.make(quadSource, blurSource, {"rect", "source", "direction", "taps", "weights"})
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

/// \p transform as a matrix of GLSL (columns)
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
            fbo->reset(new QOpenGLFramebufferObject(smallSize, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, GL_RGBA16F));
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
    auto toLinear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    f->glUniform3f(d->ambient["base"], float(toLinear(base.redF())), float(toLinear(base.greenF())), float(toLinear(base.blueF())));
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
    // the widest frost, read from a level up to twice as wide (three of its sigmas), and what the rim shows around it
    return 6 * KisStudioGlass::blurSigma * KisStudioGlass::maxBlurScale + seenAround(KisStudioGlass::bevel(), 1);
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

    // each panel's frost, as wide as the panel is large (see KisStudioGlass::blurScale), in device pixels
    QVector<qreal> frost(surfaces.size());
    qreal widest = 0;
    for (int i = 0; i < surfaces.size(); i++) {
        frost[i] = KisStudioGlass::blurSigma * KisStudioGlass::blurScale(surfaces[i].rect.size() / pixelRatio) * pixelRatio;
        widest = qMax(widest, frost[i]);
    }
    // as many levels as the widest frost needs, the last at least as wide
    int count = 2;
    while (count < maxLevels && levelSigma(count - 1) < widest) count++;

    /**
     * What of the levels is made: around the panels as far as they show
     * (seenAround); the fine ones as far around that as they read of each
     * other (a few of their pixels), the coarse ones as far as the widest
     * frost reaches (three of its sigmas, and a B-spline). Beyond, the gaussians
     * weigh next to nothing: what was made there last (or nothing yet, which
     * weighs nothing) changes nothing that shows.
     */
    const KisStudioGlass::Bevel bevel = KisStudioGlass::bevel();
    const qreal seen = seenAround(bevel, pixelRatio);
    auto around = [&](qreal margin) {
        const int m = qCeil(margin);
        QVector<QRect> areas;
        for (const Surface &surface : surfaces) {
            const QRect area = surface.rect.toAlignedRect().adjusted(-m, -m, m, m) & whole;
            if (!area.isEmpty()) areas << area;
        }
        return merged(areas);
    };
    const QVector<QRect> fine = around(seen + 8 * (1 << (fineLevels - 1)));
    const QVector<QRect> coarse = count > fineLevels ? around(seen + 3 * widest + 2 * (1 << (count - 1))) : QVector<QRect>();

    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    GLint target = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    d->ensureLevels(f, viewport);
    if (d->vao.isCreated()) d->vao.bind();
    f->glDisable(GL_BLEND);
    f->glEnable(GL_SCISSOR_TEST);
    f->glActiveTexture(GL_TEXTURE0);
    auto scissor = [f](const QRect &r) { f->glScissor(r.x(), r.y(), r.width(), r.height()); };

    d->timing.begin();
    // only the brush outline moved, say: what is behind the glass is what was blurred last time
    auto madeAlready = [](const QVector<QRect> &areas, const QVector<QRect> &made) {
        return std::all_of(areas.cbegin(), areas.cend(), [&](const QRect &area) {
            return std::any_of(made.cbegin(), made.cend(), [&](const QRect &m) { return m.contains(area); });
        });
    };
    const bool again = canvasChanged || count > d->madeCount || !madeAlready(fine, d->fineMade) || !madeAlready(coarse, d->coarseMade);
    if (again) {
        // into a level (k down from the canvas), from source, in areas (pixels of the canvas)
        auto draw = [&](QOpenGLFramebufferObject *into, int k, GLuint source, const QVector<QRect> &areas) {
            into->bind();
            f->glViewport(0, 0, into->width(), into->height());
            f->glBindTexture(GL_TEXTURE_2D, source);
            for (const QRect &a : areas) {
                scissor(QRect(QPoint(a.left() >> k, a.top() >> k), QPoint(a.right() >> k, a.bottom() >> k)));
                f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
            }
        };
        // the canvas as it is mixed, and shrunk
        for (Program *program : {&d->weigh, &d->shrink}) {
            (*program)->bind();
            f->glUniform4f((*program)["rect"], -1, -1, 1, 1);
            f->glUniform1i((*program)["source"], 0);
            f->glUniform1i((*program)["decode"], !linearCanvas);
            f->glUniform1f((*program)["salience"], KisStudioGlass::salience);
            f->glUniform1f((*program)["power"], KisStudioGlass::lightnessPower);
        }
        d->weigh->bind();
        draw(d->levels[0].data(), 0, canvas, fine);
        d->shrink->bind();
        draw(d->shrunk.data(), fineLevels - 1, canvas, coarse);
        // each level from the one before (the first coarse one from the canvas shrunk), as blurred (in its own
        // pixels) as the levels are
        static const QVector4D fromCanvas = halvingTaps(4 * levelVariance);
        static const QVector4D fromLevel = halvingTaps(4 * levelVariance - levelVariance);
        static const QVector4D fromShrunk = halvingTaps(4 * levelVariance - boxVariance);
        d->halve->bind();
        f->glUniform4f(d->halve["rect"], -1, -1, 1, 1);
        f->glUniform1i(d->halve["source"], 0);
        for (int k = 1; k < count; k++) {
            const QVector4D taps = k == 1 ? fromCanvas : k == fineLevels ? fromShrunk : fromLevel;
            f->glUniform4f(d->halve["taps"], taps.x(), taps.y(), taps.z(), taps.w());
            const GLuint source = k == fineLevels ? d->shrunk->texture() : d->levels[k - 1]->texture();
            draw(d->levels[k].data(), k, source, k < fineLevels ? fine : coarse);
        }
        d->fineMade = fine;
        d->coarseMade = coarse;
        d->madeCount = count;
    }

    d->timing.mark();
    // the glass, over what is drawn
    f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(target));
    f->glViewport(0, 0, viewport.width(), viewport.height());
    scissor(drawn);
    f->glEnable(GL_BLEND);
    f->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    d->glass->bind();
    float lambdas[maxLevels] = {};
    for (int k = 0; k < maxLevels; k++) {
        lambdas[k] = float(std::log2(levelSigma(k)));
        f->glUniform1i(d->glass[QByteArray(QByteArray("level") + QByteArray::number(k)).constData()], k);
        f->glActiveTexture(GLenum(GL_TEXTURE0 + k));
        f->glBindTexture(GL_TEXTURE_2D, d->levels[qMin(k, count - 1)]->texture());
    }
    f->glActiveTexture(GLenum(GL_TEXTURE0 + maxLevels));
    f->glBindTexture(GL_TEXTURE_2D, canvas);
    f->glUniform1i(d->glass["under"], maxLevels);
    f->glUniform1fv(d->glass["levelLambda"], maxLevels, lambdas);
    f->glUniform1i(d->glass["levelCount"], count);
    f->glUniform1i(d->glass["decode"], !linearCanvas);
    f->glUniform1f(d->glass["power"], KisStudioGlass::lightnessPower);
    f->glUniform2f(d->glass["tintStrength"], KisStudioGlass::tintOverDark, KisStudioGlass::tintOverBright);
    f->glUniform1f(d->glass["veil"], KisStudioGlass::veil);
    f->glUniform2f(d->glass["viewport"], viewport.width(), viewport.height());
    f->glUniform1f(d->glass["dither"], dither);
    f->glUniform3f(d->glass["bevel"], bevel.width * pixelRatio, bevel.thickness * pixelRatio, bevel.lift * pixelRatio);
    f->glUniform1f(d->glass["polish"], bevel.polish);
    f->glUniform1f(d->glass["clearing"], bevel.clear);
    f->glUniform1f(d->glass["gloss"], bevel.gloss);
    const QVector3D ior = refractiveIndices(bevel.abbeNumber);
    f->glUniform3f(d->glass["ior"], ior.x(), ior.y(), ior.z());
    f->glUniform1f(d->glass["lowerStrength"], float(0.5 * (KisStudioGlass::tintOverDark + KisStudioGlass::tintOverBright)));
    for (int i = 0; i < surfaces.size(); i++) {
        const Surface &surface = surfaces[i];
        const QRectF r = surface.rect;
        if (!r.intersects(whole)) continue;
        const QRectF quad = r.adjusted(-1, -1, 1, 1);
        f->glUniform4f(d->glass["rect"], quad.left() / viewport.width() * 2 - 1, quad.top() / viewport.height() * 2 - 1,
                       quad.right() / viewport.width() * 2 - 1, quad.bottom() / viewport.height() * 2 - 1);
        f->glUniform4f(d->glass["shape"], r.center().x(), r.center().y(), r.width() / 2, r.height() / 2);
        f->glUniform2f(d->glass["corner"], surface.reach, surface.exponent);
        const QVector3D tintLab = oklabOf(surface.tint);
        f->glUniform3f(d->glass["tintLab"], tintLab.x(), tintLab.y(), tintLab.z());
        // as light as it looks (Oklab, about the cube root of its luminance), as what is behind is measured
        auto linear = [](qreal c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
        const qreal luminance = 0.2126 * linear(surface.tint.redF()) + 0.7152 * linear(surface.tint.greenF()) + 0.0722 * linear(surface.tint.blueF());
        f->glUniform1f(d->glass["tintLightness"], float(std::cbrt(luminance)));
        f->glUniform1f(d->glass["opacity"], surface.opacity);
        f->glUniform1f(d->glass["frostLambda"], float(std::log2(frost[i])));
        f->glUniform1f(d->glass["frostSigma"], float(frost[i]));
        f->glUniform1f(d->glass["spacing"], float(2 * frost[i]));
        // the sides it goes on beyond the canvas on: left, right, bottom, top
        f->glUniform4f(d->glass["beyond"], r.left() < 0, r.right() > viewport.width(), r.top() < 0, r.bottom() > viewport.height());
        // the glass under it, where it lies over that (the nearest four)
        GLfloat lowerShape[4 * 4] = {};
        GLfloat lowerCorner[4 * 2] = {};
        GLfloat lowerTint[4 * 3] = {};
        int lowerCount = 0;
        for (int j = i - 1; j >= 0 && lowerCount < 4; j--) {
            const Surface &lower = surfaces[j];
            if (lower.opacity <= 0 || !lower.rect.intersects(whole) || !lower.rect.intersects(r)) continue;
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
    for (int k = maxLevels; k >= 0; k--) {
        f->glActiveTexture(GLenum(GL_TEXTURE0 + k));
        f->glBindTexture(GL_TEXTURE_2D, 0);
    }
    d->glass->release();
    if (d->vao.isCreated()) d->vao.release();
}
