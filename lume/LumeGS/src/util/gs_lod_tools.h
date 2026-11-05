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

#ifndef PLUGIN_GS_LOD_TOOLS_H
#define PLUGIN_GS_LOD_TOOLS_H

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <numeric>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <thread>

#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/environment_component.h>
#include <3d/ecs/components/light_component.h>
#include <3d/ecs/components/local_matrix_component.h>
#include <3d/ecs/components/material_component.h>
#include <3d/ecs/components/mesh_component.h>
#include <3d/ecs/components/name_component.h>
#include <3d/ecs/components/node_component.h>
#include <3d/ecs/components/post_process_configuration_component.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/render_handle_component.h>
#include <3d/ecs/components/render_mesh_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/components/uri_component.h>
#include <3d/ecs/components/world_matrix_component.h>
#include <3d/ecs/systems/intf_animation_system.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <base/containers/array_view.h>
#include <base/containers/byte_array.h>
#include <base/containers/fixed_string.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/float_packer.h>
#include <base/math/matrix_util.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/json/json.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/plugin/intf_class_factory.h>
#include <core/property/intf_property_api.h>
#include <core/property/intf_property_handle.h>
#include <core/property/property_handle_util.h>
#include <core/util/parallel_sort.h>
#include <render/device/intf_gpu_resource_manager.h>

#include "gs_utils.h"
#include "gs_loading.h"

namespace GS {

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;

constexpr int OCTREE_NODE_COUNT = 8;

Math::Vec3 EigenvalueDecompositionValues(const Math::Mat3X3& S) noexcept
{
    constexpr float convergenceTolerance = 1e-8f;
    Math::Mat3X3 Sprime = S;

    bool changed = true;
    constexpr size_t maxIterations = 100;
    size_t iters = 0;

    while (changed && iters < maxIterations) {
        iters++;

        changed = false;
        float pivot = 0.0f;
        size_t pivot_i = 0;
        size_t pivot_j = 0;

        // find the largest off-diagonal element
        GetLargestOffDiagonalElement(Sprime, pivot, pivot_i, pivot_j);

        if (pivot < convergenceTolerance) {
            break;
        }

        // Jacobi rotation angle
        const float theta =
            0.5f * std::atan2(2.0f * Sprime[pivot_i][pivot_j], Sprime[pivot_j][pivot_j] - Sprime[pivot_i][pivot_i]);

        const float cc = std::cos(theta);
        const float ss = std::sin(theta);

        // givens rotation matrix
        Math::Mat3X3 G = Math::Mat3X3(1.0f);
        G[pivot_i][pivot_i] = cc;
        G[pivot_j][pivot_j] = cc;
        G[pivot_i][pivot_j] = ss;
        G[pivot_j][pivot_i] = -ss;

        Sprime = Math::Transpose(G) * Sprime * G;
        changed = true;
    }

    return Math::Vec3(Sprime[0][0], Sprime[1][1], Sprime[2][2]);
}

float GsRadOptim(const Math::Vec4& covA, const Math::Vec4& covB) noexcept
{
    // An optimized approximation. Significantly faster, but doesn't produce perfect results.

    const float a00 = covA.x;
    const float a11 = covB.x;
    const float a22 = covB.z;
    const float a01 = covA.y;
    const float a02 = covA.z;
    const float a12 = covB.y;

    // characteristic polynomial coefficients
    const float c2 = -(a00 + a11 + a22);
    const float c1 = a00 * a11 + a00 * a22 + a11 * a22 - a01 * a01 - a02 * a02 - a12 * a12;
    const float c0 = -(a00 * a11 * a22 + 2 * a01 * a02 * a12 - a00 * a12 * a12 - a11 * a02 * a02 - a22 * a01 * a01);

    const float p = c1 - c2 * c2 / 3.0f;
    const float q = 2.0f * c2 * c2 * c2 / 27.0f - c1 * c2 / 3.0f + c0;

    const float discriminant = 4.0f * p * p * p + 27.0f * q * q;

    if (discriminant <= 0.0f) {
        const float phi = Math::acos(-0.5f * q * Math::sqrt(-27.0f / (p * p * p)));
        const float r = 2.0f * Math::sqrt(-p / 3.0f);

        const float lambda1 = r * Math::cos(phi / 3.0f) - c2 / 3.0f;
        const float lambda2 = r * Math::cos((phi + 2.0f * Math::PI) / 3.0f) - c2 / 3.0f;
        if (lambda1 < 0.0f || lambda2 < 0.0f) {
            return 0.0f;
        }

        const float retVal = Math::sqrt(Math::max(lambda1, lambda2));
        return std::isnan(retVal) ? 0.0f : retVal;
    } else {
        const float u = std::cbrt(-q / 2.0f + Math::sqrt(discriminant / 27.0f));
        const float v = std::cbrt(-q / 2.0f - Math::sqrt(discriminant / 27.0f));

        const float lambda = u + v - c2 / 3.0f;

        const float retVal = lambda < 0.0f ? 0.0f : Math::sqrt(lambda);
        return std::isnan(retVal) ? 0.0f : retVal;
    }
}

void CullTransparentGaussians(GS::GsPrimitive& primitive, GS::GsObjectLoadDataExt* loadData,
    IThreadPool::Ptr threadPool, const bool useAutomaticCulling,
    const float transparentCullingOpacityAbsolute, const GS::GsLoadContext& loadCtx)
{
    // this function must be called before LODs have been created
    if (primitive.numGs1 != 0u) {
        CORE_LOG_E("GsSystem::CullTransparentGaussians cannot be called after LOD creation!");
        return;
    }

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsSystem::CullTransparentGaussians", "", 0xFF00FF);

    if (primitive.numGs <= 0u) {
        return;
    }

    BASE_NS::vector<float> opacities(primitive.numGs, 0.0f);

    struct ThreadStats {
        float sum { 0.0f };
        float sum2 { 0.0f };
        float sum3 { 0.0f };
        float sum4 { 0.0f };
        uint32_t count { 0 };
    };

    const auto numThreads = threadPool->GetNumberOfThreads();
    BASE_NS::vector<IThreadPool::IResult::Ptr> results;
    const uint32_t gsPerThread = (primitive.numGs + numThreads - 1) / numThreads;

    BASE_NS::vector<ThreadStats> sizeStats(numThreads);
    BASE_NS::vector<ThreadStats> opacityStats(numThreads);

    // compute statistics
    for (uint32_t tt = 0; tt < numThreads; tt++) {
        const uint32_t startIdx = tt * gsPerThread;
        const size_t endIdx = std::min(startIdx + gsPerThread, primitive.numGs);

        if (startIdx >= primitive.numGs) {
            break;
        }

        auto task = CreateFunctionTask([&, tt, startIdx, endIdx]() {
            ThreadStats& threadSizeStats = sizeStats[tt];
            ThreadStats& threadOpacityStats = opacityStats[tt];

            for (size_t ii = startIdx; ii < endIdx; ii++) {
                const GsObjectLoadData::GsSplatData& splatData = loadData->gsVerticesData.Get(ii);
                const float opacity = splatData.positionOpacity.w;
                opacities[ii] = opacity;

                const float splatSize =
                    GsRadOptim(splatData.covAScale, splatData.covBColorPacked);

                // accumulate statistics for sizes
                threadSizeStats.count++;
                threadSizeStats.sum += splatSize;
                const float sd = splatSize;
                const float sd2 = sd * sd;
                threadSizeStats.sum2 += sd2;
                threadSizeStats.sum3 += sd2 * sd;
                threadSizeStats.sum4 += sd2 * sd2;

                // accumulate statistics for opacities
                threadOpacityStats.count++;
                threadOpacityStats.sum += opacity;
                const float od = opacity;
                const float od2 = od * od;
                threadOpacityStats.sum2 += od2;
                threadOpacityStats.sum3 += od2 * od;
                threadOpacityStats.sum4 += od2 * od2;
            }
        });

        auto result = threadPool->Push(BASE_NS::move(task));
        results.push_back(BASE_NS::move(result));
    }

    for (auto& result : results) {
        result->Wait();
    }

    const auto combineStats = [](const BASE_NS::vector<ThreadStats>& threadStats) {
        ThreadStats combined;

        for (const auto& stats : threadStats) {
            combined.count += stats.count;
            combined.sum += stats.sum;
            combined.sum2 += stats.sum2;
            combined.sum3 += stats.sum3;
            combined.sum4 += stats.sum4;
        }
        return combined;
    };

    auto calculateMoments = [](const ThreadStats& stats) {
        const float n = static_cast<float>(stats.count);

        if (n <= 1.0f) {
            return std::make_tuple(0.0f, 0.0f, 0.0f);
        }

        const float mean = stats.sum / n;
        const float variance = (stats.sum2 / n) - (mean * mean);

        if (variance <= 0.0f) {
            return std::make_tuple(mean, 0.0f, 0.0f);
        }

        const float stddev = Math::sqrt(variance);
        const float stddev3 = stddev * stddev * stddev;
        const float stddev4 = stddev3 * stddev;

        const float m2 = variance;
        const float m3 = (stats.sum3 / n) - 3.0f * mean * m2 - mean * mean * mean;
        const float m4 = (stats.sum4 / n) - 4.0f * mean * m3 - 6.0f * mean * mean * m2 - mean * mean * mean * mean;

        const float skew = m3 / stddev3;
        const float kurtosis = m4 / stddev4 - 3.0f;

        return std::make_tuple(mean, skew, kurtosis);
    };

    const ThreadStats combinedSizeStats = combineStats(sizeStats);
    const ThreadStats combinedOpacityStats = combineStats(opacityStats);

    const auto [sizeMean, sizeSkew, sizeKurtosis] = calculateMoments(combinedSizeStats);
    const auto [opacityMean, opacitySkew, opacityKurtosis] = calculateMoments(combinedOpacityStats);

    CORE_LOG_I("Size skew: %f", sizeSkew);
    CORE_LOG_I("Opacity skew: %f", opacitySkew);
    CORE_LOG_I("Size kurtosis: %f", sizeKurtosis);
    CORE_LOG_I("Opacity kurtosis: %f", opacityKurtosis);
    CORE_LOG_I("Average size: %f", sizeMean);

    const float automaticCullingPercentile = Math::max(
        0.0f, Math::min(1.0f, (0.153f * std::logf(std::max(0.1f, sizeSkew)) - 0.195f * opacityKurtosis - 0.452f) /
                                  (1.0f + sizeKurtosis / 12000.0f)));
    const size_t percentilePos =
            static_cast<size_t>(static_cast<float>(primitive.numGs) * automaticCullingPercentile);

    float automaticCullingThreshold = 0.0f;
    if (useAutomaticCulling && percentilePos < primitive.numGs) {
        std::nth_element(
            opacities.begin(), opacities.begin() + static_cast<int64_t>(percentilePos), opacities.end());
        automaticCullingThreshold = opacities[percentilePos];
    }

    const float absoluteThreshold =
        transparentCullingOpacityAbsolute <= 1.0f
            ? (transparentCullingOpacityAbsolute > 0.0f ? transparentCullingOpacityAbsolute : 1.0f)
            : 0.1f;

    const float opacityThreshold = (automaticCullingThreshold + transparentCullingOpacityAbsolute > 1e-6f)
                                       ? Math::min(automaticCullingThreshold, absoluteThreshold)
                                       : 0.0f;

    BASE_NS::vector<Math::Vec4> positions;
    BASE_NS::vector<GS::GsHarmonicsData> sphericalHarmonics;
    GsVertexBlob verticesData;
    if (loadCtx.tempRenderData && loadCtx.tempRenderData->CanUsePackedData()) {
        verticesData.SetPackingEnabled(true);
    }

    if (!loadCtx.renderContext_) {
        return;
    }
    RENDER_NS::IGpuResourceManager& gpuResourceMgr = loadCtx.renderContext_->GetDevice().GetGpuResourceManager();
    verticesData.SetGPUAllocator(gpuResourceMgr);

    positions.reserve(primitive.numGs);
    verticesData.Reserve(primitive.numGs);

    if (primitive.RequireSH()) {
        sphericalHarmonics.reserve(primitive.numGs);
    }

    for (size_t ii = 0; ii < primitive.numGs; ii++) {
        const GsObjectLoadData::GsSplatData& vd = loadData->gsVerticesData.Get(ii);

        if (vd.positionOpacity.w < opacityThreshold) {
            continue;
        }
        positions.emplace_back(loadData->gaussianPositions[ii]);
        verticesData.PushBack(vd);
        if (primitive.RequireSH()) {
            sphericalHarmonics.emplace_back(loadData->sphericalHarmonics[ii]);
        }
    }

    loadData->gaussianPositions = positions;
    loadData->gsVerticesData = BASE_NS::move(verticesData);
    loadData->gsVerticesData.ShrinkToFit();
    if (primitive.RequireSH()) {
        loadData->sphericalHarmonics = sphericalHarmonics;
    }

    const uint32_t newNumGs = static_cast<uint32_t>(loadData->gsVerticesData.Size());

    const float percentCulled = static_cast<float>(primitive.numGs - newNumGs) / static_cast<float>(primitive.numGs);
    CORE_LOG_I("Bottom %f%% gaussians culled, with the opacity of %f", percentCulled * 100.0f, opacityThreshold);
    CORE_LOG_I(
        "Number of gaussians before culling: %d, after culling: %d", primitive.numGs, static_cast<uint32_t>(newNumGs));

    primitive.numGs = static_cast<uint32_t>(newNumGs);
}

class OctreeNode {
public:
    struct AABB {
        BASE_NS::Math::Vec3 min { 0.0f, 0.0f, 0.0f };
        BASE_NS::Math::Vec3 max { 0.0f, 0.0f, 0.0f };
    };

