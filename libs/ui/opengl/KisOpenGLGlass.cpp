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
#include <QOpenGLVertexArrayObject>
#include <QPolygonF>
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

/// Maximum blur taps either way, in downsampled pixels: three sigma of the widest blur (most frosted,
/// largest panel). Fewer would cut the gaussian short and make it look like a box blur.
constexpr int maxTaps = 64;

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
 * In eight bits, intermediate textures store square roots, which keeps about sRGB's precision near 0;
 * in half floats (KEPT_LINEAR) they store the values themselves, so the GPU can interpolate them.
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
#if KEPT_LINEAR
    return kept;
#else
    return vec4(kept.rgb * kept.rgb, kept.a);
#endif
}

vec4 keep(vec4 weighted)
{
#if KEPT_LINEAR
    return weighted;
#else
    return vec4(sqrt(max(weighted.rgb, 0.0)), weighted.a);
#endif
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
 * Downsamples the canvas: each pixel is the mean of a factor x factor block, with saturated colors
 * weighted more (Oklab chroma is at most about 0.32 in sRGB). It stores Oklab lightness raised to
 * KisStudioGlass::lightnessPower and a, b offset by 0.5; or, for statistics, the chroma, which
 * later shows how much the hues agree.
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

/**
 * A glass panel's quad, plus its tint strength: enough for its content to stay readable over what is
 * behind the whole panel (more over a bright canvas, less over a dark one). It is uniform across the
 * panel so the background keeps its light and dark, and computed in the vertex shader.
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

    // Average Oklab lightness behind the panel, sampled on a grid as fine as the blur is wide
    // (12 x 12 at most) so the tint doesn't flicker as the canvas scrolls underneath.
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
 * A glass panel, its edge after Apple's Liquid Glass as preyneyv's Shadertoy ("Liquid Glass", wfGXzh)
 * draws it:
 *
 *  - Along the rim, a band where the glass shows what is behind from further in, as if bent toward the
 *    edge: how far grows slowly from deep in the band and steeply at the edge (1 - cos), to twice the
 *    band's width, and a little more for blue than for red, so the colors part at the edge. Inside the
 *    band nothing is moved.
 *  - The frost follows how thick the glass is, (1 - u^4)(1 - v^4) with u and v from -1 to 1 across the
 *    panel: clear at the edges, the most in the middle, changing evenly as the blur narrows by octaves
 *    toward the canvas itself.
 *  - At the very edge, where the bend is steepest, light gathers into a hairline: what is seen there,
 *    brighter and more vivid, brightest facing the room's light (top left and bottom right).
 *
 * Everything is computed in linear light, tinted like real tinted glass (see KisStudioGlass::veil).
 */
const char *const glassSource = R"(
uniform sampler2D blurred;
// The frost at a half, a quarter and an eighth of its width, for a rim clearer than the face.
uniform sampler2D halfFrost;
uniform sampler2D quarterFrost;
uniform sampler2D eighthFrost;
uniform sampler2D statistics;
uniform vec2 shift;
uniform ivec4 slot;
uniform sampler2D under;
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
// The refracting band's width, the size of a widget pixel in surface pixels, and how bright the rim's light is.
uniform vec3 rim;
// The power each color's bend grows by toward the edge: a little more for blue than for red.
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

// How much of the blur d pixels inside a canvas edge comes from the canvas. If the panel ends within
// the canvas, all of it: the clamped textures continue the canvas past the window edge instead of a
// haze of panel color. If the panel crosses the edge (crossing = 1) and continues as a plain panel,
// it fades to none at the edge so the glass meets the plain part without a seam.
float within(float d, float crossing)
{
    return mix(1.0, max(erf(d / (sigma * 1.41421356)), 0.0), crossing);
}

// Signed distance in pixels from p to a squircle shape (center and half size; corner reach and
// exponent), negative inside, and the outward normal. Each corner is a quarter of the superellipse
// |x|^n + |y|^n = reach^n.
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
    // Approximate distance to f = reach: the difference over the gradient's length.
    vec2 gradient = pow(q / f, vec2(n - 1.0));
    normal = normalize(gradient) * side;
    return (f - corner.x) / length(gradient);
}

// outsideOf() for this panel's own shape.
float outside(vec2 p, out vec2 normal)
{
    return outsideOf(p, shape, corner, normal);
}

// Weights of the four texels around a point t of the way from the second to the third (cubic B-spline).
vec4 bspline(float t)
{
    float t2 = t * t;
    float t3 = t2 * t;
    return vec4(1.0 - 3.0 * t + 3.0 * t2 - t3, 4.0 - 6.0 * t2 + 3.0 * t3, 1.0 + 3.0 * t + 3.0 * t2 - 3.0 * t3, t3) / 6.0;
}

