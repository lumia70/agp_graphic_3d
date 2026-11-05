#version 460 core
#extension GL_ARB_separate_shader_objects : enable

#include "common/v2_1_gaussian.glsl"

layout(location = 0) out vec2 outDeltaPixels;
layout(location = 1) out vec4 outConicOpacity;
layout(location = 2) out vec4 outColorDepth;
layout(location = 3) flat out uint outGaussianId;
layout(location = 4) flat out vec2 outDiscAlpha;

void main()
{
    uint sortedIndex = uint(gl_InstanceIndex);
    uint id = sortEntries[sortedIndex].gaussianIndex;
    vec2 corner = V21QuadCorner(uint(gl_VertexIndex));
    if (id >= v21.counts.x ||
        (graphicsPush.applyCumulativeMask != 0u && states[id].cumulativeMask != 0u) ||
        projected[id].centerDepth.w < 0.5) {
        gl_Position = vec4(2.0, 2.0, 1.0, 1.0);
        outDeltaPixels = vec2(0.0);
        outConicOpacity = vec4(0.0);
        outColorDepth = vec4(0.0);
        outGaussianId = V21_INVALID_ID;
        outDiscAlpha = v21.discAlphaGap.xy;
        return;
    }
    V21ProjectedGaussian gaussian = projected[id];
    vec2 delta = corner * gaussian.extentScale.xy;
    vec4 centerClip = v21.viewProjection * vec4(V21Position(id), 1.0);
    vec2 ndcOffset = delta * (2.0 / v21.intrinsicsViewport.zw);
    gl_Position = centerClip;
    gl_Position.xy += ndcOffset * centerClip.w;
    outDeltaPixels = delta;
    outConicOpacity = gaussian.conicOpacity;
    float normalizedDepth = clamp((gaussian.centerDepth.z - v21.principalNearFar.z) /
        (v21.principalNearFar.w - v21.principalNearFar.z), 0.0, 1.0);
    outColorDepth = vec4(V21ShColor(id), normalizedDepth);
    outGaussianId = id;
    outDiscAlpha = v21.discAlphaGap.xy;
}
