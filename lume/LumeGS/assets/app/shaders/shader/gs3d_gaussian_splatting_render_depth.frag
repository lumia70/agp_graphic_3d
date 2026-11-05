#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// includes

#include "common/gs_common.h"

#include "render/shaders/common/render_compatibility_common.h"
#include "3d/shaders/common/3d_dm_structures_common.h"

// in / out

layout(location = 0) in flat uint vColor;
layout(location = 1) in CORE_RELAXEDP vec2 vPosition;
layout(location = 2) in flat float vDepth;

layout(location = 0) out CORE_RELAXEDP vec4 outColor;
layout(location = 1) out vec4 outDepth;

void main(void)
{
    const CORE_RELAXEDP float r2 = dot(vPosition, vPosition);
    if (r2 > 10.0f) {
        discard;
    }
 
    const CORE_RELAXEDP vec4 color = unpackUnorm4x8(vColor).abgr;
    const CORE_RELAXEDP float weight = exp2((-0.5f * r2) * 1.44269504f) * color.a;
 
    if (weight < (1.0f / 255.0f)) {
        discard;
    }
    
    outColor = vec4(weight * color.rgb, weight);
    outDepth = vec4(weight * vDepth, weight, 0.0f, weight);
}