// Smooth sample of a weighted downsampled texture (the blur or its statistics) at p (surface pixels),
// within this surface's slot: a cubic B-spline over 4 x 4 texels, since stretching a small texture
// bilinearly leaves creases along its grid. The weights are interpolated too.
vec3 softAt(sampler2D source, vec2 p)
{
    vec2 t = p / factor - 0.5 + shift;
    vec2 i = floor(t);
    vec2 f = t - i;
    vec4 wx = bspline(f.x);
    vec4 wy = bspline(f.y);
#if KEPT_LINEAR
    // Four bilinear fetches, each placed between two texels so the hardware gives their weighted sum.
    vec2 g0 = vec2(wx.x + wx.y, wy.x + wy.y);
    vec2 g1 = vec2(wx.z + wx.w, wy.z + wy.w);
    vec2 h0 = i - 1.0 + vec2(wx.y, wy.y) / g0;
    vec2 h1 = i + 1.0 + vec2(wx.w, wy.w) / g1;
    vec2 size = vec2(textureSize(source, 0));
    vec2 lo = vec2(slot.xy);
    vec2 hi = vec2(slot.zw);
    vec4 m = g0.y * (g0.x * texture(source, (clamp(h0, lo, hi) + 0.5) / size) + g1.x * texture(source, (clamp(vec2(h1.x, h0.y), lo, hi) + 0.5) / size))
        + g1.y * (g0.x * texture(source, (clamp(vec2(h0.x, h1.y), lo, hi) + 0.5) / size) + g1.x * texture(source, (clamp(h1, lo, hi) + 0.5) / size));
#else
    vec4 m = vec4(0.0);
    for (int y = 0; y < 4; y++) {
        vec4 row = vec4(0.0);
        for (int x = 0; x < 4; x++) {
            row += wx[x] * weighted(texelFetch(source, clamp(ivec2(i) + ivec2(x - 1, y - 1), slot.xy, slot.zw), 0));
        }
        m += wy[y] * row;
    }
#endif
    return m.rgb / max(m.a, 1e-5);
}

// The Oklab mean stored in source at p, decoding the lightness power and the a, b offset.
vec3 meanAt(sampler2D source, vec2 p)
{
    vec3 m = softAt(source, p);
    return vec3(pow(max(m.x, 0.0), 1.0 / power), m.yz - 0.5);
}

/**
 * The frost at p in Oklab: the mean of the wide blur, with a power mean for lightness (see
 * KisStudioGlass::lightnessPower). Averaging a and b never produces a hue that isn't between the
 * inputs (mixing in linear light turns orange and blue into pink), and a colored stroke on white keeps
 * its chroma. Chroma lost where hues disagree is partly restored, never beyond the average chroma.
 */
vec3 frostedLab(vec2 p)
{
    vec3 wide = meanAt(blurred, p);
    float chroma = softAt(statistics, p).x;
    float agree = clamp(length(wide.yz) / max(chroma, 1e-4), 0.0, 1.0);
    return vec3(wide.x, wide.yz * (1.0 + restore * (1.0 - agree)));
}

// The canvas at q in Oklab, as clear glass shows it.
vec3 clearLab(vec2 q)
{
    vec3 c = texture(under, q / viewport).rgb;
    return toOklab(clamp(decode ? fromSrgb(c) : c, 0.0, 1.0));
}

// The canvas at q softened over about 1.5 pixels: four bilinear taps around it, and q itself.
vec3 softClearLab(vec2 q)
{
    vec3 sum = 2.0 * clearLab(q);
    sum += clearLab(q + vec2(1.2, 1.2)) + clearLab(q + vec2(-1.2, 1.2)) + clearLab(q + vec2(1.2, -1.2)) + clearLab(q + vec2(-1.2, -1.2));
    return sum / 6.0;
}

// Level k of the frost at q: 0 the full frost, 1 to 3 a half, a quarter and an eighth of it, 4 the canvas
// slightly softened, 5 the canvas itself.
vec3 levelLab(int k, vec2 q)
{
    if (k <= 0) return frostedLab(q);
    if (k == 1) return meanAt(halfFrost, q);
    if (k == 2) return meanAt(quarterFrost, q);
    if (k == 3) return meanAt(eighthFrost, q);
    if (k == 4) return softClearLab(q);
    return clearLab(q);
}

