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

#include "render_data_store_default_gs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <future>
#include <limits>
#include <numeric>
#include <securec.h>

#include <base/containers/array_view.h>
#include <base/math/matrix_util.h>
#include <core/intf_engine.h>
#include <core/log.h>
#include <core/namespace.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/util/parallel_sort.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/resource_handle.h>

#include "app/shaders/common/gs_common.h"
#include "app/shaders/common/gs_struct_common.h"

#include "util/gs_utils.h"

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;

// Effective Inclusive 8-Points Scan Algorithm
static void EffectiveInplaceScan8pTLab(BASE_NS::vector<uint32_t>& counts)
{
    int32_t countsSize = static_cast<int32_t>(counts.size());
    uint32_t* countsPointer = counts.data();
    int32_t step = 8u;
    uint32_t sum = 0u;
    uint32_t node0 = 0u;
    uint32_t node1 = 0u;
    uint32_t node2 = 0u;
    uint32_t node3 = 0u;
    uint32_t node4 = 0u;
    uint32_t node5 = 0u;
    uint32_t node6 = 0u;
    uint32_t node7 = 0u;
    while (countsSize >= step) {
        node0 = countsPointer[0u] + sum;
        node1 = countsPointer[1u];
        node2 = countsPointer[2u];
        node3 = countsPointer[3u];
        node4 = countsPointer[4u];
        node5 = countsPointer[5u];
        node6 = countsPointer[6u];
        node7 = countsPointer[7u];

        node1 += node0; // Stage 1
        node3 += node2;
        node5 += node4;
        node7 += node6;

        node2 += node1; // Stage 2
        node3 += node1;
        node7 += node5;

        node4 += node3; // Stage 3
        node5 += node3;
        node7 += node3;

        node6 += node5; // Stage 4

        countsPointer[0u] = node0;
        countsPointer[1u] = node1;
        countsPointer[2u] = node2;
        countsPointer[3u] = node3;
        countsPointer[4u] = node4;
        countsPointer[5u] = node5;
        countsPointer[6u] = node6;
        countsPointer[7u] = node7;

        sum = node7;
        countsPointer += step;
        countsSize -= step;
    }
    while (countsSize > 0) {
        *countsPointer += sum;
        sum = *countsPointer;
        countsPointer++;
        countsSize--;
    }
}

namespace GS {

const string_view RENDER_DATA_STORE_DEFAULT_STAGING { "RenderDataStoreDefaultStaging" };
constexpr float DRS_VALUE_DEFAULT = 1.1f;

RenderDataStoreDefaultGS::RenderDataStoreDefaultGS(IRenderContext& context, const string_view name)
    : fileManager_(context.GetEngine().GetFileManager()), context_(context), name_(name.data()),
      gpuResourceMgr_(context_.GetDevice().GetGpuResourceManager()),
      gsRenderData0_(std::make_unique<GS::GsRenderData>()), gsRenderData1_(std::make_unique<GS::GsRenderData>())
{
    const auto factory = GetInstance<ITaskQueueFactory>(UID_TASK_QUEUE_FACTORY);
    if (factory) {
        threadPool_ =
            factory->CreateThreadPool(Math::max(static_cast<uint32_t>(std::thread::hardware_concurrency()) - 1, 1u));

        sortCache_.SetSortBits(GS_COUNT_SORT_BITS);
        sortCache_.Initialize(threadPool_->GetNumberOfThreads(), 1u);
    }

    sortIsDone_ = std::make_shared<std::atomic_bool>(false);
    sortInProgress_ = std::make_shared<std::atomic_bool>(false);
    activeNumGS_ = std::make_shared<std::atomic_uint32_t>(0);
}

RenderDataStoreDefaultGS::~RenderDataStoreDefaultGS()
{
    // wait for sort to finish
    while (sortInProgress_ && sortInProgress_->load()) {
        std::this_thread::yield();
    }

    // explicit clear and release
    gsRenderData0_ = nullptr;
    gsRenderData1_ = nullptr;
}

void RenderDataStoreDefaultGS::PostRender()
{
    Clear();
}

void RenderDataStoreDefaultGS::Clear() {}

void RenderDataStoreDefaultGS::Ref()
{
    refcnt_.fetch_add(1, std::memory_order_relaxed);
}

void RenderDataStoreDefaultGS::Unref()
{
    if (std::atomic_fetch_sub_explicit(&refcnt_, 1, std::memory_order_release) == 1) {
        std::atomic_thread_fence(std::memory_order_acquire);
        delete this;
    }
}

BASE_NS::Math::Vec2 RenderDataStoreDefaultGS::GetDepthRange(const BASE_NS::Math::Mat4X4& viewMatrix)
{
    float minDepth = std::numeric_limits<float>::max();
    float maxDepth = std::numeric_limits<float>::lowest();

    for (const GS::GsPrimitive& prim : primitives_) {
        const BASE_NS::Math::Vec4 aabbMin = BASE_NS::Math::Vec4(prim.aabbMin, 1.0f);
        const BASE_NS::Math::Vec4 aabbMax = BASE_NS::Math::Vec4(prim.aabbMax, 1.0f);

        const Math::Mat4X4 vwMat = viewMatrix * prim.worldMatrix;

        // acquire min-max depths from all 8 corners of the AABB, since the
        // viewMatrix can also contain the world matrix.
        const BASE_NS::Math::Vec4 corners[8u] = {
            BASE_NS::Math::Vec4(aabbMin.x, aabbMin.y, aabbMin.z, 1),
            BASE_NS::Math::Vec4(aabbMax.x, aabbMin.y, aabbMin.z, 1),
            BASE_NS::Math::Vec4(aabbMin.x, aabbMax.y, aabbMin.z, 1),
            BASE_NS::Math::Vec4(aabbMax.x, aabbMax.y, aabbMin.z, 1),
            BASE_NS::Math::Vec4(aabbMin.x, aabbMin.y, aabbMax.z, 1),
            BASE_NS::Math::Vec4(aabbMax.x, aabbMin.y, aabbMax.z, 1),
            BASE_NS::Math::Vec4(aabbMin.x, aabbMax.y, aabbMax.z, 1),
            BASE_NS::Math::Vec4(aabbMax.x, aabbMax.y, aabbMax.z, 1),
        };

        for (const auto& corner : corners) {
            const float depth = (vwMat * corner).z;
            minDepth = std::min(minDepth, depth);
            maxDepth = std::max(maxDepth, depth);
        }
    }

    return BASE_NS::Math::Vec2(minDepth, maxDepth);
}

BASE_NS::Math::Mat4X4 RenderDataStoreDefaultGS::GetWorldMatrix(const uint32_t gsIdx)
{
    return primitives_[gsIdx].worldMatrix;
}

GsRenderData* RenderDataStoreDefaultGS::GetGsRenderData()
{
    if (gsRenderDataAccess_.load()) {
        return gsRenderData1_.get();
    } else {
        return gsRenderData0_.get();
    }
}

GsRenderData* RenderDataStoreDefaultGS::GetSafeLoadingGsRenderData()
{
    if (sortThread_.joinable()) {
        sortThread_.join();
    }

    const auto lg = GetDataLockGuard();

    if (gsRenderDataAccess_.load()) {
        CloneGsRenderData(gsRenderData1_.get(), gsRenderData0_.get());
        return gsRenderData0_.get();
    } else {
        CloneGsRenderData(gsRenderData0_.get(), gsRenderData1_.get());
        return gsRenderData1_.get();
    }
}

int32_t RenderDataStoreDefaultGS::GetRefCount()
{
    return refcnt_;
}

void RenderDataStoreDefaultGS::AddGsPrimitive(const GsPrimitive& primitive)
{
    // wait for sort to finish
    if (sortThread_.joinable()) {
        sortThread_.join();
    }

    const auto lg = GetDataLockGuard();

#ifndef NDEBUG
    CORE_LOG_D("AddGsPrimitive");
#endif

    primitives_.emplace_back(primitive);
}

void RenderDataStoreDefaultGS::RemoveGsPrimitive(const Entity& entity)
{
    // wait for sort to finish
    if (sortThread_.joinable()) {
        sortThread_.join();
    }

    const auto lg = GetDataLockGuard();

    size_t removeIdx = 0;
    bool found = false;

    for (size_t ii = 0; ii < primitives_.size(); ii++) {
        if (primitives_[ii].splatEntity == entity) {
            removeIdx = ii;
            found = true;
            if (ii == 0) {
                GetGsRenderData()->isMaterialValid = false;
            }
            break;
        }
    }

    if (!found) {
        CORE_LOG_E("Error: Could not find GS3D primitive to remove!");
        return;
    }

#ifndef NDEBUG
    CORE_LOG_D("RemoveGsPrimitive");
#endif

    primitives_.erase(primitives_.begin() + static_cast<int64_t>(removeIdx));
}

bool RenderDataStoreDefaultGS::RemoveGsPrimitiveIfPresent(const Entity& entity)
{
    // wait for sort to finish
    if (sortThread_.joinable()) {
        sortThread_.join();
    }

    const auto lg = GetDataLockGuard();

    for (size_t ii = 0; ii < primitives_.size(); ii++) {
        if (primitives_[ii].splatEntity != entity) {
            continue;
        }

        if (ii == 0) {
            if (GsRenderData* renderData = GetGsRenderData()) {
                renderData->isMaterialValid = false;
            }
        }

#ifndef NDEBUG
        CORE_LOG_D("RemoveGsPrimitiveIfPresent");
#endif

        primitives_.erase(primitives_.begin() + static_cast<int64_t>(ii));
        return true;
    }

    return false;
}

BASE_NS::vector<GsPrimitive>& RenderDataStoreDefaultGS::GetGsPrimitives()
{
    return primitives_;
}

BASE_NS::array_view<const GsPrimitive> RenderDataStoreDefaultGS::GetGsPrimitives() const
{
    return { primitives_.data(), primitives_.size() };
}

void RenderDataStoreDefaultGS::EnableSortFlag()
{
    sortingEnabled_.store(true);
}

void RenderDataStoreDefaultGS::DisableSortFlag()
{
    sortingEnabled_.store(false);

    if (sortThread_.joinable()) {
        sortThread_.join();
    }
}

int RenderDataStoreDefaultGS::GetGsPrimitiveIndex(const CORE_NS::Entity entity) const
{
    for (size_t ii = 0; ii < primitives_.size(); ii++) {
        if (primitives_[ii].splatEntity == entity) {
            return static_cast<int>(ii);
        }
    }

    return -1;
}

bool RenderDataStoreDefaultGS::ApplyOpacityMask(uint32_t primitiveIndex, const std::vector<int>& mask)
{
    if (primitiveIndex >= primitives_.size()) {
        CORE_LOG_E("ApplyOpacityMask: invalid primitiveIndex %u", primitiveIndex);
        return false;
    }

    auto renderData = GetGsRenderData();
    if (!renderData) {
        CORE_LOG_E("ApplyOpacityMask: renderData is null");
        return false;
    }

    const GsPrimitive& prim = primitives_[primitiveIndex];
    const uint32_t start = prim.startOffset;
    const uint32_t count = prim.numGs + prim.numGs1;

    if (mask.size() < count) {
        CORE_LOG_E("ApplyOpacityMask: mask size %zu < required %u", mask.size(), count);
        return false;
    }

    if (static_cast<size_t>(start) + count > renderData->gaussianPositions.size()) {
        CORE_LOG_E("ApplyOpacityMask: start+count out of range");
        return false;
    }

    {
        OpacityBackup backup;
        backup.opacityValues.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            backup.opacityValues[i] = renderData->gaussianPositions[start + i].w;
        }
        opacityBackups_[primitiveIndex] = std::move(backup);
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (mask[i] == 0) {
            renderData->gaussianPositions[start + i].w = 0.0f;
        }
    }

