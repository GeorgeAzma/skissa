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
 * A glass panel: a pane held slightly above the canvas, with a frosted face and a polished bevel
 * curving down to the rim. Each pixel follows the view ray down through the glass to the canvas:
 *
 *  - On the flat face it passes straight through and the frost scatters it: the blur, tinted.
 *  - On the bevel it refracts on entry (Snell's law) and again leaving the flat underside, so the
 *    frost appears bent and magnified toward the rim and joins the face without a seam. Each color
 *    channel has its own index of refraction, giving a faint dispersion fringe. Where the bevel is
 *    steep the ray is totally internally reflected and shows the frost at that point.
 *  - Where the bevel is steep the face also reflects (Fresnel) the canvas around the panel, so the
 *    rim picks up the surrounding colors.
 *
 * There are no highlights or lighting; the edge shows only through refraction. Everything is computed
 * in linear light, tinted like real tinted glass (see KisStudioGlass::veil).
 */
const char *const glassSource = R"(
uniform sampler2D blurred;
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
uniform vec3 bevel;
uniform float polish;
uniform float clearing;
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

// Height above the underside and slope of a bevel w wide and h thick, d pixels in from the rim.
// The profile is a quarter superellipse of exponent 4: steep at the rim and flat at the face,
// with no crease where they meet.
vec2 profile(float d, float w, float h)
{
    float u = 1.0 - clamp(d / w, 0.0, 1.0);
    float u3 = u * u * u;
    float k = max(1.0 - u3 * u, 1e-5);
    return vec2(h * pow(k, 0.25), h / w * u3 * pow(k, -0.75));
}

// The bevel's height at the pixel d in from the edge and its slope averaged over the pixel rather
// than at its center. The slope is unbounded at the rim, but the rise across a pixel is finite, so
// this antialiases the rim into a clean line instead of scattered steep pixels.
vec2 bevelOver(float d, float w, float h)
{
    float a = max(d - 0.5, 0.0);
    float b = max(d + 0.5, a + 1e-3);
    return vec2(profile(0.5 * (a + b), w, h).x, (profile(b, w, h).x - profile(a, w, h).x) / (b - a));
}

// Traces the view ray through a bevel point with surface normal `normal`, glass index n and the face
// `height` above the underside. Returns the offset to where it meets the canvas (xy); z is 1 if it
// reaches the canvas, 0 if it is totally internally reflected, and w is 1 if that reflected ray
// leaves through the face (its direction in `back`).
vec4 trace(vec3 normal, float height, float n, out vec3 back)
{
    // Refract into the face.
    vec3 t = refract(vec3(0.0, 0.0, -1.0), normal, 1.0 / n);
    vec2 offset = t.xy / max(-t.z, 1e-3) * height;
    // Leave through the flat underside and cross the gap down to the canvas.
    float sinGlass = length(t.xy);
    float sinAir = n * sinGlass;
    if (sinAir >= 1.0) {
        // Reflected by the underside, then possibly out through the face.
        back = refract(vec3(t.xy, -t.z), -normal, n);
        return vec4(offset, 0.0, dot(back, back) > 0.0 ? 1.0 : 0.0);
    }
    back = vec3(0.0);
    offset += t.xy / max(sinGlass, 1e-6) * (sinAir / sqrt(1.0 - sinAir * sinAir)) * bevel.z;
    return vec4(offset, 1.0, 0.0);
}

// Reflectance of the glass at an angle of acos(c) to its normal (Schlick's approximation).
float fresnel(float c, float n)
{
    float r = (n - 1.0) / (n + 1.0);
    r *= r;
    return r + (1.0 - r) * pow(1.0 - clamp(c, 0.0, 1.0), 5.0);
}

// Bilinear sample of a weighted downsampled texture (the blur or its statistics) at p (surface
// pixels), within this surface's slot. The weights are interpolated too.
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

// The Oklab mean stored in source at p, decoding the lightness power and the a, b offset.
vec3 meanAt(sampler2D source, vec2 p)
{
    vec3 m = softAt(source, p);
    return vec3(pow(max(m.x, 0.0), 1.0 / power), m.yz - 0.5);
}

// The same in linear light, not yet gamut mapped (that happens once, in tintedLab).
vec3 meanLightAt(sampler2D source, vec2 p)
{
    return max(fromOklab(meanAt(source, p)), 0.0);
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

// The unblurred canvas at q in linear light, as seen through the clear polished rim.
vec3 canvasLightAt(vec2 q)
{
    vec3 c = texture(under, q / viewport).rgb;
    return decode ? fromSrgb(c) : c;
}

/**
 * The canvas area a pixel at q sees through the bevel (jx, jy: its footprint across and down),
 * averaged in linear light. Where the bevel bends little this is about one pixel, keeping all detail;
 * where it compresses a lot of canvas into one pixel, taps are spread along the compression so
 * nothing is skipped (no shimmer or stair steps) and nothing is blurred beyond it.
 */
vec3 clearLab(vec2 q, vec2 jx, vec2 jy)
{
    bool across = dot(jx, jx) > dot(jy, jy);
    vec2 major = across ? jx : jy;
    vec2 minor = across ? jy : jx;
    int n = int(clamp(ceil(length(major)), 1.0, 12.0));
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 12; i++) {
        if (i >= n) break;
        vec2 o = major * ((float(i) + 0.5) / float(n) - 0.5);
        sum += canvasLightAt(q + o + 0.25 * minor) + canvasLightAt(q + o - 0.25 * minor);
    }
    return toOklab(sum / float(2 * n));
}

// Clamps a point the rim sees to the area around the panel that was blurred (reach). A steep rim
// can look further than that.
vec2 held(vec2 q)
{
    return clamp(q, reach.xy, reach.zw);
}

// The canvas where the reflected ray d (z toward the viewer) from p meets it, blurred because
// the frosted glass is not a mirror.
vec3 reflectedAt(vec3 d, vec2 p)
{
    return meanLightAt(blurred, held(p + d.xy / max(-d.z, 0.2) * bevel.z));
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

vec3 tinted(vec3 behind, vec3 tintLab, float s)
{
    return tintedLab(toOklab(behind), tintLab, s);
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
 * Light the glowing image casts on the glass beside it. The rim catches it where it faces the image
 * and the bevel is steep enough to reflect it (Fresnel): a thin line in the colors of the image's
 * nearest edge, fading with distance. Nothing over the image itself.
 */
vec3 imageLightAt(vec2 p, vec3 normal, vec2 normal2d, bool onBevel)
{
    if (!onBevel) {
        return vec3(0.0);
    }
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
    float facing = max(dot(normal2d, (q - p) / dist), 0.0);
    float faceOn = fresnel(1.0, ior.g);
    float edge = (fresnel(normal.z, ior.g) - faceOn) / (1.0 - faceOn);
    return light * falloff * 0.45 * facing * edge * smoothstep(0.0, 24.0, dist);
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    vec2 normal2d;
    float d = -outside(p, normal2d);

    // The bevel is no wider than the corner radius or the panel itself.
    float w = min(bevel.x, min(corner.x, min(shape.z, shape.w)));
    bool onBevel = w > 0.5 && d < w;
    float h = bevel.y * w / max(bevel.x, 1e-3);
    vec2 bend = vec2(0.0);
    vec3 normal = vec3(0.0, 0.0, 1.0);
    // Where this pixel sees the canvas (green ray) and its footprint. Computed before any discard,
    // since derivatives are taken across the 2x2 pixel quad.
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
    // How much comes from the canvas rather than the plain panel beyond its edges.
    float canvas = within(p.x, beyond.x) * within(viewport.x - p.x, beyond.y) * within(p.y, beyond.z) * within(viewport.y - p.y, beyond.w);
    vec3 tintLight = fromSrgb(tint);
    vec3 tintLab = toOklab(tintLight);

    vec3 color;
    if (onBevel) {
        // Polished toward the rim it shows the bent canvas itself, fading into the frost of the face.
        float clear = polish * (1.0 - smoothstep(clearing, 1.0, d / w));

        vec3 light3 = vec3(0.0);
        for (int i = 0; i < 3; i++) {
            vec3 back;
            vec4 ray = trace(normal, bend.x, ior[i], back);
            // A ray reflected back inside shows the frost at this pixel. The result is mixed with the
            // tint in Oklab like the face, so the bevel blends into it without a step.
            bool leaves = ray.z > 0.0;
            vec2 q = leaves ? held(p + ray.xy) : p;
            vec3 seen = frostedLab(q);
            if (leaves && clear > 0.0) seen = mix(seen, clearLab(q, jx, jy), clear);
            seen = throughLower(seen, q);
            vec3 through = max(fromOklab(mix(tintLab, seen, canvas)), 0.0);
            light3[i] = through[i];
        }
        // Thinner glass toward the rim tints less (Beer-Lambert), giving a band along the edge that reads
        // like a border.
        color = tinted(light3, tintLab, strength * mix(0.3, 1.0, bend.x / h));
        // Steep parts reflect (Fresnel) the canvas around the panel. Upward reflections of the room
        // are already part of the tint, so there is no gloss.
        vec3 mirrored = reflect(vec3(0.0, 0.0, -1.0), normal);
        float faceOn = fresnel(1.0, ior.g);
        float reflected = (fresnel(normal.z, ior.g) - faceOn) / (1.0 - faceOn) * smoothstep(0.0, 0.25, -mirrored.z);
        color = mix(color, reflectedAt(mirrored, p), reflected);
    } else {
        color = tintedLab(mix(tintLab, throughLower(frostedLab(p), p), canvas), tintLab, strength);
    }
    if (imageLit) {
        color += imageLightAt(p, normal, normal2d, onBevel);
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

/// A shader program and its uniform locations.
struct Program {
    QSharedPointer<QOpenGLShaderProgram> program;
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
    Program shrink;
    Program blur;
    Program glass;
    Program ambientShrink;
    Program ambient;
    bool valid {false};

    void make()
    {
        valid = shrink.make(quadSource, shrinkSource, {"rect", "source", "shift", "factor", "decode", "salience", "power", "statistics"})
            && blur.make(quadSource, blurSource, {"rect", "source", "slot", "direction", "taps", "weights"})
            && glass.make(glassVertexSource, glassSource,
                          {"rect", "blurred", "statistics", "shift", "slot", "restore", "power",
                           "factor", "shape", "corner", "tint", "tintLightness", "tintStrength", "opacity", "viewport", "sigma",
                           "beyond", "spacing", "dither", "veil", "bevel", "polish", "clearing", "ior", "under", "decode", "reach",
                           "imageLit", "toImage", "fromImage", "imageLight", "imageLightFactor", "imageLightReach",
                           "lowerCount", "lowerShape", "lowerCorner", "lowerTint", "lowerStrength"})
            && ambientShrink.make(quadSource, ambientShrinkSource, {"rect", "source", "factor", "decode", "toImage"})
            && ambient.make(quadSource, ambientSource, {"rect", "ambient", "factor", "toImage", "base", "strength", "encode", "dither"});
    }
};

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
    QOpenGLVertexArrayObject vao;
    /// Downsampled canvas, blurred across, then down; and the same for its Oklab statistics.
    QScopedPointer<QOpenGLFramebufferObject> small;
    QScopedPointer<QOpenGLFramebufferObject> across;
    QScopedPointer<QOpenGLFramebufferObject> blurred;
    QScopedPointer<QOpenGLFramebufferObject> smallStatistics;
    QScopedPointer<QOpenGLFramebufferObject> acrossStatistics;
    QScopedPointer<QOpenGLFramebufferObject> statistics;
    /// Intermediate texture format: half floats where renderable, eight bits otherwise.
    GLenum keptFormat {GL_RGBA};
    /// State of the last blur, which is redone only when the canvas or the surfaces change.
    QVector<QRectF> blurredFor;
    KisOpenGLGpuTiming timing {QStringLiteral("glass gpu:"), {QStringLiteral("shrink"), QStringLiteral("blur"), QStringLiteral("draw")}};
    QSize blurredViewport;
    qreal blurredRatio {0};
    qreal blurredAmount {0};

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

    /// Grows the textures to at least \p size; they never shrink, as panels come and go.
    void ensureSize(const QSize &size)
    {
        if (small && small->width() >= size.width() && small->height() >= size.height()) {
            return;
        }
        const QSize grown = small ? size.expandedTo(small->size()) : size;
        for (QScopedPointer<QOpenGLFramebufferObject> *fbo : {&small, &across, &blurred, &smallStatistics, &acrossStatistics, &statistics}) {
            fbo->reset(texture(grown));
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

qreal KisOpenGLGlass::reach()
{
    // Three sigma of the widest blur, plus the pixels merged by downsampling.
    return 3 * KisStudioGlass::blurSigma() * KisStudioGlass::maxBlurScale + 8;
}

void KisOpenGLGlass::render(uint canvas, uint canvasFormat, const QSize &viewport, qreal pixelRatio,
                            const QVector<Surface> &surfaces, const QRect &clip, bool canvasChanged)
{
    // A floating point (HDR) canvas is already linear and needs no dithering.
    const bool linearCanvas = canvasFormat == GL_RGBA16F;
    const float dither = linearCanvas ? 0.0f : canvasFormat == GL_RGB10_A2 ? 1.0f / 1023 : 1.0f / 255;
    // The blur is stale even if nothing is drawn now, so a panel shown later must not reuse it.
    if (canvasChanged) {
        d->blurredFor.clear();
    }
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

    // Downsample by a factor that keeps blur widths constant in widget pixels at any screen scale.
    const int factor = qMax(1, qRound(4 * pixelRatio));
    // Larger panels blur wider (see KisStudioGlass::blurScale).
    auto scaleOf = [pixelRatio](const Surface &surface) { return KisStudioGlass::blurScale(surface.rect.size() / pixelRatio); };

    const QSize smallSize((viewport.width() + factor - 1) / factor, (viewport.height() + factor - 1) / factor);
    const QRect smallWhole(QPoint(), smallSize);

    /**
     * Each panel is blurred in its own slot of the small textures, since panels blur by different
     * amounts and a shared area would let one panel's blur bleed into a neighbor's. A slot covers the
     * panel's downsampled pixels (plus one for upscaling), what the rim sees around it, and the blur's
     * reach beyond that. The whole panel is blurred because the brightness behind it sets its tint.
     */
    struct Region {
        QRect area;
        QRect around;
        /// Offset from the downsampled canvas to the slot.
        QPoint shift;
        Kernel wide;
    };
    QVector<Region> regions(surfaces.size());
    const int rimReach = qCeil(KisStudioGlass::bevel().lift * pixelRatio);
    {
        // Pack the slots in rows, left to right.
        QPoint next;
        int rowHeight = 0;
        for (int i = 0; i < surfaces.size(); i++) {
            const QRect r = surfaces[i].rect.toAlignedRect().adjusted(-rimReach, -rimReach, rimReach, rimReach) & whole;
            if (r.isEmpty()) continue;
            Region &region = regions[i];
            region.area = QRect(QPoint(r.left() / factor, r.top() / factor), QPoint(r.right() / factor + 1, r.bottom() / factor + 1)) & smallWhole;
            region.wide = gaussian(KisStudioGlass::blurSigma() * scaleOf(surfaces[i]) * pixelRatio / factor);
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
    // Reuse the last blur when nothing behind the glass changed, e.g. only the brush outline moved.
    QVector<QRectF> rects;
    for (const Surface &surface : surfaces) rects << surface.rect;
    const bool blurAgain = canvasChanged || rects != d->blurredFor || viewport != d->blurredViewport || pixelRatio != d->blurredRatio
        || KisStudioGlass::blur() != d->blurredAmount;
    d->blurredFor = rects;
    d->blurredViewport = viewport;
    d->blurredRatio = pixelRatio;
    d->blurredAmount = KisStudioGlass::blur();

    // Blur only the panels on the canvas, and only if what is behind them changed.
    QVector<Region> blurring;
    if (blurAgain) {
        for (const Region &region : qAsConst(regions)) {
            if (!region.around.isEmpty()) blurring << region;
        }
    }
    // Downsample the canvas, as colors and as statistics.
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
    // Run each pass for all panels at once, so the render target (which stalls the GPU) switches four
    // times in total rather than four times per panel.
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
    d->glass->bind();
    f->glUniform1i(d->glass["blurred"], 0);
    f->glUniform1i(d->glass["statistics"], 1);
    f->glUniform1i(d->glass["under"], 2);
    f->glUniform1i(d->glass["decode"], !linearCanvas);
    f->glUniform1f(d->glass["restore"], KisStudioGlass::restore);
    f->glUniform1f(d->glass["power"], KisStudioGlass::lightnessPower);
    f->glUniform1f(d->glass["factor"], factor);
    f->glUniform2f(d->glass["tintStrength"], KisStudioGlass::tintOverDark(), KisStudioGlass::tintOverBright());
    f->glUniform1f(d->glass["veil"], KisStudioGlass::veil);
    f->glUniform2f(d->glass["viewport"], viewport.width(), viewport.height());
    f->glUniform1f(d->glass["dither"], dither);
    const KisStudioGlass::Bevel bevel = KisStudioGlass::bevel();
    f->glUniform3f(d->glass["bevel"], bevel.width * pixelRatio, bevel.thickness * pixelRatio, bevel.lift * pixelRatio);
    f->glUniform1f(d->glass["polish"], bevel.polish);
    f->glUniform1f(d->glass["clearing"], bevel.clear);
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
        f->glUniform1f(d->glass["spacing"], 2 * KisStudioGlass::blurSigma() * scale * pixelRatio);
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
