/*
 *  SPDX-FileCopyrightText: 2026 GeorgeAzma <lumi.main.mail@gmail.com>
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "KisOpenGLGlass.h"

#include <array>
#include <cmath>

#include <QHash>
#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLVertexArrayObject>
#include <QPolygonF>
#include <QSharedPointer>
#include <QTransform>
#include <QVector3D>
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

/// Maximum blur taps either way, in downsampled pixels, for the ambient glow: three sigma of its blur.
/// Fewer would cut the gaussian short and make it look like a box blur.
constexpr int maxTaps = 64;

/// Levels of the frost pyramid above the canvas: the coarsest is 2^7 = 128 times smaller, blurred
/// wider than the widest frost.
constexpr int pyramidLevels = 7;

/// A triangle strip covering rect (x0, y0, x1, y1 in clip coordinates).
const char *const quadSource = R"(
uniform vec4 rect;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);
}
)";

/**
 * Intermediate textures store square roots, which keeps about sRGB's precision near 0 in eight bits.
 * The blur weights colors (see KisStudioGlass::salience): RGB holds value times weight and alpha holds
 * the weight, both normalized by the maximum weight, so a mean is RGB divided by alpha.
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

/// Decodes a stored texel into weighted value and weight.
vec4 weighted(vec4 kept)
{
    return vec4(kept.rgb * kept.rgb, kept.a);
}

vec4 keep(vec4 weighted)
{
    return vec4(sqrt(max(weighted.rgb, 0.0)), weighted.a);
}

/// Oklab (Bjoern Ottosson, 2020): a perceptual lightness and two color axes.
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
 * The frost pyramid: the canvas around the panels at half, a quarter, ... of its size, each level a
 * little blurred, so any blur from a pixel to the widest frost is two nearby levels, mixed.
 *
 * Each texel is a mean in Oklab with saturated colors weighted more (Oklab chroma is at most about
 * 0.32 in sRGB): lightness raised to KisStudioGlass::lightnessPower and a, b offset by 0.5, all times
 * the weight, then the weight itself, all normalized by the largest weight. A mean is RGB divided by
 * alpha, and since the weight grows with chroma, alpha also holds the mean chroma. Levels are made by
 * averaging, which is linear in these values; with eight bit textures RGB holds square roots, which
 * keeps about sRGB's precision near 0.
 *
 *  - mode 0 makes level 1 from the canvas, each texel the mean of 2 x 2 canvas pixels;
 *  - mode 1 blurs a level along `direction` with a five tap binomial (a gaussian of one texel);
 *  - mode 2 makes the next level from the one below and blurs it across: each of its texels is the
 *    mean of the 2 x 2 below, one bilinear fetch at their shared corner.
 */
const char *const pyramidSource = R"(
uniform sampler2D source;
uniform int mode;
uniform ivec2 origin;
uniform ivec2 size;
uniform ivec2 sourceOrigin;
uniform ivec2 sourceSize;
uniform vec2 sourceTexel;
uniform ivec2 box;
uniform ivec2 direction;
uniform bool decode;
uniform bool rooted;
uniform float salience;
uniform float power;

vec4 unpack(vec4 v)
{
    return rooted ? vec4(v.rgb * v.rgb, v.a) : v;
}

vec4 pack(vec4 v)
{
    return rooted ? vec4(sqrt(max(v.rgb, 0.0)), v.a) : v;
}

vec4 load(ivec2 i)
{
    return unpack(texelFetch(source, sourceOrigin + clamp(i, ivec2(0), sourceSize - 1), 0));
}

vec4 below(ivec2 i)
{
    i = clamp(i, ivec2(0), size - 1);
    return unpack(texture(source, vec2(sourceOrigin + 2 * i + 1) * sourceTexel));
}

void main()
{
    ivec2 i = ivec2(gl_FragCoord.xy) - origin;
    const float binomial[3] = float[3](6.0 / 16.0, 4.0 / 16.0, 1.0 / 16.0);
    vec4 sum = vec4(0.0);
    if (mode == 0) {
        ivec2 last = textureSize(source, 0) - 1;
        for (int y = 0; y < 2; y++) {
            for (int x = 0; x < 2; x++) {
                vec3 c = texelFetch(source, clamp(box + 2 * i + ivec2(x, y), ivec2(0), last), 0).rgb;
                c = clamp(decode ? fromSrgb(c) : c, 0.0, 1.0);
                vec3 lab = toOklab(c);
                float weight = 1.0 + salience * length(lab.yz);
                sum += vec4(vec3(pow(max(lab.x, 0.0), power), lab.yz + 0.5) * weight, weight);
            }
        }
        sum /= 4.0 * (1.0 + salience * 0.33);
    } else if (mode == 1) {
        for (int t = -2; t <= 2; t++) sum += binomial[abs(t)] * load(i + direction * t);
    } else {
        for (int t = -2; t <= 2; t++) sum += binomial[abs(t)] * below(i + ivec2(t, 0));
    }
    fragColor = pack(sum);
}
)";

/// One separable gaussian pass, across or down, clamped to a surface's slot (its first and last texel).
const char *const blurSource = R"(
uniform sampler2D source;
uniform ivec4 slot;
uniform ivec2 direction;
uniform int taps;
uniform float weights[65];

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

/// Reading the frost pyramid, shared by the glass's vertex and fragment shaders.
const char *const pyramidReadSource = R"(
uniform sampler2D pyramid;
uniform vec2 pyramidTexel;
// The surface pixel at the pyramid's corner, and each level's corner and size in the pyramid texture.
uniform vec2 box;
uniform vec2 levelOrigin[8];
uniform vec2 levelSize[8];
uniform int levels;
uniform bool rooted;
uniform float power;
uniform float salience;

vec4 unpack(vec4 v)
{
    return rooted ? vec4(v.rgb * v.rgb, v.a) : v;
}

// Level k bilinearly at texel position i (texel centers are whole numbers), never past its edge.
vec4 fetchLevel(int k, vec2 i)
{
    i = clamp(i, vec2(0.0), levelSize[k] - 1.0);
    return unpack(texture(pyramid, (levelOrigin[k] + i + 0.5) * pyramidTexel));
}

