/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef SHADERS__COMMON__GS_SPHERICAL_HARMONICS_COMMON_H
#define SHADERS__COMMON__GS_SPHERICAL_HARMONICS_COMMON_H

#include "common/gs_common.h"
#include "common/gs_struct_common.h"
#include <3d/shaders/common/3d_dm_structures_common.h>

#ifdef VULKAN
#define SH_C0 0.28209479177387814f
#define SH_C1 0.4886025119029199f

#define SH_C2_0 1.0925484305920792f
#define SH_C2_1 1.0925484305920792f
#define SH_C2_2 0.31539156525252005f
#define SH_C2_3 1.0925484305920792f
#define SH_C2_4 0.5462742152960396f

#define SH_C3_0 0.5900435899266435f
#define SH_C3_1 2.890611442640554f
#define SH_C3_2 0.4570457994644658f
#define SH_C3_3 0.3731763325901154f
#define SH_C3_4 0.4570457994644658f
#define SH_C3_5 1.445305721320277f
#define SH_C3_6 0.5900435899266435f

layout(set = 0, binding = 1, std140) readonly uniform uCameraMatrices
{
    GsCameraUboStruct cameraUbo;
};

layout(set = 0, binding = 2, std430) readonly restrict buffer inSHData {
    GsHarmonicsData harmonicsData[];
};

layout(set = 0, binding = 3, std140) readonly uniform uGSDynamicDataBuffer
{
    GsDynamicDataStruct gsDynamicDatas[GS_MAX_NUM_GS_OBJECTS];
};

layout(push_constant, std430) uniform uGsMetadata {
    GsShPushConstantStruct uPc;
};

layout(set = 0, binding = 4) uniform texture2D[] shImages;
layout(set = 0, binding = 5) uniform sampler gSampler;

vec3 GetSHCoef(const uint coefs[24], const int idx) {
    const uint readIdx = (uint(idx) * 3u) >> 1u;
    
    const vec2 coef0 = unpackHalf2x16(coefs[readIdx]);
    const vec2 coef1 = unpackHalf2x16(coefs[readIdx + 1]);
    
    return (uint(idx) & 1u) == 0u ? vec3(coef0.xy, coef1.x) : vec3(coef0.y, coef1.xy);
}

vec3 GetAstcSHCoef(const uvec2 shOffset, const uint shidx, const uint texOffset, const uint gsIndex) {
    const uint newx = (shOffset.x + shidx * gsDynamicDatas[gsIndex].attribute2DSingleWidth) %
        gsDynamicDatas[gsIndex].compressedShWidth;
    const uint texidx = texOffset + (shOffset.x + shidx * gsDynamicDatas[gsIndex].attribute2DSingleWidth) /
        gsDynamicDatas[gsIndex].compressedShWidth;
    
    const ivec2 newShOffset = ivec2(newx, shOffset.y);
    return texelFetch(sampler2D(shImages[texidx], gSampler), newShOffset, 0).xyz * 
        (gsDynamicDatas[gsIndex].shRestMaxs - gsDynamicDatas[gsIndex].shRestMins) + gsDynamicDatas[gsIndex].shRestMins;
}

vec3 GetAstcSHCoefV2(const uvec2 shOffset, const uint shidx, const uint texOffset, const uint gsIndex) {
    const uint newx = (shOffset.x +
        shidx % (gsDynamicDatas[gsIndex].compressedShWidth / gsDynamicDatas[gsIndex].attribute2DSingleWidth) *
        gsDynamicDatas[gsIndex].attribute2DSingleWidth);
    const uint newy = (shOffset.y +
        shidx / (gsDynamicDatas[gsIndex].compressedShWidth / gsDynamicDatas[gsIndex].attribute2DSingleWidth) *
        gsDynamicDatas[gsIndex].attribute2DSingleHeight);
    const uint texidx = texOffset;
 
    const ivec2 newShOffset = ivec2(newx, newy);
    const vec3 sh_rgb = texelFetch(sampler2D(shImages[texidx], gSampler), newShOffset, 0).xyz;
    
    const uint Idx = shidx/4;
    const uint idx = shidx%4;
    float coef_r = (sh_rgb.x) *
        (gsDynamicDatas[gsIndex].shRestMaxs_R[Idx][idx] -
            gsDynamicDatas[gsIndex].shRestMins_R[Idx][idx]) + gsDynamicDatas[gsIndex].shRestMins_R[Idx][idx];
    float coef_g = (sh_rgb.y) *
        (gsDynamicDatas[gsIndex].shRestMaxs_G[Idx][idx] -
            gsDynamicDatas[gsIndex].shRestMins_G[Idx][idx]) + gsDynamicDatas[gsIndex].shRestMins_G[Idx][idx];
    float coef_b = (sh_rgb.z) *
        (gsDynamicDatas[gsIndex].shRestMaxs_B[Idx][idx] -
            gsDynamicDatas[gsIndex].shRestMins_B[Idx][idx]) + gsDynamicDatas[gsIndex].shRestMins_B[Idx][idx];
 
    return vec3(coef_r, coef_g, coef_b);
}


