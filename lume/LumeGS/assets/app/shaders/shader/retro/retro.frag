#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// From Core:
#include "render/shaders/common/render_color_conversion_common.h"

// sets
layout(set = 0, binding = 0, std140) uniform uShaderData
{
    vec4 inParams[2];
};
layout(set = 0, binding = 1) uniform sampler2D uImgSampler;

// in / out
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#define PI 3.14159265359f

const float MASK_BORDER = 0.9f;
const float MASK_INTENSITY = 0.2f;
const float SPREAD = 0.0005f;
const float SWEEP_DURATION = 2.0f;
const float DELAY_DURATION = 2.0f;
const float SCANLINE_THICKNESS = 0.005f;
const float SCANLINE_H_OFFSET_STRENGTH = 0.009f;

float Random(const vec2 cc) {
    return fract(sin(dot(cc.xy, vec2(12.9898f, 78.233f))) * 43758.5453f);
}

// 2D Noise based on Morgan McGuire @morgan3d
// https://www.shadertoy.com/view/4dS3Wd
float Noise (const vec2 st) {
    const vec2 ii = floor(st);
    const vec2 ff = fract(st);

    const float aa = Random(ii);
    const float bb = Random(ii + vec2(1.0f, 0.0f));
    const float cc = Random(ii + vec2(0.0f, 1.0f));
    const float dd = Random(ii + vec2(1.0f, 1.0f));

    const vec2 uu = ff * ff * (3.0f - 2.0f * ff);

    return mix(aa, bb, uu.x) + (cc - aa)* uu.y * (1.0f - uu.x) + (dd - bb) * uu.x * uu.y;
}

const float bayerMatrix8x8[64] = float[64](0.0f / 64.0f, 48.0f / 64.0f, 12.0f / 64.0f, 60.0f / 64.0f, 3.0f / 64.0f,
    51.0f / 64.0f, 15.0f / 64.0f, 63.0f / 64.0f, 32.0f / 64.0f, 16.0f / 64.0f, 44.0f / 64.0f, 28.0f / 64.0f, 35.0f / 64.0f,
    19.0f / 64.0f, 47.0f / 64.0f, 31.0f / 64.0f, 8.0f / 64.0f, 56.0f / 64.0f, 4.0f / 64.0f, 52.0f / 64.0f, 11.0f / 64.0f,
    59.0f / 64.0f, 7.0f / 64.0f, 55.0f / 64.0f, 40.0f / 64.0f, 24.0f / 64.0f, 36.0f / 64.0f, 20.0f / 64.0f, 43.0f / 64.0f,
    27.0f / 64.0f, 39.0f / 64.0f, 23.0f / 64.0f, 2.0f / 64.0f, 50.0f / 64.0f, 14.0f / 64.0f, 62.0f / 64.0f, 1.0f / 64.0f,
    49.0f / 64.0f, 13.0f / 64.0f, 61.0f / 64.0f, 34.0f / 64.0f, 18.0f / 64.0f, 46.0f / 64.0f, 30.0f / 64.0f, 33.0f / 64.0f,
    17.0f / 64.0f, 45.0f / 64.0f, 29.0f / 64.0f, 10.0f / 64.0f, 58.0f / 64.0f, 6.0f / 64.0f, 54.0f / 64.0f, 9.0f / 64.0f,
    57.0f / 64.0f, 5.0f / 64.0f, 53.0f / 64.0f, 42.0f / 64.0f, 26.0f / 64.0f, 38.0f / 64.0f, 22.0f / 64.0f, 41.0f / 64.0f,
    25.0f / 64.0f, 37.0f / 64.0f, 21.0f / 64.0f);

vec3 Dither(const vec2 uv, vec3 color) {
    const vec2 resolution = inParams[0].xy;
    const float colorNum = inParams[1].x;

    // const int xx = int(uv.x * resolution.x) % 8;
    // const int yy = int(uv.y * resolution.y) % 8;
    // const float threshold = bayerMatrix8x8[yy * 8 + xx];

    // color.rgb += threshold * 0.0;
    color.r = floor(color.r * (colorNum - 1.0f) + 0.5f) / (colorNum - 1.0f);
    color.g = floor(color.g * (colorNum - 1.0f) + 0.5f) / (colorNum - 1.0f);
    color.b = floor(color.b * (colorNum - 1.0f) + 0.5f) / (colorNum - 1.0f);

    return color;
}

void main(void)
{
    const bool isTargetSrgb = inParams[0].w == 1.0f;

    const float colorNum = inParams[1].x;
    const float curvature = inParams[1].w;
    const bool doBlend = inParams[1].z == 1.0f;
    const vec2 resolution = inParams[0].xy;
    const float time = inParams[0].z;
    const float pixelSize = inParams[1].y;

    vec2 curveUV = inUv * 2.0f - 1.0f;
    const vec2 offset = curveUV.yx * curvature;
    curveUV += curveUV * offset * offset;
    curveUV = curveUV * 0.5f + 0.5f;

    // XOR shader CRT mask
    const vec2 pixel = inUv * resolution;
    const vec2 coord = pixel / pixelSize;
    const vec2 subcoord = coord * vec2(3.0f, 1.0f);
    const vec2 cellOffset = vec2(0.0f, mod(floor(coord.x), 3.0f) * 0.1f);

    const float ind = mod(floor(subcoord.x), 3.0f);
    vec3 maskColor = vec3(ind == 0.0f, ind == 1.0f, ind == 2.0f) * 2.0f;

    const vec2 cellUV = fract(subcoord + cellOffset) * 2.0f - 1.0f;
    const vec2 border = 1.0f - cellUV * cellUV * MASK_BORDER;
    maskColor.rgb *= border.x * border.y;

    vec2 rgbCellUV = floor(coord + cellOffset) * pixelSize / resolution;

    const float cycleDuration = SWEEP_DURATION + DELAY_DURATION;

    const float timeInCycle = mod(time, cycleDuration);
    const float line_factor = 1.0f; 
    float hOffset = 0.0f;

    if (timeInCycle < SWEEP_DURATION) {
        const float lineY = (timeInCycle / SWEEP_DURATION);
        const float dist = abs(inUv.y - lineY);

        const float isInsideScanline = 1.0f - step(SCANLINE_THICKNESS, dist);

        hOffset = SCANLINE_H_OFFSET_STRENGTH * isInsideScanline; 
    }

    curveUV.x += hOffset;
    rgbCellUV.x += hOffset;

    vec4 color = vec4(1.0f);
    color.r = textureLod(uImgSampler, rgbCellUV + SPREAD, 0).r;
    color.g = textureLod(uImgSampler, inUv, 0).g;
    color.b = textureLod(uImgSampler, rgbCellUV - SPREAD, 0).b;

    if (isTargetSrgb) {
        color.rgb = LinearToSrgb(color.rgb);
    }

    color.rgb = Dither(rgbCellUV, color.rgb);

    if (doBlend) {
        color.rgb *= 1.0f + (maskColor - 1.0f) * MASK_INTENSITY;
    } else {
        color.rgb *= maskColor;
    }

    const vec2 edge = smoothstep(0.0f, 0.02f, curveUV) * (1.0f - smoothstep(1.0f - 0.02f, 1.0f, curveUV));
    color.rgb *= edge.x * edge.y;

    outColor = color;

    if (isTargetSrgb) {
        outColor.rgb = SrgbToLinear(outColor.rgb);
    }
}