// The mean in Oklab held in m, and in z the mean chroma (see pyramidSource).
vec4 labOf(vec4 m)
{
    vec3 mean = m.rgb / max(m.a, 1e-5);
    float chroma = max((m.a * (1.0 + salience * 0.33) - 1.0) / salience, 0.0);
    return vec4(pow(max(mean.x, 0.0), 1.0 / power), mean.yz - 0.5, chroma);
}
)";

/**
 * A glass panel's quad, plus its tint strength: enough for its content to stay readable over what is
 * behind the whole panel (more over a bright canvas, less over a dark one). It is uniform across the
 * panel so the background keeps its light and dark, and computed in the vertex shader.
 */
const char *const glassVertexSource = R"(
uniform vec4 rect;
uniform vec4 shape;
uniform float tintLightness;
uniform vec2 tintStrength;
uniform float spacing;
flat out float strength;

void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    gl_Position = vec4(mix(rect.xy, rect.zw, corner), 0.0, 1.0);

    // Average Oklab lightness behind the panel, sampled on a grid as fine as the frost is wide
    // (12 x 12 at most) from a level about as coarse, so the tint doesn't flicker as the canvas scrolls.
    int k = clamp(int(floor(log2(max(spacing, 1.0)))), 1, levels);
    float scale = exp2(float(k));
    ivec2 count = clamp(ivec2(ceil(2.0 * shape.zw / spacing)), ivec2(1), ivec2(12));
    float sum = 0.0;
    for (int y = 0; y < count.y; y++) {
        for (int x = 0; x < count.x; x++) {
            vec2 p = shape.xy + shape.zw * (2.0 * (vec2(x, y) + 0.5) / vec2(count) - 1.0);
            sum += labOf(fetchLevel(k, (p - box) / scale - 0.5)).x;
        }
    }
    strength = mix(tintStrength.x, tintStrength.y, clamp(abs(sum / float(count.x * count.y) - tintLightness), 0.0, 1.0));
}
)";

/**
 * A glass panel: a pane held slightly above the canvas, its top flat in the middle and curving down to
 * the rim, its underside flat. Every pixel is the same model; there is no separate edge:
 *
 *  - The view ray refracts into the top (Snell's law) and out of the underside, and crosses the gap to
 *    the canvas, landing where the surface's slope sends it: straight through where the top is flat,
 *    bent toward the rim where it curves. Each color channel has its own index of refraction, a faint
 *    dispersion fringe where the bend is strong.
 *  - The top is frosted: it scatters what passes into a cone, so the canvas is seen blurred by how far
 *    the light still travels below it, times how rough it is there. Toward the rim the glass is thinner
 *    and polished, so the blur shrinks smoothly to almost none. Where the bend squeezes much canvas into
 *    a pixel, that adds blur too, so nothing shimmers. One blur, read from the frost pyramid.
 *  - It is tinted by how much glass the light crossed (Beer-Lambert), less toward the thin rim, and the
 *    steep rim reflects (Fresnel) the canvas around the panel.
 *
 * The surface is built on a distance from the rim that is exact along the sides and a squircle around
 * the corners, with its gradient worked out rather than estimated, so the slope is smooth everywhere and
 * the corners curve like the sides. The profile is a cubic, flat where it meets the face. Everything is
 * computed in linear light, tinted like real tinted glass (see KisStudioGlass::veil).
 */
const char *const glassSource = R"(
uniform sampler2D under;
uniform bool decode;
uniform float restore;
uniform vec4 shape;
uniform vec2 corner;
uniform vec3 tint;
uniform float opacity;
uniform vec2 viewport;
uniform float sigma;
uniform float levelSigma[8];
uniform vec4 beyond;
uniform float dither;
uniform float veil;
uniform vec3 bevel;
uniform float polish;
uniform vec3 ior;
uniform vec4 reach;
// The image's ambient glow: where the image is, and its blurred light around it.
uniform bool imageLit;
uniform mat3 toImage;
uniform mat3 fromImage;
uniform sampler2D imageLight;
uniform float imageLightFactor;
uniform float imageLightReach;
// Glass panels below this one (e.g. a panel under a menu): shape, corners, Oklab tint and tint strength.
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

// How much of the frost d pixels inside a canvas edge comes from the canvas. If the panel ends within
// the canvas, all of it: the clamped textures continue the canvas past the window edge instead of a
// haze of panel color. If the panel crosses the edge (crossing = 1) and continues as a plain panel,
// it fades to none at the edge so the glass meets the plain part without a seam.
float within(float d, float crossing)
{
    return mix(1.0, max(erf(d / (sigma * 1.41421356)), 0.0), crossing);
}

/**
 * How far p is inside a squircle shape (center and half size; corner reach and exponent), negative
 * outside, and the gradient of that distance, pointing inward. Along the sides it is the distance to
 * the side; around each corner, a quarter of the superellipse |x|^n + |y|^n = reach^n, it is the reach
 * less the superellipse norm. Its gradient is continuous where they meet, and computed exactly.
 */
float insideOf(vec2 p, vec4 shape, vec2 corner, out vec2 gradient)
{
    vec2 from = p - shape.xy;
    vec2 side = vec2(from.x < 0.0 ? -1.0 : 1.0, from.y < 0.0 ? -1.0 : 1.0);
    vec2 edge = shape.zw - abs(from);
    vec2 q = max(corner.x - edge, 0.0);
    if (q.x > 0.0 && q.y > 0.0) {
        float n = corner.y;
        float g = pow(pow(q.x, n) + pow(q.y, n), 1.0 / n);
        gradient = -pow(q / g, vec2(n - 1.0)) * side;
        return corner.x - g;
    }
    gradient = edge.x < edge.y ? vec2(-side.x, 0.0) : vec2(0.0, -side.y);
    return min(edge.x, edge.y);
}

// Where the view ray through a top with this normal lands on the canvas, from the top `height` above
// the underside, through glass of index n.
vec2 through(vec3 normal, float height, float n)
{
    vec3 t = refract(vec3(0.0, 0.0, -1.0), normal, 1.0 / n);
    vec2 offset = t.xy / max(-t.z, 1e-3) * height;
    float sinGlass = length(t.xy);
    if (sinGlass > 1e-6) {
        float sinAir = min(n * sinGlass, 0.97);
        offset += t.xy / sinGlass * (sinAir / sqrt(1.0 - sinAir * sinAir)) * bevel.z;
    }
    return offset;
}

