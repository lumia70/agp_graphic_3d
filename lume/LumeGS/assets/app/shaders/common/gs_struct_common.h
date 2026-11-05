/*
 * Copyright (c) 2025 Huawei Device Co., Ltd.
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

#ifndef SHADERS__COMMON__GS_SIMULATION_COMMON_H
#define SHADERS__COMMON__GS_SIMULATION_COMMON_H

#include "render/shaders/common/render_compatibility_common.h"

#ifdef VULKAN
#include "common/gs_common.h"
#else
#include "gs_common.h"
#endif

#ifndef VULKAN
using mat3 = BASE_NS::Math::Mat3X3;
#endif

struct GsSplatData {
    vec4 positionOpacity;   // .xyz position, .w opacity
    vec4 covAScale;         // .xyz covA, .w scale modifier
    vec4 covBColorPacked;   // .xyz covB, .w 8bit abgr packed as uintBitsToFloat
    uvec4 astcOffsetColor;  // .xy ASTC-packed spherical harmonics offset, .z 8bit abgr packed (dc), .w is empty
};

struct GsSplatDataPacked {
    uvec4 covColorPacked;          // .xyz cov packed, .w 8bit abgr packed
    uvec2 positionOpacityPacked;   // .x position.xy packed, .y position.z + opacity packed
    uvec2 astcOffsetColor;         // .x ASTC-packed spherical harmonics offset, .y 8bit abgr packed (dc)
};

struct GsSplatDataTilePacked {
    uvec4 covColorPacked;
    vec4 positionOpacityPacked;
};

struct GsHarmonicsData {
    // Ascending order spherical harmonic coefficients.
    // Packed as 16bit floats (16*3 / 2 = 24)
    uint shCoefficients[24];
};

struct GsSortPushConstantStruct {
    vec2 depthRange;
    uint numGs;
    float renderScaleFactor;

    uint numGsObjects;

    // flag to use indirect dispatch counter from editor
    uint useIndirectDispatch;
    uint gsObjectIndex;     // gsDynamicDatas index (only used when != 0xFFFFffff)
    uint enableGsOpt;
};

struct GsRenderPushConstantStruct {
    vec2 viewportSize;
    // index of the jittered frame
    uint jitterIndex;
    // index of the gaussians, used for animation fade data access
    uint numGsObjects;
    float renderScaleFactor;

    // degree of spherical harmonics
    uint shDegree;

    // flag to use indirect dispatch counter from editor
    uint useIndirectDispatch;

    // render covariance multiplier, 3.0f = 99.999% coverage
    float covarianceStddev;

    // adhoc culling scale, negative = no culling
    float adhocCullingScale;

    uint enableGsOpt;
};

struct GsDepthPushConstantStruct {
    vec2 viewportSize;
    uint numGs;
    uint stageIndex;
    float renderScaleFactor;
};


struct GsShPushConstantStruct {
    vec2 viewportSize;
    uint baseOffset;
    uint numGsTotal;
    uint numGsObjects;

    // degree of spherical harmonics
    uint shDegree;
    uint convertGsColorSrgb;

    uint numLod0;
};

struct GsPrefixSumPushConstantStruct {
    uint splitGroupCount;
    uint stage;
};

struct GsRadixSortPushConstantStruct {
    uint bytePosition;
    uint numGs;
    uint bufferSlotOffset;  // byte offset into pool buffer for this tile's splats (0 for non-tiled)
    uint useIndirectDispatch;
};

struct GsEdgePassPushConstantStruct {
    vec4 cameraParams; // x: near, y: far, zw: mousepos
    vec4 rayHitPos;    // .xyz = world hit position, .w unused
    mat4 invViewProj;
};

struct GsEdgeMinMaxPushConstantStruct {
    mat4 invViewProj;
    vec3 rayHitPos;
    float isFirstPass;
};

// <depth, index> pairs for depth sorting
struct SortKeyIndex {
    // depth-as-uint
    uint key;

    // index of the splat in the original buffer
    uint gsIndex;
};

// equivalent to VkDrawIndirectCommand
struct GsDrawIndirectCommand {
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

// equivalent to VkDispatchIndirectCommand
struct GsDispatchIndirectCommand {
    uint x;
    uint y;
    uint z;
};

struct GsRenderStatusUbo {
    uint jitterFrameRendered;
    uint frameIdx;
    float renderScaleFactor;
    uint convertColorSrgb;

    float DRSScale;
    uint depthPassEnabled;
    float sharpeningCoeff;
    uint enableXegUpscale;  // XEG upscale enabled flag from configuration

};

struct GsCameraUboStruct {
    mat4 view;
    mat4 proj;
    mat4 viewProj;

    mat4 viewInv;

    uint frameIdx;
    float deltaTime;
    float totalTickTimeSeconds;
    float animationRunningTimeSeconds;

    vec2 viewportSize;

    float pad1;
    float pad2;
};

struct GsDepthCulledStruct {
    vec3 cov;
    uint depth;
    int screenPxX;
    int screenPxY;
};

struct GsEdgesBufferLayout {
    int minX;
    int minY;
    int maxX;
    int maxY;
    vec4 worldMinX;
    vec4 worldMinY;
    vec4 worldMaxX;
    vec4 worldMaxY;
};

// used to store per-gs data that can be used to modify it on the fly
struct GsDynamicDataStruct {
    vec3 fadeStartPosition;
    float fadeSpeed;

    // dynamic crop-box AABB min
    vec3 aabbMin;
    // time in seconds how long it takes for all points to become visible.
    float timeToFadeFullPoints;

    // dynamic crop-box AABB max
    vec3 aabbMax;
    // wait for this many seconds before starting the fade at all
    float fadeWaitOffset;

    // distance in meters to subtract from the fade point distance
    float fadeDistanceOffset;
    uint useFadeAnimation;
    uint useCropBox;
    uint computeDepthPass;

    mat4 worldMat;

    uint startOffset;
    uint endOffset;

    uint enabled;

    uint shDegree;

    // codec related data
    uint compressedShWidth;
    uint compressedShAstcBlock;
    uint attribute2DSingleWidth;
    uint codecVersion;
    float shRestMins;
    float shRestMaxs;
    float shDcMins;
    float shDcMaxs;
    uint textIdOffset;
    uint attribute2DTexNum;
    uint startShOffset;

    // for 16-byte alignment
    uint placeholder1;
    uint placeholder2;
    uint placeholder3;

    // for frame generation
    uint frameGenerationEnabled;

    uint placeholder4;
    vec4 shRestMins_R[4];
    vec4 shRestMaxs_R[4];
    vec4 shRestMins_G[4];
    vec4 shRestMaxs_G[4];
    vec4 shRestMins_B[4];
    vec4 shRestMaxs_B[4];
    uint compressedShHeight;
    uint attribute2DSingleHeight;
    float impMax;
    float impMin;
};

struct GsDenseObjectPushConstantStruct {
    vec3 rayOrigin;
    uint numSplats;
    vec3 rayDirection;
    uint stage;
    uint numGsObjects;
};

struct GsObjectHitResult {
    uint minDistanceUint;
    vec3 worldPos;
};

struct GsIndirectDrawDispatch {
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;

    uint x;
    uint y;
    uint z;
    uint padding;
};

struct GsDebugLinesVertex {
    vec4 position;
};

struct GsTilePrecomputePushConstantStruct {
    vec2 viewportSize;
    uint jitterIndex;
    uint gsObjectIndex;
    float renderScaleFactor;
    uint elementOffset;

    // render covariance multiplier, 3.0f = 99.999% coverage
    float covarianceStddev;
    uint enableGsOpt;
};

struct GsTileSortedDataCopyPushConstantStruct {
    uint numGs;
    uint commandIndex;
    uint firstInstance;
};

struct GsTiledPreSortPushConstantStruct {
    vec2 depthRange;
    uint numGs;
    uint gsObjectIndex;
    uint gsIndexBase;
    uint sortKeyBits;
    uint enableGsOpt;
};

struct GsTileWorkgroupMetadataStruct {
    uint slotIdx;
    uint mergedOffset;
    uint splatCount;
    uint wgIndexInTile;
};

#endif // SHADERS__COMMON__GS_SIMULATION_COMMON_H
