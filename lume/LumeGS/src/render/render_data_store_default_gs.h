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

#ifndef RENDER_DATA_STORE_DEFAULT_GS_H
#define RENDER_DATA_STORE_DEFAULT_GS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <base/containers/array_view.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/util/uid.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/namespace.h>
#include <core/threading/intf_thread_pool.h>
#include <render/datastore/intf_render_data_store.h>
#include <render/datastore/intf_render_data_store_default_staging.h>
#include <render/intf_render_context.h>
#include <render/resource_handle.h>

#include "plugin_gs/render/intf_render_data_store_default_gs.h"

namespace RENDER_NS {
class IRenderContext;
class IGpuResourceManager;
class IShaderManager;
class IRenderDataStoreManager;

} // namespace RENDER_NS

namespace {
#include "app/shaders/common/gs_common.h"
}

namespace GS {

constexpr float SH_C0 { 0.28209479177387814f };

// this is how the GS data is represented in the binary .ply file
struct PlyVertexDataFullHarmonics {
    float position[3];
    float normal[3];
    float shs[48]; // spherical harmonics
    float opacity;
    float scale[3];
    float rotation[4];
};

struct PlyVertexDataRestrictedHarmonics {
    float position[3];
    float normal[3];
    float shs[3]; // spherical harmonics
    float opacity;
    float scale[3];
    float rotation[4];
};

struct PlyVertexDataRestrictedHarmonicsMinimal {
    float position[3];
    float shs[3]; // spherical harmonics
    float opacity;
    float scale[3];
    float rotation[4];
};

// PLY layout: position/scale/shs/opacity/rotation (no normal)
struct PlyVertexDataRestrictedHarmonicsMinimalV2 {
    float position[3];
    float scale[3];
    float shs[3]; // spherical harmonics
    float opacity;
    float rotation[4];
};

// PLY layout: position/rotation/scale/opacity/shs (no normal, 14 floats)
struct PlyVertexDataRestrictedHarmonicsMinimalV3 {
    float position[3];
    float rotation[4];
    float scale[3];
    float opacity;
    float shs[3]; // spherical harmonics
};

enum class PlyFormatType {
    Unknown = 0,
    FullHarmonics,                 // position/normal/shs[48]/opacity/scale/rotation
    RestrictedHarmonics,           // position/normal/shs[3]/opacity/scale/rotation
    RestrictedHarmonicsMinimal,    // position/shs[3]/opacity/scale/rotation
    RestrictedHarmonicsMinimalV2,  // position/scale/shs[3]/opacity/rotation
    RestrictedHarmonicsMinimalV3,  // position/rotation/scale/opacity/shs[3]
    Packed,                        // compressed
};

struct GsPackedData {
    uint32_t position;
    uint32_t rotation;
    uint32_t scale;
    uint32_t color;
};

struct GsChunkData {
    float minX;
    float minY;
    float minZ;
    float maxX;
    float maxY;
    float maxZ;

    float minScaleX;
    float minScaleY;
    float minScaleZ;
    float maxScaleX;
    float maxScaleY;
    float maxScaleZ;
};

struct PlyProperty {
    BASE_NS::string type;
    BASE_NS::string name;
};

struct PlyHeader {
    BASE_NS::string format;

    size_t numVertices { 0 };
    size_t numFaces { 0 };
    size_t numChunks { 0 };
    size_t numShCodebook { 0 };
    size_t numSkyBox { 0 };
    size_t numFeatures { 0 };

    BASE_NS::vector<PlyProperty> vertexProperties;
    BASE_NS::vector<PlyProperty> chunkProperties;
    BASE_NS::vector<PlyProperty> faceProperties;
    BASE_NS::vector<PlyProperty> shIndicesProperties;
    BASE_NS::vector<PlyProperty> shCodeBookProperties;

    float compress { 0.0f };
    float featureRestZeroPoint { 0.0f };
    float featureRestScale { 0.0f };
    float featureDcMin { -0.5f / SH_C0 };
    float featureDcMax { 0.5f / SH_C0 };
    size_t shDegree { 0 };
};

class UniqueScopedLock {
private:
    std::mutex* mtx_;
    bool owns_;

public:
    explicit UniqueScopedLock(std::mutex& mutex) : mtx_(&mutex), owns_(true)
    {
        mtx_->lock();
    }
    ~UniqueScopedLock()
    {
        if (owns_ && mtx_) {
            mtx_->unlock();
        }
    }
    UniqueScopedLock(const UniqueScopedLock&) = delete;
    UniqueScopedLock& operator=(const UniqueScopedLock&) = delete;

