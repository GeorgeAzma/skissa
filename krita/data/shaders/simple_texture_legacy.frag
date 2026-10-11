/*
 *  shader for handling scaling
 *
 *  SPDX-License-Identifier: GPL-3.0-or-later
 */

uniform sampler2D texture0;

varying mediump vec4 v_textureCoordinate;

// the image in grays, each as light as its color looks (its luminance): to judge the values
uniform float grayscale;

vec4 toGray(vec4 c)
{
    vec3 linear = mix(c.rgb / 12.92, pow((c.rgb + 0.055) / 1.055, vec3(2.4)), step(0.04045, c.rgb));
    float y = dot(linear, vec3(0.2126, 0.7152, 0.0722));
    float encoded = y <= 0.0031308 ? 12.92 * y : 1.055 * pow(y, 1.0 / 2.4) - 0.055;
    return vec4(vec3(encoded), c.a);
}

void main() {
    vec4 col = texture2D(texture0, v_textureCoordinate.st);


#ifdef USE_OCIO
    gl_FragColor = OCIODisplay(col);
#else /* USE_OCIO */
    gl_FragColor = col;
#endif /* USE_OCIO */

    if (grayscale > 0.5) {
        gl_FragColor = toGray(gl_FragColor);
    }
}