// How wide level k blurs, in surface pixels: the blur at its width, with what the downsampling adds.
float levelSigma(int k)
{
    if (k >= 5) return 0.5;
    if (k == 4) return 1.5;
    float s = sigma / exp2(float(k));
    return sqrt(s * s + 0.25 * factor * factor);
}

/**
 * The frost at q blurred by `blur` pixels, from the full frost down to the canvas itself. Where it lies
 * among the levels is measured in octaves between their widths, and the three levels around it are mixed
 * with quadratic B-spline weights, which change smoothly with it: mixing only two would crease where one
 * hands over to the next.
 */
vec3 frostAt(vec2 q, float blur)
{
    float t = 5.0;
    for (int k = 4; k >= 0; k--) {
        float wide = levelSigma(k);
        float narrow = levelSigma(k + 1);
        if (blur >= narrow && narrow < wide) t = float(k) + clamp(log2(wide / blur) / log2(wide / narrow), 0.0, 1.0);
    }
    if (blur >= levelSigma(0)) t = 0.0;
    if (t <= 0.0) return frostedLab(q);
    int k = int(floor(t + 0.5));
    float f = t - float(k);
    vec3 lab = (0.75 - f * f) * levelLab(k, q);
    lab += 0.5 * (0.5 - f) * (0.5 - f) * levelLab(max(k - 1, 0), q);
    lab += 0.5 * (0.5 + f) * (0.5 + f) * levelLab(min(k + 1, 5), q);
    return lab;
}

// Clamps a point the rim sees to the area around the panel that was blurred (reach).
vec2 held(vec2 q)
{
    return clamp(q, reach.xy, reach.zw);
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
    // a share of the chroma veiled, all of it at full strength, where the glass is solid
    float m = mix(veil, 1.0, smoothstep(-0.02, 0.06, l - b.x));
    float scattered = s * m + (1.0 - m) * s * s * s;
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
        vec2 n;
        float d = outsideOf(p, lowerShape[i], lowerCorner[i], n);
        float cover = clamp(0.5 - d / max(2.0 * sigma, 1.0), 0.0, 1.0);
        if (cover > 0.0) lab = mix(lab, toOklab(tintedLab(lab, lowerTint[i], lowerStrength)), cover);
    }
    return lab;
}

/**
 * Light the glowing image casts on the rim beside it, where the rim faces the image: in the colors of the
 * image's nearest edge, fading with distance. Nothing over the image itself.
 */
vec3 imageLightAt(vec2 p, vec2 outward)
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
    return light * falloff * 0.45 * facing * smoothstep(0.0, 24.0, dist);
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 outward;
    float d = -outside(p, outward);
    float coverage = clamp(d + 0.5, 0.0, 1.0) * opacity;
    if (coverage <= 0.0) {
        discard;
    }
    // How much comes from the canvas rather than the plain panel beyond its edges.
    float canvas = within(p.x, beyond.x) * within(viewport.x - p.x, beyond.y) * within(p.y, beyond.z) * within(viewport.y - p.y, beyond.w);
    vec3 tintLab = toOklab(fromSrgb(tint));
    const float quarterTurn = 1.5707963;

    // How thick the glass is: the frost is clear at the edges and the most in the middle.
    vec2 uv = clamp((p - shape.xy) / shape.zw, -1.0, 1.0);
    vec2 uv2 = uv * uv;
    float thickness = (1.0 - uv2.x * uv2.x) * (1.0 - uv2.y * uv2.y);
    float blur = levelSigma(0) * thickness;
    // The band along the rim bends what is behind toward the edge. It stays within the corners' curve,
    // where the distance to the rim has no crease and its direction turns smoothly.
    float band = min(rim.x, min(0.9 * corner.x, min(shape.z, shape.w)));
    float bent = band > 0.5 ? 1.0 - cos(clamp(1.0 - d / band, 0.0, 1.0) * quarterTurn) : 0.0;
    // The frost is read once, where green is bent to (the shader stays small enough to link quickly); the
    // colors part only where the glass is clear, so red and blue come from the canvas itself, a little apart.
    vec3 reached = pow(vec3(bent), ior) * 2.0 * band;
    vec2 q = bent > 0.0 ? held(p - outward * reached.g) : p;
    vec3 seen = throughLower(frostAt(q, blur), p);
    float sharp = bent > 0.0 ? (1.0 - thickness) * (1.0 - thickness) : 0.0;
    if (sharp > 0.01) {
        vec3 light = max(fromOklab(seen), 0.0);
        light.r = mix(light.r, max(fromOklab(throughLower(clearLab(held(p - outward * reached.r)), p)).r, 0.0), sharp);
        light.b = mix(light.b, max(fromOklab(throughLower(clearLab(held(p - outward * reached.b)), p)).b, 0.0), sharp);
        seen = toOklab(light);
    }
    vec3 color = tintedLab(mix(tintLab, seen, canvas), tintLab, strength);

    // At the very edge, where the bend is steepest, light gathers into a hairline: what is seen there,
    // brighter and more vivid, brightest on the sides facing the room's light (top left and bottom right).
    // about a widget pixel wide, with a hard inner edge: a soft falloff reads as a blurred band
    float line = clamp(1.0 + (rim.y - d) / max(0.5 * rim.y, 0.5), 0.0, 1.0);
    if (line > 0.0) {
        vec3 lab = toOklab(max(color, 0.0));
        vec3 gathered = inGamut(min(lab.x + 0.25, 1.0), lab.yz * 1.7);
        float facing = abs(dot(outward, vec2(-0.70710678, 0.70710678)));
        color = mix(color, gathered, clamp(line * (0.3 + 0.6 * facing) * rim.z, 0.0, 1.0));
        if (imageLit) {
            color += imageLightAt(p, outward) * line * rim.z;
        }
    }

    /**
     * Triangular dither of one target step either way (the sum of two uniform noises) turns banding
     * in smooth gradients into invisible noise without any pattern. No grain is added otherwise.
     */
    uvec2 u = uvec2(p);
    float steps = hash(u) + hash(u + uvec2(7919u, 104729u)) - 1.0;
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