    UniqueScopedLock(UniqueScopedLock&& other) noexcept : mtx_(other.mtx_), owns_(other.owns_)
    {
        other.mtx_ = nullptr;
        other.owns_ = false;
    }

    UniqueScopedLock& operator=(UniqueScopedLock&& other) noexcept
    {
        if (this != &other) {
            // release this
            if (owns_ && mtx_) {
                mtx_->unlock();
            }

            // transfer ownership
            mtx_ = other.mtx_;
            owns_ = other.owns_;
            other.mtx_ = nullptr;
            other.owns_ = false;
        }
        return *this;
    }
};

/**
RenderDataStoreDefaultGS

 - Handles/Managers gs buffers and effects.
*/
class RenderDataStoreDefaultGS final : public IRenderDataStoreDefaultGs {
public:
    RenderDataStoreDefaultGS(RENDER_NS::IRenderContext& engine, const BASE_NS::string_view name);
    ~RenderDataStoreDefaultGS() override;

    void AddGsPrimitive(const GsPrimitive& gsPrimitive) override;
    void RemoveGsPrimitive(const CORE_NS::Entity& entity) override;
    bool RemoveGsPrimitiveIfPresent(const CORE_NS::Entity& entity);

    BASE_NS::vector<GsPrimitive>& GetGsPrimitives() override;
    BASE_NS::array_view<const GsPrimitive> GetGsPrimitives() const override;
    int GetGsPrimitiveIndex(const CORE_NS::Entity entity) const override;

    struct OpacityBackup {
        BASE_NS::vector<float> opacityValues;
    };

    // primitiveIndex -> backup
    BASE_NS::unordered_map<uint32_t, OpacityBackup> opacityBackups_;
    bool ApplyOpacityMask(uint32_t primitiveIndex, const std::vector<int>& mask);
    // Undo transparency changes to the specified primitive
    // Return: Whether the operation was successful (will fail if no backup exists)
    bool UndoOpacityMask(uint32_t primitiveIndex);
    // Check whether the specified primitive has a rollbackable backup
    bool CanUndoOpacityMask(uint32_t primitiveIndex) const;
    // Delete the backup of the specified primitive (call this when the undo is no longer needed)
    void ClearOpacityBackup(uint32_t primitiveIndex);

    BASE_NS::pair<uint32_t, uint32_t> GetPrimitiveRange(uint32_t primitiveIndex) const;

    void DisableFadeForGsModel(const uint32_t gsIdx) override;
    void DisableUBOCopyFlagForGsModel(const uint32_t gsIdx) override;
    void SetDirtyForGsModel(const uint32_t gsIdx, const bool isDirty) override;

    uint32_t GetNumActiveGs() override;

    // IRenderDataStore
    void PreRender() override {}
    // Reset and start indexing from the beginning. i.e. frame boundary reset.
    void PostRender() override;
    void PreRenderBackend() override {}
    void PostRenderBackend() override {}
    void Clear() override;

    uint32_t GetFlags() const override
    {
        return 0;
    }

    BASE_NS::string_view GetTypeName() const override
    {
        return TYPE_NAME;
    }

    BASE_NS::string_view GetName() const override
    {
        return name_;
    }

    const BASE_NS::Uid& GetUid() const override
    {
        return UID;
    }

    void Ref() override;
    void Unref() override;
    int32_t GetRefCount() override;

    void EnableSortFlag() override;
    void DisableSortFlag() override;

    // for plugin / factory interface
    static constexpr char const* TYPE_NAME = "RenderDataStoreDefaultGS";
    static BASE_NS::refcnt_ptr<IRenderDataStore> Create(RENDER_NS::IRenderContext& renderContext, char const* name);

    bool SortGaussians(const BASE_NS::Math::Mat4X4 viewMatrix, const BASE_NS::Math::Mat4X4 projMatrix,
        const BASE_NS::Math::IVec2 viewport, bool& indexPingPong) override;

    void CopyIndexBufferToGpu(RENDER_NS::RenderHandleReference indexBufferReference);

    bool SortGaussiansMultiview(const BASE_NS::Math::Mat4X4 viewMatrix, const BASE_NS::Math::Mat4X4 projMatrix,
        const BASE_NS::Math::IVec2 viewport, RENDER_NS::RenderHandleReference perViewIndexBuffer);

    void SetNeedsSort() override;

    void SetResourcesAsLoaded() override;

    BASE_NS::Math::Vec2 GetDepthRange(const BASE_NS::Math::Mat4X4& viewMatrix);