    std::array<std::unique_ptr<OctreeNode>, OCTREE_NODE_COUNT> children;

    AABB bounds;

    BASE_NS::vector<GS::GsObjectLoadData::GsSplatData> points;
    BASE_NS::vector<uint32_t> pointIndices;
    int depth { 0 };

    OctreeNode(const AABB& nodeBounds, const int depth) : bounds(nodeBounds), depth(depth) {}

    bool IsLeaf() const
    {
        return !children[0u] && !children[1u] && !children[2u] && !children[3u] && !children[4u] && !children[5u] &&
               !children[6u] && !children[7u];
    }

    float LocalDensity() const
    {
        if (points.empty()) {
            return 0.0f;
        }

        Math::Vec3 min(points.front().positionOpacity);
        Math::Vec3 max = min;

        for (const auto& pt : points) {
            min.x = Math::min(min.x, pt.positionOpacity.x);
            min.y = Math::min(min.y, pt.positionOpacity.y);
            min.z = Math::min(min.z, pt.positionOpacity.z);

            max.x = Math::max(max.x, pt.positionOpacity.x);
            max.y = Math::max(max.y, pt.positionOpacity.y);
            max.z = Math::max(max.z, pt.positionOpacity.z);
        }

        const Math::Vec3 size = max - min;
        const float volume = Math::max(size.x * size.y * size.z, 1e-6f);

        return static_cast<float>(points.size()) / volume;
    }
};

OctreeNode::AABB ComputeBoundingBox(const BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>& vertices)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "ComputeBoundingBox", "", 0xFF00FF);

    if (vertices.empty()) {
        return { { 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f } };
    }

    OctreeNode::AABB bbox;
    bbox.min = { vertices[0].positionOpacity.x, vertices[0].positionOpacity.y, vertices[0].positionOpacity.z };
    bbox.max = bbox.min;

    for (size_t ii = 1; ii < vertices.size(); ii++) {
        const auto& v = vertices[ii];

        bbox.min.x = Math::min(bbox.min.x, v.positionOpacity.x);
        bbox.min.y = Math::min(bbox.min.y, v.positionOpacity.y);
        bbox.min.z = Math::min(bbox.min.z, v.positionOpacity.z);

        bbox.max.x = Math::max(bbox.max.x, v.positionOpacity.x);
        bbox.max.y = Math::max(bbox.max.y, v.positionOpacity.y);
        bbox.max.z = Math::max(bbox.max.z, v.positionOpacity.z);
    }

    return bbox;
}