/// The GLSL version, and whether intermediate textures keep values as they are (see commonSource)
QByteArray header(bool keptLinear)
{
    QByteArray version;
    if (KisOpenGL::hasOpenGLES()) {
        version = "#version 300 es\nprecision highp float;\nprecision highp int;\nprecision highp sampler2D;\n";
    } else {
#ifdef Q_OS_MACOS
        version = "#version 150 core\n";
#else
        version = "#version 130\n";
#endif
    }
    return version + (keptLinear ? "#define KEPT_LINEAR 1\n" : "#define KEPT_LINEAR 0\n");
}

#ifndef GL_COMPLETION_STATUS_KHR
#define GL_COMPLETION_STATUS_KHR 0x91B1
#endif

/**
 * A shader program and its uniform locations. It is linked without waiting: where the driver links on
 * threads of its own (KHR_parallel_shader_compile, as ANGLE does) the glass shader takes seconds, and
 * waiting for it would freeze the window; until it is done the panels paint solid.
 */
struct Program {
    GLuint id {0};
    QList<QByteArray> names;
    QHash<QByteArray, int> locations;
    bool pending {false};
    bool linked {false};

    bool make(const char *vertex, const char *fragment, const QList<QByteArray> &uniforms, bool keptLinear)
    {
        QOpenGLFunctions *f = QOpenGLContext::currentContext()->functions();
        const QByteArray sources[2] = {header(keptLinear) + commonSource + vertex,
                                       header(keptLinear) + "out vec4 fragColor;\n" + commonSource + fragment};
        const GLenum kinds[2] = {GL_VERTEX_SHADER, GL_FRAGMENT_SHADER};
        id = f->glCreateProgram();
        if (!id) return false;
        for (int i = 0; i < 2; i++) {
            const GLuint shader = f->glCreateShader(kinds[i]);
            const char *text = sources[i].constData();
            f->glShaderSource(shader, 1, &text, nullptr);
            f->glCompileShader(shader);
            f->glAttachShader(id, shader);
            // freed with the program
            f->glDeleteShader(shader);
        }
        f->glLinkProgram(id);
        names = uniforms;
        pending = true;
        return true;
    }

    /// Whether it is linked and ready; while the driver is still linking (\p parallel), false without waiting
    bool finish(bool parallel)
    {
        if (!pending) return linked;
        QOpenGLContext *context = QOpenGLContext::currentContext();
        if (!context) return false;
        QOpenGLFunctions *f = context->functions();
        if (parallel) {
            GLint done = 0;
            f->glGetProgramiv(id, GL_COMPLETION_STATUS_KHR, &done);
            if (!done) return false;
        }
        pending = false;
        GLint ok = 0;
        f->glGetProgramiv(id, GL_LINK_STATUS, &ok);
        linked = ok;
        if (!linked) {
            char log[4096] = {};
            f->glGetProgramInfoLog(id, sizeof(log) - 1, nullptr, log);
            warnOpenGL << "Glass is drawn without OpenGL:" << log;
            return false;
        }
        for (const QByteArray &name : names) locations.insert(name, f->glGetUniformLocation(id, name.constData()));
        return true;
    }