    BASE_NS::Math::Mat4X4 GetWorldMatrix(const uint32_t gsIdx);

    // returns a valid data storage for rendering
    GsRenderData* GetGsRenderData() override;

    GsTilePool* GetTilePool() override;
    void SetTilePoolConfig(uint32_t maxTiles, uint32_t splatsPerTile) override;
    void SubmitTileStreamingRequest(const CORE_NS::Entity& entity, uint32_t tileIndex, bool makeResident) override;

    // returns a safe data storage for loading, cloned
    GsRenderData* GetSafeLoadingGsRenderData();

    // flips the valid data storage and invalidates the other one
    void FlipValidGsRenderData();
    void ClearRenderData();
    void ResetTiledRenderData();

    GS::UniqueScopedLock GetDataLockGuard();

    BASE_NS::Math::UVec2 GetRenderResolution() const override;
    void SetRenderResolution(const BASE_NS::Math::UVec2 resolution) override;

    uint32_t GetGsScaleFactor() const override;
    void SetGsScaleFactor(const uint32_t scaleFactor) override;
    float GetDRSScale() const override;
    void SetDRSScale(float scale);
    float GetRenderScaleFactor() const override;
    bool GetDynamicRendering() const;
    void SetDynamicRendering(bool isDynamicRendering);
    bool GetSingleFrameGenEnable() const;
    void SetSingleFrameGenEnable(bool enabled);
    void UpdateFrameGenerationState(bool cameraMoved,
        const BASE_NS::Math::Mat4X4& viewMatrix, BASE_NS::Math::UVec2 currFrameRenderResolution);

    // XEG Upscale configuration
    float GetXegUpscaleFactor() const override;
    void SetXegUpscaleFactor(float factor) override;
    // Internal sharpness parameter (not exposed via public API)
    float GetXegUpscaleSharpnessInternal() const;
    void SetXegUpscaleSharpnessInternal(float sharpness);

    // XEG Upscale enabled flag (from configuration)
    bool GetXegUpscaleEnabled() const override;
    void SetXegUpscaleEnabled(bool enabled) override;

    void SetCurrentAnimationTime(const float time);
    float GetCurrentAnimationTime() const;

    bool use24BitGpuSort = false;

    uint32_t GetGpuSortKeyBits()
    {
        return use24BitGpuSort ? GPU_SORT_KEY_BITS_24 : GPU_SORT_KEY_BITS_32;
    }

    uint32_t GetGpuSortByteCount()
    {
        return use24BitGpuSort ? GPU_SORT_BYTES_24 : GPU_SORT_BYTES_32;
    }

    float GetRenderCovarianceStddev() const
    {
        return renderCovarianceStddev_;
    }

    void SetRenderCovarianceStddev(const float stddev)
    {
        renderCovarianceStddev_ = stddev;
    }

    float GetRenderSharpeningCoefficient() const
    {
        return renderSharpeningCoefficient_;
    }

    void SetRenderSharpeningCoefficient(const float sharpeningCoeff)
    {
        renderSharpeningCoefficient_ = sharpeningCoeff;
    }

private:
    void GaussianSortCpu(GsRenderData* renderData, const BASE_NS::Math::Mat4X4 viewMatrix,
        const BASE_NS::Math::Mat4X4 projMatrix, const BASE_NS::Math::IVec2 viewport);

    void GaussianSortCpuSync(GsRenderData* renderData, const BASE_NS::Math::Mat4X4 viewMatrix,
        const BASE_NS::Math::Mat4X4 projMatrix, const BASE_NS::Math::IVec2 viewport);

    enum class GaussianSortCpuTaskExecuteMode { Async = 0, Sync };
    void ExecuteGaussianSortCpuTask(GsRenderData* renderData, const BASE_NS::Math::Mat4X4 viewMatrix,
        const BASE_NS::Math::Mat4X4 projMatrix, const BASE_NS::Math::IVec2 viewport, CORE_NS::IThreadPool* threadPool,
        GaussianSortCpuTaskExecuteMode mode = GaussianSortCpuTaskExecuteMode::Async);

    void CloneGsRenderData(const GsRenderData* from, GsRenderData* to);

    CORE_NS::IFileManager& fileManager_;
    RENDER_NS::IRenderContext& context_;
    BASE_NS::string name_;
    RENDER_NS::IGpuResourceManager& gpuResourceMgr_;

    BASE_NS::vector<GsPrimitive> primitives_;

    std::mutex gsDataMtx_;
    std::shared_ptr<std::atomic_bool> sortIsDone_;
    std::shared_ptr<std::atomic_bool> sortInProgress_;
    std::shared_ptr<std::atomic_uint32_t> activeNumGS_;
    std::atomic_bool needsSort_ { true };

