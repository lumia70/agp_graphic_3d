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

#ifndef GS_FLOATER_V2_1_BINDINGS_GLSL
#define GS_FLOATER_V2_1_BINDINGS_GLSL

#include "common/gs_struct_common.h"

#define V21_LOCAL_SIZE 256u
#define V21_INVALID_ID 0xffffffffu
#define V21_SH_DIMENSIONS 48u
#define V21_HISTOGRAM_BINS 256u
#define V21_PI 3.14159265358979323846
#define V21_METRIC_SCALE 1000000u

#define V21_STOP_NONE 0u
#define V21_STOP_ZERO_PROPOSAL 1u
#define V21_STOP_RESIDUAL_CLEARED 2u
#define V21_STOP_SMALL_BATCH_CONVERGED 3u
#define V21_STOP_PROTECTION_LIMIT 4u
#define V21_STOP_MAX_PEELS 5u

#define V21_PROTECT_STABLE_SURFACE 1u
#define V21_PROTECT_THIN_STRUCTURE 2u
#define V21_PROTECT_DEPTH_EDGE 4u

layout(set = 0, binding = 0, std140) uniform V21FrameParams
{
    mat4 viewProjection;
    mat4 view;
    vec4 cameraPosition;
    vec4 intrinsicsViewport;
    vec4 principalNearFar;
    vec4 discAlphaGap;
    vec4 weights0;
    vec4 weights1;
    vec4 candidate0;
    vec4 candidate1;
    vec4 seed0;
    vec4 seed1;
    vec4 weak0;
    vec4 weak1;
    vec4 graph0;
    vec4 graph1;
    vec4 shWeights;
    vec4 shLimits;
    vec4 sampleGates;
    vec4 adaptive0;
    vec4 protection0;
    vec4 protection1;
    uvec4 counts;
    uvec4 peel;
    uvec4 revisions;
} v21;

// The Gaussian input is split across two buffers in LumeGS:
//   gsSplatData[]   - GsSplatData (64 bytes/splat): position, opacity, covariance, scale modifier
//   harmonicsData[] - GsHarmonicsData (96 bytes/splat): spherical harmonic coefficients
// The original V21GaussianInput struct (244 bytes) is replaced by these LumeGS native types.

struct V21ProjectedGaussian {
    vec4 centerDepth;
    vec4 conicOpacity;
    vec4 axes;
    vec4 extentScale;
};

struct V21Evidence {
    uint ownerPixels;
    uint anomalyPixels;
    uint strongPixels;
    uint relativeGapSumQ;
    uint alphaTotalQ;
    uint alphaAnomalyQ;
    uint validFootprintSamples;
    uint frontFootprintSamples;
};