float ComputeSurfaceArea(const OctreeNode::AABB& box)
{
    const float width = box.max.x - box.min.x;
    const float height = box.max.y - box.min.y;
    const float depth = box.max.z - box.min.z;

    return 2.0f * (width * height + width * depth + height * depth);
}

float ComputeSplitPosition(const BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>& points, const size_t axis,
    const OctreeNode::AABB& bounds)
{
    constexpr int numBuckets = 32;

    std::array<int, numBuckets> countRight = { 0 };

    for (const auto& point : points) {
        const float value =
            axis == 0U ? point.positionOpacity.x : (axis == 1U ? point.positionOpacity.y : point.positionOpacity.z);
        const size_t bucketIdx = static_cast<size_t>(Math::min(numBuckets - 1,
            static_cast<int>((value - bounds.min[axis]) / (bounds.max[axis] - bounds.min[axis]) * numBuckets)));

        countRight[bucketIdx]++;
    }

    float bestCost = std::numeric_limits<float>::max();
    int bestBucket = -1;

    int totalRight = static_cast<int>(points.size());
    int totalLeft = 0;

    for (int ii = 0; ii < numBuckets - 1; ii++) {
        totalLeft += countRight[static_cast<size_t>(ii)];
        totalRight -= countRight[static_cast<size_t>(ii)];

        if (totalLeft == 0 || totalRight == 0)
            continue;

        const float splitPos = bounds.min[axis] + (bounds.max[axis] - bounds.min[axis]) *
                                                      (static_cast<float>(ii) + 1.0f) / static_cast<float>(numBuckets);

        OctreeNode::AABB leftBox = bounds;
        leftBox.max[axis] = splitPos;

        OctreeNode::AABB rightBox = bounds;
        rightBox.min[axis] = splitPos;

        const float leftSA = ComputeSurfaceArea(leftBox);
        const float rightSA = ComputeSurfaceArea(rightBox);
        const float totalSA = ComputeSurfaceArea(bounds);

        // surface area heuristic cost
        const float cost = 1.0f + (leftSA / totalSA * static_cast<float>(totalLeft) +
                                      rightSA / totalSA * static_cast<float>(totalRight));

        if (cost < bestCost) {
            bestCost = cost;
            bestBucket = ii;
        }
    }

    if (bestBucket == -1) {
        return (bounds.min[axis] + bounds.max[axis]) * 0.5f;
    }

    return bounds.min[axis] + (bounds.max[axis] - bounds.min[axis]) * (static_cast<float>(bestBucket) + 1.0f) /
                                  static_cast<float>(numBuckets);
}

std::unique_ptr<OctreeNode> BuildOctreeNode(const BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>& points,
    BASE_NS::vector<uint32_t>& indices, const OctreeNode::AABB& nodeBounds, const size_t maxPointsPerNode,
    IThreadPool* threadPool, std::shared_ptr<std::atomic<int>> activeThreads, const uint32_t threadingPointThreshold,
    const int depth)
{
    // The parallelization is better than nothing but fundamentally flawed,
    // since threads on lower depths cannot exit before all children have
    // been computed. Some kind of work-stealing queue could work better.

    std::unique_ptr<OctreeNode> node = std::make_unique<OctreeNode>(nodeBounds, depth);

    if (points.empty()) {
        return node;
    }

    if (points.size() <= maxPointsPerNode) {
        node->points = points;
        node->pointIndices = indices;
        return node;
    }

    const Math::Vec3 midpoint { ComputeSplitPosition(points, 0, nodeBounds),
        ComputeSplitPosition(points, 1, nodeBounds), ComputeSplitPosition(points, 2, nodeBounds) };

    std::array<BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>, OCTREE_NODE_COUNT> childPoints;
    std::array<BASE_NS::vector<uint32_t>, OCTREE_NODE_COUNT> childIndices;

    const size_t estimatedPointsPerChild = points.size() / OCTREE_NODE_COUNT + 1;
    for (size_t ii = 0; ii < OCTREE_NODE_COUNT; ii++) {
        childPoints[ii].reserve(estimatedPointsPerChild);
        childIndices[ii].reserve(estimatedPointsPerChild);
    }

    std::array<OctreeNode::AABB, OCTREE_NODE_COUNT> childBoxes;
    for (uint32_t ii = 0u; ii < OCTREE_NODE_COUNT; ii++) {
        OctreeNode::AABB& childBox = childBoxes[static_cast<size_t>(ii)];

        childBox.min.x = (ii & 1u) ? midpoint.x : nodeBounds.min.x;
        childBox.max.x = (ii & 1u) ? nodeBounds.max.x : midpoint.x;

        childBox.min.y = (ii & 2u) ? midpoint.y : nodeBounds.min.y;
        childBox.max.y = (ii & 2u) ? nodeBounds.max.y : midpoint.y;

        childBox.min.z = (ii & 4u) ? midpoint.z : nodeBounds.min.z;
        childBox.max.z = (ii & 4u) ? nodeBounds.max.z : midpoint.z;
    }

    for (size_t ii = 0; ii < points.size(); ii++) {
        const auto& v = points[ii];
        uint32_t idx = 0;

        idx |= (v.positionOpacity.x > midpoint.x) ? 1u : 0u;
        idx |= (v.positionOpacity.y > midpoint.y) ? 2u : 0u;
        idx |= (v.positionOpacity.z > midpoint.z) ? 4u : 0u;

        childPoints[static_cast<size_t>(idx)].push_back(v);
        childIndices[static_cast<size_t>(idx)].push_back(indices[ii]);
    }

    auto childNodes = std::make_shared<std::array<std::unique_ptr<OctreeNode>, OCTREE_NODE_COUNT>>();
    std::array<IThreadPool::IResult::Ptr, OCTREE_NODE_COUNT> results;
    std::array<bool, OCTREE_NODE_COUNT> hasChildren = {};

    const size_t numThreadsMax = std::thread::hardware_concurrency();

    for (size_t ii = 0; ii < OCTREE_NODE_COUNT; ii++) {
        if (childPoints[ii].empty() || childPoints[ii].size() == points.size()) {
            continue;
        }

        if (childPoints[ii].size() > threadingPointThreshold) {
            // parallelization condition met, create threads

            if (static_cast<size_t>(activeThreads->load(std::memory_order_relaxed)) < numThreadsMax) {
                activeThreads->fetch_add(1, std::memory_order_relaxed);
                hasChildren[ii] = true;

                auto task = CreateFunctionTask(
                    [childNodes, depth, ii, points = BASE_NS::move(childPoints[ii]),
                        indices = BASE_NS::move(childIndices[ii]), box = childBoxes[ii], maxPointsPerNode, threadPool,
                        activeThreads, threadingPointThreshold]() mutable {
                        (*childNodes)[ii] = BuildOctreeNode(points, indices, box, maxPointsPerNode, threadPool,
                            activeThreads, threadingPointThreshold, depth + 1);
                        activeThreads->fetch_sub(1, std::memory_order_relaxed);
                    });

                results[ii] = threadPool->Push(BASE_NS::move(task));
            } else {
                // not enough threads available, build sequentially
                node->children[ii] = BuildOctreeNode(childPoints[ii], childIndices[ii], childBoxes[ii],
                    maxPointsPerNode, threadPool, activeThreads, threadingPointThreshold, depth + 1);
            }
        } else {
            // parallelization condition not met, build sequentially
            node->children[ii] = BuildOctreeNode(childPoints[ii], childIndices[ii], childBoxes[ii], maxPointsPerNode,
                threadPool, activeThreads, threadingPointThreshold, depth + 1);
        }
    }

    for (size_t ii = 0; ii < OCTREE_NODE_COUNT; ii++) {
        if (hasChildren[ii]) {
            results[ii]->Wait();
        }
    }

    for (size_t ii = 0; ii < OCTREE_NODE_COUNT; ii++) {
        if (hasChildren[ii]) {
            node->children[ii] = BASE_NS::move((*childNodes)[ii]);
        }
    }

    return node;
}

