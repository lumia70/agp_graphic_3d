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

#ifndef SHADERS__COMMON__GS_COMMON_H
#define SHADERS__COMMON__GS_COMMON_H

#include "render/shaders/common/render_compatibility_common.h"

#define GS_USE_CROPBOX

#ifdef VULKAN

/// GS3D COMMON VALUES

#define GS_MAX_NUM_GS_OBJECTS 4

/// GS3D SORT VALUES

#define GS_COMPUTE_TGS 128
#define GS_SORT_TGS 512
#define GS_PRE_SUM_TGS 128
#define GS_PROCESS_DATA_TGS 64
#define GS_DENSE_OBJECT_TGS 64
#define GS_DEPTH_TGS 64

#define GS_DEPTH_APPLY_XY_TGS 16

#define RADIX_SORT_BINS 256u
#define NUM_BLOCKS_PER_GROUP 1u
#define NUM_BLOCKS_PER_GROUP_RADIX 64u

#define UINT32_MAX 4294967295.0f

// 2.0f would be full res, 1.5f is typically "good enough"
#define GS_QUAD_SIZE_BASE 1.9f
#define GS_QUAD_SIZE_MIN 1.5f

#define GS_DEPTH_SCALE_FACTOR 8
// cull away smaller than this splats when computing the depth
#define GS_DEPTH_SIZE_THR 0.5f

#define GS_DEPTH_STAGE_CLEAR 0
#define GS_DEPTH_STAGE_COMPUTE 1

/// GS3D RENDERING VALUES

// e.g. 0.2 = 20% outside the frustum to cull away
#define GS_FRUSTUM_CULLING_THRESHOLD 0.2

#define GS_JITTER_AMOUNT 1.0f

#define GS_DEGENERACY_THR 1e-5f

// threshold for ignoring invisible gaussians
#define MAX_SCREEN_SPLAT_SIZE 1024.0f

// 2048x2048 is good until 4194304 gaussians in the scene
#define GS_DATA_STORE_TEX_SIZE 4096

#define GS_FRAME_BUFFER_SIZE_SCALE1 4096
#define GS_FRAME_BUFFER_SIZE_SCALE2 2048
#define GS_FRAME_BUFFER_SIZE_SCALE4 1024

/// GS3D FADE ANIMATION VALUES

#define GS_FADE_ANIMATION_POINT_SIZE 2.0

#define GS_FADE_RATE_EXPONENT 2.0f
#define GS_FADE_MAX_BOUNDARY 10000.0

// used to be 4.0f, but OHOS has a bug in un/packHalf2x16 so a higher value is needed.
#define GS_COV_PACK_MULTIPLIER 1024.0f

/* ============================== for frame generation start ============================== */
#define FLOATER_SPLAT_AREA                              0.03
#define FLOATER_SPLAT_MAX_SORTED_DEPTH                  0.05    // 0.0 - 1.0
#define FLOATER_SPLAT_MAX_OPACITY                       0.7     // 0.0 - 1.0
#define FLOATER_SPLAT_MAX_AXIS_LEN                      0.5
#define FLOATER_MAX_OPACITY                             0.25
/* =============================== for frame generation end =============================== */

#define MERGED_SLOT_ALIGNMENT 32u

// if set to 1, uses a Moire-pattern mitigated rendering by jittering the color+alpha.
// Incurs a -2% performance degradation.
#define MOIRE_MITIGATED_RENDERING 1
#define MOIRE_MITIGATED_RENDER_HASH_0 0x85EBCA6Bu
#define MOIRE_MITIGATED_RENDER_HASH_1 0xC2B2AE35u
#define GS_MAX_SPLAT_EXTENT 3.1622776f
#else

/// GS3D COMMON VALUES

constexpr uint32_t GS_MAX_NUM_GS_OBJECTS { 4u };
constexpr uint32_t GS_MAX_NUM_GS_TEXTURES { 60u };  // should be 15 * GS_MAX_NUM_GS_OBJECTS
constexpr uint32_t GS_FULL_HARMONICS_HEADER_SIZE { 62u };
constexpr uint32_t GS_RESTRICTED_HARMONICS_HEADER_SIZE { 17u };
constexpr uint32_t GS_RESTRICTED_HARMONICS_HEADER_SIZE_MINIMAL { 14u };
constexpr uint32_t GS_PACKED_HEADER_SIZE { 4u };

/// GS3D SORT VALUES

