#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// includes

#include "common/gs_common.h"
#include "common/gs_struct_common.h"
#include "3d/shaders/common/3d_dm_structures_common.h"

// sets and specializations

#include "3d/shaders/common/3d_dm_frag_layout_common.h"

// in / out

// NOTE: cannot be CORE_RELAXEDP. Quality issues.
layout(location = 0) in vec2 inCoordXY;

layout(location = 0) out vec4 outColor;

layout(set = 3, binding = 0) uniform texture2D uGSTex0; // (0, 0)
layout(set = 3, binding = 1) uniform texture2D uGSTex1; // (1, 0)
layout(set = 3, binding = 2) uniform texture2D uGSTex2; // (0, 1)
layout(set = 3, binding = 3) uniform texture2D uGSTex3; // (1, 1)
layout(set = 3, binding = 4) uniform sampler gSamplerNearest;
layout(set = 3, binding = 5) uniform sampler gSamplerLinear;

layout(set = 3, binding = 6, std140) readonly uniform uGSRenderStatusUbo
{
    GsRenderStatusUbo renderStatus;
};

layout(set = 3, binding = 7) uniform texture2D gsDepthTex;

bool GetCompositeDepth(out float compositeDepth)
{
    const vec2 viewportSize = uGeneralData.viewportSizeInvViewportSize.xy;
    const vec2 uv = gl_FragCoord.xy / viewportSize;
    const vec2 depthData = texture(sampler2D(gsDepthTex, gSamplerLinear), uv).rg;

    const float weightedDepthSum = depthData.r;
    const float compositeOpacity = depthData.g;

    if (compositeOpacity <= (1.0f / 255.0f) || isnan(weightedDepthSum) || isnan(compositeOpacity) || isinf(weightedDepthSum) || isinf(compositeOpacity)) {
        return false;
    }

    compositeDepth = clamp(weightedDepthSum / compositeOpacity, 0.0f, 1.0f);
    return true;
}

vec4 GetJitterCombinedSample(const vec2 texCoords, const vec2 fullResPixelCoord)
{
    // no jitter renderable, return 0
    if (renderStatus.jitterFrameRendered != 4) {
        const vec2 texelSize = 1.0f / vec2(textureSize(sampler2D(uGSTex0, gSamplerNearest), 0));
        const float offset = 0.3f;

        const vec4 center = texture(sampler2D(uGSTex0, gSamplerNearest), texCoords);
        const vec4 left = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2(-offset * texelSize.x, 0.0f));
        const vec4 right = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2( offset * texelSize.x, 0.0f));
        const vec4 up = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2(0.0f, -offset * texelSize.y));
        const vec4 down = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2(0.0f,  offset * texelSize.y));

        return (center * 2.0f + left + right + up + down) / 6.0f;
    }

    // combine samples from all 4 frames

    const ivec2 ipxCoord = ivec2(floor(fullResPixelCoord));
    const int scaleFactor = int(renderStatus.renderScaleFactor);
    const int subPxX = ipxCoord.x % scaleFactor;
    const int subPxY = ipxCoord.y % scaleFactor;

    const ivec2 texSize = textureSize(sampler2D(uGSTex0, gSamplerNearest), 0);
    const ivec2 texelCoord = clamp(ipxCoord / scaleFactor, ivec2(0), texSize - 1);
    
    const vec4 frameColor0 = texelFetch(sampler2D(uGSTex0, gSamplerNearest), texelCoord, 0);
    const vec4 frameColor1 = texelFetch(sampler2D(uGSTex1, gSamplerNearest), texelCoord, 0);
    const vec4 frameColor2 = texelFetch(sampler2D(uGSTex2, gSamplerNearest), texelCoord, 0);
    const vec4 frameColor3 = texelFetch(sampler2D(uGSTex3, gSamplerNearest), texelCoord, 0);
 
    const float factorX = float(subPxX) / float(max(scaleFactor - 1, 1));
    const float factorY = float(subPxY) / float(max(scaleFactor - 1, 1));

    const vec4 xBlend1 = mix(frameColor0, frameColor1, factorX);
    const vec4 xBlend2 = mix(frameColor2, frameColor3, factorX);

    return mix(xBlend1, xBlend2, factorY);
}

