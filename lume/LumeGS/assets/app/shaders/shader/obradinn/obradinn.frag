#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// From Core:
#include "render/shaders/common/render_color_conversion_common.h"

// sets
layout(set = 0, binding = 0, std140) uniform uShaderData
{
    vec4 inParams[2];
    uvec4 inParams2[1];
};
layout(set = 0, binding = 1) uniform sampler2D uImgSampler;

// in / out
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

float Random(const vec2 st)
{
    return fract(sin(dot(st.xy, vec2(12.9898f, 78.233f))) * 43758.5453123f);
}

void main(void)
{
    const bool isTargetSrgb = inParams2[0].z == 1;

    const vec3 color1 = vec3(float((inParams2[0].x >> 16) & 0xFF) / 255.0f,
        float((inParams2[0].x >> 8) & 0xFF) / 255.0f, float(inParams2[0].x & 0xFF) / 255.0f);

    const vec3 color2 = vec3(float((inParams2[0].y >> 16) & 0xFF) / 255.0f,
        float((inParams2[0].y >> 8) & 0xFF) / 255.0f, float(inParams2[0].y & 0xFF) / 255.0f);

    const float noiseStrength = inParams[1].x;
    const float threshold = inParams[1].y;

    vec4 color = textureLod(uImgSampler, inUv, 0);
    if (isTargetSrgb) {
        color.rgb = LinearToSrgb(color.rgb);
    }

    const vec4 fragPos = gl_FragCoord;

    const float gray = 0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b;

    const int x = int(mod(fragPos.x, 8.0));
    const int y = int(mod(fragPos.y, 8.0));
    const int index = (x * 8) + y;

    const float bayerMatrix[64] = float[64](0.0, 48.0, 12.0, 60.0, 3.0, 51.0, 15.0, 61.0, 32.0, 16.0, 44.0, 28.0, 35.0,
        19.0, 47.0, 31.0, 8.0, 56.0, 4.0, 52.0, 11.0, 59.0, 7.0, 55.0, 40.0, 24.0, 36.0, 20.0, 43.0, 27.0, 39.0, 23.0,
        2.0, 50.0, 14.0, 62.0, 1.0, 49.0, 13.0, 61.0, 34.0, 18.0, 46.0, 30.0, 33.0, 17.0, 45.0, 29.0, 10.0, 58.0, 6.0,
        54.0, 9.0, 57.0, 5.0, 53.0, 42.0, 26.0, 38.0, 22.0, 41.0, 25.0, 37.0, 21.0);

    const float dither = bayerMatrix[index] / 64.0f;

    const float noise = Random(fragPos.xy);
    const float noiseMod = noise * noiseStrength * pow(1.0f - gray, 2.0f);

    if (gray - noiseMod < dither * 2.0f) {
        outColor = vec4(color2, 1.0f);
    } else {
        outColor = vec4(color1, 1.0f);
    }

    const float range = 0.2f;
    const float upperThr = threshold + range;
    const float lowerThr = threshold - range;

    if (gray > upperThr) {
        outColor = vec4(color1, 1.0f);
    } else if (gray < lowerThr) {
        outColor = vec4(color2, 1.0f);
    }

    if (isTargetSrgb) {
        outColor.rgb = SrgbToLinear(outColor.rgb);
    }
}
