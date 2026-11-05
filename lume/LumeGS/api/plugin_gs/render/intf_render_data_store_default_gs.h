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

#ifndef PLUGIN_API_RENDER_DATA_STORE_DEFAULT_GS_H
#define PLUGIN_API_RENDER_DATA_STORE_DEFAULT_GS_H

#include <cstdint>
#include <limits>
#include <vector>

#include <3d/ecs/components/layer_defines.h>
#include <3d/ecs/components/local_matrix_component.h>
#include <base/containers/array_view.h>
#include <base/containers/shared_ptr.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/vector.h>
#include <base/math/matrix.h>
#include <base/math/vector.h>
#include <core/ecs/entity.h>
#include <core/ecs/entity_reference.h>
#include <render/datastore/intf_render_data_store.h>
#include <render/namespace.h>
#include <render/render_data_structures.h>

namespace {
#include "../assets/app/shaders/common/gs_common.h"
} // namespace

namespace RENDER_NS {
class IGpuResourceManager;
class IShaderManager;
} // namespace RENDER_NS

namespace GS {

static constexpr BASE_NS::string_view DEFAULT_GS_RENDER_DATA_STORE_NAME_POST_FIX = "RenderDataStoreDefaultGs";

enum class GsTileState : uint8_t { UNLOADED, STREAMING, RESIDENT, EVICTING };

struct GsTileMetadata {
    BASE_NS::Math::Vec3 aabbMin;
    uint32_t splatCount { 0 };
    BASE_NS::Math::Vec3 aabbMax;
    int32_t gpuPoolSlot { -1 }; // -1 = not resident
    uint32_t gpuPoolOffset { 0 };
    uint32_t gpuPoolCapacity { 0 };
    GsTileState state { GsTileState::UNLOADED };
    float lastAccessDistance { 0.0f };
    uint32_t lastAccessFrame { 0 };
    uint32_t currentRadixPass { 0 };
    bool sortOutputValid { false }; // true after first full 4-pass radix sort completes
};

struct GsTileGrid {
    BASE_NS::Math::Vec3 origin { 0.0f, 0.0f, 0.0f };
    float cellSize { 0.0f };
    BASE_NS::Math::UVec3 dims { 0u, 0u, 0u };
    BASE_NS::vector<GsTileMetadata> tiles; // flattened [x + dims.x*(y + dims.y*z)]
};

struct GsTilePool {
    RENDER_NS::RenderHandleReference splatPoolBuffer;
    RENDER_NS::RenderHandleReference shPoolBuffer;
    RENDER_NS::RenderHandleReference sortIndexPool0;
    RENDER_NS::RenderHandleReference sortIndexPool1;
    RENDER_NS::RenderHandleReference tileMetaBuffer;
    RENDER_NS::RenderHandleReference tileIndirectDrawBuffer;
    RENDER_NS::RenderHandleReference tileDataStorageCacheHandle;
    RENDER_NS::RenderHandleReference tileViewZStoreCacheHandle;
    RENDER_NS::RenderHandleReference sortCountPoolBuffer;

    uint32_t sortCountSlotSize { 0U };
    uint32_t maxTiles { 16U };
    uint32_t splatsPerTile { 1U };
    uint32_t splatCapacity { 0U };
    uint32_t sortCapacity { 0U };
    uint32_t allocatedSplatCapacity { 0U };
    uint32_t allocatedSortCapacity { 0U };
    uint32_t allocatedTileCapacity { 0U };
    uint32_t resourceVersion { 0U };

    BASE_NS::vector<int32_t> freeSlots;
};

struct GsHarmonicsData {
    // Ascending order spherical harmonic coefficients.
    // Packed as 16bit floats (16*3 / 2 = 24)
    uint32_t shCoefficients[24u];
};

struct GsAnimationFadeProperties {
    BASE_NS::Math::Vec3 fadeStartPosition;
    float fadeSpeed { 1.0f };
    float fadeWaitOffset { 1.0f };
    float timeToFadeFullPoints { 4.0f };

    float fadeDistanceOffset { 0.0f };

    bool useFadeAnimation { false };
    bool useFadingCopyFlag { true };
};

struct GsCropBoxProperties {
    BASE_NS::Math::Vec3 aabbMin;
    BASE_NS::Math::Vec3 aabbMax;
    bool useCropBox { false };