std::unique_ptr<OctreeNode> BuildGSOctree(const BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>& vertices,
    const size_t maxPointsPerNode, IThreadPool* threadPool)
{
    BASE_NS::vector<uint32_t> indices;
    indices.resize(vertices.size());
    std::iota(indices.begin(), indices.end(), 0);

    const OctreeNode::AABB rootBounds = ComputeBoundingBox(vertices);

    auto activeThreads = std::make_shared<std::atomic<int>>(0);

    return BuildOctreeNode(
        vertices, indices, rootBounds, maxPointsPerNode, threadPool, activeThreads, GS_LOD_OCTREE_PAR_THR, 0);
}

void GatherGSOctreeLeaves(const OctreeNode* node, BASE_NS::vector<const OctreeNode*>& leaves)
{
    if (!node) {
        return;
    }

    if (node->IsLeaf()) {
        if (!node->points.empty()) {
            leaves.push_back(node);
        }
    } else {
        for (const auto& child : node->children) {
            if (child) {
                GatherGSOctreeLeaves(child.get(), leaves);
            }
        }
    }
}

float SrgbToLinear(const float u)
{
    return u <= 0.04045f ? u / 12.92f : Math::pow((u + 0.055f) / 1.055f, 2.4f);
}

float LinearToSrgb(const float u)
{
    return u <= 0.0031308f ? u * 12.92f : 1.055f * Math::pow(u, 1.0f / 2.4f) - 0.055f;
}

Math::Vec3 XyzToLab(const Math::Vec3& xyz)
{
    const float xn = 0.95047f;
    const float yn = 1.0f;
    const float zn = 1.08883f;

    auto f = [](const float t) {
        const float delta = 6.0f / 29.0f;
        return t > delta * delta * delta ? std::cbrt(t) : t / (3.0f * delta * delta) + 4.0f / 29.0f;
    };

    const float fx = f(xyz.x / xn);
    const float fy = f(xyz.y / yn);
    const float fz = f(xyz.z / zn);

    return Math::Vec3(116.0f * fy - 16.0f, 500.0f * (fx - fy), 200.0f * (fy - fz));
}

Math::Vec3 LabToXyz(const Math::Vec3& lab)
{
    const float xn = 0.95047f;
    const float yn = 1.0f;
    const float zn = 1.08883f;

    const float fy = (lab.x + 16.0f) / 116.0f;
    const float fx = lab.y / 500.0f + fy;
    const float fz = fy - lab.z / 200.0f;

    auto finv = [](const float t) {
        const float delta = 6.0f / 29.0f;
        return t > delta ? t * t * t : 3.0f * delta * delta * (t - 4.0f / 29.0f);
    };

    return Math::Vec3(xn * finv(fx), yn * finv(fy), zn * finv(fz));
}

Math::Vec3 ComputePerceivedColorAverage(
    const BASE_NS::vector<Math::Vec3>& colors, const BASE_NS::vector<float>& opacities, const float gamma)
{
    // Compute the "perceived mean color" by taking the average in the LAB color space.

    if (colors.empty()) {
        return Math::Vec3(0, 0, 0);
    }

    if (colors.size() == 1) {
        return colors.front();
    }

    float sumOpacity = 0.0f;
    Math::Vec3 sumLab(0.0f, 0.0f, 0.0f);

    // convert colors to LAB and sum
    for (size_t ii = 0; ii < colors.size(); ii++) {
        const auto& c = colors[ii];
        const float opacity = opacities[ii];
        const float r = SrgbToLinear(c.x / 255.0f);
        const float g = SrgbToLinear(c.y / 255.0f);
        const float b = SrgbToLinear(c.z / 255.0f);

        // rgb to xyz
        const float x = 0.4124564f * r + 0.3575761f * g + 0.1804375f * b;
        const float y = 0.2126729f * r + 0.7151522f * g + 0.0721750f * b;
        const float z = 0.0193339f * r + 0.1191920f * g + 0.9503041f * b;

        const Math::Vec3 lab = XyzToLab(Math::Vec3(x, y, z));
        sumLab += lab * opacity;
        sumOpacity += opacity;
    }

    if (sumOpacity == 0.0f) {
        return Math::Vec3(0, 0, 0);
    }

    // average LAB
    Math::Vec3 avgLab = sumLab / sumOpacity;

    // apply LAB-space L-component gamma correction
    const float normalizedL = Math::pow(avgLab.x / 100.0f, gamma);
    avgLab.x = Math::clamp(normalizedL * 100.0f, 0.0f, 100.0f);

    const Math::Vec3 xyz = LabToXyz(avgLab);

    // xyz to linear rgb
    const float rLinear = Math::clamp01(3.2404542f * xyz.x - 1.5371385f * xyz.y - 0.4985314f * xyz.z);
    const float gLinear = Math::clamp01(-0.9692660f * xyz.x + 1.8760108f * xyz.y + 0.0415560f * xyz.z);
    const float bLinear = Math::clamp01(0.0556434f * xyz.x - 0.2040259f * xyz.y + 1.0572252f * xyz.z);

    const float r = Math::round(Math::clamp(LinearToSrgb(rLinear) * 255.0f, 0.0f, 255.0f));
    const float g = Math::round(Math::clamp(LinearToSrgb(gLinear) * 255.0f, 0.0f, 255.0f));
    const float b = Math::round(Math::clamp(LinearToSrgb(bLinear) * 255.0f, 0.0f, 255.0f));

    return Math::Vec3(r, g, b);
}

Math::Vec3 UnpackColor(float colorPacked)
{
    const uint32_t colorPackedUint = BitCast<uint32_t>(colorPacked);
    const Math::Vec3 color(static_cast<float>((colorPackedUint >> 24u) & 0xFFu),
        static_cast<float>((colorPackedUint >> 16u) & 0xFFu), static_cast<float>((colorPackedUint >> 8u) & 0xFFu));

    return color;
}

Math::Vec3 RgbToXyz(const Math::Vec3& rgb)
{
    float r = rgb.x / 255.0f;
    float g = rgb.y / 255.0f;
    float b = rgb.z / 255.0f;

    r = (r > 0.04045f) ? std::pow((r + 0.055f) / 1.055f, 2.4f) : r / 12.92f;
    g = (g > 0.04045f) ? std::pow((g + 0.055f) / 1.055f, 2.4f) : g / 12.92f;
    b = (b > 0.04045f) ? std::pow((b + 0.055f) / 1.055f, 2.4f) : b / 12.92f;

    return Math::Vec3(r * 0.4124f + g * 0.3576f + b * 0.1805f, r * 0.2126f + g * 0.7152f + b * 0.0722f,
        r * 0.0193f + g * 0.1192f + b * 0.9505f);
}

Math::Vec3 RgbToLab(const Math::Vec3& rgb)
{
    return XyzToLab(RgbToXyz(rgb));
}

Math::Vec3 PackedToLab(float colorPacked)
{
    return RgbToLab(UnpackColor(colorPacked));
}

