#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// includes

#include "3d/shaders/common/3d_dm_structures_common.h"

// in / out

// NOTE: cannot be CORE_RELAXEDP. Quality issues.
layout(location = 0) out vec2 outCoordXY;

void main(void)
{
    const vec2 vertexPositions[4] = vec2[](
        vec2(-1.0f, 1.0f),
        vec2(1.0f, 1.0f),
        vec2(-1.0f, -1.0f),
        vec2(1.0f, -1.0f)
    );

    const uint vertexIndex = gl_VertexIndex;
    const vec2 corner = vertexPositions[vertexIndex];

    CORE_VERTEX_OUT(vec4(corner, 0, 1));

    outCoordXY = corner;
}