constexpr uint32_t GS_COMPUTE_TGS { 128u };
constexpr uint32_t GS_SORT_TGS { 512u };
constexpr uint32_t GS_PRE_SUM_TGS { 128u };
constexpr uint32_t GS_PROCESS_DATA_TGS { 64u };
constexpr uint32_t GS_DENSE_OBJECT_TGS { 64u };
constexpr uint32_t GS_DEPTH_TGS { 64u };

constexpr uint32_t GS_DEPTH_APPLY_XY_TGS { 16u };

constexpr uint32_t RADIX_SORT_BINS { 256u };
constexpr uint32_t NUM_RADIX_PASSES { 4u };

constexpr uint32_t NUM_BLOCKS_PER_GROUP { 1u };
constexpr uint32_t NUM_BLOCKS_PER_GROUP_RADIX { 64u };

constexpr uint32_t GS_DEPTH_SCALE_FACTOR { 8u };

constexpr size_t GS_NUM_HARMONICS { 16 };
constexpr size_t GS_HARMONICS_PACKED_SIZE { 24 };

constexpr float GS_16BIT_COUNT_SORT_COARSITY { 4096.0f };
constexpr uint32_t GS_COUNT_SORT_BITS { 18u };


constexpr uint32_t GS_DEPTH_STAGE_CLEAR { 0u };
constexpr uint32_t GS_DEPTH_STAGE_COMPUTE { 1u };

// don't cull at all if less than this many gaussians
constexpr uint32_t GS_CULLING_THRESHOLD { 250000u };

// do not create LODs for models with less than this many splats
constexpr uint32_t GS_LOD_MIN_NUM_GS { 200000u };

constexpr uint32_t GS_LOD_OCTREE_PAR_THR { 100000u };
constexpr uint32_t GS_LOD_LEAF_SIZE { 400u };
constexpr float GS_LOD_LAB_DISTANCE { 10.0f };
constexpr float GS_LOD_SPLAT_SIMILARITY_THR { 0.75f };
constexpr float GS_LOD_ORIENTATION_DEG { 60.0f };
constexpr float GS_LOD_ORIENTATION_SHAPE_THR { 0.75f };
constexpr float GS_LOD_GAMMA_CORRECTION { 1.075f };
constexpr float GS_LOD_LARGE_SPLAT_PERCENTILE { 0.95f };

constexpr uint32_t GS_LOD_NUM_PASSES { 5u };

constexpr uint32_t GS_LOD_PERCENTILE_INTERP_MIN { 300000 };
constexpr uint32_t GS_LOD_PERCENTILE_INTERP_MAX { 1000000 };
constexpr float GS_LOD_PERCENTILE_MIN { 0.5f };
constexpr float GS_LOD_PERCENTILE_MAX { 0.90f };

constexpr float GS_LOD_BASE_BIAS { 1.0f };

// 3.0 is 99.99% coverage, 2.0 is 95% coverage
constexpr float GS_COVARIANCE_STDDEV_DEFAULT { 2.0f };

// at least how many splats worth of spherical harmonics to compute per-frame.
constexpr uint32_t GS_SH_MIN_COMP_PER_FRAME { 50000u };

/// GS3D RENDERING VALUES

// 2048x2048 is good until 4194304 gaussians in the scene
// 4096x4096 is good until 16777216 gaussians in the scene
constexpr uint32_t GS_DATA_STORE_TEX_SIZE { 4096u };

constexpr float GS_FADE_MAX_WAIT_POINT_SECONDS { 4.0f };
constexpr float GS_FADE_RATE_EXPONENT { 2.0f };

constexpr float SH_C0 { 0.28209479177387814f };

constexpr uint32_t MERGED_SLOT_ALIGNMENT { 32u };

constexpr uint32_t GPU_SORT_KEY_BITS_24 { 24u };
constexpr uint32_t GPU_SORT_KEY_BITS_32 { 32u };
constexpr uint32_t GPU_SORT_BYTES_24 { 3u };
constexpr uint32_t GPU_SORT_BYTES_32 { 4u };

constexpr uint32_t MOIRE_MITIGATED_RENDERING { 1u };
constexpr uint32_t MOIRE_MITIGATED_RENDER_HASH_0 { 0x85EBCA6Bu };
constexpr uint32_t MOIRE_MITIGATED_RENDER_HASH_1 { 0xC2B2AE35u };
constexpr float GS_MAX_SPLAT_EXTENT { 3.1622776f };
#endif

#endif // SHADERS__COMMON__GS_COMMON_H