int GetGsObjectIndex(const uint elementId) {
    for (int ii = 0; ii < int(uPc.numGsObjects); ii++) {
        if (elementId >= gsDynamicDatas[ii].startOffset && elementId < gsDynamicDatas[ii].endOffset) {
            return ii;
        }
    }

    return -1;
}

vec3 GetSHColor(const vec3 viewDir, const GsHarmonicsData shData, const uint shDegree) {
    const float x = viewDir.x;
    const float y = viewDir.y;
    const float z = viewDir.z;

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float yz = y * z;
    const float xz = x * z;
    vec3 colorSH = vec3(0.5f) + GetSHCoef(shData.shCoefficients, 0) * SH_C0;
    colorSH +=
        // 1st degree
        - GetSHCoef(shData.shCoefficients, 1) * SH_C1 * y
        + GetSHCoef(shData.shCoefficients, 2) * SH_C1 * z
        - GetSHCoef(shData.shCoefficients, 3) * SH_C1 * x;

    if (shDegree > 1) {
        colorSH +=
            // 2nd degree
            + GetSHCoef(shData.shCoefficients, 4) * SH_C2_0 * xy
            - GetSHCoef(shData.shCoefficients, 5) * SH_C2_1 * yz
            + GetSHCoef(shData.shCoefficients, 6) * SH_C2_2 * (2.0f * zz - xx - yy)
            - GetSHCoef(shData.shCoefficients, 7) * SH_C2_3 * xz
            + GetSHCoef(shData.shCoefficients, 8) * SH_C2_4 * (xx - yy);
    }

    if (shDegree > 2) {
        colorSH +=
            // 3rd degree
            - GetSHCoef(shData.shCoefficients, 9) * SH_C3_0 * y * (3.0f * xx - yy)
            + GetSHCoef(shData.shCoefficients, 10) * SH_C3_1 * xy * z
            - GetSHCoef(shData.shCoefficients, 11) * SH_C3_2 * y * (4.0f * zz - xx - yy)
            + GetSHCoef(shData.shCoefficients, 12) * SH_C3_3 * z * (2.0f * zz - 3.0f * xx - 3.0f * yy)
            - GetSHCoef(shData.shCoefficients, 13) * SH_C3_4 * x * (4.0f * zz - xx - yy)
            + GetSHCoef(shData.shCoefficients, 14) * SH_C3_5 * z * (xx - yy)
            - GetSHCoef(shData.shCoefficients, 15) * SH_C3_6 * x * (xx - 3.0f * yy);
    }
    return colorSH;
}