    if (RenderHandleUtil::IsValid(renderData->gsSplatData.GetHandle())) {
        void* gpuData = gpuResourceMgr_.MapBufferMemory(renderData->gsSplatData);
        if (gpuData) {
            size_t splatSize = renderData->CanUsePackedData() ? sizeof(GsObjectLoadData::GsSplatDataPacked)
                                                              : sizeof(GsObjectLoadData::GsSplatData);

            for (uint32_t i = 0; i < count; ++i) {
                if (mask[i] == 0) {
                    auto* ptr = static_cast<uint8_t*>(gpuData) + (start + i) * splatSize;
                    if (renderData->CanUsePackedData()) {
                        auto* splat = reinterpret_cast<GsObjectLoadData::GsSplatDataPacked*>(ptr);
                        splat->positionOpacityPacked.y &= 0xFFFFu;
                    } else {
                        auto* splat = reinterpret_cast<GsObjectLoadData::GsSplatData*>(ptr);
                        splat->positionOpacity.w = 0.0f;
                    }
                }
            }
            gpuResourceMgr_.UnmapBuffer(renderData->gsSplatData);
        }
    }

    CORE_LOG_I("ApplyOpacityMask: applied to primitive %u, %u splats affected", primitiveIndex, count);
    SetNeedsSort();
    return true;
}

BASE_NS::pair<uint32_t, uint32_t> RenderDataStoreDefaultGS::GetPrimitiveRange(uint32_t primitiveIndex) const
{
    if (primitiveIndex >= primitives_.size()) {
        return {0u, 0u};
    }
    const GsPrimitive& prim = primitives_[primitiveIndex];
    return {prim.startOffset, prim.numGs + prim.numGs1};
}

