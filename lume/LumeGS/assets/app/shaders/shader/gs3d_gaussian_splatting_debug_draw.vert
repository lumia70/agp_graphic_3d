#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// includes

#include "3d/shaders/common/3d_dm_structures_common.h"

#include "common/gs_struct_common.h"
#include "common/gs_common.h"

// in / out

layout(location = 0) in vec4 position;

layout(location = 0) out vec4 vColor;

layout(set = 0, binding = 0, std140) readonly uniform uCameraMatrices
{
    GsCameraUboStruct cameraUbo;
};

layout(set = 0, binding = 1, std430) readonly buffer uGSDebugDrawLinesBuffer
{
    GsDebugLinesVertex gsDebugDrawLines[];
};

layout(set = 0, binding = 2, std140) readonly uniform uGSDynamicDataBuffer
{
    GsDynamicDataStruct gsDynamicDatas[GS_MAX_NUM_GS_OBJECTS];
};

layout(push_constant, std430) uniform uPushConstant
{
    GsRenderPushConstantStruct uPc;
};

void main(void)
{
    GsDebugLinesVertex vertex = gsDebugDrawLines[gl_VertexIndex];
    gl_Position  = cameraUbo.proj * cameraUbo.view * vec4(vertex.position.xyz, 1.0);

    vColor = vec4(0.0, 1.0, 0.0, 1.0);
}