    void bind() const { QOpenGLContext::currentContext()->functions()->glUseProgram(id); }
    void release() const { QOpenGLContext::currentContext()->functions()->glUseProgram(0); }
    /// looked up without a copy of the name: about fifty a frame
    int operator[](const char *uniform) const { return locations.value(QByteArray::fromRawData(uniform, int(qstrlen(uniform))), -1); }
};

struct Programs {
    Program shrink;
    Program blur;
    Program glass;
    Program ambientShrink;
    Program ambient;
    /// Every program was handed to the driver
    bool valid {false};
    /// The driver links them on threads of its own
    bool parallel {false};

    /// Whether all of them are linked; false without waiting while they are being linked
    bool ready()
    {
        if (!valid) return false;
        bool all = true;
        for (Program *program : {&shrink, &blur, &glass, &ambientShrink, &ambient}) {
            if (!program->finish(parallel)) all = false;
        }
        if (all) return true;
        // one that failed to link never will
        for (Program *program : {&shrink, &blur, &glass, &ambientShrink, &ambient}) {
            if (!program->pending && !program->linked) valid = false;
        }
        return false;
    }

    void make(bool keptLinear)
    {
        QOpenGLContext *context = QOpenGLContext::currentContext();
        parallel = context->hasExtension("GL_KHR_parallel_shader_compile");
        if (parallel) {
            using MaxThreads = void(QOPENGLF_APIENTRYP)(GLuint);
            if (auto *maxThreads = reinterpret_cast<MaxThreads>(context->getProcAddress("glMaxShaderCompilerThreadsKHR"))) {
                maxThreads(0xFFFFFFFF);
            }
        }
        valid = shrink.make(quadSource, shrinkSource, {"rect", "source", "shift", "factor", "decode", "salience", "power", "statistics"}, keptLinear)
            && blur.make(quadSource, blurSource, {"rect", "source", "slot", "direction", "taps", "weights"}, keptLinear)
            && glass.make(glassVertexSource, glassSource,
                          {"rect", "blurred", "halfFrost", "quarterFrost", "eighthFrost", "statistics", "shift", "slot", "restore", "power",
                           "factor", "shape", "corner", "tint", "tintLightness", "tintStrength", "opacity", "viewport", "sigma",
                           "beyond", "spacing", "dither", "veil", "rim", "ior", "under", "decode", "reach",
                           "imageLit", "toImage", "fromImage", "imageLight", "imageLightFactor", "imageLightReach",
                           "lowerCount", "lowerShape", "lowerCorner", "lowerTint", "lowerStrength"}, keptLinear)
            && ambientShrink.make(quadSource, ambientShrinkSource, {"rect", "source", "factor", "decode", "toImage"}, keptLinear)
            && ambient.make(quadSource, ambientSource, {"rect", "ambient", "factor", "toImage", "base", "strength", "encode", "dither"}, keptLinear);
    }
};

/**
 * Programs shared by all contexts in a share group (all of Krita's canvases), so the slow glass shader
 * compiles only once. Parented to the group, so it is destroyed with it.
 */
struct SharedPrograms : public QObject {
    Programs programs;
};