    bool cropBoxChanged { true };
};

struct GsPrimitive {
    CORE_NS::Entity splatEntity;

    // start index offset of this object
    uint32_t startOffset { 0 };
    // number of total elements for this object
    uint32_t gsBufferSize { 0 };

    // start index offset of the object's shs
    uint32_t startShOffset { 0 };

    // LOD0 num gaussians
    uint32_t numGs { 0 };
    // LOD1 num gaussians
    uint32_t numGs1 { 0 };

    BASE_NS::Math::Mat4X4 worldMatrix { 1.0f };
    BASE_NS::Math::Mat4X4 prevUpdateWorldMatrix { 1.0f };
    bool active { true };
    bool isDirty { true };
    BASE_NS::string gsPath { "" };

    BASE_NS::Math::Vec3 aabbMin { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max() };
    BASE_NS::Math::Vec3 aabbMax { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest() };

    GsAnimationFadeProperties animationFadeProperties;
    GsCropBoxProperties cropBoxProperties;

    // Per-object CPU side data

    // gsSizeData is only computed for LOD1
    BASE_NS::vector<float> gsSizeData;
    float gsMedianSize { 0.0f };
    float lodBias { 0.5f };

    bool isValid { false };
    bool enabled { true };
    bool prevEnabled { true };
    bool computeLods { true };

    uint32_t shDegree { 3u };

    BASE_NS::vector<BASE_NS::vector<uint32_t>> gsLodLookup; // gsLodLookup[LOD1Index] = LOD0Indices

    // codec related data
    uint32_t codecVersion { 0 };
    uint32_t compressedShWidth { 0 };
    uint32_t compressedShHeight { 0 };
    uint32_t compressedShAstcBlock { 0 };
    uint32_t attribute2DSingleWidth { 0 };
    float shRestMins { -1.0f };
    float shRestMaxs { 1.0f };
    float shDcMins { -1.0f };
    float shDcMaxs { 1.0f };
    uint32_t textIdOffset { GS_MAX_NUM_GS_TEXTURES };
    uint32_t attribute2DTexNum { 0 };