bool RenderDataStoreDefaultGS::UndoOpacityMask(uint32_t primitiveIndex)
{
    if (primitiveIndex >= primitives_.size()) {
        CORE_LOG_E("UndoOpacityMask: invalid primitiveIndex %u", primitiveIndex);
        return false;
    }

    auto it = opacityBackups_.find(primitiveIndex);
    if (it == opacityBackups_.end()) {
        CORE_LOG_E("UndoOpacityMask: no backup found for primitive %u", primitiveIndex);
        return false;
    }

    auto renderData = GetGsRenderData();
    if (!renderData) {
        CORE_LOG_E("UndoOpacityMask: no renderData");
        return false;
    }

    const GsPrimitive& prim = primitives_[primitiveIndex];
    const uint32_t start = prim.startOffset;
    const uint32_t count = static_cast<uint32_t>(it->second.opacityValues.size());

    if (static_cast<size_t>(start) + count > renderData->gaussianPositions.size()) {
        CORE_LOG_E("UndoOpacityMask: start+count out of range");
        return false;
    }

    // Recover gaussianPositions on CPU
    for (uint32_t i = 0; i < count; ++i) {
        renderData->gaussianPositions[start + i].w = it->second.opacityValues[i];
    }

    // Recover gsSplatData on GPU
    if (RenderHandleUtil::IsValid(renderData->gsSplatData.GetHandle())) {
        void* gpuData = gpuResourceMgr_.MapBufferMemory(renderData->gsSplatData);
        if (gpuData) {
            size_t splatSize = renderData->CanUsePackedData() ? sizeof(GsObjectLoadData::GsSplatDataPacked)
                                                              : sizeof(GsObjectLoadData::GsSplatData);

            for (uint32_t i = 0; i < count; ++i) {
                auto* ptr = static_cast<uint8_t*>(gpuData) + (start + i) * splatSize;
                if (renderData->CanUsePackedData()) {
                    auto* splat = reinterpret_cast<GsObjectLoadData::GsSplatDataPacked*>(ptr);
                    float posZ = UnpackHalf2x16(splat->positionOpacityPacked.y).x;
                    splat->positionOpacityPacked.y = PackHalf2x16(posZ, it->second.opacityValues[i]);
                } else {
                    auto* splat = reinterpret_cast<GsObjectLoadData::GsSplatData*>(ptr);
                    splat->positionOpacity.w = it->second.opacityValues[i];
                }
            }
            gpuResourceMgr_.UnmapBuffer(renderData->gsSplatData);
        }
    }

    opacityBackups_.erase(it);
    SetNeedsSort();

    CORE_LOG_I("UndoOpacityMask: restored primitive %u", primitiveIndex);
    return true;
}
bool RenderDataStoreDefaultGS::CanUndoOpacityMask(uint32_t primitiveIndex) const
{
    return opacityBackups_.find(primitiveIndex) != opacityBackups_.end();
}
void RenderDataStoreDefaultGS::ClearOpacityBackup(uint32_t primitiveIndex)
{
    opacityBackups_.erase(primitiveIndex);
}
void RenderDataStoreDefaultGS::FlipValidGsRenderData()
{
    // wait for sort to finish
    if (sortThread_.joinable()) {
        sortThread_.join();
    }

#ifndef NDEBUG
    CORE_LOG_D("FlipValidGsRenderData");
#endif

    const auto lg = GetDataLockGuard();

    // flip
    gsRenderDataAccess_.store(!gsRenderDataAccess_.load());
    if (sortIsDone_ != nullptr) {
        sortIsDone_->store(false);
    }
    if (sortInProgress_ != nullptr) {
        sortInProgress_->store(false);
    }
    needsSort_.store(true);

    // invalidate the now-loading side. When tiled, mirror the active side instead of
    // wiping: the tiled path never re-clones between flips, so any render handles
    // populated once at pool init (tilePool, dataStorageCacheHandle, viewZStoreCacheHandle,
    // etc.) must persist on both ping-pong buffers or the draw loses them every other flip.
    GsRenderData* toClear = gsRenderDataAccess_.load() ? gsRenderData0_.get() : gsRenderData1_.get();
    const GsRenderData* nowActive = gsRenderDataAccess_.load() ? gsRenderData1_.get() : gsRenderData0_.get();
    if (toClear) {
        if (!primitives_.empty() && nowActive && nowActive->isTiled) {
            *toClear = *nowActive;
        } else {
            *toClear = {};
        }
    }
}

void RenderDataStoreDefaultGS::ClearRenderData()
{
    if (sortThread_.joinable()) {
        sortThread_.join();
    }

    const auto lg = GetDataLockGuard();

    if (gsRenderData0_) {
        *gsRenderData0_ = {};
    }
    if (gsRenderData1_) {
        *gsRenderData1_ = {};
    }

    gsRenderDataAccess_.store(false);
    if (sortIsDone_) {
        sortIsDone_->store(false);
    }
    if (sortInProgress_) {
        sortInProgress_->store(false);
    }
    if (activeNumGS_) {
        activeNumGS_->store(0u);
    }
    needsSort_.store(true);
    isDynamicRendering_ = false;
    isSingleFrameGenEnabled_ = false;
}

void RenderDataStoreDefaultGS::ResetTiledRenderData()
{
    if (sortThread_.joinable()) {
        sortThread_.join();
    }

    const auto clearTiledFields = [](GsRenderData* rd) {
        if (!rd) {
            return;
        }

        rd->isTiled = false;
        rd->tilePool.reset();
        rd->areResourcesLoaded = false;
    };

    const auto lg = GetDataLockGuard();
    clearTiledFields(gsRenderData0_.get());
    clearTiledFields(gsRenderData1_.get());

    needsSort_.store(true);
    isSingleFrameGenEnabled_ = false;
}

GS::UniqueScopedLock RenderDataStoreDefaultGS::GetDataLockGuard()
{
    return BASE_NS::move(UniqueScopedLock(gsDataMtx_));
}

BASE_NS::Math::UVec2 RenderDataStoreDefaultGS::GetRenderResolution() const
{
    return renderResolution;
}

void RenderDataStoreDefaultGS::SetRenderResolution(const BASE_NS::Math::UVec2 resolution)
{
    renderResolution = resolution;
}

uint32_t RenderDataStoreDefaultGS::GetGsScaleFactor() const
{
    return gsScaleFactor;
}

void RenderDataStoreDefaultGS::SetGsScaleFactor(const uint32_t scaleFactor)
{
    if (this->gsScaleFactor != 0xFFFFFFFFu) {
        CORE_LOG_I("RenderDataStoreDefaultGS::SetGsScaleFactor: scale factor already set to %du, skipping.",
            this->gsScaleFactor);
        return;
    }

    this->gsScaleFactor = scaleFactor;
}

float RenderDataStoreDefaultGS::GetRenderScaleFactor() const
{
    return GetDRSScale() * static_cast<float>(GetGsScaleFactor()) * GetXegUpscaleFactor();
}

float RenderDataStoreDefaultGS::GetDRSScale() const
{
    if (this->isDynamicRendering_) {
        return this->DRSScale_;
    } else {
        return 1.0f;
    }
}

void RenderDataStoreDefaultGS::SetDRSScale(float value)
{
    if (value <= 0) {
        CORE_LOG_E("drs value not valid, fallback to default");
        this->DRSScale_ = DRS_VALUE_DEFAULT;
        return;
    }
    this->DRSScale_ = value;
}

bool RenderDataStoreDefaultGS::GetDynamicRendering() const
{
    return this->isDynamicRendering_;
}

void RenderDataStoreDefaultGS::SetDynamicRendering(bool isDynamicRendering)
{
    this->isDynamicRendering_ = isDynamicRendering;
}

bool RenderDataStoreDefaultGS::GetSingleFrameGenEnable() const
{
    return this->isSingleFrameGenEnabled_;
}

void RenderDataStoreDefaultGS::SetSingleFrameGenEnable(bool enabled)
{
    this->isSingleFrameGenEnabled_ = enabled;
}