const Programs &programsOf(QOpenGLContextGroup *group, bool keptLinear)
{
    for (QObject *child : group->children()) {
        if (auto *shared = dynamic_cast<SharedPrograms *>(child)) return shared->programs;
    }
    auto *shared = new SharedPrograms;
    shared->programs.make(keptLinear);
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
    /// The glow around the image was drawn again since the last blur: what is wholly inside the image (in surface
    /// pixels, empty if it is turned) kept its look
    bool ambientChanged {false};
    QRect ambientInside;
    int ambientFactor {0};
    /// What the glow was last made for; it is in the canvas until the canvas is drawn again without it.
    QTransform ambientSurfaceToImage;
    QColor ambientBase;
    QSize ambientViewport;
    qreal ambientRatio {0};
    QOpenGLVertexArrayObject vao;
    /// Downsampled canvas, blurred across, then down; and the same for its Oklab statistics.
    QScopedPointer<QOpenGLFramebufferObject> small;
    QScopedPointer<QOpenGLFramebufferObject> across;
    QScopedPointer<QOpenGLFramebufferObject> blurred;
    /// The frost at a half, a quarter and an eighth of its width (across, then down), for the rim.
    QScopedPointer<QOpenGLFramebufferObject> finerAcross[3];
    QScopedPointer<QOpenGLFramebufferObject> finer[3];
    QScopedPointer<QOpenGLFramebufferObject> smallStatistics;
    QScopedPointer<QOpenGLFramebufferObject> acrossStatistics;
    QScopedPointer<QOpenGLFramebufferObject> statistics;
    /// Intermediate texture format: half floats where renderable, eight bits otherwise.
    GLenum keptFormat {GL_RGBA};
    /// A slot as last blurred: its content depends only on these and on the canvas it reads.
    struct Blur {
        QRect area;
        QRect around;
        QPoint shift;
        qreal sigma {0};

        bool operator==(const Blur &other) const
        {
            return area == other.area && around == other.around && shift == other.shift && sigma == other.sigma;
        }
    };
    /// The slots of the last blur, which stay valid where the canvas under them did not change.
    QVector<Blur> blurs;
    /// The canvas drawn again since the last blur, in surface pixels; all of it if changedAll.
    QRect changed;
    bool changedAll {true};
    /// Whether canvasRedrawn() was called since the last render().
    bool reported {false};
    KisOpenGLGpuTiming timing {QStringLiteral("glass gpu:"), {QStringLiteral("shrink"), QStringLiteral("blur"), QStringLiteral("draw")}};
    QSize blurredViewport;
    int blurredFactor {0};
    uint blurredCanvas {0};
    uint blurredFormat {0};

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
        // read bilinearly by softAt() where the values are kept as they are
        GLint texture = 0;
        f->glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        f->glBindTexture(GL_TEXTURE_2D, fbo->texture());
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        f->glBindTexture(GL_TEXTURE_2D, GLuint(texture));
        return fbo;
    }

    /// Grows the textures to at least \p size; they never shrink, as panels come and go. Returns whether
    /// they were made anew, losing their content.
    bool ensureSize(const QSize &size)
    {
        if (small && small->width() >= size.width() && small->height() >= size.height()) {
            return false;
        }
        const QSize grown = small ? size.expandedTo(small->size()) : size;
        for (QScopedPointer<QOpenGLFramebufferObject> *fbo : {&small, &across, &blurred, &smallStatistics, &acrossStatistics, &statistics}) {
            fbo->reset(texture(grown));
        }
        for (int i = 0; i < 3; i++) {
            finerAcross[i].reset(texture(grown));
            finer[i].reset(texture(grown));
        }
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
    static_cast<Programs &>(*d) = programsOf(context->shareGroup(), halfFloats);
    if (d->valid && KisOpenGL::supportsVAO()) {
        d->vao.create();
    }
    return d->valid;
}

bool KisOpenGLGlass::isValid() const
{
    return d->ready();
}

