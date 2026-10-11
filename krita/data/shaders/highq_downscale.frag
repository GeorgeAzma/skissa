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

vec4 filterPureLinear8(sampler2D texture, vec2 texcoord)
{
    float newTexelSize = texelSize / viewportScale;
    float support = 0.5 * newTexelSize;
    float step = texelSize * 1.0;

    float offset = support - 0.5*texelSize;

    float level = 0.0;


    if (viewportScale < 0.03125) {
        level = 4.0;
    } else if (viewportScale < 0.0625) {
        level = 3.0;
    } else if (viewportScale < 0.125) {
        level = 2.0;
    } else if (viewportScale < 0.25) {
        level = 1.0;
    }

/*
    vec4 p1 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y - offset), level);
    vec4 p2 = 2.0*textureLod(texture, vec2(texcoord.x         , texcoord.y - offset), level);
    vec4 p3 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y - offset), level);

    vec4 p4 = 2.0*textureLod(texture, vec2(texcoord.x - offset, texcoord.y), level);
    vec4 p5 = 5.0*textureLod(texture, vec2(texcoord.x         , texcoord.y), level);
    vec4 p6 = 2.0*textureLod(texture, vec2(texcoord.x + offset, texcoord.y), level);

    vec4 p7 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y + offset), level);
    vec4 p8 = 2.0*textureLod(texture, vec2(texcoord.x         , texcoord.y + offset), level);
    vec4 p9 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y + offset), level);

    return (p1 + p2 + p3 + p4 + p5 + p6 + p7 + p8 + p9) / 17.0;
*/

    vec4 p1 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y - offset), level);
    vec4 p2 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y - offset), level);

    vec4 p3 = vec4(4.0) * textureLod(texture, vec2(texcoord.x, texcoord.y), level);

    vec4 p4 = textureLod(texture, vec2(texcoord.x - offset, texcoord.y + offset), level);
    vec4 p5 = textureLod(texture, vec2(texcoord.x + offset, texcoord.y + offset), level);

    vec4 p6 = vec4(3.0) * textureLod(texture, vec2(texcoord.x, texcoord.y), level + 1.0);

    return (p1 + p2 + p3 + p4 + p5 + p6) / vec4(11.0);

}

#endif /* HIGHQ_SCALING */

void main() {
    vec4 col;

#if defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH

    if (viewportScale < 0.5 - eps) {

#ifdef DIRECT_LOD_FETCH

        if (fixedLodLevel > eps) {
            col = textureLod(texture0, v_textureCoordinate.st, fixedLodLevel);
        } else

#endif /* DIRECT_LOD_FETCH */

        {

#ifdef HIGHQ_SCALING
            col = filterPureLinear8(texture0, v_textureCoordinate.st);
#else /* HIGHQ_SCALING */
            col = texture(texture0, v_textureCoordinate.st);
#endif /* HIGHQ_SCALING */

        }
    } else

#endif /* defined HIGHQ_SCALING || defined DIRECT_LOD_FETCH */

    {
#ifdef DIRECT_LOD_FETCH

        if (fixedLodLevel > eps) {
            col = textureLod(texture0, v_textureCoordinate.st, fixedLodLevel);
        } else

#endif /* DIRECT_LOD_FETCH */
        {
            col = texture(texture0, v_textureCoordinate.st);
        }
    }

#ifdef USE_OCIO
    fragColor = OCIODisplay(col);
#else /* USE_OCIO */
    fragColor = col;
#endif /* USE_OCIO */
    if (valueView) fragColor = valueOf(fragColor);

}