void RenderDataStoreDefaultGS::UpdateFrameGenerationState(bool cameraMoved,
    const BASE_NS::Math::Mat4X4& viewMatrix, BASE_NS::Math::UVec2 currFrameRenderResolution)
{
    bool cameraHasValidMotion = cameraMoved;
    const Math::Vec3 currentCameraForward =
        Math::Normalize(Math::Vec3(viewMatrix[2u][0u], viewMatrix[2u][1u], viewMatrix[2u][2u]));
    const Math::Vec3 currentCameraPosition = ExtractCameraPositionFromViewMatrix(viewMatrix);

    if (cameraHasValidMotion) {
        const float dotP = Math::Dot(currentCameraForward, lastFrameForwardDirection_);
        const float positionDifferenceSq = Math::Distance2(currentCameraPosition, lastFramePosition_);
        if ((Math::abs(dotP - 1.0f) <= MAX_FRAME_GEN_CAMERA_MOTION_ANGLE) &&
                   (positionDifferenceSq <= MAX_FRAME_GEN_CAMERA_MOTION_DIST_SQR)) {
            cameraHasValidMotion = true;
        } else {
            cameraHasValidMotion = false;
        }
    }

    if (Math::Dot(lastFrameForwardDirection_, lastFrameForwardDirection_) == 0.0f) {
        cameraHasValidMotion = true;
    }
    isSingleFrameGenEnabled_ = cameraHasValidMotion && (preFrameRenderResolution_ == currFrameRenderResolution);
    lastFrameForwardDirection_ = currentCameraForward;
    lastFramePosition_ = currentCameraPosition;
    preFrameRenderResolution_ = currFrameRenderResolution;
}

// XEG Upscale configuration getters/setters
float RenderDataStoreDefaultGS::GetXegUpscaleFactor() const
{
    return xegUpscaleEnabled_ ? xegUpscaleFactor_ : 1.0f;
}

void RenderDataStoreDefaultGS::SetXegUpscaleFactor(float factor)
{
    xegUpscaleFactor_ = Math::max(factor, 1.0f);
}

// Internal sharpness parameter
float RenderDataStoreDefaultGS::GetXegUpscaleSharpnessInternal() const
{
    return xegUpscaleSharpness_;
}

void RenderDataStoreDefaultGS::SetXegUpscaleSharpnessInternal(float sharpness)
{
    xegUpscaleSharpness_ = Math::clamp(sharpness, 0.0f, 1.0f);
}

// XEG Upscale enabled flag
bool RenderDataStoreDefaultGS::GetXegUpscaleEnabled() const
{
    return xegUpscaleEnabled_;
}

void RenderDataStoreDefaultGS::SetXegUpscaleEnabled(bool enabled)
{
    xegUpscaleEnabled_ = enabled;
}

void RenderDataStoreDefaultGS::SetCurrentAnimationTime(const float time)
{
    animationTimeSeconds_ = time;
}

float RenderDataStoreDefaultGS::GetCurrentAnimationTime() const
{
    return animationTimeSeconds_;
}

void RenderDataStoreDefaultGS::DisableFadeForGsModel(const uint32_t gsIdx)
{
    primitives_[gsIdx].animationFadeProperties.useFadeAnimation = false;
}

void RenderDataStoreDefaultGS::DisableUBOCopyFlagForGsModel(const uint32_t gsIdx)
{
    primitives_[gsIdx].animationFadeProperties.useFadingCopyFlag = false;
}

void RenderDataStoreDefaultGS::SetDirtyForGsModel(const uint32_t gsIdx, const bool isDirty)
{
    primitives_[gsIdx].isDirty = isDirty;
}

uint32_t RenderDataStoreDefaultGS::GetNumActiveGs()
{
    if (GetGsRenderData()->CanUseGpuSort()) {
        if (!GetGsRenderData()->anyComputeLods) {
            return GetGsRenderData()->GetTotalGsCount();
        } else {
            return activeNumGS_->load();
        }
    } else {
        return activeNumGS_->load();
    }
}

// for plugin / factory interface
refcnt_ptr<IRenderDataStore> RenderDataStoreDefaultGS::Create(IRenderContext& context, char const* name)
{
    return refcnt_ptr<IRenderDataStore>(new RenderDataStoreDefaultGS(context, name));
}

