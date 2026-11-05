#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// From Core:
#include "render/shaders/common/render_color_conversion_common.h"

// sets
layout(set = 0, binding = 0, std140) uniform uShaderData
{
    vec4 inParams[3];
};
layout(set = 0, binding = 1) uniform sampler2D uImgSampler;

// in / out
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

float LuminanceFromColor(const vec3 color) {
    return dot(color, vec3(0.299f, 0.587f, 0.114f));
}

vec3 AdjustTemperature(const vec3 color, const float temperature) {
    const float tempScale = temperature * 0.1f;
    const vec3 warmColor = vec3(1.0f + tempScale, 1.0f + tempScale * 0.5f, 1.0f - tempScale);

    return color * warmColor;
}

vec3 AdjustTint(const vec3 color, const float tint) {
    const float tintScale = tint * 0.1f;
    const vec3 tintColor = vec3(1.0f + tintScale, 1.0f - tintScale, 1.0f + tintScale);

    return color * tintColor;
}

vec3 AdjustSaturation(const vec3 color, const float saturation) {
    const float luminance = LuminanceFromColor(color);
    const vec3 gray = vec3(luminance);

    return mix(gray, color, saturation);
}

vec3 AdjustVibrance(const vec3 color, const float vibrance) {
    const float luminance = LuminanceFromColor(color);
    const float maxColor = max(max(color.r, color.g), color.b);
    const float minColor = min(min(color.r, color.g), color.b);
    const float colorRange = maxColor - minColor;

    const float vibranceAmount = vibrance * (1.0f - colorRange);
    const vec3 gray = vec3(luminance);

    return mix(gray, color, 1.0f + vibranceAmount);
}

void main(void) {
    const float exposure = inParams[1].x;
    const float contrast = inParams[1].y;
    const float temperature = inParams[1].z;
    const float tint = inParams[1].w;
    const float saturation = inParams[2].x;
    const float vibrance = inParams[2].y;

    const vec4 rawSample = textureLod(uImgSampler, inUv, 0);
    vec3 color = rawSample.rgb;

    // exposure
    color *= pow(2.0f, exposure);

    // contrast
    color = (color - 0.5f) * contrast + 0.5f;

    // temperature
    color = AdjustTemperature(color, temperature);

    // color tint
    color = AdjustTint(color, tint);

    // saturation
    color = AdjustSaturation(color, saturation);

    // vibrance
    color = AdjustVibrance(color, vibrance);

    // ensure valid
    color = clamp(color, 0.0f, 1.0f);

    outColor = vec4(color.rgb, rawSample.a);
}