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


void main(void)
{
    const bool isTargetSrgb = inParams[0].w == 1.0f;

    const float lineThreshold = inParams[1].x;
    const vec3 lineColor = inParams[1].yzw;
    const vec2 resolution = inParams[0].xy;
    const float time = inParams[0].z;

    const vec4 inputColor = textureLod(uImgSampler, inUv, 0);

    const vec2 texel = vec2(1.0f / resolution.x, 1.0f / resolution.y);

    const mat3 dx = mat3(-1.0f, -2.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 1.0f);
    const mat3 dy = mat3(-1.0f, 0.0f, 1.0f, -2.0f, 0.0f, 2.0f, -1.0f, 0.0f, 1.0f);

    const float tx0y0 = textureLod(uImgSampler, inUv + texel * vec2(-1.0f, -1.0f), 0).r;
    const float tx0y1 = textureLod(uImgSampler, inUv + texel * vec2(-1.0f, 0.0f), 0).r;
    const float tx0y2 = textureLod(uImgSampler, inUv + texel * vec2(-1.0f, 1.0f), 0).r;

    const float tx1y0 = textureLod(uImgSampler, inUv + texel * vec2(0.0f, -1.0f), 0).r;
    const float tx1y1 = textureLod(uImgSampler, inUv + texel * vec2(0.0f, 0.0f), 0).r;
    const float tx1y2 = textureLod(uImgSampler, inUv + texel * vec2(0.0f, 1.0f), 0).r;

    const float tx2y0 = textureLod(uImgSampler, inUv + texel * vec2(1.0f, -1.0f), 0).r;
    const float tx2y1 = textureLod(uImgSampler, inUv + texel * vec2(1.0f, 0.0f), 0).r;
    const float tx2y2 = textureLod(uImgSampler, inUv + texel * vec2(1.0f, 1.0f), 0).r;

    const float dxValue = dx[0][0] * tx0y0 + dx[1][0] * tx1y0 + dx[2][0] * tx2y0 +
        dx[0][1] * tx0y1 + dx[1][1] * tx1y1 + dx[2][1] * tx2y1 +
        dx[0][2] * tx0y2 + dx[1][2] * tx1y2 + dx[2][2] * tx2y2;

    const float dyValue = dy[0][0] * tx0y0 + dy[1][0] * tx1y0 + dy[2][0] * tx2y0 +
        dy[0][1] * tx0y1 + dy[1][1] * tx1y1 + dy[2][1] * tx2y1 +
        dy[0][2] * tx0y2 + dy[1][2] * tx1y2 + dy[2][2] * tx2y2;

    const float grad = sqrt((dxValue * dxValue) + (dyValue * dyValue));

    if (grad > lineThreshold) {
        outColor = vec4(lineColor, 1.0f);
    } else {
        outColor = vec4(vec3(1.0f - grad) * inputColor.rgb, 1.0f);
    }
}