vec3 GetAstcSHColor(
    const vec3 viewDir, const ivec2 shOffset, const vec3 normalizedDc, const int gsIndex, const uint shDegree) {
    const float x = viewDir.x;
    const float y = viewDir.y;
    const float z = viewDir.z;

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float yz = y * z;
    const float xz = x * z;

    const uint textIdOffset = gsDynamicDatas[gsIndex].textIdOffset;
    vec3 colorSH = vec3(0.5f) +
        (normalizedDc * (gsDynamicDatas[gsIndex].shDcMaxs - gsDynamicDatas[gsIndex].shDcMins) +
        gsDynamicDatas[gsIndex].shDcMins) * SH_C0;
    colorSH += 
        // 1st degree
        - GetAstcSHCoef(shOffset, 0, textIdOffset, gsIndex) * SH_C1 * y
        + GetAstcSHCoef(shOffset, 1, textIdOffset, gsIndex) * SH_C1 * z
        - GetAstcSHCoef(shOffset, 2, textIdOffset, gsIndex) * SH_C1 * x;

    if (shDegree > 1) {
        colorSH +=
            // 2nd degree
            + GetAstcSHCoef(shOffset, 3, textIdOffset, gsIndex) * SH_C2_0 * xy
            - GetAstcSHCoef(shOffset, 4, textIdOffset, gsIndex) * SH_C2_1 * yz
            + GetAstcSHCoef(shOffset, 5, textIdOffset, gsIndex) * SH_C2_2 * (2.0f * zz - xx - yy)
            - GetAstcSHCoef(shOffset, 6, textIdOffset, gsIndex) * SH_C2_3 * xz
            + GetAstcSHCoef(shOffset, 7, textIdOffset, gsIndex) * SH_C2_4 * (xx - yy);
    }

    if (shDegree > 2) {
        colorSH +=
            // 3rd degree
            - GetAstcSHCoef(shOffset, 8, textIdOffset, gsIndex) * SH_C3_0 * y * (3.0f * xx - yy)
            + GetAstcSHCoef(shOffset, 9, textIdOffset, gsIndex) * SH_C3_1 * xy * z
            - GetAstcSHCoef(shOffset, 10, textIdOffset, gsIndex) * SH_C3_2 * y * (4.0f * zz - xx - yy)
            + GetAstcSHCoef(shOffset, 11, textIdOffset, gsIndex) * SH_C3_3 * z * (2.0f * zz - 3.0f * xx - 3.0f * yy)
            - GetAstcSHCoef(shOffset, 12, textIdOffset, gsIndex) * SH_C3_4 * x * (4.0f * zz - xx - yy)
            + GetAstcSHCoef(shOffset, 13, textIdOffset, gsIndex) * SH_C3_5 * z * (xx - yy)
            - GetAstcSHCoef(shOffset, 14, textIdOffset, gsIndex) * SH_C3_6 * x * (xx - 3.0f * yy);
    }
    return colorSH;
}
vec3 GetAstcSHColorV2(
    const vec3 viewDir, const ivec2 shOffset, const vec3 normalizedDc,
    const int gsIndex, const uint shDegree, const uint imp) {
    const float x = viewDir.x;
    const float y = viewDir.y;
    const float z = viewDir.z;

    const float xx = x * x;
    const float yy = y * y;
    const float zz = z * z;
    const float xy = x * y;
    const float yz = y * z;
    const float xz = x * z;

    const uint textIdOffset = gsDynamicDatas[gsIndex].textIdOffset;
    vec3 colorSH = vec3(0.0f);
    colorSH += 
        // 1st degree
        - GetAstcSHCoefV2(shOffset, 0, textIdOffset, gsIndex) * SH_C1 * y
        + GetAstcSHCoefV2(shOffset, 1, textIdOffset, gsIndex) * SH_C1 * z
        - GetAstcSHCoefV2(shOffset, 2, textIdOffset, gsIndex) * SH_C1 * x;

    if (shDegree > 1) {
        colorSH +=
            // 2nd degree
            + GetAstcSHCoefV2(shOffset, 3, textIdOffset, gsIndex) * SH_C2_0 * xy
            - GetAstcSHCoefV2(shOffset, 4, textIdOffset, gsIndex) * SH_C2_1 * yz
            + GetAstcSHCoefV2(shOffset, 5, textIdOffset, gsIndex) * SH_C2_2 * (2.0f * zz - xx - yy)
            - GetAstcSHCoefV2(shOffset, 6, textIdOffset, gsIndex) * SH_C2_3 * xz
            + GetAstcSHCoefV2(shOffset, 7, textIdOffset, gsIndex) * SH_C2_4 * (xx - yy);
    }

    if (shDegree > 2) {
        colorSH +=
            // 3rd degree
            - GetAstcSHCoefV2(shOffset, 8, textIdOffset, gsIndex) * SH_C3_0 * y * (3.0f * xx - yy)
            + GetAstcSHCoefV2(shOffset, 9, textIdOffset, gsIndex) * SH_C3_1 * xy * z
            - GetAstcSHCoefV2(shOffset, 10, textIdOffset, gsIndex) * SH_C3_2 * y * (4.0f * zz - xx - yy)
            + GetAstcSHCoefV2(shOffset, 11, textIdOffset, gsIndex) * SH_C3_3 * z * (2.0f * zz - 3.0f * xx - 3.0f * yy)
            - GetAstcSHCoefV2(shOffset, 12, textIdOffset, gsIndex) * SH_C3_4 * x * (4.0f * zz - xx - yy)
            + GetAstcSHCoefV2(shOffset, 13, textIdOffset, gsIndex) * SH_C3_5 * z * (xx - yy)
            - GetAstcSHCoefV2(shOffset, 14, textIdOffset, gsIndex) * SH_C3_6 * x * (xx - 3.0f * yy);
    }
    colorSH /= max(float(imp-1)/ 14.0f *
        (gsDynamicDatas[gsIndex].impMax - gsDynamicDatas[gsIndex].impMin) + gsDynamicDatas[gsIndex].impMin, 0.0001f);
    colorSH += vec3(0.5f) +
        (normalizedDc * (gsDynamicDatas[gsIndex].shDcMaxs - gsDynamicDatas[gsIndex].shDcMins) +
        gsDynamicDatas[gsIndex].shDcMins) * SH_C0;
    return colorSH;
}

#endif

#endif // SHADERS__COMMON__GS_SPHERICAL_HARMONICS_COMMON_H