vec4 ApplySharpening(const vec4 centerColor, const vec2 texCoords, const vec2 pixelOffset, const float strength)
{
    const vec4 top = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2(0.0f, pixelOffset.y));
    const vec4 bottom = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2(0.0f, -pixelOffset.y));
    const vec4 left = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2(-pixelOffset.x, 0.0f));
    const vec4 right = texture(sampler2D(uGSTex0, gSamplerLinear), texCoords + vec2(pixelOffset.x, 0.0f));

    const vec3 lumCoeff = vec3(0.2126f, 0.7152f, 0.0722f);
    const float centerLuma = dot(centerColor.rgb, lumCoeff);

    const vec4 avgNeighbor = (top + bottom + left + right) * 0.25f;
    const float avgLuma = dot(avgNeighbor.rgb, lumCoeff);

    const float lumaDelta = centerLuma - avgLuma;

    const float edgeStrength = abs(lumaDelta);
    const float adaptiveStrength = strength * smoothstep(0.0f, 0.1f, edgeStrength);

    const float sharpenAmount = lumaDelta * adaptiveStrength;
    const vec3 sharpened = centerColor.rgb + sharpenAmount;

    return vec4(clamp(sharpened, 0.0f, 1.0f), centerColor.a);
}

vec4 ConvertColorSrgbToLinear(const vec4 color)
{
    const vec3 cutoff = vec3(0.04045f);
    const vec3 a = vec3(0.055f);
    const vec3 gamma = vec3(2.4f);
    const vec3 linear = color.rgb / 12.92f;
    const vec3 exponent = pow((color.rgb + a) / (1.0f + a), gamma);

    return vec4(mix(exponent, linear, lessThanEqual(color.rgb, cutoff)), color.a);
}

void main(void)
{
    const bool depthPassEnabled = (renderStatus.depthPassEnabled != 0u);
    float compositeDepth = 0.0f;
    const bool hasCompositeDepth = depthPassEnabled && GetCompositeDepth(compositeDepth);

    if (depthPassEnabled && !hasCompositeDepth) {
        discard;
    }

    const vec2 viewportSize = uGeneralData.viewportSizeInvViewportSize.xy;
    const float viewportWidth = viewportSize.x;
    const float viewportHeight = viewportSize.y;

    const vec2 texCoords = (inCoordXY * 0.5f + 0.5f) / renderStatus.DRSScale;
    const vec2 fullResPixelCoord = texCoords * viewportSize;

    const float scaleFactor = float(renderStatus.renderScaleFactor);
    const vec2 gsRenderSize = viewportSize / scaleFactor;

    // XEG upscale enabled with actual upscaling (renderScaleFactor > 1.0): sample from upscale output (uGSTex1)
    // When enableXegUpscale but renderScaleFactor <= 1.0, no upscaling needed, fall through to RT0 path
    if (renderStatus.enableXegUpscale == 1u && renderStatus.renderScaleFactor > 1.001f) {
        vec4 outWriteColor = texture(sampler2D(uGSTex1, gSamplerNearest), inCoordXY * 0.5f + 0.5f, 0);
        if (renderStatus.convertColorSrgb == 1) {
            outWriteColor = ConvertColorSrgbToLinear(outWriteColor);
        }
        outColor = outWriteColor;
        gl_FragDepth = hasCompositeDepth ? compositeDepth : 0.0f;
        return;
    }

    // No upscaling needed (renderScaleFactor <= 1.0), sample directly from RT0
    if (renderStatus.renderScaleFactor <= 1.001f) {
        vec4 outWriteColor = texture(sampler2D(uGSTex0, gSamplerNearest), texCoords, 0);
        if (renderStatus.convertColorSrgb == 1) {
            outWriteColor = ConvertColorSrgbToLinear(outWriteColor);
        }
        outColor = outWriteColor;
        gl_FragDepth = hasCompositeDepth ? compositeDepth : 0.0f;
        return;
    }

    const vec4 sampleColor = GetJitterCombinedSample(texCoords, fullResPixelCoord);

    const vec2 pixelOffset = vec2(1.0f) / gsRenderSize;
    const float sharpCoef = renderStatus.sharpeningCoeff * (renderStatus.jitterFrameRendered != 4 ? 1.0f : 0.5f);
    const vec4 sharpened = ApplySharpening(sampleColor, texCoords, pixelOffset, sharpCoef);

    vec4 writeColor = sharpened;
    if (renderStatus.convertColorSrgb == 1) {
        writeColor = ConvertColorSrgbToLinear(writeColor);
    }

    outColor = writeColor;
    gl_FragDepth = hasCompositeDepth ? compositeDepth : 0.0f;
}