Math::Vec3 GetPrincipalDirection(const Math::Vec3& covA, const Math::Vec3& covB)
{
    // scale the covariance values so that we have enough numerical precision
    const float scaleBias = 100.0f;
    const float data[9u] = { covA.x * scaleBias, covA.y * scaleBias, covA.z * scaleBias, covA.y * scaleBias,
        covB.x * scaleBias, covB.y * scaleBias, covA.z * scaleBias, covB.y * scaleBias, covB.z * scaleBias };
    const Math::Mat3X3 covariance(data);

    // faster power iteration instead of full eigen value decompositions
    Math::Vec3 bb(1.0f, 1.0f, 1.0f);
    bb = Math::Normalize(bb);

    for (uint32_t ii = 0; ii < 4u; ii++) {
        const Math::Vec3 Ab(data[0u] * bb.x + data[3u] * bb.y + data[6u] * bb.z,
            data[1u] * bb.x + data[4u] * bb.y + data[7u] * bb.z, data[2u] * bb.x + data[5u] * bb.y + data[8u] * bb.z);

        const float norm = Math::Magnitude(Ab);
        if (norm < 1e-6f)
            break;

        bb = Ab / norm;
    }

    return bb;
}

float OrientationAngle(const Math::Vec3& dir1, const Math::Vec3& dir2)
{
    const float dotProduct = Math::clamp(Math::Dot(dir1, dir2), -1.0f, 1.0f);
    return std::acos(Math::abs(dotProduct));
}

float ComputePointStddev(const BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>& vertices)
{
    if (vertices.empty()) {
        return 0.0f;
    }

    Math::Vec3 mean(0.0f, 0.0f, 0.0f);
    for (const auto& vd : vertices) {
        mean += vd.positionOpacity;
    }
    mean /= static_cast<float>(vertices.size());

    float variance = 0.0f;
    for (const auto& vd : vertices) {
        variance += Math::SqrMagnitude(Math::Vec3(vd.positionOpacity) - mean);
    }

    variance /= static_cast<float>(vertices.size());
    return Math::sqrt(variance);
}

struct GsColorInfo {
    Math::Vec3 position;
    Math::Vec3 labColor;
    float lightness { 0.0f };
    Math::Vec3 principalDirection;
    GS::GsObjectLoadData::GsSplatData vd;
    BASE_NS::vector<uint32_t> originalIndices;
    float featureRatio { 1.0f };
    float radius { 0.0f };
    bool isActive { true }; // false if combined
};

struct CombinationPair {
    size_t index1 { 0 };
    size_t index2 { 0 };
    float combinationCost;

    bool operator<(const CombinationPair& other) const
    {
        return combinationCost < other.combinationCost;
    }
};

// cost for combining two splats (lower = a better combination)
float CalculateCombinationCost(const GsColorInfo& aSplat, const GsColorInfo& bSplat, const float maxSpatialDist,
    const float maxColorDist, const float maxOrientationDiff)
{
    const float spatialDist = Math::Magnitude(aSplat.position - bSplat.position);
    const float colorDist = Math::Magnitude(aSplat.labColor - bSplat.labColor);
    const float orientationDiff = OrientationAngle(aSplat.principalDirection, bSplat.principalDirection);
    const float radiusDiff = Math::abs(aSplat.radius - bSplat.radius);
    const float featureDiff = Math::abs(aSplat.featureRatio - bSplat.featureRatio);

    // normalize
    const float spatialComponent = maxSpatialDist > 0.0f ? spatialDist / maxSpatialDist : 0.0f;
    const float colorComponent = maxColorDist > 0.0f ? colorDist / maxColorDist : 0.0f;
    const float orientationComponent = maxOrientationDiff > 0.0f ? orientationDiff / maxOrientationDiff : 0.0f;
    const float maxRadius = Math::max(aSplat.radius, bSplat.radius);
    const float radiusComponent = maxRadius > 0.0f ? radiusDiff / maxRadius : 0.0f;
    const float featureComponent = featureDiff;

    // TODO: tune weights

    // weighted combination
    return spatialComponent * 0.4f + colorComponent * 0.25f + orientationComponent * 0.2f + radiusComponent * 0.1f +
           featureComponent * 0.05f;
}

GsColorInfo CombineSplats(const GsColorInfo& aSplat, const GsColorInfo& bSplat)
{
    GsColorInfo combined;

    // merge the original indices
    combined.originalIndices = aSplat.originalIndices;
    combined.originalIndices.insert(
        combined.originalIndices.end(), bSplat.originalIndices.begin(), bSplat.originalIndices.end());

    // weight by opacity
    const float weightA = aSplat.vd.positionOpacity.w;
    const float weightB = bSplat.vd.positionOpacity.w;
    const float totalWeight = weightA + weightB;

    if (totalWeight > 0.0f) {
        combined.position = (aSplat.position * weightA + bSplat.position * weightB) / totalWeight;
    } else {
        combined.position = (aSplat.position + bSplat.position) * 0.5f;
    }

    // combined opacity using alpha blending
    const float transmittance = (1.0f - weightA) * (1.0f - weightB);
    const float combinedOpacity = Math::clamp01(1.0f - transmittance);
    combined.vd.positionOpacity = Math::Vec4(combined.position, combinedOpacity);

    // average feature ratio
    combined.featureRatio = (aSplat.featureRatio + bSplat.featureRatio) * 0.5f;

    // combine covariance matrices
    const Math::Vec3 offsetA = aSplat.position - combined.position;
    const Math::Vec3 offsetB = bSplat.position - combined.position;

    const float covDataA[9u] = { aSplat.vd.covAScale.x, aSplat.vd.covAScale.y, aSplat.vd.covAScale.z,
        aSplat.vd.covAScale.y, aSplat.vd.covBColorPacked.x, aSplat.vd.covBColorPacked.y,
        aSplat.vd.covAScale.z, aSplat.vd.covBColorPacked.y, aSplat.vd.covBColorPacked.z };

    const float covDataB[9u] = { bSplat.vd.covAScale.x, bSplat.vd.covAScale.y, bSplat.vd.covAScale.z,
        bSplat.vd.covAScale.y, bSplat.vd.covBColorPacked.x, bSplat.vd.covBColorPacked.y,
        bSplat.vd.covAScale.z, bSplat.vd.covBColorPacked.y, bSplat.vd.covBColorPacked.z };

    float combinedCovData[9u];
    std::fill(&combinedCovData[0u], &combinedCovData[8u], 0.0f);

    //  prevent the splats from becoming overly bloated
    const float smoothAdjust = Math::min(aSplat.featureRatio, bSplat.featureRatio);

    for (size_t ii = 0; ii < 9; ii++) {
        combinedCovData[ii] = (covDataA[ii] * weightA + covDataB[ii] * weightB);
    }

    // add offset contributions with parallel axis theorem
    combinedCovData[0u] += (offsetA.x * offsetA.x * weightA + offsetB.x * offsetB.x * weightB) * smoothAdjust;
    combinedCovData[1u] += (offsetA.x * offsetA.y * weightA + offsetB.x * offsetB.y * weightB) * smoothAdjust;
    combinedCovData[2u] += (offsetA.x * offsetA.z * weightA + offsetB.x * offsetB.z * weightB) * smoothAdjust;
    combinedCovData[3u] += (offsetA.y * offsetA.x * weightA + offsetB.y * offsetB.x * weightB) * smoothAdjust;
    combinedCovData[4u] += (offsetA.y * offsetA.y * weightA + offsetB.y * offsetB.y * weightB) * smoothAdjust;
    combinedCovData[5u] += (offsetA.y * offsetA.z * weightA + offsetB.y * offsetB.z * weightB) * smoothAdjust;
    combinedCovData[6u] += (offsetA.z * offsetA.x * weightA + offsetB.z * offsetB.x * weightB) * smoothAdjust;
    combinedCovData[7u] += (offsetA.z * offsetA.y * weightA + offsetB.z * offsetB.y * weightB) * smoothAdjust;
    combinedCovData[8u] += (offsetA.z * offsetA.z * weightA + offsetB.z * offsetB.z * weightB) * smoothAdjust;

    if (totalWeight > 0.0f) {
        for (size_t ii = 0; ii < 9; ii++) {
            combinedCovData[ii] /= totalWeight;
        }
    } else {
        for (size_t ii = 0; ii < 9; ii++) {
            combinedCovData[ii] *= 0.5f;
        }
    }

    // ensure symmetry
    combinedCovData[1u] = combinedCovData[3u] = (combinedCovData[1u] + combinedCovData[3u]) * 0.5f;
    combinedCovData[2u] = combinedCovData[6u] = (combinedCovData[2u] + combinedCovData[6u]) * 0.5f;
    combinedCovData[5u] = combinedCovData[7u] = (combinedCovData[5u] + combinedCovData[7u]) * 0.5f;

    // ensure positive definite
    constexpr float epsilon = 1e-9f;
    combinedCovData[0u] = Math::max(combinedCovData[0u], epsilon);
    combinedCovData[4u] = Math::max(combinedCovData[4u], epsilon);
    combinedCovData[8u] = Math::max(combinedCovData[8u], epsilon);

    combined.vd.covAScale = Math::Vec4(combinedCovData[0u], combinedCovData[1u], combinedCovData[2u], 0.0f);

    const BASE_NS::vector<Math::Vec3> colors = { UnpackColor(aSplat.vd.covBColorPacked.w),
        UnpackColor(bSplat.vd.covBColorPacked.w) };
    const BASE_NS::vector<float> opacities = { weightA, weightB };

    const Math::Vec3 combinedColor = ComputePerceivedColorAverage(colors, opacities, GS_LOD_GAMMA_CORRECTION);
    const uint32_t colorPacked = PackGSColor((combinedColor / 255.0f), combinedOpacity);
    const float colorFloat = BitCast<float>(colorPacked);

    combined.vd.covBColorPacked = Math::Vec4(combinedCovData[4u], combinedCovData[5u], combinedCovData[8u], colorFloat);
    combined.vd.astcOffsetColor.x = aSplat.vd.astcOffsetColor.x;
    combined.vd.astcOffsetColor.y = aSplat.vd.astcOffsetColor.y;
    combined.vd.astcOffsetColor.z = colorPacked;

    combined.labColor = PackedToLab(colorFloat);
    combined.lightness = combined.labColor.x;
    combined.principalDirection = GetPrincipalDirection(combined.vd.covAScale, combined.vd.covBColorPacked);
    combined.radius = GsRadOptim(combined.vd.covAScale, combined.vd.covBColorPacked);
    combined.isActive = true;

    // recompose true feature ratio
    const Math::Mat3X3 covMatrix(combinedCovData);
    const Math::Vec3 eigenvalues = EigenvalueDecompositionValues(covMatrix);
    const float maxEigen = Math::max(Math::max(eigenvalues.x, eigenvalues.y), eigenvalues.z);
    const float minEigen = Math::min(Math::min(eigenvalues.x, eigenvalues.y), eigenvalues.z);
    if (minEigen > 1e-6f) {
        const float ratio = maxEigen / minEigen;
        combined.featureRatio = Math::clamp01(1.0f / (1.0f + std::logf(ratio) * 0.1f));
    } else {
        combined.featureRatio = 0.0f;
    }

    return combined;
}