void RenderDataStoreDefaultGS::ExecuteGaussianSortCpuTask(GsRenderData* renderData,
    const BASE_NS::Math::Mat4X4 viewMatrix, const BASE_NS::Math::Mat4X4 projMatrix, const BASE_NS::Math::IVec2 viewport,
    IThreadPool* threadPool, GaussianSortCpuTaskExecuteMode mode)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "RenderDataStoreDefaultGS::GaussianSortCpu", "", 0xFF00FF);

    BASE_NS::vector<uint32_t>& indices = renderData->gaussianIndices;
    const BASE_NS::vector<Math::Vec4>& positions = renderData->gaussianPositions;

    const float frustumScale = renderData->frustumScale;

    const BASE_NS::vector<float>& maxScales = renderData->gaussianMaxScales;
    const bool enableScoreCulling = renderData->enableScoreCulling;
    const uint32_t gsOptFlag = renderData->enableGsOpt;

    size_t maxNumGsTheoretical = 0;
    for (const auto& prim : primitives_) {
        maxNumGsTheoretical += prim.numGs;
    }

    if (maxNumGsTheoretical == 0) {
        activeNumGS_->store(0);
        if (mode == GaussianSortCpuTaskExecuteMode::Async) {
            sortIsDone_->store(true);
            sortInProgress_->store(false);
            needsSort_.store(false);
        }
        return;
    }

    // reference viewport size
    const Math::Vec2 viewportRef(1920.0f, 1027.0f);
    const float viewDiagonalRef = Math::Magnitude(Math::Vec2(viewportRef.x, viewportRef.y));

    const uint32_t numThreads = Math::clamp(threadPool->GetNumberOfThreads() - 1u, 1u, 16u);

    struct GsPrimitiveWorkUnit {
        const GS::GsPrimitive* prim;
        uint32_t startIdx;
        uint32_t endIdx;
        size_t startOffset;
        size_t offsetLod1;
        Math::Mat4X4 vp;
        Math::Mat4X4 mvp;
        Math::Vec3 cameraPos;
        Math::Vec3 cameraViewDir;

        float gsAverageRadius;
        float lodBias;
        float radiusScalingFactor;
    };

    BASE_NS::vector<GsPrimitiveWorkUnit> workUnits;
    workUnits.reserve(primitives_.size() * 32u);

    constexpr uint32_t chunkSize = 4096u;

    // build work units
    for (const auto& prim : primitives_) {
        const Math::Mat4X4 viewWorld = viewMatrix * prim.worldMatrix;
        const Math::Mat4X4 projViewWorld = projMatrix * viewWorld;
        const size_t startOffset = prim.startOffset;

        if (prim.computeLods) {
            const Math::Vec3 cameraPos = Math::Inverse(projViewWorld)[3];
            const Math::Vec3 cameraViewDir = Math::Vec3(projViewWorld[0][2], projViewWorld[1][2], projViewWorld[2][2]);
            const size_t offsetLod1 = startOffset + prim.numGs;

            const float gsAverageRadius = prim.gsMedianSize;
            const float lodBias = prim.lodBias;
            const float radiusScalingFactor =
                Math::clamp(gsAverageRadius * 800.0f, 0.25f, 2.0f) * lodBias * GS_LOD_BASE_BIAS;

            const uint32_t numChunks = (prim.numGs1 + chunkSize - 1) / chunkSize;
            uint32_t startIdx = 0u;
            for (uint32_t chunk = 0; chunk < numChunks; chunk++) {
                const uint32_t endIdx = Math::min(startIdx + chunkSize, prim.numGs1);

                workUnits.push_back({ &prim, startIdx, endIdx, startOffset, offsetLod1, viewWorld, projViewWorld,
                    cameraPos, cameraViewDir, gsAverageRadius, lodBias, radiusScalingFactor });
                startIdx += chunkSize;
            }
        } else {
            const uint32_t numChunks = (prim.numGs + chunkSize - 1) / chunkSize;
            uint32_t startIdx = 0u;
            for (uint32_t chunk = 0; chunk < numChunks; chunk++) {
                const uint32_t endIdx = Math::min(startIdx + chunkSize, static_cast<uint32_t>(prim.numGs));

                workUnits.push_back(
                    { &prim, startIdx, endIdx, startOffset, 0, viewWorld, projViewWorld, {}, {}, 0.0f, 0.0f, 0.0f });
                startIdx += chunkSize;
            }
        }
    }

    auto& threadData = sortCache_.threadData;
    sortCache_.ResetThreadData();
    threadData.resize(numThreads);

    const size_t estimatedPerThread = (maxNumGsTheoretical / numThreads) * 2u;
    for (uint32_t tt = 0; tt < numThreads; tt++) {
        threadData[tt].gsToSort.reserve(estimatedPerThread + estimatedPerThread / 2u);
        threadData[tt].minRawDepth = std::numeric_limits<float>::infinity();
        threadData[tt].maxRawDepth = -std::numeric_limits<float>::infinity();
    }

    // work distribution counter
    std::atomic<uint32_t> nextWorkUnit(0);
    const uint32_t totalWorkUnits = static_cast<uint32_t>(workUnits.size());

    auto& results = sortCache_.results;
    results.clear();
    results.reserve(numThreads);

    const float halfTanFovY = -1.0f / float(projMatrix[1][1]);
    const float tanFovX = halfTanFovY * float(viewport.x) / float(viewport.y);
    const float fov = std::atan(tanFovX) * 2.0f;
    float cullingThreshold = enableScoreCulling ? cullingThreshold_ : 0.0f;

    // process all work units in parallel
    for (uint32_t tt = 0; tt < numThreads; tt++) {
        auto task = CreateFunctionTask([&nextWorkUnit, &threadData, &positions, &maxScales, &workUnits, frustumScale,
                                           totalWorkUnits, viewDiagonalRef, fov, cullingThreshold, tt, gsOptFlag]() {
            auto& localData = threadData[tt];

            while (true) {
                const uint32_t workIdx = nextWorkUnit.fetch_add(1);
                if (workIdx >= totalWorkUnits) {
                    break;
                }

                const auto& work = workUnits[workIdx];
                const auto* prim = work.prim;

                if (prim->computeLods) {
                    // process LODs

                    const float radiusScalingFactor = work.radiusScalingFactor;

                    for (uint32_t ii = work.startIdx; ii < work.endIdx; ii++) {
                        const Math::Vec4 point = Math::Vec4(Math::Vec3(positions[work.offsetLod1 + ii]), 1.0f);
                        const Math::Vec4 ndc = work.mvp * point;

                        bool isBehindCamera = ndc.z < 0;
                        if (frustumScale > 0.0f) {
                            float bound = ndc.w * frustumScale;
                            isBehindCamera =
                                ndc.z < 0 || ndc.x > bound || ndc.x < -bound || ndc.y > bound || ndc.y < -bound;
                        }
                        if (isBehindCamera) {
                            continue;
                        }

                        const float size = prim->gsSizeData[ii];

                        const float depth = ndc.z;

                        const float absDepth = Math::abs(depth);
                        const float sizeScreen = (absDepth > 1e-6f) ? Math::abs(size / absDepth) * viewDiagonalRef
                                                                    : std::numeric_limits<float>::infinity();

                        const Math::Vec3 cameraToSplat = Math::Normalize((Math::Vec3(point) - work.cameraPos));
                        const float splatPerpendicularity = Math::abs(Math::Dot(cameraToSplat, work.cameraViewDir));

                        const BASE_NS::vector<uint32_t>& lod0indices = prim->gsLodLookup[ii];

                        const uint32_t lod0Count = static_cast<uint32_t>(lod0indices.size());
                        // aggregation factor, higher = more original splats combined
                        const float aggregationFactor = Math::clamp(static_cast<float>(lod0Count) / 10.0f, 1.0f, 5.0f);

                        // if the splat is less than 1px in size, LOD1 = LOD0 in terms of quality
                        const bool isSplatSmall = sizeScreen < 1.0f;
                        // if the splat is farther away than its thresholded size, use LOD1
                        const float distanceFactor = 1.0f - Math::clamp(ndc.z / ndc.w, 0.1f, 1.0f);
                        // detail assumed to be proportional to LOD merged splats
                        const float detailThreshold = Math::lerp(0.5f, 1.5f, distanceFactor) / aggregationFactor;
                        const bool isSplatSmallScaled =
                            Math::abs(depth) * radiusScalingFactor > (sizeScreen * detailThreshold);
                        // peripheral vision is LOD1
                        const bool isSplatFoveated = splatPerpendicularity < 0.75f;
                        // if the splat is very large, it's probably not a high-detail splat, thus use LOD1
                        const bool isSplatLarge = (sizeScreen * radiusScalingFactor > 30.0f);

                        const bool uselod1 =
                            (isBehindCamera || isSplatSmall || isSplatSmallScaled || isSplatFoveated) || isSplatLarge;

                        if (!uselod1) {
                            // use LOD0
                            for (const uint32_t& idx : lod0indices) {
                                const Math::Vec4 lod0Point =
                                    Math::Vec4(Math::Vec3(positions[work.startOffset + idx]), 1.0f);
                                const Math::Vec4 lod0Ndc = work.mvp * lod0Point;
                                auto sortFlag = -lod0Ndc.z / lod0Ndc.w;
                                if (gsOptFlag != 0) {
                                    sortFlag *= -1;
                                }
                                const float sortDepthValue =  sortFlag * GS_16BIT_COUNT_SORT_COARSITY;
                                
                                localData.minRawDepth = Math::min(localData.minRawDepth, sortDepthValue);
                                localData.maxRawDepth = Math::max(localData.maxRawDepth, sortDepthValue);

                                localData.gsToSort.push_back(
                                    { static_cast<uint32_t>(work.startOffset) + idx, sortDepthValue });
                            }
                        } else {
                            // use LOD1
                            auto sortFlag = -depth / ndc.w;
                            if (gsOptFlag != 0) {
                                sortFlag *= -1;
                            }
                            const float sortDepthValue = sortFlag * GS_16BIT_COUNT_SORT_COARSITY;

                            localData.minRawDepth = Math::min(localData.minRawDepth, sortDepthValue);
                            localData.maxRawDepth = Math::max(localData.maxRawDepth, sortDepthValue);

                            localData.gsToSort.push_back(
                                { static_cast<uint32_t>(work.offsetLod1 + ii), sortDepthValue });
                        }
                    }
                } else {
                    for (uint32_t ii = work.startIdx; ii < work.endIdx; ii++) {
                        const Math::Vec4 point = Math::Vec4(Math::Vec3(positions[work.startOffset + ii]), 1.0f);
                        const Math::Vec4 ndc = work.mvp * point;

                        bool isBehindCamera = ndc.z < 0;
                        if (frustumScale > 0.0f) {
                            float bound = ndc.w * frustumScale;
                            isBehindCamera =
                                ndc.z < 0 || ndc.x > bound || ndc.x < -bound || ndc.y > bound || ndc.y < -bound;
                        }
                        if (isBehindCamera) {
                            continue;
                        }

                        if (cullingThreshold > 0.0f) {
                            const Math::Vec3 gsSceneDistance = prim->aabbMax - prim->aabbMin;
                            const float importModifier = gsSceneDistance.x + gsSceneDistance.y + gsSceneDistance.z;
                            const float opacity = positions[work.startOffset + ii].w;
                            const float maxScale = maxScales[work.startOffset + ii];
                            const Math::Vec4 viewPoint = work.vp * point;
                            const float len =
                                Math::Magnitude(BASE_NS::Math::Vec3(viewPoint.x, viewPoint.y, viewPoint.z));
                            const float importance = opacity * maxScale / (fov * len);
                            if (importance < cullingThreshold * importModifier) {
                                localData.distanceCullCount += 1;
                                continue;
                            }
                        }
                        auto sortFlag = -ndc.z / ndc.w;
                        if (gsOptFlag != 0) {
                            sortFlag *= -1;
                        }
                        const float sortDepthValue = sortFlag * GS_16BIT_COUNT_SORT_COARSITY;

                        localData.minRawDepth = Math::min(localData.minRawDepth, sortDepthValue);
                        localData.maxRawDepth = Math::max(localData.maxRawDepth, sortDepthValue);

                        localData.gsToSort.push_back({ static_cast<uint32_t>(work.startOffset + ii), sortDepthValue });
                    }
                }
            }
        });

        auto result = threadPool->Push(BASE_NS::move(task));
        results.push_back(BASE_NS::move(result));
    }

    for (auto& result : results) {
        result->Wait();
    }

    size_t totalGaussians = 0;
    size_t totalDistanceCullCount = 0;
    float minRawDepth = std::numeric_limits<float>::infinity();
    float maxRawDepth = -std::numeric_limits<float>::infinity();
    auto& gsToSort = sortCache_.gsToSort;

    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "RenderDataStoreDefaultGS::GaussianSortCpu::ParallelMerge", "", 0xFF00FF);

        for (uint32_t tt = 0; tt < numThreads; tt++) {
            const auto& data = threadData[tt];
            totalGaussians += data.gsToSort.size();
            totalDistanceCullCount += data.distanceCullCount;

            minRawDepth = Math::min(minRawDepth, data.minRawDepth);
            maxRawDepth = Math::max(maxRawDepth, data.maxRawDepth);
        }

        if (totalGaussians == 0) {
            activeNumGS_->store(0);
            if (mode == GaussianSortCpuTaskExecuteMode::Async) {
                sortIsDone_->store(true);
                sortInProgress_->store(false);
                needsSort_.store(false);
            }
            return;
        }

        if (cullingThreshold > 0.0f) {
            const float disCullRate =
                static_cast<float>(totalDistanceCullCount) / (totalDistanceCullCount + totalGaussians);
            const float maxCullingRate = 0.11f;
            const float ascendStep = 1.33f;
            const float descendStep = 0.75f;
            const float maxCullingThreshold = 0.000007f;
            if (disCullRate < maxCullingRate && !isMaxThresholdFixed_ && cullingThreshold_ < maxCullingThreshold) {
                cullingThreshold_ *= ascendStep;
            } else if (disCullRate > maxCullingRate) {
                cullingThreshold_ = cullingThreshold_ * descendStep;
                isMaxThresholdFixed_ = true;
            }
        }

        gsToSort.clear();
        gsToSort.reserve(totalGaussians);

        // offsets for parallel copy
        BASE_NS::vector<size_t> offsets(numThreads + 1, 0u);
        for (uint32_t tt = 0; tt < numThreads; tt++) {
            offsets[tt + 1u] = offsets[tt] + threadData[tt].gsToSort.size();
        }

        gsToSort.resize(totalGaussians);

        // parallel copy
        results.clear();
        for (uint32_t tt = 0; tt < numThreads; tt++) {
            auto task = CreateFunctionTask([&threadData, &gsToSort, &offsets, tt]() {
                const auto& src = threadData[tt].gsToSort;
                auto* dst = gsToSort.data() + offsets[tt];

                BASE_NS::CloneData(dst, src.size() * sizeof(GaussianSortCache::GsSortInfo), src.data(),
                    src.size() * sizeof(GaussianSortCache::GsSortInfo));
            });

            auto result = threadPool->Push(BASE_NS::move(task));
            results.push_back(BASE_NS::move(result));
        }

        for (auto& result : results) {
            result->Wait();
        }
    }

    const uint32_t numGaussians = static_cast<uint32_t>(totalGaussians);

    // N-bit count sort, sort all of the indices in one go
    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "RenderDataStoreDefaultGS::GaussianSortCpu::NBitCountSort", "", 0xFF00FF);

        const float depthRange = maxRawDepth - minRawDepth;
        const float invDepthRange = 1.0f / depthRange;

        auto& counts = sortCache_.counts;
        std::fill(counts.begin(), counts.end(), 0);

        auto& normalizedDepths = sortCache_.normalizedDepths;
        normalizedDepths.resize(numGaussians);

        const uint32_t sortMax = sortCache_.GetSortMax();

        {
            CORE_CPU_PERF_SCOPE(
                "LUMEGS", "RenderDataStoreDefaultGS::GaussianSortCpu::16BitCountSort::NormalizeDepth", "", 0xFF00FF);

            const uint32_t itemsPerThread = (numGaussians + numThreads - 1u) / numThreads;
            results.clear();

            for (uint32_t tt = 0; tt < numThreads; tt++) {
                auto task = CreateFunctionTask([&gsToSort, &normalizedDepths, minRawDepth, invDepthRange, depthRange,
                                                   numGaussians, sortMax, itemsPerThread, tt]() {
                    const uint32_t startIdx = tt * itemsPerThread;
                    const uint32_t endIdx = Math::min(startIdx + itemsPerThread, numGaussians);

                    for (uint32_t ii = startIdx; ii < endIdx; ii++) {
                        const float clampedDepth =
                            Math::clamp(gsToSort[ii].sortDepth, minRawDepth, minRawDepth + depthRange);
                        const float depth01 = (clampedDepth - minRawDepth) * invDepthRange;
                        // nonlinearize the depth to improve the numerical precision of nearby splats
                        normalizedDepths[ii] =
                            static_cast<uint32_t>(static_cast<float>(sortMax - 1) * depth01 / (5.0f - 4.0f * depth01));
                    }
                });
                auto result = threadPool->Push(BASE_NS::move(task));
                results.push_back(BASE_NS::move(result));
            }

            for (auto& result : results) {
                result->Wait();
            }

            // count histogram
            {
                // Loop unrolling
                size_t ii = 0;
                for (; ii < (numGaussians & ~7u); ii += 8u) {
                    counts[normalizedDepths[ii]]++;
                    counts[normalizedDepths[ii + 1u]]++;
                    counts[normalizedDepths[ii + 2u]]++;
                    counts[normalizedDepths[ii + 3u]]++;
                    counts[normalizedDepths[ii + 4u]]++;
                    counts[normalizedDepths[ii + 5u]]++;
                    counts[normalizedDepths[ii + 6u]]++;
                    counts[normalizedDepths[ii + 7u]]++;
                }
                for (; ii < numGaussians; ++ii) {
                    counts[normalizedDepths[ii]]++;
                }
            }
        }

        EffectiveInplaceScan8pTLab(counts);

        indices.resize(numGaussians);
        {
            size_t ii = 0;
            for (; ii < (numGaussians & ~7u); ii += 8u) {
                uint32_t depthKey0 = normalizedDepths[ii];
                uint32_t depthKey1 = normalizedDepths[ii + 1u];
                uint32_t depthKey2 = normalizedDepths[ii + 2u];
                uint32_t depthKey3 = normalizedDepths[ii + 3u];
                uint32_t depthKey4 = normalizedDepths[ii + 4u];
                uint32_t depthKey5 = normalizedDepths[ii + 5u];
                uint32_t depthKey6 = normalizedDepths[ii + 6u];
                uint32_t depthKey7 = normalizedDepths[ii + 7u];
                uint32_t writeIndex0 = --counts[depthKey0]; // writeIndex always non-negative
                uint32_t writeIndex1 = --counts[depthKey1];
                uint32_t writeIndex2 = --counts[depthKey2];
                uint32_t writeIndex3 = --counts[depthKey3];
                uint32_t writeIndex4 = --counts[depthKey4];
                uint32_t writeIndex5 = --counts[depthKey5];
                uint32_t writeIndex6 = --counts[depthKey6];
                uint32_t writeIndex7 = --counts[depthKey7];
                indices[writeIndex0] = gsToSort[ii].index;
                indices[writeIndex1] = gsToSort[ii + 1u].index;
                indices[writeIndex2] = gsToSort[ii + 2u].index;
                indices[writeIndex3] = gsToSort[ii + 3u].index;
                indices[writeIndex4] = gsToSort[ii + 4u].index;
                indices[writeIndex5] = gsToSort[ii + 5u].index;
                indices[writeIndex6] = gsToSort[ii + 6u].index;
                indices[writeIndex7] = gsToSort[ii + 7u].index;
            }
            for (; ii < numGaussians; ii++) {
                const uint32_t depthKey = normalizedDepths[ii];
                const uint32_t writeIndex = --counts[depthKey];
                indices[writeIndex] = gsToSort[ii].index;
            }
        }
    }
    if (mode == GaussianSortCpuTaskExecuteMode::Async) {
        sortIsDone_->store(true);
        sortInProgress_->store(false);
        needsSort_.store(false);
    }
    activeNumGS_->store(numGaussians > 1u ? numGaussians - 1u : numGaussians);
}
void RenderDataStoreDefaultGS::GaussianSortCpuSync(GsRenderData* renderData, const BASE_NS::Math::Mat4X4 viewMatrix,
    const BASE_NS::Math::Mat4X4 projMatrix, const BASE_NS::Math::IVec2 viewport)
{
    sortCache_.Reset();

    IThreadPool* threadPool = threadPool_.get();

    ExecuteGaussianSortCpuTask(
        renderData, viewMatrix, projMatrix, viewport, threadPool, GaussianSortCpuTaskExecuteMode::Sync);
}
void RenderDataStoreDefaultGS::GaussianSortCpu(
    GsRenderData* renderData, const Math::Mat4X4 viewMatrix, const Math::Mat4X4 projMatrix, const Math::IVec2 viewport)
{
    sortCache_.Reset();

    IThreadPool* threadPool = threadPool_.get();

    sortThread_ = std::thread([this, viewMatrix, projMatrix, viewport, renderData, threadPool]() {
        CORE_CPU_PERF_SCOPE("LUMEGS", "RenderDataStoreDefaultGS::GaussianSortCpu", "", 0xFF00FF);

        const auto lg = GetDataLockGuard();

        ExecuteGaussianSortCpuTask(renderData, viewMatrix, projMatrix, viewport, threadPool);
    });
}