bool KisOpenGLGlass::isPending() const
{
    return !d->ready() && d->valid;
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
    if (!d->ready() || !canvas) {
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
    // The glow is drawn all around the image, not over it: panels over the image keep their blur.
    d->ambientChanged = true;
    d->ambientInside = d->imageToSurface.type() <= QTransform::TxScale
        ? d->imageToSurface.mapRect(QRectF(0, 0, 1, 1)).toAlignedRect().adjusted(1, 1, -1, -1)
        : QRect();
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
    d->ambientShrink.bind();
    f->glUniform4f(d->ambientShrink["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->ambientShrink["source"], 0);
    f->glUniform1i(d->ambientShrink["factor"], factor);
    f->glUniform1i(d->ambientShrink["decode"], !linearCanvas);
    setMatrix(f, d->ambientShrink["toImage"], surfaceToImage);
    d->ambientSmall->bind();
    f->glBindTexture(GL_TEXTURE_2D, canvas->texture());
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    // Blur it so the light falls off with distance.
    d->blur.bind();
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
    d->ambient.bind();
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
    d->ambient.release();
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
    // Three sigma of the widest blur, plus the pixels merged by downsampling and sampling, plus how far
    // the rim looks out.
    return 3 * KisStudioGlass::blurSigma() * KisStudioGlass::maxBlurScale + 16 + KisStudioGlass::rim().reach();
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
    // Changes accumulate until the next blur, so a panel shown later doesn't reuse a stale one. A change
    // not reported to canvasRedrawn() may be anywhere.
    if (canvasChanged && !d->reported) {
        d->changedAll = true;
    }
    d->reported = false;
    if (!d->ready() || surfaces.isEmpty() || viewport.isEmpty()) {
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

    // Downsample by a factor that keeps blur widths constant in widget pixels at any screen scale.
    const int factor = qMax(1, qRound(4 * pixelRatio));
    // Larger panels blur wider (see KisStudioGlass::blurScale).
    auto scaleOf = [pixelRatio](const Surface &surface) { return KisStudioGlass::blurScale(surface.rect.size() / pixelRatio); };

    const QSize smallSize((viewport.width() + factor - 1) / factor, (viewport.height() + factor - 1) / factor);
    const QRect smallWhole(QPoint(), smallSize);

    /**
     * Each panel is blurred in its own slot of the small textures, since panels blur by different
     * amounts and a shared area would let one panel's blur bleed into a neighbor's. A slot covers the
     * panel's downsampled pixels (plus one around for upscaling), what the rim sees around it, and the blur's
     * reach beyond that. The whole panel is blurred because the brightness behind it sets its tint.
     */
    struct Region {
        QRect area;
        QRect around;
        /// Offset from the downsampled canvas to the slot.
        QPoint shift;
        qreal sigma {0};
        Kernel wide;
        /// The frost at a half, a quarter and an eighth of its width
        Kernel finer[3];
    };
    QVector<Region> regions(surfaces.size());
    // How far the rim looks: in from the edge, and out for its reflection.
    const int rimReach = qCeil(KisStudioGlass::rim().reach() * pixelRatio) + 2;
    {
        // Pack the slots in rows, left to right.
        QPoint next;
        int rowHeight = 0;
        for (int i = 0; i < surfaces.size(); i++) {
            const QRect r = surfaces[i].rect.toAlignedRect().adjusted(-rimReach, -rimReach, rimReach, rimReach) & whole;
            if (r.isEmpty()) continue;
            Region &region = regions[i];
            // Bilinear sampling reaches into the pixel before the first one as well as past the last.
            region.area = QRect(QPoint(r.left() / factor - 1, r.top() / factor - 1), QPoint(r.right() / factor + 1, r.bottom() / factor + 1)) & smallWhole;
            region.sigma = qMax<qreal>(0.35, KisStudioGlass::blurSigma() * scaleOf(surfaces[i]) * pixelRatio / factor);
            region.wide = gaussian(region.sigma);
            for (int k = 0; k < 3; k++) region.finer[k] = gaussian(region.sigma / (2 << k));
            region.around = region.area.adjusted(-region.wide.taps, -region.wide.taps, region.wide.taps, region.wide.taps) & smallWhole;
            if (next.x() + region.around.width() > smallSize.width()) {
                next = QPoint(0, next.y() + rowHeight);
                rowHeight = 0;
            }
            region.shift = next - region.around.topLeft();
            next.rx() += region.around.width();
            rowHeight = qMax(rowHeight, region.around.height());
        }
        if (d->ensureSize(QSize(smallSize.width(), qMax(1, next.y() + rowHeight)))) {
            // New textures hold no blur yet.
            d->changedAll = true;
        }
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

    d->shrink.bind();
    f->glUniform4f(d->shrink["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->shrink["source"], 0);
    f->glUniform1i(d->shrink["factor"], factor);
    f->glUniform1i(d->shrink["decode"], !linearCanvas);
    f->glUniform1f(d->shrink["salience"], KisStudioGlass::salience);
    f->glUniform1f(d->shrink["power"], KisStudioGlass::lightnessPower);
    d->blur.bind();
    f->glUniform4f(d->blur["rect"], -1, -1, 1, 1);
    f->glUniform1i(d->blur["source"], 0);

    d->timing.begin();
    // Reuse a panel's last blur when its slot is the same and nothing behind it changed, e.g. only the
    // brush outline moved or the image changed elsewhere.
    if (d->changedAll || viewport != d->blurredViewport || factor != d->blurredFactor || canvas != d->blurredCanvas
        || canvasFormat != d->blurredFormat) {
        d->blurs.clear();
    }
    QVector<Region> blurring;
    QVector<Private::Blur> blurs;
    for (const Region &region : qAsConst(regions)) {
        if (region.around.isEmpty()) continue;
        const Private::Blur blur {region.area, region.around, region.shift, region.sigma};
        // The canvas pixels the slot is downsampled from.
        const QRect read(region.around.topLeft() * factor, region.around.size() * factor);
        if (!d->blurs.contains(blur) || read.intersects(d->changed) || (d->ambientChanged && !d->ambientInside.contains(read))) {
            blurring << region;
        }
        blurs << blur;
    }
    d->blurs = blurs;
    d->changed = QRect();
    d->changedAll = false;
    d->ambientChanged = false;
    d->blurredViewport = viewport;
    d->blurredFactor = factor;
    d->blurredCanvas = canvas;
    d->blurredFormat = canvasFormat;
    // Downsample the canvas, as colors and as statistics.
    d->shrink.bind();
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
    // Run each pass for all panels at once, so the render target (which stalls the GPU) switches once a
    // pass in total rather than once a pass per panel.
    d->blur.bind();
    struct Pass {
        QOpenGLFramebufferObject *source;
        QOpenGLFramebufferObject *target;
        bool across;
        /// -1 the full frost, otherwise which of the finer ones
        int finer;
    };
    const Pass passes[] = {
        {d->small.data(), d->across.data(), true, -1},
        {d->smallStatistics.data(), d->acrossStatistics.data(), true, -1},
        {d->small.data(), d->finerAcross[0].data(), true, 0},
        {d->small.data(), d->finerAcross[1].data(), true, 1},
        {d->small.data(), d->finerAcross[2].data(), true, 2},
        {d->across.data(), d->blurred.data(), false, -1},
        {d->acrossStatistics.data(), d->statistics.data(), false, -1},
        {d->finerAcross[0].data(), d->finer[0].data(), false, 0},
        {d->finerAcross[1].data(), d->finer[1].data(), false, 1},
        {d->finerAcross[2].data(), d->finer[2].data(), false, 2},
    };
    for (const Pass &pass : passes) {
        if (blurring.isEmpty()) break;
        pass.target->bind();
        f->glBindTexture(GL_TEXTURE_2D, pass.source->texture());
        f->glUniform2i(d->blur["direction"], pass.across ? 1 : 0, pass.across ? 0 : 1);
        for (const Region &region : qAsConst(blurring)) {
            const Kernel &k = pass.finer < 0 ? region.wide : region.finer[pass.finer];
            f->glUniform1i(d->blur["taps"], k.taps);
            f->glUniform1fv(d->blur["weights"], maxTaps + 1, k.weights);
            const QRect place = region.around.translated(region.shift);
            f->glUniform4i(d->blur["slot"], place.left(), place.top(), place.right(), place.bottom());
            // The horizontal pass also covers the rows the vertical pass will read.
            scissor((pass.across ? region.area.adjusted(0, -k.taps, 0, k.taps) & region.around : region.area).translated(region.shift));
            f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        }
    }

    d->timing.mark();
    // Draw the glass over the canvas.
    f->glBindFramebuffer(GL_FRAMEBUFFER, GLuint(target));
    f->glViewport(0, 0, viewport.width(), viewport.height());
    scissor(drawn);
    f->glEnable(GL_BLEND);
    f->glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    d->glass.bind();
    f->glUniform1i(d->glass["blurred"], 0);
    f->glUniform1i(d->glass["statistics"], 1);
    f->glUniform1i(d->glass["under"], 2);
    f->glUniform1i(d->glass["halfFrost"], 4);
    f->glUniform1i(d->glass["quarterFrost"], 5);
    f->glUniform1i(d->glass["eighthFrost"], 6);
    f->glUniform1i(d->glass["decode"], !linearCanvas);
    f->glUniform1f(d->glass["restore"], KisStudioGlass::restore);
    f->glUniform1f(d->glass["power"], KisStudioGlass::lightnessPower);
    f->glUniform1f(d->glass["factor"], factor);
    f->glUniform2f(d->glass["tintStrength"], KisStudioGlass::tintOverDark(), KisStudioGlass::tintOverBright());
    f->glUniform1f(d->glass["veil"], KisStudioGlass::veil);
    f->glUniform2f(d->glass["viewport"], viewport.width(), viewport.height());
    f->glUniform1f(d->glass["dither"], dither);
    const KisStudioGlass::Rim rim = KisStudioGlass::rim();
    f->glUniform3f(d->glass["rim"], rim.band * pixelRatio, pixelRatio, float(KisStudioGlass::rimLight()));
    // crown glass's 1.51, 1.52 and 1.53 for red, green and blue, their spread made five times as wide
    f->glUniform3f(d->glass["ior"], 1.47f, 1.52f, 1.57f);
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
    for (int k = 0; k < 3; k++) {
        f->glActiveTexture(GL_TEXTURE4 + k);
        f->glBindTexture(GL_TEXTURE_2D, d->finer[k]->texture());
    }
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
        const qreal scale = scaleOf(surface);
        f->glUniform1f(d->glass["sigma"], KisStudioGlass::blurSigma() * scale * pixelRatio);
        f->glUniform1f(d->glass["spacing"], qMax<qreal>(8 * pixelRatio, 2 * KisStudioGlass::blurSigma() * scale * pixelRatio));
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
    for (int k = 0; k < 3; k++) {
        f->glActiveTexture(GL_TEXTURE4 + k);
        f->glBindTexture(GL_TEXTURE_2D, 0);
    }
    f->glActiveTexture(GL_TEXTURE3);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, 0);
    d->glass.release();
    if (d->vao.isCreated()) d->vao.release();
}
