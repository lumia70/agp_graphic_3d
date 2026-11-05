#version 460 core
#extension GL_ARB_separate_shader_objects : enable

layout(location = 0) in vec2 inDeltaPixels;
layout(location = 1) in vec4 inConicOpacity;
layout(location = 2) in vec4 inColorDepth;
layout(location = 3) flat in uint inGaussianId;
layout(location = 4) flat in vec2 inDiscAlpha;

layout(location = 0) out vec4 outPreview;
layout(location = 1) out vec4 outDepthAccumulator;

void main()
{
    float q = inConicOpacity.x * inDeltaPixels.x * inDeltaPixels.x +
        2.0 * inConicOpacity.y * inDeltaPixels.x * inDeltaPixels.y +
        inConicOpacity.z * inDeltaPixels.y * inDeltaPixels.y;
    if (q > 9.0 || inGaussianId == 0xffffffffu) {
        discard;
    }
    float alpha = min(0.99, inConicOpacity.w * exp(-0.5 * q));
    if (alpha < inDiscAlpha.y) {
        discard;
    }
    outPreview = vec4(inColorDepth.rgb * alpha, alpha);
    outDepthAccumulator = vec4(alpha * inColorDepth.a, alpha, 0.0, alpha);
}