// Reflectance of the glass at an angle of acos(c) to its normal (Schlick's approximation).
float fresnel(float c, float n)
{
    float r = (n - 1.0) / (n + 1.0);
    r *= r;
    return r + (1.0 - r) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0);
}

// Clamps a point the glass sees to the area around the panel that was made into the pyramid (reach).
// A steep rim can look further than that.
vec2 held(vec2 q)
{
    return clamp(q, reach.xy, reach.zw);
}

// Level k at p (surface pixels): a cubic B-spline (no creases between texels) from four bilinear fetches.
vec4 levelAt(int k, vec2 p)
{
    vec2 t = (p - box) / exp2(float(k)) - 0.5;
    vec2 i = floor(t);
    vec2 f = t - i;
    vec2 f2 = f * f;
    vec2 f3 = f2 * f;
    vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    vec2 w3 = f3 / 6.0;
    vec2 g0 = w0 + w1;
    vec2 g1 = w2 + w3;
    vec2 h0 = i - 1.0 + w1 / g0;
    vec2 h1 = i + 1.0 + w3 / g1;
    return g0.y * (g0.x * fetchLevel(k, h0) + g1.x * fetchLevel(k, vec2(h1.x, h0.y)))
        + g1.y * (g0.x * fetchLevel(k, vec2(h0.x, h1.y)) + g1.x * fetchLevel(k, h1));
}

// The canvas itself at p, in the pyramid's terms.
vec4 canvasAt(vec2 p)
{
    vec3 c = texture(under, p / viewport).rgb;
    c = clamp(decode ? fromSrgb(c) : c, 0.0, 1.0);
    vec3 lab = toOklab(c);
    float weight = 1.0 + salience * length(lab.yz);
    return vec4(vec3(pow(max(lab.x, 0.0), power), lab.yz + 0.5) * weight, weight) / (1.0 + salience * 0.33);
}

/**
 * The canvas around p in Oklab, frosted by a blur of `blur` pixels: the two pyramid levels around it,
 * mixed by where it lies between them in octaves. Averaging a and b never produces a hue that isn't
 * between the inputs (mixing in linear light turns orange and blue into pink), and a colored stroke on
 * white keeps its chroma. Chroma lost where hues disagree is partly restored, never beyond the mean
 * chroma.
 */
vec3 frostLab(vec2 p, float blur)
{
    int k = 0;
    for (int i = 1; i < 8; i++) {
        if (i <= levels && levelSigma[i] <= blur) k = i;
    }
    vec4 m;
    if (k >= levels) {
        m = levelAt(levels, p);
    } else {
        float a = clamp(log2(max(blur, levelSigma[0]) / levelSigma[k]) / log2(levelSigma[k + 1] / levelSigma[k]), 0.0, 1.0);
        m = mix(k == 0 ? canvasAt(p) : levelAt(k, p), levelAt(k + 1, p), a);
    }
    vec4 lab = labOf(m);
    float agree = clamp(length(lab.yz) / max(lab.w, 1e-4), 0.0, 1.0);
    return vec3(lab.x, lab.yz * (1.0 + restore * (1.0 - agree)));
}

// Lightness l with hue ab, its chroma reduced by bisection until it fits the sRGB gamut.
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

// What is behind (Oklab) seen through glass with tint strength s, in linear light: lightness moves
// toward the tint, chroma is kept but slightly veiled (see KisStudioGlass::veil), then gamut mapped.
vec3 tintedLab(vec3 b, vec3 tintLab, float s)
{
    float l = clamp(mix(b.x, tintLab.x, s), 0.0, 1.0);
    float scattered = s * mix(veil, 1.0, smoothstep(-0.02, 0.06, l - b.x));
    return inGamut(l, mix(b.yz, tintLab.yz, scattered));
}

/**
 * Applies the tint of each glass panel below this one (e.g. a panel under a menu) to lab at p, so
 * stacked glass looks like glass over glass. Their edges are softened by this glass' frost.
 */
vec3 throughLower(vec3 lab, vec2 p)
{
    for (int i = 0; i < 4; i++) {
        if (i >= lowerCount) break;
        vec2 g;
        float d = insideOf(p, lowerShape[i], lowerCorner[i], g);
        float cover = clamp(0.5 + d / max(2.0 * sigma, 1.0), 0.0, 1.0);
        if (cover > 0.0) lab = mix(lab, toOklab(tintedLab(lab, lowerTint[i], lowerStrength)), cover);
    }
    return lab;
}

/**
 * Light the glowing image casts on the glass beside it. The rim catches it where it faces the image
 * and the slope is steep enough to reflect it (Fresnel): a soft line in the colors of the image's
 * nearest edge, fading with distance. Nothing over the image itself.
 */
