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

#ifndef GS_FLOATER_V2_1_GPU_TYPES_H
#define GS_FLOATER_V2_1_GPU_TYPES_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace GS {

/*
 * Shader-visible v2.1 storage ABI. Every scalar is exactly 32 bits. The matching
 * GLSL declarations use std430 and scalar arrays, so the offsets below are part
 * of the cross-platform Windows/OH contract rather than implementation detail.
 */
inline constexpr uint32_t GPU_ABI_VERSION = 3u;
inline constexpr uint32_t GPU_SH_DIMENSIONS = 48u;

struct GpuProjectedGaussian {
    float centerDepth[4];
    float conicOpacity[4];
    float axes[4];
    float extentScale[4];
};

struct GpuEvidence {
    uint32_t ownerPixels;
    uint32_t anomalyPixels;
    uint32_t strongPixels;
    uint32_t relativeGapSumQ;
    uint32_t alphaTotalQ;
    uint32_t alphaAnomalyQ;
    uint32_t validFootprintSamples;
    uint32_t frontFootprintSamples;
};

struct GpuFeatures {
    float depthScore;
    float firstPurity;
    float alphaPurity;
    float meanGap;
    float frontSuspension;
    float detailProtection;
    float isolation;
    float shAffinity;
    float propagationConfidence;
    float preliminaryScore;
    float footprintFrontQ75;
    float weakEvidence;
    float edgeStrength;
    float thinness;
    float structuralSupport;
    float stableSupport;
};

struct GpuGaussianAnalysis {
    GpuEvidence evidence;
    GpuFeatures features;
};

struct GpuGaussianState {
    uint32_t cumulativeMask;
    uint32_t depthCandidateMask;
    uint32_t seedMask;
    uint32_t propagatedMask;
    uint32_t currentDepthMask;
    uint32_t currentSeedMask;
    uint32_t currentPropagatedMask;
    uint32_t candidateSource;
    uint32_t peelRound;
    uint32_t hashNext;
    float score;
    uint32_t protectionMask;
};

struct GpuSortEntry {
    uint32_t orderedDepthKey;
    uint32_t gaussianIndex;
};

/* The two fields are independent compact arrays sharing one allocation. */
struct GpuCompactEntry {
    uint32_t weakGaussianIndex;
    uint32_t candidateStableId;
};

struct GpuGlobalCounters {
    uint32_t compactCount;
    uint32_t proposedCount;
    uint32_t appliedThisPeel;
    uint32_t applyCurrentPeel;
    uint32_t currentStrongPixels;
    uint32_t initialStrongPixels;
    uint32_t peakProposedCount;
    uint32_t smallBatchStreak;
    uint32_t stopReason;
    uint32_t committedPeels;
    uint32_t roiCount;
    uint32_t cumulativeCount;
    uint32_t candidateListCount;
    uint32_t proposedOwnerPixels;
    uint32_t protectedCount;
    uint32_t proposedPixelCoverageQ;
    uint32_t stableProtectedCount;
    uint32_t thinProtectedCount;
    uint32_t edgeProtectedCount;
};

struct GpuHashBucket {
    uint32_t head;
};

struct GpuHistogramBin {
    uint32_t count;
};

struct GpuShRobustStats {
    uint32_t orderedMinimum;
    uint32_t orderedMaximum;
    float median;
    float mad;
};

struct GpuProgressRecord {
    uint32_t completedRound;
    uint32_t committedPeels;
    uint32_t newCandidateCount;
    uint32_t proposedCount;
    uint32_t cumulativeCount;
    uint32_t currentStrongPixels;
    uint32_t initialStrongPixels;
    uint32_t peakProposedCount;
    uint32_t proposedPixelCoverageQ;
    uint32_t protectedCount;
    uint32_t stopReason;
    uint32_t applyCurrentPeel;
    uint32_t stableProtectedCount;
    uint32_t thinProtectedCount;
    uint32_t edgeProtectedCount;
};

static_assert(std::is_standard_layout_v<GpuProjectedGaussian>);
static_assert(std::is_trivially_copyable_v<GpuProjectedGaussian>);
static_assert(sizeof(GpuProjectedGaussian) == 64u);
static_assert(sizeof(GpuEvidence) == 32u);
static_assert(sizeof(GpuFeatures) == 64u);
static_assert(sizeof(GpuGaussianAnalysis) == 96u);
static_assert(offsetof(GpuGaussianAnalysis, features) == 32u);
static_assert(sizeof(GpuGaussianState) == 48u);
static_assert(offsetof(GpuGaussianState, score) == 40u);
static_assert(sizeof(GpuSortEntry) == 8u);
static_assert(sizeof(GpuCompactEntry) == 8u);
static_assert(sizeof(GpuGlobalCounters) == 76u);
static_assert(sizeof(GpuHashBucket) == 4u);
static_assert(sizeof(GpuHistogramBin) == 4u);
static_assert(sizeof(GpuShRobustStats) == 16u);
static_assert(sizeof(GpuProgressRecord) == 60u);

} // namespace GS

#endif // GS_FLOATER_V2_1_GPU_TYPES_H
