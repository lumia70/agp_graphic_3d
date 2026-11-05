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

// Sobel kernels
const mat3 Gx = mat3(-1, -2, -1, 0, 0, 0, 1, 2, 1);
const mat3 Gy = mat3(-1, 0, 1, -2, 0, 2, -1, 0, 1);

vec4 computeStructureTensor(const vec2 uv)
{
    const vec2 resolution = inParams[0].xy;

    const vec3 tx0y0 = textureLod(uImgSampler, uv + vec2(-1, -1) / resolution.xy, 0).rgb;
    const vec3 tx0y1 = textureLod(uImgSampler, uv + vec2(-1, 0) / resolution.xy, 0).rgb;
    const vec3 tx0y2 = textureLod(uImgSampler, uv + vec2(-1, 1) / resolution.xy, 0).rgb;
    const vec3 tx1y0 = textureLod(uImgSampler, uv + vec2(0, -1) / resolution.xy, 0).rgb;
    const vec3 tx1y1 = textureLod(uImgSampler, uv + vec2(0, 0) / resolution.xy, 0).rgb;
    const vec3 tx1y2 = textureLod(uImgSampler, uv + vec2(0, 1) / resolution.xy, 0).rgb;
    const vec3 tx2y0 = textureLod(uImgSampler, uv + vec2(1, -1) / resolution.xy, 0).rgb;
    const vec3 tx2y1 = textureLod(uImgSampler, uv + vec2(1, 0) / resolution.xy, 0).rgb;
    const vec3 tx2y2 = textureLod(uImgSampler, uv + vec2(1, 1) / resolution.xy, 0).rgb;

    const vec3 Sx = Gx[0][0] * tx0y0 + Gx[1][0] * tx1y0 + Gx[2][0] * tx2y0 + Gx[0][1] * tx0y1 + Gx[1][1] * tx1y1 +
                    Gx[2][1] * tx2y1 + Gx[0][2] * tx0y2 + Gx[1][2] * tx1y2 + Gx[2][2] * tx2y2;

    const vec3 Sy = Gy[0][0] * tx0y0 + Gy[1][0] * tx1y0 + Gy[2][0] * tx2y0 + Gy[0][1] * tx0y1 + Gy[1][1] * tx1y1 +
                    Gy[2][1] * tx2y1 + Gy[0][2] * tx0y2 + Gy[1][2] * tx1y2 + Gy[2][2] * tx2y2;

    return vec4(dot(Sx, Sx), dot(Sy, Sy), dot(Sx, Sy), 1.0);
}

void main(void)
{
    outColor = computeStructureTensor(inUv);
}