vec3 imageLightAt(vec2 p, vec3 normal, vec2 outward)
{
    vec3 u = toImage * vec3(p, 1.0);
    vec3 n = fromImage * vec3(clamp(u.xy / u.z, 0.0, 1.0), 1.0);
    vec2 q = n.xy / n.z;
    float dist = length(q - p);
    if (dist < 1.0) {
        return vec3(0.0);
    }
    // Bilinear sample of the small, smooth glow texture at the edge.
    vec2 t = q / imageLightFactor - 0.5;
    ivec2 i = ivec2(floor(t));
    vec2 f = t - floor(t);
    ivec2 last = textureSize(imageLight, 0) - 1;
    vec4 a = weighted(texelFetch(imageLight, clamp(i, ivec2(0), last), 0));
    vec4 b = weighted(texelFetch(imageLight, clamp(i + ivec2(1, 0), ivec2(0), last), 0));
    vec4 c = weighted(texelFetch(imageLight, clamp(i + ivec2(0, 1), ivec2(0), last), 0));
    vec4 e = weighted(texelFetch(imageLight, clamp(i + ivec2(1, 1), ivec2(0), last), 0));
    vec4 m = mix(mix(a, b, f.x), mix(c, e, f.x), f.y);
    vec3 light = m.rgb / max(m.a, 1e-4);
    float falloff = 1.0 / (1.0 + (dist / imageLightReach) * (dist / imageLightReach));
    float facing = max(dot(outward, (q - p) / dist), 0.0);
    float faceOn = fresnel(1.0, ior.g);
    float edge = (fresnel(normal.z, ior.g) - faceOn) / (1.0 - faceOn);
    return light * falloff * 0.45 * facing * edge * smoothstep(0.0, 24.0, dist);
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 gradient;
    float d = insideOf(p, shape, corner, gradient);

    // The curve down to the rim is no wider than the corner reach or the panel itself; its thickness
    // scales with it.
    float w = min(bevel.x, min(corner.x, min(shape.z, shape.w)));
    bool curved = w > 0.5;
    float thick = curved ? bevel.y * w / max(bevel.x, 1e-3) : 0.0;
    float u = curved ? clamp(d / w, 0.0, 1.0) : 1.0;
    float height = thick * (1.0 - (1.0 - u) * (1.0 - u) * (1.0 - u));
    vec2 slope = curved ? gradient * (3.0 * thick / w * (1.0 - u) * (1.0 - u)) : vec2(0.0);
    vec3 normal = normalize(vec3(-slope, 1.0));
    vec2 offset = through(normal, height, ior.g);
    vec2 q = p + offset;
    // How much canvas one pixel sees. Taken before any discard, since derivatives are taken across the
    // 2x2 pixel quad.
    float footprint = max(length(dFdx(q)), length(dFdy(q)));

    float coverage = clamp(d / max(length(gradient), 0.5) + 0.5, 0.0, 1.0) * opacity;
    if (coverage <= 0.0) {
        discard;
    }
    // How much comes from the canvas rather than the plain panel beyond its edges.
    float canvas = within(p.x, beyond.x) * within(viewport.x - p.x, beyond.y) * within(p.y, beyond.z) * within(viewport.y - p.y, beyond.w);
    vec3 tintLab = toOklab(fromSrgb(tint));

    // The frost: rough on the face, polished toward the rim, spread over the distance still to travel.
    float rough = 1.0 - polish * (1.0 - u) * (1.0 - u);
    float full = thick / ior.g + bevel.z;
    float travel = full > 1e-3 ? (height / ior.g + bevel.z) / full : 1.0;
    float spread = sigma * rough * travel;
    float blur = sqrt(spread * spread + 0.25 * footprint * footprint);

    vec3 seen;
    vec2 red = through(normal, height, ior.r);
    vec2 blue = through(normal, height, ior.b);
    if (max(distance(red, offset), distance(blue, offset)) < 0.25) {
        seen = frostLab(held(q), blur);
    } else {
        vec3 light = vec3(max(fromOklab(frostLab(held(p + red), blur)).r, 0.0), max(fromOklab(frostLab(held(q), blur)).g, 0.0),
                          max(fromOklab(frostLab(held(p + blue), blur)).b, 0.0));
        seen = toOklab(light);
    }
    seen = throughLower(seen, held(q));

    // Thinner glass toward the rim tints less (Beer-Lambert), a band along the edge that reads like a border.
    float crossed = thick > 0.0 ? mix(0.3, 1.0, height / thick) : 1.0;
    float s = 1.0 - pow(1.0 - clamp(strength, 0.0, 1.0), crossed);
    vec3 color = tintedLab(mix(tintLab, seen, canvas), tintLab, s);

    if (curved && u < 1.0) {
        // Steep parts reflect (Fresnel) the canvas around the panel, frosted. Upward reflections of the
        // room are already part of the tint, so there is no gloss.
        vec3 mirrored = reflect(vec3(0.0, 0.0, -1.0), normal);
        float faceOn = fresnel(1.0, ior.g);
        float reflected = (fresnel(normal.z, ior.g) - faceOn) / (1.0 - faceOn) * smoothstep(0.0, 0.25, -mirrored.z);
        if (reflected > 0.0) {
            vec3 around = max(fromOklab(frostLab(held(p + mirrored.xy / max(-mirrored.z, 0.2) * bevel.z), sigma)), 0.0);
            color = mix(color, around, reflected);
        }
        if (imageLit) {
            color += imageLightAt(p, normal, -gradient / max(length(gradient), 1e-3));
        }
    }

    /**
     * Triangular dither of one target step either way (the sum of two uniform noises) turns banding
     * in smooth gradients into invisible noise without any pattern. No grain is added otherwise.
     */
    uvec2 h = uvec2(p);
    float steps = hash(h) + hash(h + uvec2(7919u, 104729u)) - 1.0;
    if (coverage < 1.0) {
        /**
         * A partly covered pixel (an edge, or glass fading in or out) mixes with the canvas here in
         * linear light. Blending would mix encoded values, making edges too dark over a light canvas
         * and too light over a dark one.
         */
        vec3 behind = texture(under, p / viewport).rgb;
        color = mix(decode ? fromSrgb(behind) : behind, color, coverage);
    }
    fragColor = vec4((decode ? toSrgb(color) : color) + steps * dither, 1.0);
}
)";

/**
 * Downsamples the canvas for the ambient glow, sampling every other pixel of each factor x factor
 * block and only inside the image (toImage maps to 0..1 across it). Stores linear light and image
 * coverage in the same encoding as the blur.
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
 * Draws the ambient glow around (not over) the image: the blurred image light, scaled by how much
 * image is nearby so it falls off with distance, slightly boosted in chroma (a wall lit by a screen
 * shows its colors more than its grays), added to the surrounding color.
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

QOpenGLShaderProgram *compile(const QByteArray &vertex, const QByteArray &fragment)
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

/// A shader program and its uniform locations.
struct Program {
    QSharedPointer<QOpenGLShaderProgram> program;
    QHash<QByteArray, int> locations;

    bool make(const QByteArray &vertex, const QByteArray &fragment, const QList<QByteArray> &uniforms)
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
 * Refractive indices for red, green and blue light (610, 550 and 465 nm), derived from the index for
 * yellow light and the Abbe number using Cauchy's equation n = A + B / wavelength^2.
 */
QVector3D refractiveIndices(qreal abbeNumber)
{
    const qreal nd = KisStudioGlass::refractiveIndex;
    // Spread from blue (F line, 486.1 nm) to red (C line, 656.3 nm).
    const qreal spread = (nd - 1) / abbeNumber;
    const qreal b = spread / (1 / (0.4861 * 0.4861) - 1 / (0.6563 * 0.6563));
    const qreal a = nd - b / (0.5876 * 0.5876);
    auto at = [a, b](qreal micrometers) { return float(a + b / (micrometers * micrometers)); };
    return QVector3D(at(0.610), at(0.550), at(0.465));
}