// adaptive octree leaf size calculation, based on density estimation
size_t CalculateAdaptiveLeafSize(const BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>& verticesData)
{
    if (verticesData.empty()) {
        return GS_LOD_LEAF_SIZE;
    }
    const size_t sampleSize = Math::min(verticesData.size(), static_cast<size_t>(10000u));
    const size_t stride = verticesData.size() / sampleSize;

    BASE_NS::vector<float> sampleRadii;
    sampleRadii.reserve(sampleSize);

    for (size_t ii = 0; ii < verticesData.size(); ii += stride) {
        const auto& vd = verticesData[ii];
        sampleRadii.push_back(GsRadOptim(vd.covAScale, vd.covBColorPacked));
    }

    if (sampleRadii.empty()) {
        return GS_LOD_LEAF_SIZE;
    }
    std::sort(sampleRadii.begin(), sampleRadii.end());

    // statistics
    const float minRadius = sampleRadii.front();
    const float maxRadius = sampleRadii.back();
    const float medianRadius = sampleRadii[sampleRadii.size() / 2u];
    const float varianceRatio = (maxRadius - minRadius) / Math::max(medianRadius, 1e-6f);

    const float varianceAdjustment = Math::clamp(1.0f / (varianceRatio * 0.01f), 0.3f, 4.0f);

    const size_t adaptiveLeafSize = static_cast<size_t>(GS_LOD_LEAF_SIZE * varianceAdjustment);

    return Math::clamp(
        adaptiveLeafSize, static_cast<size_t>(GS_LOD_LEAF_SIZE / 2u), static_cast<size_t>(GS_LOD_LEAF_SIZE * 10u));
}

