#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// includes

#include "common/gs_common.h"

#include "render/shaders/common/render_compatibility_common.h"
#include "3d/shaders/common/3d_dm_structures_common.h"

// sets and specializations

// in / out

layout(location = 0) in flat uint vColor;
layout(location = 1) in CORE_RELAXEDP vec2 vPosition;

layout(location = 0) out CORE_RELAXEDP vec4 outColor;

void main(void)
{
    const CORE_RELAXEDP float r2 = dot(vPosition, vPosition);
    if (r2 > 10.0f) {
        discard;
    }

#if (MOIRE_MITIGATED_RENDERING)
    const uvec2 posBits = floatBitsToUint(vPosition);
    uint h = (vColor ^ posBits.x) * MOIRE_MITIGATED_RENDER_HASH_0;
    h = (h ^ posBits.y) * MOIRE_MITIGATED_RENDER_HASH_1;
    h ^= h >> 16u;
#endif

    const CORE_RELAXEDP vec4 color = unpackUnorm4x8(vColor).abgr;
    const CORE_RELAXEDP float B = exp2((-0.5f * r2) * 1.44269504f) * color.a;

    if (B < (1.0f / 255.0f)) {
        discard;
    }
#if (MOIRE_MITIGATED_RENDERING)
    // TPDF dither
    const CORE_RELAXEDP vec2 u = unpackUnorm2x16(h);
    const CORE_RELAXEDP float d = fma(u.x + u.y, 1.0f / 255.0f, -1.0f / 255.0f);
    outColor = vec4(B * color.rgb, B) + d;
#else
    outColor = vec4(B * color.rgb, B);
#endif
}