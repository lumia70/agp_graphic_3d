#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// includes

#include "common/gs_common.h"

#include "render/shaders/common/render_compatibility_common.h"
#include "3d/shaders/common/3d_dm_structures_common.h"

// sets and specializations

// in / out

layout(location = 0) in vec4 vColor;

layout(location = 0) out CORE_RELAXEDP vec4 outColor;

void main(void)
{
    
    outColor = vColor;
}