std::tuple<BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>, BASE_NS::vector<BASE_NS::vector<uint32_t>>>
DownsampleLeafNodeMultiPass(const OctreeNode* node, const float refRad, const float targetReduction)
{
    const size_t numPoints = node->points.size();
    if (numPoints <= 1u) {
        return { {}, {} };
    }

    BASE_NS::vector<GsColorInfo> colorPoints;
    colorPoints.reserve(numPoints);

    BASE_NS::vector<float> allRadii;
    allRadii.reserve(numPoints);

    for (size_t ii = 0; ii < numPoints; ii++) {
        const auto& vd = node->points[ii];

        GsColorInfo info;
        info.position = Math::Vec3(vd.positionOpacity);
        info.labColor = PackedToLab(vd.covBColorPacked.w);
        info.lightness = info.labColor.x;
        info.principalDirection = GetPrincipalDirection(vd.covAScale, vd.covBColorPacked);
        info.vd = vd;
        info.originalIndices = { node->pointIndices[ii] };
        info.radius = GsRadOptim(vd.covAScale, vd.covBColorPacked);
        info.isActive = true;

        // feature ratio
        const float covData[9u] = { vd.covAScale.x, vd.covAScale.y, vd.covAScale.z,
            vd.covAScale.y, vd.covBColorPacked.x, vd.covBColorPacked.y,
            vd.covAScale.z, vd.covBColorPacked.y, vd.covBColorPacked.z };
        const Math::Mat3X3 cov(covData);
        const Math::Vec3 eigenvalues = EigenvalueDecompositionValues(cov);
        const float maxEigen = Math::max(Math::max(eigenvalues.x, eigenvalues.y), eigenvalues.z);
        const float minEigen = Math::min(Math::min(eigenvalues.x, eigenvalues.y), eigenvalues.z);

        if (minEigen > 1e-6f) {
            const float ratio = maxEigen / minEigen;
            info.featureRatio = Math::clamp01(1.0f / (1.0f + std::logf(ratio) * 0.1f));
        } else {
            info.featureRatio = 0.0f;
        }

        colorPoints.push_back(info);
        allRadii.push_back(info.radius);
    }

    // calculate the percentile threshold for large splats
    BASE_NS::vector<float> sortedRadii = allRadii;
    std::sort(sortedRadii.begin(), sortedRadii.end());
    const size_t percentile95Index =
        static_cast<size_t>(GS_LOD_LARGE_SPLAT_PERCENTILE * static_cast<float>(sortedRadii.size() - 1));
    const float largeSplatThreshold = sortedRadii[percentile95Index];

    const float localStddev = ComputePointStddev(node->points);
    const float baseSpatialThreshold = Math::min(localStddev / std::log(static_cast<float>(numPoints) + 1.0f), refRad);

    // dynamic anisotropy downsampling target
    float avgFeatureRatio = 0.0f;
    int activeCount = 0;
    for (const auto& info : colorPoints) {
        if (info.isActive) {
            avgFeatureRatio += info.featureRatio;
            activeCount++;
        }
    }
    avgFeatureRatio = (activeCount > 0) ? avgFeatureRatio / static_cast<float>(activeCount) : 0.0f;
    const float downsamplingTarget = Math::lerp(0.3f, 0.5f, avgFeatureRatio);

    // multi-pass reduction
    const size_t targetFinalSize =
        Math::max(static_cast<size_t>(static_cast<float>(numPoints) * (1.0f - downsamplingTarget)), (size_t)1u);

    for (uint32_t pass = 0; pass < GS_LOD_NUM_PASSES; pass++) {
        size_t activeSplats = 0;
        for (const auto& info : colorPoints) {
            if (info.isActive)
                activeSplats++;
        }

        if (activeSplats <= targetFinalSize) {
            break;
        }
        const float passProgress = static_cast<float>(pass) / static_cast<float>(GS_LOD_NUM_PASSES);

        // relax thresholds with each pass
        const float spatialThreshold = baseSpatialThreshold * (1.0f + passProgress * 0.5f);
        const float colorThreshold = GS_LOD_LAB_DISTANCE * (1.0f + passProgress * 0.3f);
        const float orientationThreshold = Math::DEG2RAD * GS_LOD_ORIENTATION_DEG * (1.0f + passProgress * 0.2f);

        BASE_NS::vector<CombinationPair> validPairs;

        // find all valid pairs for pass
        for (size_t ii = 0; ii < colorPoints.size(); ii++) {
            if (!colorPoints[ii].isActive) {
                continue;
            }

            for (size_t jj = ii + 1; jj < colorPoints.size(); jj++) {
                if (!colorPoints[jj].isActive) {
                    continue;
                }

                const GsColorInfo& aSplat = colorPoints[ii];
                const GsColorInfo& bSplat = colorPoints[jj];

                // skip if too large, protect large splats like skybox
                if (aSplat.radius > largeSplatThreshold || bSplat.radius > largeSplatThreshold) {
                    continue;
                }

                // spatial distance
                const float spatialDist = Math::Magnitude(aSplat.position - bSplat.position);
                if (spatialDist > spatialThreshold) {
                    continue;
                }

                // color similarity
                const float colorDist = Math::Magnitude(aSplat.labColor - bSplat.labColor);
                if (colorDist > colorThreshold) {
                    continue;
                }

                // orientation similarity
                const float orientationDiff = OrientationAngle(aSplat.principalDirection, bSplat.principalDirection);
                const float adaptedOrientationThreshold =
                    orientationThreshold * Math::clamp(aSplat.featureRatio, 0.25f, 1.0f);
                if (orientationDiff > adaptedOrientationThreshold &&
                    aSplat.featureRatio < GS_LOD_ORIENTATION_SHAPE_THR &&
                    bSplat.featureRatio < GS_LOD_ORIENTATION_SHAPE_THR) {
                    continue;
                }

                // feature ratio similarity
                const float featureDiff = Math::abs(aSplat.featureRatio - bSplat.featureRatio);
                if (featureDiff > GS_LOD_SPLAT_SIMILARITY_THR) {
                    continue;
                }

                // combination cost for this merge
                const float cost = CalculateCombinationCost(
                    aSplat, bSplat, spatialThreshold, colorThreshold, adaptedOrientationThreshold);

                validPairs.push_back({ ii, jj, cost });
            }
        }

        if (validPairs.empty()) {
            break;
        }

        // sort by cost, best combinations first
        std::sort(validPairs.begin(), validPairs.end());

        // greedy combine
        BASE_NS::vector<bool> usedInThisPass(colorPoints.size(), false);
        size_t combinationsThisPass = 0;

        for (const auto& pair : validPairs) {
            if (usedInThisPass[pair.index1] || usedInThisPass[pair.index2]) {
                continue;
            }
            if (!colorPoints[pair.index1].isActive || !colorPoints[pair.index2].isActive) {
                continue;
            }

            // check for large splats
            const GsColorInfo& aSplat = colorPoints[pair.index1];
            const GsColorInfo& bSplat = colorPoints[pair.index2];
            if (aSplat.radius > largeSplatThreshold || bSplat.radius > largeSplatThreshold) {
                continue;
            }

            const GsColorInfo combined = CombineSplats(colorPoints[pair.index1], colorPoints[pair.index2]);

            // replace first with merged, mark second as inactive
            colorPoints[pair.index1] = combined;
            colorPoints[pair.index2].isActive = false;

            usedInThisPass[pair.index1] = true;
            usedInThisPass[pair.index2] = true;
            combinationsThisPass++;
        }

        if (combinationsThisPass == 0) {
            break;
        }
    }

    // collect final results
    BASE_NS::vector<GS::GsObjectLoadData::GsSplatData> finalSplats;
    BASE_NS::vector<BASE_NS::vector<uint32_t>> finalLookup;

    for (const auto& info : colorPoints) {
        if (info.isActive) {
            finalSplats.push_back(info.vd);
            finalLookup.push_back(info.originalIndices);
        }
    }

    return { finalSplats, finalLookup };
}

void CreateDynamicLODs(GS::GsPrimitive& primitive, GS::GsObjectLoadDataExt* loadData, IThreadPool::Ptr threadPool)
{
    BASE_NS::vector<const OctreeNode*> leaves;
    std::unique_ptr<OctreeNode> gsOctree;

    const uint32_t numGs0 = primitive.numGs;

    // suboptimal to take a copy, perhaps a slice reference?
    const BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>& verticesData = loadData->gsVerticesData.GetOriginVector();

    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "BuildOctree", "", 0xFF00FF);

        const size_t targetLeafSize = CalculateAdaptiveLeafSize(verticesData);

        CORE_LOG_I("Using adaptive leaf size: %zu for model with %zu gaussians", targetLeafSize, verticesData.size());

        gsOctree = BuildGSOctree(verticesData, targetLeafSize, threadPool.get());

        {
            CORE_CPU_PERF_SCOPE("LUMEGS", "GatherOctreeLeaves", "", 0xFF00FF);
            GatherGSOctreeLeaves(gsOctree.get(), leaves);
        }

#ifndef NDEBUG
        size_t childSizeSum = 0;
        size_t childMaxSize = 0;
        for (const OctreeNode* on : leaves) {
            childSizeSum += on->points.size();
            childMaxSize = Math::max(childMaxSize, on->points.size());
        }

        CORE_LOG_I("Gaussian octree created with %d leaves, average points-per-child %d, max points-per-child %d",
            static_cast<int>(leaves.size()), static_cast<int>(childSizeSum / leaves.size()),
            static_cast<int>(childMaxSize));
