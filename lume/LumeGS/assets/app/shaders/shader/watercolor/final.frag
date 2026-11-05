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
// layout(set = 0, binding = 2) uniform sampler2D uWatesrcolorTexture;

// in / out
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

float Hash(vec2 pp)
{
    pp = fract(pp * vec2(123.34, 456.21));
    pp += dot(pp, pp + 45.32);
    return fract(pp.x * pp.y);
}

// 2D value noise
float Noise(const vec2 p)
{
    const vec2 ii = floor(p);
    const vec2 ff = fract(p);

    const float aa = Hash(ii);
    const float bb = Hash(ii + vec2(1.0, 0.0));
    const float cc = Hash(ii + vec2(0.0, 1.0));
    const float dd = Hash(ii + vec2(1.0, 1.0));

    // Smooth interpolation
    const vec2 u = ff * ff * (3.0 - 2.0 * ff);
    return mix(mix(aa, bb, u.x), mix(cc, dd, u.x), u.y);
}

// Fractional Brownian Motion
float Fbm(const vec2 p)
{
    float value = 0.0f;
    float amplitude = 0.5f;
    float frequency = 3.0f;

    for (int ii = 0; ii < 5; ii++) {
        value += amplitude * Noise(p * frequency);
        amplitude *= 0.5f;
        frequency *= 2.0f;
    }

    return value;
}

vec4 GeneratePaperTexture(const vec2 uv)
{
    const vec2 scaled = uv * 50.0f;

    const vec3 paperColor = vec3(0.95f, 0.94f, 0.92f);
    const float grain = Fbm(scaled);

    const float detail = Noise(scaled * 5.0f) * 0.05f;
    const float pattern = grain * 0.2f + detail;

    const vec3 colorVariation = vec3(pattern * 0.1f);
    const vec3 finalColor = paperColor - colorVariation;
    const float alpha = 0.9f + pattern * 0.1f;

    return vec4(finalColor, alpha);
}

vec3 ACESFilm(const vec3 x)
{
    const float aa = 2.51f;
    const float bb = 0.03f;
    const float cc = 2.43f;
    const float dd = 0.59f;
    const float ee = 0.14f;
    return clamp((x * (aa * x + bb)) / (x * (cc * x + dd) + ee), 0.0f, 1.0f);
}

vec3 sat(vec3 rgb, float adjustment)
{
    vec3 W = vec3(0.2125, 0.7154, 0.0721); // Luminance weights
    vec3 intensity = vec3(dot(rgb, W));
    return mix(intensity, rgb, adjustment);
}

void main(void)
{
    const bool isTargetSrgb = inParams[0].w == 1.0f;
    const float threshold = inParams[1].y;

    vec3 color = textureLod(uImgSampler, inUv, 0).rgb;
    if (isTargetSrgb) {
        color.rgb = LinearToSrgb(color.rgb);
    }

    // generate a dynamic watercolor paper texture
    // vec4 watercolorColor = textureLod(uWatercolorTexture, inUv, 0);
    const vec4 watercolorColor = GeneratePaperTexture(inUv);

    const vec3 grayscale = vec3(dot(color, vec3(0.299f, 0.587f, 0.114f)));

    const int nn = 16;
    const float x = (grayscale.r + grayscale.g + grayscale.b) / 3.0f;
    float qn = floor(x * float(nn - 1.0f) + 0.5f) / float(nn - 1.0f);
    qn = clamp(qn, 0.2f, 0.7f);

    if (qn < threshold) {
        color = mix(vec3(0.1f), color.rgb, qn * 2.0f);
    } else {
        color = mix(color.rgb, vec3(1.0f), (qn - 0.5f) * 2.0f);
    }

    color = sat(color, 1.5f);
    color = ACESFilm(color);
    vec4 outputColor = vec4(color, 1.0f);
    outputColor *= watercolorColor;

    outColor = outputColor;

    if (isTargetSrgb) {
        outColor.rgb = SrgbToLinear(outColor.rgb);
    }
}
