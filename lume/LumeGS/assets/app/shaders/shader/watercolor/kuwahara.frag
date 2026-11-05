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
layout(set = 0, binding = 2) uniform sampler2D uTensorSampler;

// in / out
layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;

#define SECTOR_COUNT 8

vec3 SampleColor(const vec2 offset)
{
    const vec2 resolution = inParams[0].xy;
    const vec2 coord = inUv + offset / resolution.xy;
    return textureLod(uImgSampler, coord, 0).rgb;
}

vec4 DominantOrientation(const vec4 structureTensor)
{
    const float Jxx = structureTensor.r;
    const float Jyy = structureTensor.g;
    const float Jxy = structureTensor.b;

    const float trace = Jxx + Jyy;
    const float det = Jxx * Jyy - Jxy * Jxy;

    const float lambda1 = trace * 0.5f + sqrt(trace * trace * 0.25f - det);
    const float lambda2 = trace * 0.5f - sqrt(trace * trace * 0.25f - det);

    const float jxyStrength = abs(Jxy) / (abs(Jxx) + abs(Jyy) + abs(Jxy) + 1e-6f);

    vec2 orientation;

    if (jxyStrength > 0.0f) {
        orientation = normalize(vec2(-Jxy, Jxx - lambda1));
    } else {
        orientation = vec2(0.0f, 1.0f);
    }

    return vec4(normalize(orientation), lambda1, lambda2);
}

float PolynomialWeight(const float xx, const float yy, const float eta, const float lambda)
{
    const float polyValue = (xx + eta) - lambda * (yy * yy);
    return max(0.0f, polyValue * polyValue);
}

void SectorVarianceAndAverageColor(
    const mat2 anisotropyMat, const float angle, const float radius, out vec3 avgColor, out float variance)
{
    vec3 weightedColorSum = vec3(0.0f);
    vec3 weightedSquaredColorSum = vec3(0.0f);
    float totalWeight = 0.0f;

    const float eta = 0.1f;
    const float lambda = 0.5f;

    for (float rr = 1.0f; rr <= radius; rr += 2.0f) {
        for (float aa = -0.392699f; aa < 0.392699f; aa += 0.392699f) {
            vec2 sampleOffset = rr * vec2(cos(angle + aa), sin(angle + aa));
            sampleOffset *= anisotropyMat;

            const vec3 color = SampleColor(sampleOffset);
            const float weight = PolynomialWeight(sampleOffset.x, sampleOffset.y, eta, lambda);

            weightedColorSum += color * weight;
            weightedSquaredColorSum += color * color * weight;
            totalWeight += weight;
        }
    }

    avgColor = weightedColorSum / totalWeight;
    const vec3 varianceRes = (weightedSquaredColorSum / totalWeight) - (avgColor * avgColor);
    // luminance
    variance = dot(varianceRes, vec3(0.299f, 0.587f, 0.114f));
}

void main()
{
    const int radius = int(inParams[1].x);

    const vec4 structureTensor = textureLod(uTensorSampler, inUv, 0);

    vec3 sectorAvgColors[SECTOR_COUNT];
    float sectorVariances[SECTOR_COUNT];

    const vec4 orientationAndAnisotropy = DominantOrientation(structureTensor);
    const vec2 orientation = orientationAndAnisotropy.xy;

    const float anisotropy = (orientationAndAnisotropy.z - orientationAndAnisotropy.w) /
                             (orientationAndAnisotropy.z + orientationAndAnisotropy.w + 1e-6f);

    const float alpha = 25.0f;
    const float scaleX = alpha / (anisotropy + alpha);
    const float scaleY = (anisotropy + alpha) / alpha;

    const mat2 anisotropyMat =
        mat2(orientation.x, -orientation.y, orientation.y, orientation.x) * mat2(scaleX, 0.0f, 0.0f, scaleY);

    for (int ii = 0; ii < SECTOR_COUNT; ii++) {
        const float angle = float(ii) * 6.28318f / float(SECTOR_COUNT);
        SectorVarianceAndAverageColor(anisotropyMat, angle, float(radius), sectorAvgColors[ii], sectorVariances[ii]);
    }

    float minVariance = sectorVariances[0];
    vec3 finalColor = sectorAvgColors[0];

    for (int ii = 1; ii < SECTOR_COUNT; ii++) {
        if (sectorVariances[ii] < minVariance) {
            minVariance = sectorVariances[ii];
            finalColor = sectorAvgColors[ii];
        }
    }

    const vec4 color = vec4(finalColor, 1.0f);
    outColor = color;
}