#endif
    }

    // calculate reference parameters
    float refRad = 0.0f;
    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "GsSystem::RefRadii", "", 0xFF00FF);
        BASE_NS::vector<float> radii(verticesData.size(), 0.0f);

        const auto numThreads = std::max(threadPool->GetNumberOfThreads(), 1u);
        const auto chunkSize = (verticesData.size() + numThreads - 1) / numThreads;
        BASE_NS::vector<std::pair<size_t, size_t>> partitions;

        size_t partStart = 0;
        for (size_t ii = 0; ii < numThreads && partStart < verticesData.size(); ii++) {
            size_t partEnd = partStart;
            if (verticesData.size() - partStart > chunkSize) {
                partEnd = partStart + chunkSize;
            } else {
                partEnd = verticesData.size();
            }
            partitions.push_back({ partStart, partEnd });
            partStart = partEnd;
        }

        BASE_NS::vector<IThreadPool::IResult::Ptr> results;
        for (const auto& part : partitions) {
            auto task = CreateFunctionTask([&verticesData, &radii, part]() {
                const size_t start = part.first;
                const size_t end = part.second;
                for (size_t ii = start; ii < end; ii++) {
                    const auto& vd = verticesData[ii];
                    radii[ii] = GsRadOptim(vd.covAScale, vd.covBColorPacked);
                }
            });
            auto result = threadPool->Push(BASE_NS::move(task));
            results.push_back(BASE_NS::move(result));
        }

        for (auto& result : results) {
            result->Wait();
        }

        const float percentile = Math::lerp(GS_LOD_PERCENTILE_MIN, GS_LOD_PERCENTILE_MAX,
            Math::clamp01(((static_cast<float>(verticesData.size()) - GS_LOD_PERCENTILE_INTERP_MIN)) /
                          static_cast<float>((GS_LOD_PERCENTILE_INTERP_MAX - GS_LOD_PERCENTILE_INTERP_MIN))));

        const size_t percentileIndex = static_cast<size_t>(percentile * static_cast<float>(radii.size() - 1));
        std::nth_element(radii.begin(), radii.begin() + static_cast<int64_t>(percentileIndex), radii.end());

        refRad = radii[percentileIndex];

        CORE_LOG_I("GS3D model reference radius: %f with percentile %f", refRad, percentile);
    }

    // multi-pass downsampling
    BASE_NS::vector<GS::GsObjectLoadData::GsSplatData> gsDownsampled;
    BASE_NS::vector<BASE_NS::vector<uint32_t>> gsIndexLookup;

    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "MultiPassDownsample", "", 0xFF00FF);

        const uint32_t numThreads = Math::max(threadPool->GetNumberOfThreads(), 1u);

        BASE_NS::vector<BASE_NS::vector<GS::GsObjectLoadData::GsSplatData>> threadDownsampled(numThreads);
        BASE_NS::vector<BASE_NS::vector<BASE_NS::vector<uint32_t>>> threadIndexLookup(numThreads);

        auto processLeafNodesRange = [&](const size_t threadId, const size_t startIdx, const size_t endIdx) {
            for (size_t ii = startIdx; ii < endIdx; ii++) {
                const OctreeNode* node = leaves[ii];
                if (node->points.empty()) {
                    continue;
                }

                constexpr float downsamplingTarget = 0.5f;

                const auto [leafDownsampled, lookup] = DownsampleLeafNodeMultiPass(node, refRad, downsamplingTarget);

                threadDownsampled[threadId].insert(
                    threadDownsampled[threadId].end(), leafDownsampled.begin(), leafDownsampled.end());
                threadIndexLookup[threadId].insert(threadIndexLookup[threadId].end(), lookup.begin(), lookup.end());
            }
        };

        BASE_NS::vector<IThreadPool::IResult::Ptr> results;
        const size_t leavesPerThread = (leaves.size() + numThreads - 1) / numThreads;

        for (uint32_t tt = 0; tt < numThreads; tt++) {
            const size_t startIdx = tt * leavesPerThread;
            const size_t endIdx = std::min(startIdx + leavesPerThread, leaves.size());

            if (startIdx >= leaves.size()) {
                break;
            }

            auto task =
                CreateFunctionTask([&, tt, startIdx, endIdx]() { processLeafNodesRange(tt, startIdx, endIdx); });
            auto result = threadPool->Push(BASE_NS::move(task));
            results.push_back(BASE_NS::move(result));
        }

        for (auto& result : results) {
            result->Wait();
        }

        size_t totalDownsampledSize = 0;
        size_t totalLookupSize = 0;

        for (uint32_t tt = 0; tt < numThreads; tt++) {
            totalDownsampledSize += threadDownsampled[tt].size();
            totalLookupSize += threadIndexLookup[tt].size();
        }

        {
            CORE_CPU_PERF_SCOPE("LUMEGS", "AppendData", "", 0xFF00FF);

            gsDownsampled.reserve(totalDownsampledSize);
            gsIndexLookup.reserve(totalLookupSize);

            for (uint32_t tt = 0; tt < numThreads; tt++) {
                for (auto& data : threadDownsampled[tt]) {
                    gsDownsampled.push_back(BASE_NS::move(data));
                }

                for (auto& lookup : threadIndexLookup[tt]) {
                    gsIndexLookup.push_back(BASE_NS::move(lookup));
                }
            }
        }
    }

    const float reductionAchieved = 1.0f - (static_cast<float>(gsDownsampled.size()) / static_cast<float>(numGs0));

    CORE_LOG_I("Multi-pass downsampling complete: %d -> %d gaussians (%.1f%% reduction)", static_cast<int>(numGs0),
        static_cast<int>(gsDownsampled.size()), reductionAchieved * 100.0f);

    const size_t numDownsampledGaussians = gsDownsampled.size();

    // assign
    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "Assign", "", 0xFF00FF);

        const size_t previousPositionsSize = loadData->gaussianPositions.size();

        loadData->gaussianPositions.resize(previousPositionsSize + numDownsampledGaussians);
        loadData->gsVerticesData.Reserve(loadData->gsVerticesData.Size() + numDownsampledGaussians);

        auto* positionsStart = loadData->gaussianPositions.data() + previousPositionsSize;
        for (size_t ii = 0; ii < gsDownsampled.size(); ii++) {
            const auto& gd = gsDownsampled[ii];
            positionsStart[ii] = Math::Vec4(Math::Vec3(gd.positionOpacity), 1.0f);
            loadData->gsVerticesData.PushBack(BASE_NS::move(gsDownsampled[ii]));
        }

        primitive.gsLodLookup = BASE_NS::move(gsIndexLookup);
        primitive.numGs1 = static_cast<uint32_t>(numDownsampledGaussians);
    }

    // create the spherical harmonics in parallel
    if (primitive.RequireSH()) {
        // assign spherical harmonics for LOD1
        loadData->sphericalHarmonics.reserve(loadData->sphericalHarmonics.size() + numDownsampledGaussians);

        CORE_CPU_PERF_SCOPE("LUMEGS", "SphericalHarmonics", "", 0xFF00FF);

        const auto numThreads = Math::max(threadPool->GetNumberOfThreads(), 1u);
        const auto entriesPerTask = (primitive.gsLodLookup.size() + numThreads - 1) / numThreads;

        BASE_NS::vector<GS::GsHarmonicsData> newHarmonicsData(primitive.gsLodLookup.size());
        BASE_NS::vector<IThreadPool::IResult::Ptr> taskResults;

        auto shCopy = [&loadData, &primitive, &newHarmonicsData](size_t startIdx, size_t endIdx) {
            for (size_t idx = startIdx; idx < endIdx; idx++) {
                const auto& gsll = primitive.gsLodLookup[idx];

                // Taking the 1st harmonic is a good enough approximation.
                newHarmonicsData[idx] = loadData->sphericalHarmonics[gsll.front()];
            }
        };
        for (size_t taskIdx = 0; taskIdx < numThreads; taskIdx++) {
            const size_t startIdx = taskIdx * entriesPerTask;
            if (startIdx >= primitive.gsLodLookup.size()) {
                break;
            }

            const size_t endIdx = Math::min(startIdx + entriesPerTask, primitive.gsLodLookup.size());

            auto task = CreateFunctionTask([shCopy, startIdx, endIdx]() {
                shCopy(startIdx, endIdx);
            });

            taskResults.push_back(threadPool->Push(BASE_NS::move(task)));
        }

        for (auto& result : taskResults) {
            result->Wait();
        }

        for (const auto& harmonicData : newHarmonicsData) {
            loadData->sphericalHarmonics.emplace_back(harmonicData);
        }
    }

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsSystem::PopulateSizeBuffer", "", 0xFF00FF);

    // populate size data buffer for LOD selection in parallel
    {
        BASE_NS::vector<float> gsSizes(primitive.numGs1, 0.0f);
        BASE_NS::vector<float> gsSizesMedian(primitive.numGs1, 0.0f);

        for (size_t ii = 0; ii < primitive.numGs1; ii++) {
            const auto& vd = loadData->gsVerticesData.Get(loadData->gsVerticesData.Size() - primitive.numGs1 + ii);
            const float radius = GsRadOptim(vd.covAScale, vd.covBColorPacked);
            gsSizes[ii] = radius;
            gsSizesMedian[ii] = radius;
        }

        CORE_NS::ParallelSort(gsSizesMedian.begin(), gsSizesMedian.end(), threadPool.get());

        primitive.gsSizeData = BASE_NS::move(gsSizes);
        primitive.gsMedianSize = gsSizesMedian[gsSizesMedian.size() / 2u];
    }
}

} // namespace GS

#endif // PLUGIN_GS_LOD_TOOLS_H