    uint32_t attribute2DSingleHeight { 0 };
    float shRestMins_R[16] {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
     -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    float shRestMaxs_R[16] { 1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,
      1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0};
    float shRestMins_G[16] {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
     -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    float shRestMaxs_G[16] { 1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,
      1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0};
    float shRestMins_B[16] {-1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0,
     -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0, -1.0};
    float shRestMaxs_B[16] { 1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,
      1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0,  1.0};
    float impMin {0.0f};
    float impMax {1.0f};

    // on e.g. most Nvidia GPUs, we have no support for BASE_FORMAT_ASTC_4x4_UNORM_BLOCK.
    // On these platforms we can still load the new packed GLB format, but we cannot compute
    // spherical harmonics correctly.
    bool platformsSupportsBlockFormat { true };

    uint64_t layerMask { CORE3D_NS::LayerConstants::DEFAULT_LAYER_MASK };

    bool RequireSH() const
    {
        return codecVersion < 1 && shDegree > 0;
    }

    // Tiling support
    GsTileGrid tileGrid;
};

// per-object data storage for loading the GS objects
struct GsObjectLoadData {
    struct GsSplatData {
        BASE_NS::Math::Vec4 positionOpacity;
        BASE_NS::Math::Vec4 covAScale;
        BASE_NS::Math::Vec4 covBColorPacked;
        BASE_NS::Math::UVec4 astcOffsetColor { 0u, 0u, 0u, 0u };
    };

    // to use the packed data, ensure the following are DISABLED for the entire lifecycle:
    // 3rd degree sperical harmonics; gpu sort; custom shader
    struct GsSplatDataPacked {
        BASE_NS::Math::UVec4 covColorPacked;
        BASE_NS::Math::UVec2 positionOpacityPacked;
        BASE_NS::Math::UVec2 astcOffsetColor { 0u, 0u };
    };

    struct GsSplatDataTilePacked {
        BASE_NS::Math::UVec4 covColorPacked;
        BASE_NS::Math::Vec4 positionOpacityPacked;
    };

    // these will be thrown away after load
    BASE_NS::vector<GsHarmonicsData> sphericalHarmonics;
    BASE_NS::vector<GsSplatData> gsVerticesData;
    BASE_NS::vector<GsSplatDataPacked> gsVerticesDataPacked;
    BASE_NS::vector<GsSplatDataTilePacked> gsDataPacked;

    // these will be moved to GsRenderData
    BASE_NS::vector<BASE_NS::Math::Vec4> gaussianPositions;
    BASE_NS::vector<uint32_t> gaussianIndices;
    BASE_NS::vector<float> splatExtents;
    BASE_NS::vector<float> gaussianMaxScales;
};

// per debug line
struct GsDebugLinesVertexStruct {
    BASE_NS::Math::Vec4 position;
};

struct GsRenderData {
    // Render targets
    RENDER_NS::RenderHandleReference depthStoreCacheHandle; // used for e.g. the crop-box and hybrid rendering

    RENDER_NS::RenderHandleReference sampler;

    RENDER_NS::RenderHandleReference gaussianRenderTarget0;
    RENDER_NS::RenderHandleReference gaussianRenderTarget1;
    RENDER_NS::RenderHandleReference gaussianRenderTarget2;
    RENDER_NS::RenderHandleReference gaussianRenderTarget3;

    RENDER_NS::RenderHandleReference gaussianRenderTargetBB;

    RENDER_NS::RenderHandleReference gaussianRenderTargetDepth1;

    RENDER_NS::RenderHandleReference gaussianRenderTargetStatusUbo;

    // Object data
    RENDER_NS::RenderHandleReference dataStorageCacheHandle; // frame pre-projection data
    RENDER_NS::RenderHandleReference viewZStoreCacheHandle;  // Z coordinate of gaussians in view space
    RENDER_NS::RenderHandleReference gsIndicesRef;           // index ping-pong
    RENDER_NS::RenderHandleReference gsIndicesRef2;          // index ping-pong
    RENDER_NS::RenderHandleReference gsSplatData;            // splat data input for animations, "immutable"
    RENDER_NS::RenderHandleReference gsSplatDataRender;      // splat data input for rendering, output form animations
    RENDER_NS::RenderHandleReference gsHarmonicsDataRef;     // spherical harmonics (old codec)

    uint32_t gsDataCopyRefByteSize { 0 };
    uint32_t gsHarmonicsDataRefByteSize { 0 };

    // GPU sort data
    struct GpuSortBuffers {
        RENDER_NS::RenderHandleReference sortKeyIndexBuffer0;
        RENDER_NS::RenderHandleReference sortKeyIndexBuffer1;
        RENDER_NS::RenderHandleReference sortCountBuffer;
    } gpuSortBuffers;

    // CPU-side data. Any reads and writes to these must be synchronized using
    // the lock acquired from RenderDataStoreDefaultGS::GetDataLockGuard(). Failing
    // to do so can result in a data race during CPU sorting!
    BASE_NS::vector<BASE_NS::Math::Vec4> gaussianPositions; // CPU-sort data, .w contains opacity
    BASE_NS::vector<uint32_t> gaussianIndices;              // CPU-sort data
    BASE_NS::vector<float> splatExtents;                    // required by raycast acceleration volume

    // Render parameters

    bool areResourcesLoaded { false };
    bool convertGsColorSrgb { false };
    bool gsEnableDepthPass { false };
    bool use16bitSurfaceTarget { false };
    bool anyComputeLods { false };

    RENDER_NS::RenderHandleReference customAnimationShader;
    BASE_NS::vector<RENDER_NS::RenderHandleReference> customAnimationResources;
    bool useAnimations { false };
    bool isAnimationPaused { false };
    bool enableDynamicResolution { false };
    float dynamicResolutionScale { 1.0f };
    float animationSeekTimestamp { 0.0f };

    bool isAnimationRunningPrevious { false };
    bool isAnimationRunningChanged { false };

    bool useGpuSort { false };
    uint32_t numFramesToComputeSh { 6u };

    float frustumScale { -1.0f };
    float adhocCullingScale { 1.2f };
    bool enableGsPicking { true };
    bool enableCustomAnimation { true };
    bool enablePackedData { false };
    bool enableTilePackedData {false};
    bool enableCpuScheduling { false };

    bool enableScoreCulling { false };
    BASE_NS::vector<float> gaussianMaxScales;
    bool enableFrameGeneration { false };

    // XEngine upscale configuration
    bool enableXegUpscale { false };
    float xegUpscaleFactor { 1.0f };

    CORE_NS::EntityReference gsMaterial;
    CORE_NS::EntityReference gsMesh;
    bool isMaterialValid { false };
    uint32_t enableGsOpt { 0 };

    bool CanUsePackedData() const
    {
        return enablePackedData && !CanUseGpuSort() && !enableCustomAnimation;
    }

    bool CanUseTilePackedData() const
    {
        return enableTilePackedData;
    }

    bool CanUseGpuSort() const
    {
        // Cannot use GPU sort with the LOD system
        if (useGpuSort) {
            return !anyComputeLods;
        }
        return false;
    }

    // the number of all GS elements, LOD0 + LOD1
    uint32_t GetTotalGsCount() const
    {
        return (uint32_t)gaussianPositions.size();
    }

    // codec related data
    BASE_NS::vector<RENDER_NS::RenderHandleReference> gsShASTCDataRefs;
    BASE_NS::vector<BASE_NS::Math::UVec2> astcSizes;

    // Tiling support (shared_ptr so CloneGsRenderData can shallow-copy the pool)
    BASE_NS::shared_ptr<GsTilePool> tilePool;
    bool isTiled { false };
};

class IRenderDataStoreDefaultGs : public RENDER_NS::IRenderDataStore {
public:
    static constexpr BASE_NS::Uid UID { "a21a1d5f-2bfa-6f46-892f-33eb0347a654" };

    virtual void AddGsPrimitive(const GsPrimitive& gsPrimitive) = 0;
    virtual void RemoveGsPrimitive(const CORE_NS::Entity& entity) = 0;
    virtual BASE_NS::vector<GsPrimitive>& GetGsPrimitives() = 0;
    virtual BASE_NS::array_view<const GsPrimitive> GetGsPrimitives() const = 0;
    virtual int GetGsPrimitiveIndex(const CORE_NS::Entity entity) const = 0;

    /** Dispatches CPU sort. Returns true if sort is complete and false if not. */
    virtual bool SortGaussians(const BASE_NS::Math::Mat4X4 viewMatrix, const BASE_NS::Math::Mat4X4 projMatrix,
        const BASE_NS::Math::IVec2 viewport, bool& indexPingPong) = 0;

    virtual void SetNeedsSort() = 0;
    virtual void SetResourcesAsLoaded() = 0;

    // CPU sort master flags to prevent sorting during object load
    virtual void DisableSortFlag() = 0;
    virtual void EnableSortFlag() = 0;

    virtual BASE_NS::Math::UVec2 GetRenderResolution() const = 0;
    virtual void SetRenderResolution(const BASE_NS::Math::UVec2 resolution) = 0;

    virtual uint32_t GetGsScaleFactor() const = 0;
    virtual void SetGsScaleFactor(const uint32_t gsScaleFactor) = 0;
    virtual float GetRenderScaleFactor() const = 0;
    virtual float GetDRSScale() const = 0;

    virtual void DisableFadeForGsModel(const uint32_t gsIdx) = 0;
    virtual void DisableUBOCopyFlagForGsModel(const uint32_t gsIdx) = 0;
    virtual void SetDirtyForGsModel(const uint32_t gsIdx, const bool isDirty) = 0;

    virtual uint32_t GetNumActiveGs() = 0;

    virtual GsRenderData* GetGsRenderData() = 0;

    // Tiling support
    virtual GsTilePool* GetTilePool() = 0;
    virtual void SetTilePoolConfig(uint32_t maxTiles, uint32_t splatsPerTile) = 0;
    virtual void SubmitTileStreamingRequest(const CORE_NS::Entity& entity, uint32_t tileIndex, bool makeResident) = 0;

    // XEG Upscale enabled flag (from configuration)
    virtual bool GetXegUpscaleEnabled() const = 0;
    virtual void SetXegUpscaleEnabled(bool enabled) = 0;

    // XEG Upscale configuration
    virtual float GetXegUpscaleFactor() const = 0;
    virtual void SetXegUpscaleFactor(float factor) = 0;
};
} // namespace GS

#endif // PLUGIN_API_RENDER_DATA_STORE_DEFAULT_GS_H