struct V21Features {
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

struct V21GaussianAnalysis {
    V21Evidence evidence;
    V21Features features;
};

struct V21GaussianState {
    uint cumulativeMask;
    uint depthCandidateMask;
    uint seedMask;
    uint propagatedMask;
    uint currentDepthMask;
    uint currentSeedMask;
    uint currentPropagatedMask;
    uint candidateSource;
    uint peelRound;
    uint hashNext;
    float score;
    uint protectionMask;
};

struct V21SortEntry {
    uint orderedDepthKey;
    uint gaussianIndex;
};

struct V21CompactEntry {
    uint weakGaussianIndex;
    uint candidateStableId;
};

struct V21GlobalCounters {
    uint compactCount;
    uint proposedCount;
    uint appliedThisPeel;
    uint applyCurrentPeel;
    uint currentStrongPixels;
    uint initialStrongPixels;
    uint peakProposedCount;
    uint smallBatchStreak;
    uint stopReason;
    uint committedPeels;
    uint roiCount;
    uint cumulativeCount;
    uint candidateListCount;
    uint proposedOwnerPixels;
    uint protectedCount;
    uint proposedPixelCoverageQ;
    uint stableProtectedCount;
    uint thinProtectedCount;
    uint edgeProtectedCount;
};

struct V21HashBucket { uint head; };
struct V21HistogramBin { uint count; };
struct V21ShRobustStats {
    uint orderedMinimum;
    uint orderedMaximum;
    float median;
    float mad;
};
struct V21ProgressRecord {
    uint completedRound;
    uint committedPeels;
    uint newCandidateCount;
    uint proposedCount;
    uint cumulativeCount;
    uint currentStrongPixels;
    uint initialStrongPixels;
    uint peakProposedCount;
    uint proposedPixelCoverageQ;
    uint protectedCount;
    uint stopReason;
    uint applyCurrentPeel;
    uint stableProtectedCount;
    uint thinProtectedCount;
    uint edgeProtectedCount;
};

// HARMONICS binding is assigned per-profile as the next available slot after
// all other bindings. This keeps it within the shader compiler's MAX_BIND_IN_SET
// limit (16) and avoids conflicts. Pipeline layouts must include the matching
// binding entry.

#if !defined(V21_COMPUTE_SHADER)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_PROJECTED
#define V21_HAS_STATE
#define V21_HAS_SORT
#define V21_BIND_INPUT 1
#define V21_BIND_PROJECTED 2
#define V21_BIND_STATE 3
#define V21_BIND_SORT 4
#define V21_BIND_HARMONICS 5
#elif defined(V21_PROFILE_CLEAR_MAIN)
#define V21_HAS_ANALYSIS
#define V21_HAS_STATE
#define V21_BIND_ANALYSIS 1
#define V21_BIND_STATE 2
#elif defined(V21_PROFILE_CLEAR_AUX)
#define V21_HAS_HISTOGRAM
#define V21_HAS_GLOBALS
#define V21_HAS_HASH_BUCKETS
#define V21_HAS_PROGRESS
#define V21_HAS_SH_STATS
#define V21_BIND_HISTOGRAM 1
#define V21_BIND_GLOBALS 2
#define V21_BIND_HASH_BUCKETS 3
#define V21_BIND_PROGRESS 4
#define V21_BIND_SH_STATS 5
#elif defined(V21_PROFILE_PROJECT)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_PROJECTED
#define V21_HAS_SORT
#define V21_BIND_INPUT 1
#define V21_BIND_PROJECTED 2
#define V21_BIND_SORT 3
#define V21_BIND_HARMONICS 4
#elif defined(V21_PROFILE_SH_RANGE)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_GLOBALS
#define V21_HAS_SH_STATS
#define V21_BIND_INPUT 1
#define V21_BIND_GLOBALS 2
#define V21_BIND_SH_STATS 3
#define V21_BIND_HARMONICS 4
#elif defined(V21_PROFILE_SH_HISTOGRAM) || defined(V21_PROFILE_SH_MAD_HISTOGRAM)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_HISTOGRAM
#define V21_HAS_SH_STATS
#define V21_BIND_INPUT 1
#define V21_BIND_HISTOGRAM 2
#define V21_BIND_SH_STATS 3
#define V21_BIND_HARMONICS 4
#elif defined(V21_PROFILE_SH_RESOLVE)
#define V21_HAS_HISTOGRAM
#define V21_HAS_GLOBALS
#define V21_HAS_SH_STATS
#define V21_BIND_HISTOGRAM 1
#define V21_BIND_GLOBALS 2
#define V21_BIND_SH_STATS 3
#elif defined(V21_PROFILE_RADIX)
#define V21_HAS_SORT
#define V21_HAS_SORT_SCRATCH
#define V21_HAS_HISTOGRAM
#define V21_BIND_SORT 1
#define V21_BIND_SORT_SCRATCH 2
#define V21_BIND_HISTOGRAM 3
#elif defined(V21_PROFILE_DEPTH)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_STATE
#define V21_HAS_ANALYSIS
#define V21_HAS_DEPTH_TEXTURES
#define V21_HAS_GLOBALS
#define V21_BIND_INPUT 1
#define V21_BIND_STATE 2
#define V21_BIND_ANALYSIS 3
#define V21_BIND_WEIGHTED_TEXTURE 4
#define V21_BIND_OPAQUE_DEPTH_TEXTURE 5
#define V21_BIND_OPAQUE_OWNER_TEXTURE 6
#define V21_BIND_GLOBALS 7
#define V21_BIND_HARMONICS 8
#elif defined(V21_PROFILE_FOOTPRINT)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_PROJECTED
#define V21_HAS_STATE
#define V21_HAS_ANALYSIS
#define V21_HAS_WEIGHTED_TEXTURE
#define V21_BIND_INPUT 1
#define V21_BIND_PROJECTED 2
#define V21_BIND_STATE 3
#define V21_BIND_ANALYSIS 4
#define V21_BIND_WEIGHTED_TEXTURE 5
#define V21_BIND_HARMONICS 6
#elif defined(V21_PROFILE_PRE_SCORE)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_PROJECTED
#define V21_HAS_STATE
#define V21_HAS_ANALYSIS
#define V21_BIND_INPUT 1
#define V21_BIND_PROJECTED 2
#define V21_BIND_STATE 3
#define V21_BIND_ANALYSIS 4
#define V21_BIND_HARMONICS 5
#elif defined(V21_PROFILE_COMPACT) || defined(V21_PROFILE_KNN)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_PROJECTED
#define V21_HAS_STATE
#define V21_HAS_ANALYSIS
#define V21_HAS_COMPACT
#define V21_HAS_GLOBALS
#define V21_HAS_HASH_BUCKETS
#define V21_BIND_INPUT 1
#define V21_BIND_PROJECTED 2
#define V21_BIND_STATE 3
#define V21_BIND_ANALYSIS 4
#define V21_BIND_COMPACT 5
#define V21_BIND_GLOBALS 6
#define V21_BIND_HASH_BUCKETS 7
#define V21_BIND_HARMONICS 8
#elif defined(V21_PROFILE_SH_PROPAGATE)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_PROJECTED
#define V21_HAS_STATE
#define V21_HAS_ANALYSIS
#define V21_HAS_COMPACT
#define V21_HAS_GLOBALS
#define V21_HAS_HASH_BUCKETS
#define V21_HAS_SH_STATS
#define V21_BIND_INPUT 1
#define V21_BIND_PROJECTED 2
#define V21_BIND_STATE 3
#define V21_BIND_ANALYSIS 4
#define V21_BIND_COMPACT 5
#define V21_BIND_GLOBALS 6
#define V21_BIND_HASH_BUCKETS 7
#define V21_BIND_SH_STATS 8
#define V21_BIND_HARMONICS 9
#elif defined(V21_PROFILE_MASK)
#define V21_HAS_INPUT
#define V21_HAS_HARMONICS
#define V21_HAS_STATE
#define V21_HAS_ANALYSIS
#define V21_HAS_COMPACT
#define V21_HAS_GLOBALS
#define V21_HAS_PROGRESS
#define V21_BIND_INPUT 1
#define V21_BIND_STATE 2
#define V21_BIND_ANALYSIS 3
#define V21_BIND_COMPACT 4
#define V21_BIND_GLOBALS 5
#define V21_BIND_PROGRESS 6
#define V21_BIND_HARMONICS 7
#endif

#ifdef V21_HAS_INPUT
layout(set = 0, binding = V21_BIND_INPUT, std430) readonly buffer V21SplatBuffer
{
    GsSplatData gsSplatData[];
};
#endif
#ifdef V21_HAS_HARMONICS
layout(set = 0, binding = V21_BIND_HARMONICS, std430) readonly buffer V21HarmonicsBuffer
{
    GsHarmonicsData harmonicsData[];
};
#endif
#ifdef V21_HAS_PROJECTED
layout(set = 0, binding = V21_BIND_PROJECTED, std430) buffer V21ProjectedBuffer { V21ProjectedGaussian projected[]; };
#endif
#ifdef V21_HAS_ANALYSIS
layout(set = 0, binding = V21_BIND_ANALYSIS, std430) buffer V21AnalysisBuffer { V21GaussianAnalysis analysis[]; };
#endif
#ifdef V21_HAS_STATE
layout(set = 0, binding = V21_BIND_STATE, std430) buffer V21StateBuffer { V21GaussianState states[]; };
#endif
#ifdef V21_HAS_SORT
layout(set = 0, binding = V21_BIND_SORT, std430) buffer V21SortBuffer { V21SortEntry sortEntries[]; };
#endif
#ifdef V21_HAS_SORT_SCRATCH
layout(set = 0, binding = V21_BIND_SORT_SCRATCH, std430) buffer V21SortScratchBuffer { V21SortEntry sortScratch[]; };
#endif
#ifdef V21_HAS_COMPACT
layout(set = 0, binding = V21_BIND_COMPACT, std430) buffer V21CompactBuffer { V21CompactEntry compactEntries[]; };
#endif
#ifdef V21_HAS_GLOBALS
layout(set = 0, binding = V21_BIND_GLOBALS, std430) buffer V21GlobalBuffer { V21GlobalCounters globals; };
#endif
#ifdef V21_HAS_HASH_BUCKETS
layout(set = 0, binding = V21_BIND_HASH_BUCKETS, std430) buffer V21HashBuffer { V21HashBucket hashBuckets[]; };
#endif
#ifdef V21_HAS_HISTOGRAM
layout(set = 0, binding = V21_BIND_HISTOGRAM, std430) buffer V21HistogramBuffer { V21HistogramBin histogramBins[]; };
#endif
#ifdef V21_HAS_SH_STATS
layout(set = 0, binding = V21_BIND_SH_STATS, std430) buffer V21ShStatsBuffer { V21ShRobustStats shRobustStats[]; };
#endif
#ifdef V21_HAS_PROGRESS
layout(set = 0, binding = V21_BIND_PROGRESS, std430) buffer V21ProgressBuffer { V21ProgressRecord progressRecords[]; };
#endif

#ifndef V21_COMPUTE_SHADER
layout(push_constant, std430) uniform V21GraphicsPushConstant
{
    uint applyCumulativeMask;
    uint presentationReserved0;
    uint presentationReserved1;
    uint presentationReserved2;
} graphicsPush;
#else
#ifdef V21_HAS_DEPTH_TEXTURES
layout(set = 0, binding = V21_BIND_WEIGHTED_TEXTURE) uniform sampler2D weightedAccumulatorTexture;
layout(set = 0, binding = V21_BIND_OPAQUE_DEPTH_TEXTURE) uniform sampler2D opaqueDepthTexture;
layout(set = 0, binding = V21_BIND_OPAQUE_OWNER_TEXTURE) uniform usampler2D opaqueOwnerTexture;
#elif defined(V21_HAS_WEIGHTED_TEXTURE)
layout(set = 0, binding = V21_BIND_WEIGHTED_TEXTURE) uniform sampler2D weightedAccumulatorTexture;
#endif

layout(push_constant, std430) uniform V21PushConstant
{
    uint mode;
    uint count;
    uint round;
    uint reserved;
} pushData;
#endif

#ifdef V21_HAS_INPUT
// Position from GsSplatData.positionOpacity.xyz
vec3 V21Position(uint id) { return gsSplatData[id].positionOpacity.xyz; }

// Opacity from GsSplatData.positionOpacity.w
float V21Opacity(uint id) { return gsSplatData[id].positionOpacity.w; }

// Scale derived from covariance diagonal (sqrt of absolute diagonal elements).
// LumeGS stores covariance directly (covA.xyz, covB.xyz) rather than scale+rotation.
vec3 V21Scale(uint id)
{
    vec3 covA = gsSplatData[id].covAScale.xyz;
    vec3 covB = gsSplatData[id].covBColorPacked.xyz;
    vec3 diagonal = vec3(covA.x, covB.x, covB.y);
    return sqrt(max(abs(diagonal), vec3(0.0)));
}

// 3x3 covariance matrix from covA.xyz and covBColorPacked.xyz.
// The symmetric 3x3 covariance is stored as 6 floats: (covA.x, covA.y, covA.z,
// covB.x, covB.y, covB.z) mapping to (Sxx, Sxy, Sxz, Syy, Syz, Szz).
mat3 V21Covariance(uint id)
{
    vec3 covA = gsSplatData[id].covAScale.xyz;
    vec3 covB = gsSplatData[id].covBColorPacked.xyz;
    return mat3(
        covA.x, covA.y, covA.z,
        covA.y, covB.x, covB.y,
        covA.z, covB.y, covB.z);
}

// Scale modifier from GsSplatData.covAScale.w (D14)
float V21ScaleModifier(uint id) { return gsSplatData[id].covAScale.w; }
#endif

#ifdef V21_COMPUTE_SHADER
uint V21FloatToOrdered(float value)
{
    uint bits = floatBitsToUint(value);
    return ((bits & 0x80000000u) != 0u) ? ~bits : (bits ^ 0x80000000u);
}

float V21OrderedToFloat(uint ordered)
{
    uint bits = ((ordered & 0x80000000u) != 0u) ? (ordered ^ 0x80000000u) : ~ordered;
    return uintBitsToFloat(bits);
}

uint V21HashCell(ivec3 cell)
{
    uint x = uint(cell.x) * 73856093u;
    uint y = uint(cell.y) * 19349663u;
    uint z = uint(cell.z) * 83492791u;
    return (x ^ y ^ z) & (v21.peel.w - 1u);
}

ivec3 V21Cell(vec3 position)
{
    return ivec3(floor(position / max(v21.graph0.x, 1.0e-4)));
}

float V21OrderWeight(uint coefficient)
{
    if (coefficient == 0u) { return v21.shWeights.x; }
    if (coefficient <= 3u) { return v21.shWeights.y / 3.0; }
    if (coefficient <= 8u) { return v21.shWeights.z / 5.0; }
    return v21.shWeights.w / 7.0;
}

#if defined(V21_PROFILE_SH_PROPAGATE)
// V21ShCoefficient is defined in v2_1_gaussian.glsl (task T6) and reads from
// harmonicsData[].shCoefficients. All inputs[id].sh[dim] accesses are replaced
// with V21ShCoefficient(id, dim) to read from the separate harmonics buffer.
// Forward declaration required because v2_1_gaussian.glsl (which provides the
// definition) is included after this file in the translation unit.
float V21ShCoefficient(uint id, uint dimension);
float V21NormalizedShDistance(uint a, uint b)
{
    uint coefficientCount = min(16u, (v21.counts.w + 1u) * (v21.counts.w + 1u));
    float sum = 0.0;
    float normalizer = 0.0;
    for (uint coefficient = 0u; coefficient < coefficientCount; ++coefficient) {
        float orderWeight = V21OrderWeight(coefficient);
        for (uint channel = 0u; channel < 3u; ++channel) {
            uint dimension = coefficient * 3u + channel;
            float median = shRobustStats[dimension].median;
            float mad = max(shRobustStats[dimension].mad, 1.0e-5);
            float va = clamp((V21ShCoefficient(a, dimension) - median) / mad,
                -v21.shLimits.x, v21.shLimits.x);
            float vb = clamp((V21ShCoefficient(b, dimension) - median) / mad,
                -v21.shLimits.x, v21.shLimits.x);
            float difference = va - vb;
            sum += orderWeight * difference * difference;
            normalizer += orderWeight;
        }
    }
    return sqrt(sum / max(normalizer, 1.0e-6));
}
#endif
#endif

#endif