void RenderDataStoreDefaultGS::CloneGsRenderData(const GsRenderData* from, GsRenderData* to)
{
    if (!from || !to) {
        CORE_LOG_E("RenderDataStoreDefaultGS::CloneGsRenderData: invalid data pointers provided!");
        return;
    }

    if (primitives_.empty()) {
        *to = *from;
        return;
    }

    if (!dsStaging_) {
        const IRenderDataStoreManager& dsManager = context_.GetRenderDataStoreManager();
        dsStaging_ = static_cast<IRenderDataStoreDefaultStaging*>(
            dsManager.GetRenderDataStore(RENDER_DATA_STORE_DEFAULT_STAGING).get());
    }

    if (!dsStaging_) {
        CORE_LOG_E("RenderDataStoreDefaultGS: Failed to get staging data store!");
        return;
    }

#ifndef NDEBUG
    CORE_LOG_D("Cloning GsRenderData for safe access");
#endif

    // this takes a deep copy of simple values and the CPU-sort vectors,
    // shallow copy of render handles
    *to = *from;

    // the render targets can remain shallow copies,
    // they are not recreated on every load

    // object loading will only touch gsSplatData and gsHarmonicsDataRef, copy those

    const auto deepCopyBuffer = [&](const RENDER_NS::RenderHandleReference src, RENDER_NS::RenderHandleReference& dst,
                                    const uint32_t byteOffset, const uint32_t byteSize) {
        // create new render handles if the same
        if (src.GetHandle() == dst.GetHandle()) {
            const GpuBufferDesc desc = gpuResourceMgr_.GetBufferDescriptor(src);

            dst = gpuResourceMgr_.Create(desc);
        }

        // copy
        const BufferCopy bufferCopy {
            byteOffset, // srcOffset
            byteOffset, // dstOffset
            byteSize,   // size
        };

        dsStaging_->CopyBufferToBuffer(
            src, dst, bufferCopy, IRenderDataStoreDefaultStaging::ResourceCopyInfo::BEGIN_FRAME);
    };

    // copy gsSplatData
    if (RenderHandleUtil::IsValid(from->gsSplatData.GetHandle())) {
        deepCopyBuffer(from->gsSplatData, to->gsSplatData, 0, from->gsDataCopyRefByteSize);
    }

    // copy gsHarmonicsDataRef
    if (RenderHandleUtil::IsValid(from->gsHarmonicsDataRef.GetHandle())) {
        deepCopyBuffer(from->gsHarmonicsDataRef, to->gsHarmonicsDataRef, 0, from->gsHarmonicsDataRefByteSize);
    }
}

