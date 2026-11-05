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

#ifndef SHADERS__COMMON__GS_EDIT_COMMON_H
#define SHADERS__COMMON__GS_EDIT_COMMON_H

#include "render/shaders/common/render_compatibility_common.h"

#ifdef VULKAN

#define GS_EDIT_WORKGROUP_SIZE 256u

// max size guaranteed by spec
#define GS_EDIT_PUSH_CONSTANT_MAX_SIZE 128u

struct GsEditCommonData {
    uint bufferFlipIndex;
    
    // denotes the index of the current modifying (=exclude select) editing pass in the stack
    uint editPassIndex;

    // denotes the global index of this edit action pass, incremented on every pass
    uint actionIndex;

    uint padding;
};

struct GsEditPushConstantData {
    // this must be shared by all editing push constants
    GsEditCommonData editCommon;

    // can have any arbitrary data
    // (sizeBytes / sizeof(uint32_t)) - numElementsInGsEditCommonData
    uint data[(GS_EDIT_PUSH_CONSTANT_MAX_SIZE / 4u) - 4u];
};

// used for gs editing functionalities
struct IndirectEditDispatchCounterBuffer {
    uint x;
    uint y;
    uint z;

    uint counter; // count how many splats are active
    uint prevCounter;
};

struct GsEditIndirectPushConstantData {
    // number of splats in the immutable GS data buffer
    uint numInitialSplats;
    // reset the counter to this value
    uint resetCounterValue;
    // if not 0xFFFFffff, clear the index counter selectionMask[0] = indexClearValue
    uint indexClearValue;
};

// contains the temporally significant per-action camera data, e.g. the view matrix
struct ActionCameraDataStruct {
    mat4 viewMatrix;
    mat4 viewProjMatrix;
};

struct ClearWriteTargetPushConstantStruct {
    uint clearTargetIndex;
    uint maxNumSplats;
};

// editing common set0 pipeline layouts

#if (LUME_GS_EDIT_LAYOUT == 1)

layout(set = 0, binding = 0, std430) readonly restrict buffer inGSData
{
    // read-only input GS data
    GsSplatData gsData[];
};

layout(set = 0, binding = 1, std430) restrict buffer editGsData0Buffer
{
    // read-write GS data used during editin, ping-pong
    GsSplatData editGsData0[];
};

layout(set = 0, binding = 2, std430) restrict buffer editGsData1Buffer
{
    // read-write GS data used during editin, ping-pong
    GsSplatData editGsData1[];
};

layout(set = 0, binding = 3, std430) restrict buffer selectionIndexBuffer
{
    // mask for selecting splats, write in selection shaders, read everywhere else
    uint selectionMask[];
};

layout(set = 0, binding = 4, std430) restrict buffer indirectDispatchCounterBuffer
{
    // counter and indirect args to keep track of splat counts during editing
    IndirectEditDispatchCounterBuffer indirectCounter;
};

layout(set = 0, binding = 5, std430) readonly restrict buffer actionDataStructBuffer
{
    ActionCameraDataStruct actionDataStruct[];
};

layout(set = 0, binding = 6, std140) readonly uniform uCameraMatrices
{
    // camera data
    GsCameraUboStruct cameraUbo;
};

layout(set = 0, binding = 7, std140) readonly uniform uGSDynamicDataBuffer
{
    // per-GS model data, like model matrices
    GsDynamicDataStruct gsDynamicDatas[GS_MAX_NUM_GS_OBJECTS];
};

// This push constant has to be defined in the shader.
// No limits on the data of the push constant, but it must have
// the field bufferFlipIndex and have the size of 128 bytes.
// Just change GsEditPushConstantData to whatever is needed.
/*
layout(push_constant, std430) uniform uEditPushConstant {
    GsEditPushConstantData uPc;
};
*/

#endif

#else

constexpr uint32_t GS_EDIT_WORKGROUP_SIZE { 256u };

constexpr uint32_t GS_EDIT_PUSH_CONSTANT_MAX_SIZE { 128u };


// convert float to uint with 1M multiplier
constexpr float FIXED_FLOAT_MULTIPLIER { 1000000.0f };

// always mandatory push constant data
struct GsEditCommonData {
    uint32_t bufferFlipIndex { 0 };
    
    // denotes the index of the current modifying (=exclude select) editing pass in the stack
    uint32_t editPassIndex { 0 };

    // denotes the global index of this edit action pass, incremented on every pass
    uint32_t actionIndex { 0 };

    uint32_t padding { 0 };
};

struct GsEditPushConstantData {
    // this must be shared by all editing push constants
    GsEditCommonData editCommon;

    // can have any arbitrary data
    uint32_t data[
        ((GS_EDIT_PUSH_CONSTANT_MAX_SIZE / sizeof(uint32_t)) - (sizeof(GsEditCommonData) / sizeof(uint32_t)))];
};

struct GsEditIndirectPushConstantData {
    // number of splats in the immutable GS data buffer
    uint32_t numInitialSplats { 0 };
    // reset the counter to this value
    uint32_t resetCounterValue { 0 };
    // if not 0xFFFFffff, clear the index counter selectionMask[0] = indexClearValue
    uint32_t indexClearValue { 0xFFFFffff };
};

// used for gs editing functionalities
struct IndirectEditDispatchCounterBuffer {
    uint32_t x { 0 };
    uint32_t y { 0 };
    uint32_t z { 0 };

    uint32_t counter { 0 }; // count how many splats are active
};

// contains the temporally significant per-action camera data, e.g. the view matrix
struct ActionCameraDataStruct {
    mat4 viewMatrix;
    mat4 viewProjMatrix;
};

struct ClearWriteTargetPushConstantStruct {
    uint32_t clearTargetIndex { 0 };
    uint32_t maxNumSplats { 0 };
};

#endif

#endif // SHADERS__COMMON__GS_EDIT_COMMON_H