struct Programs {
    Program pyramidPass;
    Program blur;
    Program glass;
    Program ambientShrink;
    Program ambient;
    bool valid {false};

    void make()
    {
        valid = pyramidPass.make(quadSource, pyramidSource,
                                 {"rect", "source", "mode", "origin", "size", "sourceOrigin", "sourceSize", "sourceTexel", "box",
                                  "direction", "decode", "rooted", "salience", "power"})
            && blur.make(quadSource, blurSource, {"rect", "source", "slot", "direction", "taps", "weights"})
            && glass.make(QByteArray(pyramidReadSource) + glassVertexSource, QByteArray(pyramidReadSource) + glassSource,
                          {"rect", "shape", "tintLightness", "tintStrength", "spacing", "pyramid", "pyramidTexel", "box", "levelOrigin",
                           "levelSize", "levels", "rooted", "power", "salience", "under", "decode", "restore", "corner", "tint", "opacity",
                           "viewport", "sigma", "levelSigma", "beyond", "dither", "veil", "bevel", "polish", "ior", "reach",
                           "imageLit", "toImage", "fromImage", "imageLight", "imageLightFactor", "imageLightReach",
                           "lowerCount", "lowerShape", "lowerCorner", "lowerTint", "lowerStrength"})
            && ambientShrink.make(quadSource, ambientShrinkSource, {"rect", "source", "factor", "decode", "toImage"})
            && ambient.make(quadSource, ambientSource, {"rect", "ambient", "factor", "toImage", "base", "strength", "encode", "dither"});
    }
};

/**
 * The blur of each pyramid level as it is read (a cubic B-spline over its texels), in surface pixels;
 * level 0 is the canvas itself, read bilinearly. Each level adds the spread of averaging 2 x 2 texels
 * of the one below and of its own one texel binomial.
 */
const std::array<float, pyramidLevels + 1> &levelSigmas()
{
    static const std::array<float, pyramidLevels + 1> sigmas = []() {
        std::array<float, pyramidLevels + 1> result {};
        qreal spread = 1.0 / 12;
        result[0] = float(std::sqrt(spread + 1.0 / 6));
        for (int k = 1; k <= pyramidLevels; k++) {
            const qreal texel = qreal(1 << k);
            spread += (texel / 4) * (texel / 4) + texel * texel;
            result[k] = float(std::sqrt(spread + texel * texel / 3));
        }
        return result;
    }();
    return sigmas;
}

/**
 * Programs shared by all contexts in a share group (all of Krita's canvases), so the slow glass shader
 * compiles only once. Parented to the group, so it is destroyed with it.
 */
struct SharedPrograms : public QObject {
    Programs programs;
};

const Programs &programsOf(QOpenGLContextGroup *group)
{
    for (QObject *child : group->children()) {
        if (auto *shared = dynamic_cast<SharedPrograms *>(child)) return shared->programs;
    }
    auto *shared = new SharedPrograms;
    shared->programs.make();
    shared->setParent(group);
    return shared->programs;
}

} // namespace

struct KisOpenGLGlass::Private : Programs
{
    /// The ambient glow: downsampled canvas, blurred across, then down.
    QScopedPointer<QOpenGLFramebufferObject> ambientSmall;
    QScopedPointer<QOpenGLFramebufferObject> ambientAcross;
    QScopedPointer<QOpenGLFramebufferObject> ambientBlurred;
    /// Where the image was when the glow was last made (unit square to surface), and the downsampling factor.
    QTransform imageToSurface;
    int ambientFactor {0};
    /// What the glow was last made for; it is in the canvas until the canvas is drawn again without it.
    QTransform ambientSurfaceToImage;
    QColor ambientBase;
    QSize ambientViewport;
    qreal ambientRatio {0};
    QOpenGLVertexArrayObject vao;
    /// The frost pyramid (see pyramidSource), and the texture each level's across blur goes through.
    QScopedPointer<QOpenGLFramebufferObject> pyramid;
    QScopedPointer<QOpenGLFramebufferObject> pyramidAcross;
    /// Intermediate texture format: half floats where renderable, eight bits otherwise.
    GLenum keptFormat {GL_RGBA};
    /// What the pyramid was last made of: the canvas pixels it covers, and the canvas.
    QRect pyramidBox;
    QSize pyramidViewport;
    uint pyramidCanvas {0};
    uint pyramidFormat {0};
    /// The canvas drawn again since the pyramid was made, in surface pixels; all of it if changedAll.
    QRect changed;
    bool changedAll {true};
    /// Whether canvasRedrawn() was called since the last render().
    bool reported {false};
    KisOpenGLGpuTiming timing {QStringLiteral("glass gpu:"), {QStringLiteral("pyramid"), QStringLiteral("draw")}};

