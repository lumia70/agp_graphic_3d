#version 460 core
#extension GL_ARB_separate_shader_objects : enable
#extension GL_ARB_shading_language_420pack : enable

// includes

#include "3d/shaders/common/3d_dm_structures_common.h"

#include "common/gs_struct_common.h"
#include "common/gs_common.h"

// in / out

layout(location = 0) in uvec4 gsInputData;

layout(location = 0) out flat uint vColor;
layout(location = 1) out CORE_RELAXEDP vec2 vPosition;
layout(location = 2) out flat float vDepth;

layout(set = 0, binding = 0, std140) readonly uniform uCameraMatrices
{
    GsCameraUboStruct cameraUbo;
};

layout(set = 0, binding = 1, std140) readonly uniform uGSDynamicDataBuffer
{
    GsDynamicDataStruct gsDynamicDatas[GS_MAX_NUM_GS_OBJECTS];
};

layout(set = 0, binding = 2) uniform texture2D uDepthStoreTex;
layout(set = 0, binding = 3) uniform sampler uDepthStoreSampler;

layout(push_constant, std430) uniform uPushConstant
{
    GsRenderPushConstantStruct uPc;
};

const CORE_RELAXEDP vec2 VERTEX_POSITION[4] = vec2[](
    vec2(-1.0f, -1.0f),
    vec2(1.0f, -1.0f),
    vec2(1.0f, 1.0f),
    vec2(-1.0f, 1.0f)
);

#define GS_SCALER(x) (x <= 0.5f ? (x * 0.5f + 0.75f) : (x <= 0.7f ? (x + 0.5f) : (x * 0.16666667f + 1.0833333f)))
#define GS_SPLAT_EXTENT(opacityQ, sizeBias) min(sqrt(2.0f * log(max(opacityQ, 1.0f))) * (sizeBias), GS_MAX_SPLAT_EXTENT)
void main(void)
{
    if (gsInputData.x == 0u) {
        CORE_VERTEX_OUT(vec4(0, 0, 2, 1));
        vDepth = 0.0f;
        return;
    }

    const vec2 screenPosition = unpackHalf2x16(gsInputData.x);
    uint colorPacked = gsInputData.y;
    const vec2 majorAxis = unpackHalf2x16(gsInputData.z);
    const vec2 minorAxis = unpackHalf2x16(gsInputData.w);    

    // const vec2 position = GET_VERTEX_POSITION();
    const vec2 position = VERTEX_POSITION[gl_VertexIndex];
    const vec4 posOut = vec4(screenPosition + position.x * majorAxis + position.y * minorAxis, 0.0f, 1.0f);
    vColor = colorPacked;

    float gaussianOpacity;
    float opacityScaler;
    float opacityQ;
    if(uPc.enableGsOpt == 0) {
        gaussianOpacity = float(colorPacked & 0xFF) * (1.0f / 255.0f);
        opacityScaler = GS_SCALER(gaussianOpacity);
        vPosition = position * uPc.covarianceStddev * opacityScaler;
    } else {
        opacityQ = float(colorPacked & 0xFF);
        vPosition = position * GS_SPLAT_EXTENT(opacityQ, uPc.covarianceStddev * 0.5f);
    }

    const ivec2 depthUv = ivec2(int(gl_InstanceIndex % GS_DATA_STORE_TEX_SIZE), int(gl_InstanceIndex / GS_DATA_STORE_TEX_SIZE));
    vDepth = texelFetch(sampler2D(uDepthStoreTex, uDepthStoreSampler), depthUv, 0).r;

    CORE_VERTEX_OUT(posOut);
}
