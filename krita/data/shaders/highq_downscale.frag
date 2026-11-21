/*
 *  shader for handling scaling
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

uniform sampler2D texture0;

in vec4 v_textureCoordinate;
out vec4 fragColor;

const float eps = 1e-6;

/**
 * The value view: each color as a gray as light as it looks (OKLab's
 * lightness, the color panel's), in the display's own values. Its colors are
 * taken to light by the display's curves (valueCurves, row 0: a value of each
 * channel to its light; row 1: a light, at its square root, to the value of
 * it), to linear sRGB by toLinearSrgb; a gray of lightness L is linear sRGB
 * L cubed, which is grayInDisplay times that in the display's light. A linear
 * display (valueLinear: float textures, HDR) needs no curves, and has no
 * ceiling. After OCIO, its display is taken to be sRGB.
 */
uniform bool valueView;
#ifndef USE_OCIO
uniform highp sampler2D valueCurves;
uniform mat3 toLinearSrgb;
uniform vec3 grayInDisplay;
uniform bool valueLinear;

float valueCurve(int row, int channel, float x)
{
    float t = clamp(x, 0.0, 1.0) * 1023.0;
    int i = int(floor(t));
    vec4 a = texelFetch(valueCurves, ivec2(i, row), 0);
    vec4 b = texelFetch(valueCurves, ivec2(min(i + 1, 1023), row), 0);
    return mix(a[channel], b[channel], t - float(i));
}
#endif

float oklabLightness(vec3 c)
{
    vec3 lms = vec3(dot(vec3(0.4122214708, 0.5363325363, 0.0514459929), c),
                    dot(vec3(0.2119034982, 0.6806995451, 0.1073969566), c),
                    dot(vec3(0.0883024619, 0.2817188376, 0.6299787005), c));
    lms = sign(lms) * pow(abs(lms), vec3(1.0 / 3.0));
    return dot(vec3(0.2104542553, 0.7936177850, -0.0040720468), lms);
}

vec4 valueOf(vec4 col)
{
#ifdef USE_OCIO
    vec3 c = clamp(col.rgb, 0.0, 1.0);
    vec3 light = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
    float y = pow(max(oklabLightness(light), 0.0), 3.0);
    float v = y <= 0.0031308 ? 12.92 * y : 1.055 * pow(y, 1.0 / 2.4) - 0.055;
    return vec4(vec3(v), col.a);
#else
    vec3 light = valueLinear ? col.rgb : vec3(valueCurve(0, 0, col.r), valueCurve(0, 1, col.g), valueCurve(0, 2, col.b));
    float y = pow(max(oklabLightness(toLinearSrgb * light), 0.0), 3.0);
    vec3 gray = grayInDisplay * y;
    if (!valueLinear) {
        gray = vec3(valueCurve(1, 0, sqrt(max(gray.r, 0.0))), valueCurve(1, 1, sqrt(max(gray.g, 0.0))), valueCurve(1, 2, sqrt(max(gray.b, 0.0))));
    }
    return vec4(gray, col.a);
#endif
}

#if defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH
uniform float viewportScale;
#endif /* defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH */

#ifdef DIRECT_LOD_FETCH
uniform float fixedLodLevel;
#endif

#ifdef HIGHQ_SCALING

uniform float texelSize;
/// The texels each tile repeats from its neighbors, at full size: 2 to the number of mipmap levels
uniform float texelBorder;

float lanczos3(float x)
{
    x = abs(x);
    if (x < 1e-5) return 1.0;
    if (x >= 3.0) return 0.0;
    float px = 3.14159265 * x;
    return 3.0 * sin(px) * sin(px / 3.0) / (px * px);
}

const int MAX_TAPS = 14;

/**
 * The taps along one axis: the texel centers within \p radius of \p center (in texels), weighted by the kernel
 * stretched to the output's pixels; two next to each other weighing the same way are read as one linear fetch
 * between them, so a 13-texel span takes about 7 reads
 */
int axisTaps(float center, float scale, float radius, out float at[MAX_TAPS], out float weight[MAX_TAPS])
{
    int first = int(ceil(center - radius));
    int last = int(floor(center + radius));
    int n = 0;
    int i = first;
    while (i <= last && n < MAX_TAPS) {
        float w = lanczos3((float(i) - center) * scale);
        float next = i < last ? lanczos3((float(i + 1) - center) * scale) : 0.0;
        if (w * next > 0.0) {
            at[n] = float(i) + next / (w + next);
            weight[n] = w + next;
            i += 2;
        } else {
            at[n] = float(i);
            weight[n] = w;
            i += 1;
        }
        n++;
    }
    return n;
}

/**
 * The image made smaller by a Lanczos-3 filter, as photo editors shrink images: sharper than mipmaps alone, with
 * faint rings by hard edges. It filters the finest mipmap level at most twice as large as the screen, so a pixel
 * reads at most about 7 x 7 fetches; the kernel is kept inside the texels a tile shares with its neighbors, or tiles
 * would show seams (from 25% down it is cut short there, and gets a little softer).
 */
vec4 filterLanczos(sampler2D tex, vec2 texcoord)
{
    float maxLevel = log2(max(texelBorder, 1.0));
    float scale = viewportScale;
    float level = 0.0;
    for (int k = 0; k < 8; k++) {
        if (scale >= 0.5 || level >= maxLevel) break;
        scale *= 2.0;
        level += 1.0;
    }
    float size = texelSize * exp2(level);
    float radius = min(3.0 / scale, max(texelBorder / exp2(level), 1.0));
    vec2 center = texcoord / size - 0.5;

    float ax[MAX_TAPS];
    float wx[MAX_TAPS];
    float ay[MAX_TAPS];
    float wy[MAX_TAPS];
    int nx = axisTaps(center.x, scale, radius, ax, wx);
    int ny = axisTaps(center.y, scale, radius, ay, wy);

    vec4 sum = vec4(0.0);
    float total = 0.0;
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            float w = wx[i] * wy[j];
            sum += w * textureLod(tex, (vec2(ax[i], ay[j]) + 0.5) * size, level);
            total += w;
        }
    }
    vec4 col = sum / max(total, eps);
    // the negative lobes can overshoot below black; light past white is kept for HDR
    col = max(col, vec4(0.0));
    col.a = min(col.a, 1.0);
    return col;
}

#endif /* HIGHQ_SCALING */

void main() {
    vec4 col;

#ifdef DIRECT_LOD_FETCH
    // instant preview's own smaller copy
    if (fixedLodLevel > eps) {
        col = textureLod(texture0, v_textureCoordinate.st, fixedLodLevel);
    } else
#endif /* DIRECT_LOD_FETCH */
#ifdef HIGHQ_SCALING
    if (viewportScale < 1.0 - eps) {
        col = filterLanczos(texture0, v_textureCoordinate.st);
    } else
#endif /* HIGHQ_SCALING */
    {
        col = texture(texture0, v_textureCoordinate.st);
    }

#ifdef USE_OCIO
    fragColor = OCIODisplay(col);
#else /* USE_OCIO */
    fragColor = col;
#endif /* USE_OCIO */
    if (valueView) fragColor = valueOf(fragColor);

}