    /**
     * A texture to render to. Keeps the framebuffer that is bound: Qt binds the one it last knew of
     * instead, so the glass of that frame would land off screen.
     */
    QOpenGLFramebufferObject *texture(const QSize &size) const
    {
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
        GLint bound = 0;
        f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &bound);
        auto *fbo = new QOpenGLFramebufferObject(size, QOpenGLFramebufferObject::NoAttachment, GL_TEXTURE_2D, keptFormat);
        f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(bound));
        return fbo;
    }

    /// Grows the pyramid's textures to at least \p size; they never shrink, as panels come and go. Returns
    /// whether they were made anew, losing their content.
    bool ensurePyramid(const QSize &size)
    {
        if (pyramid && pyramid->width() >= size.width() && pyramid->height() >= size.height()) {
            return false;
        }
        const QSize grown = pyramid ? size.expandedTo(pyramid->size()) : size;
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
        for (QScopedPointer<QOpenGLFramebufferObject> *fbo : {&pyramid, &pyramidAcross}) {
            fbo->reset(texture(grown));
            // read bilinearly: a level is made with one fetch per 2 x 2 texels, and read with four per 4 x 4
            f->glBindTexture(GL_TEXTURE_2D, (*fbo)->texture());
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        f->glBindTexture(GL_TEXTURE_2D, 0);
        return true;
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
    // texelFetch, gl_VertexID and flat need GLSL 1.30 or ES 3.00.
    if (!context || context->format().majorVersion() < 3) {
        return false;
    }
    // Rendering to half floats needs an extension on OpenGL ES 3.
    const bool halfFloats = !context->isOpenGLES() || context->hasExtension("GL_EXT_color_buffer_half_float")
        || context->hasExtension("GL_EXT_color_buffer_float");
    d->keptFormat = halfFloats ? GL_RGBA16F : GL_RGBA;
    static_cast<Programs &>(*d) = programsOf(context->shareGroup());
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
qreal srgbToLinear(qreal c)
{
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

/// \p color (sRGB) in Oklab.
QVector3D oklabOf(const QColor &color)
{
    const qreal r = srgbToLinear(color.redF()), g = srgbToLinear(color.greenF()), b = srgbToLinear(color.blueF());
    const qreal l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const qreal m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const qreal s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return QVector3D(float(0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s), float(1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s),
                     float(0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s));
}

/// Uploads \p transform as a GLSL mat3 (column-major).
void setMatrix(QOpenGLFunctions *f, int location, const QTransform &transform)
{
    const float m[9] = {float(transform.m11()), float(transform.m12()), float(transform.m13()),
                        float(transform.m21()), float(transform.m22()), float(transform.m23()),
                        float(transform.m31()), float(transform.m32()), float(transform.m33())};
    f->glUniformMatrix3fv(location, 1, GL_FALSE, m);
}

/// Half of a normalized gaussian: its taps either way and their weights.
struct Kernel {
    int taps {0};
    float weights[maxTaps + 1] = {};
};

/// A gaussian kernel of \p sigma, in downsampled pixels.
Kernel gaussian(qreal sigma)
{
    Kernel k;
    k.taps = qBound(1, qCeil(3 * sigma), maxTaps);
    float sum = 0;
    for (int i = 0; i <= k.taps; i++) {
        k.weights[i] = float(std::exp(-0.5 * i * i / (sigma * sigma)));
        sum += i ? 2 * k.weights[i] : k.weights[i];
    }
    for (int i = 0; i <= k.taps; i++) k.weights[i] /= sum;
    return k;
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
    // Sixteen times smaller: the glow reaches far and has no fine detail.
    const int factor = qMax(1, qRound(16 * pixelRatio));
    const QSize smallSize((viewport.width() + factor - 1) / factor, (viewport.height() + factor - 1) / factor);
    if (!d->ambientSmall || d->ambientSmall->size() != smallSize) {
        for (QScopedPointer<QOpenGLFramebufferObject> *fbo : {&d->ambientSmall, &d->ambientAcross, &d->ambientBlurred}) {
            fbo->reset(d->texture(smallSize));
        }
    }
    d->imageToSurface = surfaceToImage.inverted();
    d->ambientFactor = factor;
    d->ambientSurfaceToImage = surfaceToImage;
    d->ambientBase = base;
    d->ambientViewport = viewport;
    d->ambientRatio = pixelRatio;
    // The glow is drawn all around the image.
    d->changedAll = true;
    const Kernel kernel = gaussian(KisStudioGlass::ambientSigma * pixelRatio / factor);

    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    GLint target = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    if (d->vao.isCreated()) d->vao.bind();
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    f->glActiveTexture(GL_TEXTURE0);
    f->glViewport(0, 0, smallSize.width(), smallSize.height());

    // Downsample the image's light.
    d->ambientShrink->bind();
    f->glUniform4f(d->ambientShrink["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->ambientShrink["source"], 0);
    f->glUniform1i(d->ambientShrink["factor"], factor);
    f->glUniform1i(d->ambientShrink["decode"], !linearCanvas);
    setMatrix(f, d->ambientShrink["toImage"], surfaceToImage);
    d->ambientSmall->bind();
    f->glBindTexture(GL_TEXTURE_2D, canvas->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Blur it so the light falls off with distance.
    d->blur->bind();
    f->glUniform4f(d->blur["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->blur["source"], 0);
    f->glUniform1i(d->blur["taps"], kernel.taps);
    f->glUniform1fv(d->blur["weights"], maxTaps + 1, kernel.weights);
    // The whole texture is one slot, unlike the panels' blurs.
    f->glUniform4i(d->blur["slot"], 0, 0, smallSize.width() - 1, smallSize.height() - 1);
    d->ambientAcross->bind();
    f->glUniform2i(d->blur["direction"], 1, 0);
    f->glBindTexture(GL_TEXTURE_2D, d->ambientSmall->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    d->ambientBlurred->bind();
    f->glUniform2i(d->blur["direction"], 0, 1);
    f->glBindTexture(GL_TEXTURE_2D, d->ambientAcross->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Draw the glow around the image on the canvas.
    canvas->bind();
    f->glViewport(0, 0, viewport.width(), viewport.height());
    d->ambient->bind();
    f->glUniform4f(d->ambient["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->ambient["ambient"], 0);
    f->glUniform1f(d->ambient["factor"], factor);
    setMatrix(f, d->ambient["toImage"], surfaceToImage);
    f->glUniform3f(d->ambient["base"], float(srgbToLinear(base.redF())), float(srgbToLinear(base.greenF())), float(srgbToLinear(base.blueF())));
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

bool KisOpenGLGlass::hasAmbient(const QSize &viewport, qreal pixelRatio, const QTransform &surfaceToImage, const QColor &base) const
{
    return d->ambientFactor > 0 && d->ambientViewport == viewport && d->ambientRatio == pixelRatio
        && d->ambientSurfaceToImage == surfaceToImage && d->ambientBase == base;
}

void KisOpenGLGlass::dropAmbient()
{
    d->ambientFactor = 0;
}

void KisOpenGLGlass::canvasRedrawn(const QRect &surfaceRect)
{
    d->changed |= surfaceRect;
    d->reported = true;
}

qreal KisOpenGLGlass::reach()
{
    // Three sigma of the widest frost, plus the pixels merged by the pyramid, plus how far a rim looks out.
    const KisStudioGlass::Bevel bevel = KisStudioGlass::bevel();
    return 3 * KisStudioGlass::blurSigma() * KisStudioGlass::maxBlurScale + 16 + bevel.thickness + 2 * bevel.lift;
}

qreal KisOpenGLGlass::ambientReach()
{
    // Three sigma of the glow's blur, plus a few of its sixteen times downsampled pixels.
    return 3 * KisStudioGlass::ambientSigma + 6 * 16;
}

void KisOpenGLGlass::render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                            const QVector<Surface> &surfaces, const QRect &clip, bool canvasChanged)
{
    // A floating point (HDR) canvas is already linear and needs no dithering.
    const bool linearCanvas = canvasFormat == GL_RGBA16F;
    const float dither = linearCanvas ? 0.0f : canvasFormat == GL_RGB10_A2 ? 1.0f / 1023 : 1.0f / 255;
    // Changes accumulate until the pyramid is made again, so a panel shown later doesn't read a stale
    // one. A change not reported to canvasRedrawn() may be anywhere.
    if (canvasChanged && !d->reported) {
        d->changedAll = true;
    }
    d->reported = false;
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

    // The pyramid covers the panels and what their frost and rims read around them, in whole texels of
    // its coarsest level.
    const int margin = qCeil(reach() * pixelRatio);
    QRect around;
    for (const Surface &surface : surfaces) {
        around |= surface.rect.toAlignedRect().adjusted(-margin, -margin, margin, margin);
    }
    around &= whole;
    if (around.isEmpty()) {
        return;
    }
    const int coarsest = 1 << pyramidLevels;
    auto floorTo = [coarsest](int v) { return int(std::floor(qreal(v) / coarsest)) * coarsest; };
    auto ceilTo = [coarsest](int v) { return int(std::ceil(qreal(v) / coarsest)) * coarsest; };
    const QRect box(QPoint(floorTo(around.left()), floorTo(around.top())),
                    QPoint(ceilTo(around.right() + 1) - 1, ceilTo(around.bottom() + 1) - 1));
    // Level 1 in the corner, the others stacked beside it.
    QPoint origins[pyramidLevels + 1];
    QSize sizes[pyramidLevels + 1];
    int stacked = 0;
    for (int k = 1; k <= pyramidLevels; k++) {
        sizes[k] = box.size() / (1 << k);
        origins[k] = k == 1 ? QPoint() : QPoint(sizes[1].width(), stacked);
        if (k > 1) stacked += sizes[k].height();
    }
    const QSize atlasSize(sizes[1].width() + sizes[2].width(), sizes[1].height());

    QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
    GLint target = 0;
    f->glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    if (d->vao.isCreated()) d->vao.bind();
    f->glDisable(GL_BLEND);
    f->glDisable(GL_SCISSOR_TEST);
    f->glActiveTexture(GL_TEXTURE0);

    d->timing.begin();
    const bool grown = d->ensurePyramid(atlasSize);
    const bool remake = grown || d->changedAll || box != d->pyramidBox || viewport != d->pyramidViewport || canvas != d->pyramidCanvas
        || canvasFormat != d->pyramidFormat || d->changed.intersects(box);
    d->changed = QRect();
    d->changedAll = false;
    d->pyramidBox = box;
    d->pyramidViewport = viewport;
    d->pyramidCanvas = canvas;
    d->pyramidFormat = canvasFormat;
    const QSize texture = d->pyramid->size();
    const bool rooted = d->keptFormat == GL_RGBA;
    if (remake) {
        f->glViewport(0, 0, texture.width(), texture.height());
        d->pyramidPass->bind();
        f->glUniform1i(d->pyramidPass["source"], 0);
        f->glUniform1i(d->pyramidPass["decode"], !linearCanvas);
        f->glUniform1i(d->pyramidPass["rooted"], rooted);
        f->glUniform1f(d->pyramidPass["salience"], KisStudioGlass::salience);
        f->glUniform1f(d->pyramidPass["power"], KisStudioGlass::lightnessPower);
        f->glUniform2f(d->pyramidPass["sourceTexel"], 1.0f / texture.width(), 1.0f / texture.height());
        f->glUniform2i(d->pyramidPass["box"], box.left(), box.top());
        auto pass = [&](QOpenGLFramebufferObject *into, uint from, int mode, int level, int fromLevel, const QPoint &direction) {
            into->bind();
            f->glBindTexture(GL_TEXTURE_2D, from);
            const QRect place(origins[level], sizes[level]);
            f->glUniform4f(d->pyramidPass["rect"], 2.0f * place.left() / texture.width() - 1, 2.0f * place.top() / texture.height() - 1,
                           2.0f * (place.right() + 1) / texture.width() - 1, 2.0f * (place.bottom() + 1) / texture.height() - 1);
            f->glUniform1i(d->pyramidPass["mode"], mode);
            f->glUniform2i(d->pyramidPass["origin"], place.left(), place.top());
            f->glUniform2i(d->pyramidPass["size"], place.width(), place.height());
            f->glUniform2i(d->pyramidPass["sourceOrigin"], origins[fromLevel].x(), origins[fromLevel].y());
            f->glUniform2i(d->pyramidPass["sourceSize"], sizes[fromLevel].width(), sizes[fromLevel].height());
            f->glUniform2i(d->pyramidPass["direction"], direction.x(), direction.y());
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        };
        QOpenGLFramebufferObject *levels = d->pyramid.data();
        QOpenGLFramebufferObject *across = d->pyramidAcross.data();
        pass(levels, canvas, 0, 1, 1, QPoint());
        pass(across, levels->texture(), 1, 1, 1, QPoint(1, 0));
        pass(levels, across->texture(), 1, 1, 1, QPoint(0, 1));
        for (int k = 2; k <= pyramidLevels; k++) {
            pass(across, levels->texture(), 2, k, k - 1, QPoint());
            pass(levels, across->texture(), 1, k, k, QPoint(0, 1));
        }
    }

    d->timing.mark();
    // Draw the glass over the canvas.
    f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(target));
    f->glViewport(0, 0, viewport.width(), viewport.height());
    f->glEnable(GL_SCISSOR_TEST);
    f->glScissor(drawn.x(), drawn.y(), drawn.width(), drawn.height());
    f->glEnable(GL_BLEND);
    f->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    d->glass->bind();
    f->glUniform1i(d->glass["pyramid"], 0);
    f->glUniform1i(d->glass["under"], 2);
    f->glUniform2f(d->glass["pyramidTexel"], 1.0f / texture.width(), 1.0f / texture.height());
    f->glUniform2f(d->glass["box"], box.left(), box.top());
    GLfloat levelOrigin[2 * (pyramidLevels + 1)] = {};
    GLfloat levelSize[2 * (pyramidLevels + 1)] = {};
    for (int k = 1; k <= pyramidLevels; k++) {
        levelOrigin[2 * k] = origins[k].x();
        levelOrigin[2 * k + 1] = origins[k].y();
        levelSize[2 * k] = sizes[k].width();
        levelSize[2 * k + 1] = sizes[k].height();
    }
    f->glUniform2fv(d->glass["levelOrigin"], pyramidLevels + 1, levelOrigin);
    f->glUniform2fv(d->glass["levelSize"], pyramidLevels + 1, levelSize);
    f->glUniform1fv(d->glass["levelSigma"], pyramidLevels + 1, levelSigmas().data());
    f->glUniform1i(d->glass["levels"], pyramidLevels);
    f->glUniform1i(d->glass["rooted"], rooted);
    f->glUniform1i(d->glass["decode"], !linearCanvas);
    f->glUniform1f(d->glass["restore"], KisStudioGlass::restore);
    f->glUniform1f(d->glass["power"], KisStudioGlass::lightnessPower);
    f->glUniform1f(d->glass["salience"], KisStudioGlass::salience);
    f->glUniform2f(d->glass["tintStrength"], KisStudioGlass::tintOverDark(), KisStudioGlass::tintOverBright());
    f->glUniform1f(d->glass["veil"], KisStudioGlass::veil);
    f->glUniform2f(d->glass["viewport"], viewport.width(), viewport.height());
    f->glUniform1f(d->glass["dither"], dither);
    const KisStudioGlass::Bevel bevel = KisStudioGlass::bevel();
    f->glUniform3f(d->glass["bevel"], bevel.width * pixelRatio, bevel.thickness * pixelRatio, bevel.lift * pixelRatio);
    f->glUniform1f(d->glass["polish"], bevel.polish);
    const QVector3D ior = refractiveIndices(bevel.abbeNumber);
    f->glUniform3f(d->glass["ior"], ior.x(), ior.y(), ior.z());
    // The image's glow, if it has been rendered.
    const bool imageLit = KisStudioGlass::ambientSurround() && d->ambientBlurred && d->ambientFactor > 0;
    f->glUniform1i(d->glass["imageLit"], imageLit);
    if (imageLit) {
        setMatrix(f, d->glass["toImage"], d->imageToSurface.inverted());
        setMatrix(f, d->glass["fromImage"], d->imageToSurface);
        f->glUniform1i(d->glass["imageLight"], 3);
        f->glUniform1f(d->glass["imageLightFactor"], d->ambientFactor);
        // A larger image casts light further: its reach grows with the image size.
        const QPolygonF image = d->imageToSurface.map(QPolygonF(QRectF(0, 0, 1, 1)));
        qreal area = 0;
        for (int i = 0; i < image.size(); i++) {
            const QPointF a = image[i], b = image[(i + 1) % image.size()];
            area += a.x() * b.y() - b.x() * a.y();
        }
        f->glUniform1f(d->glass["imageLightReach"], float(std::sqrt(std::abs(area) / 2) + 160 * pixelRatio));
        f->glActiveTexture(GL_TEXTURE3);
        f->glBindTexture(GL_TEXTURE_2D, d->ambientBlurred->texture());
    }
    f->glUniform1f(d->glass["lowerStrength"], float(0.5 * (KisStudioGlass::tintOverDark() + KisStudioGlass::tintOverBright())));
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, canvas);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, d->pyramid->texture());
    // How far a rim can look out: through the glass and across the gap.
    const int rimReach = qCeil((bevel.thickness + 2 * bevel.lift) * pixelRatio);
    for (int i = 0; i < surfaces.size(); i++) {
        const Surface &surface = surfaces[i];
        const QRectF r = surface.rect;
        const QRectF quad = r.adjusted(-1, -1, 1, 1);
        f->glUniform4f(d->glass["rect"], quad.left() / viewport.width() * 2 - 1, quad.top() / viewport.height() * 2 - 1,
                       quad.right() / viewport.width() * 2 - 1, quad.bottom() / viewport.height() * 2 - 1);
        f->glUniform4f(d->glass["shape"], r.center().x(), r.center().y(), r.width() / 2, r.height() / 2);
        f->glUniform2f(d->glass["corner"], surface.reach, surface.exponent);
        f->glUniform3f(d->glass["tint"], surface.tint.redF(), surface.tint.greenF(), surface.tint.blueF());
        // Perceived lightness (about the cube root of luminance), measured like the background.
        const qreal luminance = 0.2126 * srgbToLinear(surface.tint.redF()) + 0.7152 * srgbToLinear(surface.tint.greenF())
            + 0.0722 * srgbToLinear(surface.tint.blueF());
        f->glUniform1f(d->glass["tintLightness"], float(std::cbrt(luminance)));
        f->glUniform1f(d->glass["opacity"], surface.opacity);
        // Larger panels frost wider (see KisStudioGlass::blurScale).
        const qreal sigma = KisStudioGlass::blurSigma() * KisStudioGlass::blurScale(r.size() / pixelRatio) * pixelRatio;
        f->glUniform1f(d->glass["sigma"], sigma);
        f->glUniform1f(d->glass["spacing"], 2 * sigma);
        // Which sides extend beyond the canvas: left, right, bottom, top.
        f->glUniform4f(d->glass["beyond"], r.left() < 0, r.right() > viewport.width(), r.top() < 0, r.bottom() > viewport.height());
        const QRectF reach = r.adjusted(-rimReach, -rimReach, rimReach, rimReach) & QRectF(whole).adjusted(0.5, 0.5, -0.5, -0.5);
        f->glUniform4f(d->glass["reach"], reach.left(), reach.top(), reach.right(), reach.bottom());
        // Up to four nearest glass panels below this one that it overlaps.
        GLfloat lowerShape[4 * 4] = {};
        GLfloat lowerCorner[4 * 2] = {};
        GLfloat lowerTint[4 * 3] = {};
        int lowerCount = 0;
        for (int j = i - 1; j >= 0 && lowerCount < 4; j--) {
            const Surface &lower = surfaces[j];
            if (lower.opacity <= 0 || !lower.rect.intersects(r)) continue;
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
    f->glActiveTexture(GL_TEXTURE3);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    d->glass->release();
    if (d->vao.isCreated()) d->vao.release();
}