bool RenderDataStoreDefaultGS::SortGaussians(const BASE_NS::Math::Mat4X4 viewMatrix,
    const BASE_NS::Math::Mat4X4 projMatrix, const Math::IVec2 viewport, bool& indexPingPong)
{
    auto renderData = GetGsRenderData();
    if (!renderData || renderData->isTiled || renderData->gaussianPositions.empty()) {
        return false;
    }

    const auto lg = GetDataLockGuard();

    if (!sortingEnabled_.load()) {
        return false;
    }

    renderData = GetGsRenderData();
    if (!renderData || renderData->isTiled || renderData->gaussianPositions.empty()) {
        return false;
    }

    if (sortIsDone_->load()) {
        // sorted index buffer copy
        CORE_CPU_PERF_SCOPE("LUMEGS", "RenderDataStoreDefaultGS::SortGaussians::CopyIndices", "", 0xFF00FF);

        if (renderData) {
            auto& indexBufferReference = indexPingPong ? renderData->gsIndicesRef : renderData->gsIndicesRef2;

            if (RenderHandleUtil::IsValid(indexBufferReference.GetHandle())) {
                void* dst = gpuResourceMgr_.MapBufferMemory(indexBufferReference);
                if (!dst) {
                    CORE_LOG_E("Could not map index buffer memory!");
                    sortIsDone_->store(false);
                    return false;
                }

                const uint32_t numGsEffective = activeNumGS_->load();

                if (numGsEffective <= renderData->gaussianIndices.size() && numGsEffective > 0) {
                    BASE_NS::CloneData(dst,
                                       sizeof(uint32_t) * numGsEffective,
                                       reinterpret_cast<void*>(renderData->gaussianIndices.data()),
                                       sizeof(uint32_t) * numGsEffective);
                }
                gpuResourceMgr_.UnmapBuffer(indexBufferReference);
            }

            indexPingPong = !indexPingPong;
        }

        sortIsDone_->store(false);

        return true;
    }

    if (!needsSort_.load()) {
        return false;
    }

    if (!sortingEnabled_.load()) {
        return false;
    }

    bool expected = false;
    if (sortInProgress_->compare_exchange_strong(expected, true)) {
        if (sortThread_.joinable()) {
            sortThread_.join();
        }

        if (!sortingEnabled_.load()) {
            sortInProgress_->store(false);
            return false;
        }

        GaussianSortCpu(renderData, viewMatrix, projMatrix, viewport);
    }

    return false;
}