    std::atomic_int32_t refcnt_ { 0 };

    // master flag
    std::atomic_bool sortingEnabled_ { true };

    CORE_NS::IThreadPool::Ptr threadPool_;

    // 2 ping-pong data stores for thread safe object loading
    std::unique_ptr<GsRenderData> gsRenderData0_;
    std::unique_ptr<GsRenderData> gsRenderData1_;
    std::atomic_bool gsRenderDataAccess_ { false }; // false 0, true 1

    RENDER_NS::IRenderDataStoreDefaultStaging* dsStaging_ { nullptr };

    struct GaussianSortCache {
        struct GsSortInfo {
            uint32_t index;
            float sortDepth;
        };

        BASE_NS::vector<GsSortInfo> gsToSort;

        struct ThreadLocalData {
            float minRawDepth = std::numeric_limits<float>::infinity();
            float maxRawDepth = -std::numeric_limits<float>::infinity();
            BASE_NS::vector<GsSortInfo> gsToSort;
            uint32_t distanceCullCount = 0;
        };
        BASE_NS::vector<ThreadLocalData> threadData;

        BASE_NS::vector<CORE_NS::IThreadPool::IResult::Ptr> results;

        // count sort data
        BASE_NS::vector<uint32_t> counts;
        BASE_NS::vector<uint32_t> normalizedDepths;
        BASE_NS::vector<uint32_t> starts;

        size_t countSortBits { 16u };

        void SetSortBits(const size_t numBits)
        {
            countSortBits = numBits;
        }

        uint32_t GetSortMax() const
        {
            return 2u << countSortBits;
        }

        void Initialize(uint32_t maxThreads, uint32_t maxPrimitives)
        {
            const uint32_t gsBitsMax = GetSortMax();

            gsToSort.reserve(maxPrimitives);

            uint32_t detectedThreads = std::thread::hardware_concurrency();
            if (detectedThreads == 0u) {
                // default to 4 threads
                detectedThreads = 4u;
            }
            maxThreads = BASE_NS::Math::min(maxThreads, detectedThreads);
            maxThreads = BASE_NS::Math::max(maxThreads, 1u);
            threadData.resize(maxThreads);
            results.reserve(maxThreads);

            counts.resize(gsBitsMax);
            normalizedDepths.reserve(maxPrimitives);
        }

        void Reset()
        {
            gsToSort.clear();
            results.clear();

            std::fill(counts.begin(), counts.end(), 0);
            normalizedDepths.clear();
        }

        void ResetThreadData()
        {
            for (auto& data : threadData) {
                data.gsToSort.clear();
                data.minRawDepth = std::numeric_limits<float>::infinity();
                data.maxRawDepth = -std::numeric_limits<float>::infinity();
                data.distanceCullCount = 0;
            }
        }
    };

    GaussianSortCache sortCache_;

    std::thread sortThread_;

    // final render resolution without the scaler applied
    BASE_NS::Math::UVec2 renderResolution { 2u, 2u };

    uint32_t gsScaleFactor { 0xFFFFFFFFu };

    bool isDynamicRendering_ { false };
    float DRSScale_ { 1.0f };

    float animationTimeSeconds_ { 0.0f };

    float cullingThreshold_ { 0.000000042375f };
    bool isMaxThresholdFixed_ { false };

    static constexpr float MAX_FRAME_GEN_CAMERA_MOTION_ANGLE = 0.005f;
    static constexpr float MAX_FRAME_GEN_CAMERA_MOTION_DIST_SQR = 0.2f;
    bool isSingleFrameGenEnabled_ { false };
    BASE_NS::Math::Vec3 lastFrameForwardDirection_ { 0.f, 0.f, 0.f };
    BASE_NS::Math::Vec3 lastFramePosition_ { 0.f, 0.f, 0.f };
    BASE_NS::Math::UVec2 preFrameRenderResolution_ { 0u, 0u };

    float renderCovarianceStddev_ { GS_COVARIANCE_STDDEV_DEFAULT };
    float renderSharpeningCoefficient_ { 0.25f };

    float xegUpscaleSharpness_ { 0.5f }; // Internal parameter, not exposed
    bool xegUpscaleEnabled_ { false };   // XEG upscale enabled flag from configuration
    float xegUpscaleFactor_ { 1.0f };    // Default to 1.0f (no upscale)
};
} // namespace GS

#endif // RENDER_DATA_STORE_DEFAULT_GS_H