void RenderDataStoreDefaultGS::SetNeedsSort()
{
    this->needsSort_.store(true);
}

void RenderDataStoreDefaultGS::SetResourcesAsLoaded()
{
    GetGsRenderData()->areResourcesLoaded = true;
}

GsTilePool* RenderDataStoreDefaultGS::GetTilePool()
{
    GsRenderData* rd = GetGsRenderData();
    return rd ? rd->tilePool.get() : nullptr;
}

void RenderDataStoreDefaultGS::SetTilePoolConfig(uint32_t maxTiles, uint32_t splatsPerTile)
{
    GsRenderData* rd = GetGsRenderData();
    if (!rd) {
        return;
    }
    if (!rd->tilePool) {
        rd->tilePool = BASE_NS::make_shared<GsTilePool>();
    }

    GsTilePool& pool = *rd->tilePool;
    pool.maxTiles = Math::max(maxTiles, 1u);
    pool.splatsPerTile = Math::max(splatsPerTile, 1u);
    const uint64_t legacyCapacity = static_cast<uint64_t>(pool.maxTiles) * pool.splatsPerTile;
    pool.splatCapacity = Math::max(pool.splatCapacity, legacyCapacity > std::numeric_limits<uint32_t>::max()
                                                           ? std::numeric_limits<uint32_t>::max()
                                                           : static_cast<uint32_t>(legacyCapacity));
    pool.sortCapacity = Math::max(pool.sortCapacity, pool.splatCapacity);
    pool.resourceVersion++;

    pool.freeSlots.resize(pool.maxTiles);
    for (uint32_t ii = 0; ii < pool.maxTiles; ii++) {
        pool.freeSlots[ii] = static_cast<int32_t>(ii);
    }
}

void RenderDataStoreDefaultGS::SubmitTileStreamingRequest(
    const CORE_NS::Entity& entity, uint32_t tileIndex, bool makeResident)
{
    const auto lg = GetDataLockGuard();

    GsRenderData* rd = GetGsRenderData();
    if (!rd || !rd->tilePool) {
        return;
    }

    const int primIdx = GetGsPrimitiveIndex(entity);
    if (primIdx < 0 || static_cast<size_t>(primIdx) >= primitives_.size()) {
        return;
    }
    GsPrimitive& prim = primitives_[static_cast<size_t>(primIdx)];
    if (tileIndex >= prim.tileGrid.tiles.size()) {
        return;
    }

    GsTileMetadata& tile = prim.tileGrid.tiles[tileIndex];
    GsTilePool& pool = *rd->tilePool;

    if (makeResident) {
        if (tile.state == GsTileState::UNLOADED || tile.state == GsTileState::EVICTING) {
            if (!pool.freeSlots.empty()) {
                tile.gpuPoolSlot = pool.freeSlots.back();
                pool.freeSlots.pop_back();
                tile.gpuPoolOffset = static_cast<uint32_t>(tile.gpuPoolSlot) * pool.splatsPerTile;
                tile.gpuPoolCapacity = pool.splatsPerTile;
                tile.state = GsTileState::RESIDENT;
                tile.currentRadixPass = 0u;
                tile.sortOutputValid = false;
                pool.resourceVersion++;
            }
        }
    } else {
        if (tile.state == GsTileState::RESIDENT || tile.state == GsTileState::STREAMING) {
            if (tile.gpuPoolSlot >= 0) {
                pool.freeSlots.push_back(tile.gpuPoolSlot);
                tile.gpuPoolSlot = -1;
                tile.gpuPoolOffset = 0u;
                tile.gpuPoolCapacity = 0u;
            }

            tile.state = GsTileState::UNLOADED;
            tile.currentRadixPass = 0u;
            tile.sortOutputValid = false;
            pool.resourceVersion++;
        }
    }
}

void RenderDataStoreDefaultGS::CopyIndexBufferToGpu(RENDER_NS::RenderHandleReference indexBufferReference)
{
    if (!RENDER_NS::RenderHandleUtil::IsValid(indexBufferReference.GetHandle())) {
        CORE_LOG_E("RenderDataStoreDefaultGS:CopyIndexBufferToGpu indexBufferReference is invalid");
        return;
    }
    auto renderData = GetGsRenderData();
    if (!renderData) {
        CORE_LOG_E("RenderDataStoreDefaultGS:CopyIndexBufferToGpu renderData is null");
        return;
    }
    void* dst = gpuResourceMgr_.MapBufferMemory(indexBufferReference);
    if (!dst) {
        CORE_LOG_E("Could not map index buffer memory!");
        sortIsDone_->store(false);
        return;
    }
    if (renderData->gaussianIndices.data() == nullptr) {
        CORE_LOG_E("RenderDataStoreDefaultGS:CopyIndexBufferToGpu gaussianIndices is invalid");
        gpuResourceMgr_.UnmapBuffer(indexBufferReference);
        return;
    }
    const uint32_t numGsEffective = activeNumGS_->load();
    const size_t writeByteSize = sizeof(uint32_t) * numGsEffective;

    const auto bufDesc = gpuResourceMgr_.GetBufferDescriptor(indexBufferReference);
    if (writeByteSize > bufDesc.byteSize) {
        CORE_LOG_E("CopyIndexBufferToGpu: write size(%d) exceeds buffer size(%d), skip copy",
                   int(writeByteSize), int(bufDesc.byteSize));
        gpuResourceMgr_.UnmapBuffer(indexBufferReference);
        return;
    }

    if (numGsEffective <= renderData->gaussianIndices.size() && numGsEffective > 0) {
        BASE_NS::CloneData(dst, writeByteSize,
            reinterpret_cast<void*>(renderData->gaussianIndices.data()), writeByteSize);
    }
    gpuResourceMgr_.UnmapBuffer(indexBufferReference);
}

bool RenderDataStoreDefaultGS::SortGaussiansMultiview(const BASE_NS::Math::Mat4X4 viewMatrix,
                                                      const BASE_NS::Math::Mat4X4 projMatrix,
                                                      const BASE_NS::Math::IVec2 viewport,
                                                      RENDER_NS::RenderHandleReference perViewIndexBuffer)
{
    const auto lg = GetDataLockGuard();
    auto renderData = GetGsRenderData();
    if (!renderData || renderData->isTiled || renderData->gaussianPositions.empty()) {
        return false;
    }

    if (!sortingEnabled_.load()) {
        return false;
    }

    if (sortThread_.joinable()) {
        sortThread_.join();
    }
    GaussianSortCpuSync(renderData, viewMatrix, projMatrix, viewport);
    CopyIndexBufferToGpu(perViewIndexBuffer);
    return true;
}

} // namespace GS