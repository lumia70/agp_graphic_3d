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

#ifndef RENDER_NODE_GS_RENDER_H
#define RENDER_NODE_GS_RENDER_H

#include <array>
#include <chrono>
#include <optional>



#include <plugin_gs/render/intf_render_data_store_gs_edit.h>

#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <3d/implementation_uids.h>
#include <3d/namespace.h>
#include <3d/render/default_material_constants.h>
#include <3d/render/intf_render_data_store_default_camera.h>
#include <3d/render/intf_render_data_store_default_scene.h>
#include <3d/render/intf_render_node_scene_util.h>
#include <base/containers/array_view.h>
#include <base/containers/refcnt_ptr.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/matrix_util.h>
#include <base/math/vector.h>
#include <base/util/hash.h>
#include <base/util/uid.h>
#include <core/log.h>
#include <core/namespace.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/datastore/intf_render_data_store_pod.h>
#include <render/datastore/render_data_store_render_pods.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/device/pipeline_layout_desc.h>
#include <render/gsedit/gs_editor.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_node_context_pso_manager.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/nodecontext/intf_render_node_parser_util.h>
#include <render/nodecontext/intf_render_node_util.h>
#include <render/render_data_store_default_gs.h>
#include <render/render_data_structures.h>
#include <render/resource_handle.h>

namespace {
#include "app/shaders/common/gs_struct_common.h"
} // namespace

#include <render/nodecontext/intf_render_backend_node.h>

#include "app/shaders/common/gs_common.h"

namespace RENDER_NS {
class IRenderCommandList;
class IRenderNodeContextManager;
class IPipelineDescriptorSetBinder;
} // namespace RENDER_NS

namespace GS {
class IXEngineGsBackend;

#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
class RenderNodeGsRender final : public RENDER_NS::IRenderBackendNode {
public:
    RenderNodeGsRender() = default;
    ~RenderNodeGsRender() override;
    void ExecuteBackendFrame(const RENDER_NS::ILowLevelDevice& device,
        const RENDER_NS::RenderBackendRecordingState& recordingState) override;
#else
class RenderNodeGsRender final : public RENDER_NS::IRenderNode {
public:
    RenderNodeGsRender() = default;
    ~RenderNodeGsRender() override = default;
#endif

    void InitNode(RENDER_NS::IRenderNodeContextManager& renderNodeContextMgr) override;
    void PreExecuteFrame() override;
    void ExecuteFrame(RENDER_NS::IRenderCommandList& cmdList) override;

    void ExecuteGpuSort(RENDER_NS::IRenderCommandList& cmdList, const BASE_NS::Math::Mat4X4 viewMatrix);
   

    void BuildMergedWorkgroupInfos();
    uint64_t ComputeMergedTileSetHash(const uint32_t poolVersion) const;
    void ProcessPrimitiveTiles(const GsPrimitive& prim, const uint32_t primIdx, const BASE_NS::Math::Mat4X4& viewMatrix,
        const BASE_NS::Math::Mat4X4& projMatrix, const GsTilePool* pool, BASE_NS::Math::Vec2& globalDepthRange);
    void BuildMergedTileBatch(const BASE_NS::Math::Mat4X4& viewMatrix);
    bool EnsureTilePreSortWgInfoCapacity(uint32_t capacity);

    bool ExecuteTilePreSortPass(
        RENDER_NS::IRenderCommandList& cmdList, const GsTilePool& pool, const uint32_t sortByteSize);
    void ExecuteTileRadixPass(RENDER_NS::IRenderCommandList& cmdList, const GsTilePool& pool,
        const uint32_t sortByteSize, const uint32_t totalGs, const uint32_t numWorkGroups, const uint32_t bytePos);
    void ResetMergedTiledSortState();
    bool ExecuteMergedTiledGpuSort(RENDER_NS::IRenderCommandList& cmdList, const GsTilePool& pool);
    uint32_t GetMergedTiledPrecomputeChunkCount(const uint32_t totalGs) const;
    void RestartMergedTiledPrecompute(const bool forceFull);
    bool ExecuteMergedTiledPrecompute(RENDER_NS::IRenderCommandList& cmdList, const GsTilePool& pool);
    void ExecuteSortSizeCalcPass(RENDER_NS::IRenderCommandList& cmdList, const GsTilePool& pool);

    bool SetGsColorAttachment(RENDER_NS::RenderPass& activeRenderPass);
    void PrepareGsViewportAndScissor(RENDER_NS::RenderPass& activeRenderPass, RENDER_NS::ViewportDesc& viewportDesc,
        RENDER_NS::ScissorDesc& scissorDesc) const;
    void RecordGaussianDraw(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData, const uint32_t numGs);
    void BindGsRenderSet0(RENDER_NS::IRenderCommandList& cmdList, const bool useDepth);

    void ExecuteGaussianRenderPass(RENDER_NS::IRenderCommandList& cmdList, const BASE_NS::Math::Mat4X4 viewMatrix,
        GS::GsRenderData* renderData, const bool isCpuSortDone);
    
    void ExecuteGaussianDebugDrawRenderPass(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData);

    void ExecuteComputeSphericalHarmonics(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData,
        const bool computeAll);

    void ExecutePrecomputePrePasses(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData,
        const BASE_NS::Math::Mat4X4& viewMatrix);
    void BindPrecomputeDescriptorSet(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData);
    void PrecomputeGsFrameData(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData,
        const BASE_NS::Math::Mat4X4& viewMatrix);
    void ExecuteGpuSortPopulate(RENDER_NS::IRenderCommandList& cmdList);

    void ExecuteAnimationPass(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData);

    void ExecutePrefilterDepthPass(RENDER_NS::IRenderCommandList& cmdList);
    void ExecuteWorldSpaceNormalsPass(RENDER_NS::IRenderCommandList& cmdList);
    void ExecuteEdgesPass(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData);
    void ExecuteDilateEdgesPass(RENDER_NS::IRenderCommandList& cmdList);
    void ExecuteMinMaxEdgesPass(RENDER_NS::IRenderCommandList& cmdList);

    void FindDenseObject(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData);
    void GetDenseObjectResults(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData);

    ExecuteFlags GetExecuteFlags() const override
    {
        return 0U;
    }

   

    void CreateDescriptorSet(RENDER_NS::INodeContextDescriptorSetManager& descriptorSetMgr,
        RENDER_NS::PipelineLayout& pl, const uint32_t bufferSetIndex, RENDER_NS::IDescriptorSetBinder::Ptr& setBinder);
    RENDER_NS::IDescriptorSetBinder::Ptr CreateCameraDescriptorSet();
    RENDER_NS::IDescriptorSetBinder::Ptr CreatePresortDescriptorSet();
    RENDER_NS::IDescriptorSetBinder::Ptr CreatePreComputeDescriptorSet();


    // for plugin / factory interface
    static constexpr BASE_NS::Uid UID { "4ce00e60-68c9-46c8-aea8-87c4e7994d27" };
    static constexpr char const* TYPE_NAME = "RenderNodeGsRender";
    static constexpr RENDER_NS::IRenderNode::BackendFlags BACKEND_FLAGS =
        RENDER_NS::IRenderNode::BackendFlagBits::BACKEND_FLAG_BITS_DEFAULT;
#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    static constexpr RENDER_NS::IRenderNode::ClassType CLASS_TYPE =
        RENDER_NS::IRenderNode::ClassType::CLASS_TYPE_BACKEND_NODE;
#else
    static constexpr RENDER_NS::IRenderNode::ClassType CLASS_TYPE = RENDER_NS::IRenderNode::ClassType::CLASS_TYPE_NODE;
#endif
    // RenderNodeDefaultCameraController
    static constexpr BASE_NS::Uid AFTER { "46144344-29f8-4fc1-913a-ed5f6f2e20d0" };
    static constexpr BASE_NS::Uid BEFORE {};
    static RENDER_NS::IRenderNode* Create();
    static void Destroy(RENDER_NS::IRenderNode* instance);

    struct GpuSortBinders {
        struct CompBinders {
            RENDER_NS::IDescriptorSetBinder::Ptr set0;
            RENDER_NS::PipelineLayout pl;
        };

        CompBinders preSort;
        CompBinders counting;
        CompBinders scanScatter;
        CompBinders copy;
    };

    struct GsRenderBinders {
        struct CompBinders {
            RENDER_NS::IDescriptorSetBinder::Ptr set0;
            RENDER_NS::IDescriptorSetBinder::Ptr set1;
            RENDER_NS::PipelineLayout pl;
        };

        CompBinders animationPass;
        CompBinders precomputeProjection;
        CompBinders computeSphericalHarmonics;
        CompBinders updateRenderStatusUbo;
    };

    BASE_NS::Math::Mat4X4 GetCamProjMatrix() const
    {
        return cameraProjMatrix_;
    }
    BASE_NS::Math::Mat4X4 GetCamViewMatrix() const
    {
        return cameraViewMatrix_;
    }

private:
    void ParseRenderNodeInputs();

    bool GSNeedsSort(const BASE_NS::Math::Mat4X4& viewMatrix, const GsRenderData* renderData);

    bool UpdateCameraUBO(GS::GsRenderData* renderData);
    void UpdateImagesOnResize();
    bool UpdateDynamicDataUBO(RENDER_NS::IRenderCommandList& cmdList);
    void UpdateRenderStatusUbo(RENDER_NS::IRenderCommandList& cmdList);

    bool IsGsAnimationRunning(const uint32_t gsIdx);

    void SetupPipelineLayout(
        RENDER_NS::PipelineLayout& pl, const char* handle, const uint32_t maxCount, RENDER_NS::DescriptorCounts& dc);
    bool IsTiledMode() const;
    void CalculateAnimationPassCounts(RENDER_NS::DescriptorCounts& dc);
    void CalculateRenderPassCounts(RENDER_NS::DescriptorCounts& dc);
    void CalculateGpuSortCounts(RENDER_NS::DescriptorCounts& dc);
    void CalculateTileMergedCounts(RENDER_NS::DescriptorCounts& dc);
    void CreateRenderPassDescriptorSets(RENDER_NS::INodeContextDescriptorSetManager& descriptorSetMgr);
    void CreateGpuSortDescriptorSets(RENDER_NS::INodeContextDescriptorSetManager& descriptorSetMgr);
    void CreateTileMergedDescriptorSets(RENDER_NS::INodeContextDescriptorSetManager& descriptorSetMgr);
    void RecreateDescriptorSets();

    void UpdateRenderResolution(GsRenderData* renderData);

    uint32_t GetNumEffectiveSplats();
    void SetRecommended3DBox();

    void CheckEnsureEditorState();

    void checkDRS(const float* viewMatrix, GsRenderData* renderData);
    uint32_t GetCameraIdx();

    BASE_NS::vector<GsDynamicDataStruct> gsDynamicDatas_;

    struct PsoData {
        RENDER_NS::RenderHandle psoHandle;
        RENDER_NS::PushConstant pushConstant;
    };

    RenderNodeGsRender::PsoData GetPsoData(
        const RENDER_NS::PipelineLayout& pl, const RENDER_NS::RenderHandle& shaderHandle, const uint32_t flags);

    RenderNodeGsRender::PsoData GetRenderPsoData(
        RENDER_NS::IRenderNodeContextManager& renderNodeContextMgr, const RENDER_NS::RenderHandle& shaderHandle);

    RENDER_NS::IRenderNodeContextManager* renderNodeContextMgr_ { nullptr };
    CORE3D_NS::SceneRenderDataStores stores_;
    RenderDataStoreDefaultGS* dataStore_ { nullptr };
    IRenderDataStoreGsEdit* dataStoreEdit_ { nullptr };



    struct GpuSortShaders {
        RENDER_NS::RenderHandle sortedDataCopy;
        RENDER_NS::RenderHandle preSort;
        RENDER_NS::RenderHandle preSortTiled;
        RENDER_NS::RenderHandle preSortPackedTiled;
        RENDER_NS::RenderHandle counting;
        RENDER_NS::RenderHandle scanScatter;
        RENDER_NS::RenderHandle sortSizeCalcTiled;
        RENDER_NS::RenderHandle initPreSortTiled;
        RENDER_NS::RenderHandle countingTiled;
        RENDER_NS::RenderHandle scanScatterTiled;
    } gpuSortShaders_;

    struct GsRenderShaders {
        RENDER_NS::RenderHandle animationPass;
        RENDER_NS::RenderHandle precomputeProjection;
        RENDER_NS::RenderHandle precomputeProjectionPack;
        RENDER_NS::RenderHandle precomputeProjectionFg;
        RENDER_NS::RenderHandle precomputeProjectionPackFg;
        RENDER_NS::RenderHandle computeSphericalHarmonics;
        RENDER_NS::RenderHandle computeSphericalHarmonicsPack;
        RENDER_NS::RenderHandle updateRenderStatusUbo;
    } gsRenderShaders_;

    // cache all handles used for rendering to guarantee safe usage. Refreshed on every PreExecute
    struct GsHandlesCache {
        struct GpuSortBuffers {
            RENDER_NS::RenderHandleReference sortKeyIndexBuffer0;
            RENDER_NS::RenderHandleReference sortKeyIndexBuffer1;
            RENDER_NS::RenderHandleReference sortCountBuffer;
        } gpuSortBuffers;

        RENDER_NS::RenderHandleReference dataStorageCacheHandle;
        RENDER_NS::RenderHandleReference viewZStoreCacheHandle;
        RENDER_NS::RenderHandleReference gsIndicesRef;
        RENDER_NS::RenderHandleReference gsIndicesRef2;
        RENDER_NS::RenderHandleReference gsSplatData;
        RENDER_NS::RenderHandleReference gsSplatDataRender;
        RENDER_NS::RenderHandleReference gsHarmonicsDataRef;

        RENDER_NS::RenderHandleReference gaussianRenderTarget0;
        RENDER_NS::RenderHandleReference gaussianRenderTarget1;
        RENDER_NS::RenderHandleReference gaussianRenderTarget2;
        RENDER_NS::RenderHandleReference gaussianRenderTarget3;
        RENDER_NS::RenderHandleReference gaussianRenderTargetBB;

        RENDER_NS::RenderHandleReference gaussianRenderTargetDepth;
        RENDER_NS::RenderHandleReference depthStoreCacheHandle;

        RENDER_NS::RenderHandleReference gaussianRenderTargetStatusUbo;

        RENDER_NS::RenderHandleReference customAnimationShader;
        BASE_NS::vector<RENDER_NS::RenderHandleReference> customAnimationResources;

        BASE_NS::vector<RENDER_NS::RenderHandleReference> gsShASTCDataRefs;
        RENDER_NS::RenderHandleReference sampler;

        RENDER_NS::RenderHandleReference tileSplatPoolBuffer;
        RENDER_NS::RenderHandleReference tileSortIndexPool0;
        RENDER_NS::RenderHandleReference tileSortIndexPool1;
        RENDER_NS::RenderHandleReference tileSortCountPoolBuffer;
        RENDER_NS::RenderHandleReference tileMetaBuffer;
        RENDER_NS::RenderHandleReference tileIndirectDrawBuffer;

        bool IsValid(const bool useGpuSort) const
        {
            bool allAstcValid = true;
            for (const auto& astc : gsShASTCDataRefs) {
                allAstcValid = allAstcValid && astc;
            }

            const bool gpuSortBuffersValid =
                useGpuSort ? (gpuSortBuffers.sortCountBuffer && gpuSortBuffers.sortKeyIndexBuffer0 &&
                                 gpuSortBuffers.sortKeyIndexBuffer1)
                           : true;

            return gpuSortBuffersValid && dataStorageCacheHandle && viewZStoreCacheHandle && gsIndicesRef &&
                   gsIndicesRef2 && gsSplatData && gsHarmonicsDataRef && gaussianRenderTarget0 &&
                   gaussianRenderTarget1 && gaussianRenderTarget2 && gaussianRenderTarget3 && gaussianRenderTargetBB &&
                   gaussianRenderTargetStatusUbo && sampler && allAstcValid;
        }

        void Invalidate()
        {
            dataStorageCacheHandle = {};
            viewZStoreCacheHandle = {};
            gsIndicesRef = {};
            gsIndicesRef2 = {};
            gsSplatData = {};
            // gsSplatDataRender = {};
            gsHarmonicsDataRef = {};
            gaussianRenderTarget0 = {};
            gaussianRenderTarget1 = {};
            gaussianRenderTarget2 = {};
            gaussianRenderTarget3 = {};
            gaussianRenderTargetBB = {};
            gaussianRenderTargetDepth = {};
            depthStoreCacheHandle = {};
            gaussianRenderTargetStatusUbo = {};
            customAnimationShader = {};

            customAnimationResources.clear();
            gsShASTCDataRefs.clear();
            sampler = {};
            tileSplatPoolBuffer = {};
            tileSortIndexPool0 = {};
            tileSortIndexPool1 = {};
            tileSortCountPoolBuffer = {};
            tileMetaBuffer = {};
            tileIndirectDrawBuffer = {};
        }
    } gsHandlesCache_;

    // non-depth render shader
    RENDER_NS::RenderHandle gsRenderHandle_;
    RENDER_NS::IDescriptorSetBinder::Ptr gsRenderSet0_;
    RENDER_NS::PipelineLayout gsRenderPipelineLayout_;

    // depth-variant render shader
    RENDER_NS::RenderHandle gsRenderDepthHandle_;
    RENDER_NS::IDescriptorSetBinder::Ptr gsDepthRenderSet0_;
    RENDER_NS::PipelineLayout gsDepthRenderPipelineLayout_;
    RENDER_NS::RenderPass gsDepthRenderPass_;

    // prefilter depth
    RENDER_NS::RenderHandle prefilterDepthShaderHandle_;
    RENDER_NS::PipelineLayout prefilterDepthPipelineLayout_;
    RENDER_NS::IDescriptorSetBinder::Ptr prefilterDepthSet0_;
    RENDER_NS::RenderHandleReference prefilterDepthTex_;

    // normals
    RENDER_NS::RenderHandle normalsShaderHandle_;
    RENDER_NS::PipelineLayout normalsPipelineLayout_;
    RENDER_NS::IDescriptorSetBinder::Ptr normalsSet0_;
    RENDER_NS::RenderHandleReference normalsTex_;

    // edges pass
    RENDER_NS::RenderHandle edgesShaderHandle_;
    RENDER_NS::PipelineLayout edgesPipelineLayout_;
    RENDER_NS::IDescriptorSetBinder::Ptr edgesSet0_;
    RENDER_NS::RenderHandleReference edgeTex_;

    // dilate edges
    RENDER_NS::RenderHandle dilateEdgesShaderHandle_;
    RENDER_NS::PipelineLayout dilateEdgesPipelineLayout_;
    RENDER_NS::IDescriptorSetBinder::Ptr dilateEdgesSet0_;
    RENDER_NS::RenderHandleReference dilatedEdgesTex_;

    // find min max edges
    RENDER_NS::RenderHandle minmaxEdgesShaderHandle_;
    RENDER_NS::PipelineLayout minmaxEdgesPipelineLayout_;
    RENDER_NS::IDescriptorSetBinder::Ptr minmaxEdgesSet0_;

    // dense object
    RENDER_NS::RenderHandle denseObjectShaderHandle_;
    RENDER_NS::PipelineLayout denseObjectPipelineLayout_;
    RENDER_NS::IDescriptorSetBinder::Ptr denseObjectSet0_;
    RENDER_NS::RenderHandleReference denseObjectOutputBufferHandle_;

    BASE_NS::Math::Vec2 mousePos_ { 0.0f, 0.0f };
    BASE_NS::Math::Vec3 denseObjectWorldPos_;

    // debug draw
    RENDER_NS::RenderHandle debugDrawShaderHandle_;
    RENDER_NS::PipelineLayout debugDrawPipelineLayout_;
    RENDER_NS::RenderPass debugDrawRenderPass_;
    RENDER_NS::IDescriptorSetBinder::Ptr debugDrawRenderSet0_;
    RENDER_NS::RenderHandleReference gaussianDebugDrawBufferHandle_;
    BASE_NS::vector<BASE_NS::Math::Vec4> gaussianDebugLines_;

    GpuSortBinders bindersGpuSort_;
    GsRenderBinders bindersGsRender_;

    struct MergedTileEntry {
        uint32_t slot;
        uint32_t poolOffset;
        uint32_t splatCount;
        uint32_t mergedOffset;
        uint32_t primIdx;
        uint32_t tileIdx;
    };
    BASE_NS::vector<MergedTileEntry> mergedTiles_;
    uint32_t mergedTotalActiveSplats_ { 0u };
    uint64_t mergedTileSetHash_ { 0u };
    uint32_t mergedGsObjectIndex_ { 0u };
    uint32_t lastSeenPoolVersion_ { 0xFFFFFFFFu };
    BASE_NS::Math::Vec2 mergedDepthRange_ { 0.0f, 0.0f };
    bool mergedSortInitializing_ { true };

    // multi-frame distributed tile precompute
    static constexpr uint32_t kAmortFrames { 8u };
    static constexpr uint32_t kPrecomputeChunkAlignmentElems { 256u };
    uint32_t precomputeChunkIdx_ { 0u };
    bool forceFullPrecompute_ { true };
    bool mergedPrecomputeInProgress_ { false };
    uint32_t mergedPrecomputedJitterIndex_ { 0xFFFFFFFFu };

    GpuSortBinders::CompBinders tileMergedPreSortBinder_;
    GpuSortBinders::CompBinders tileMergedCountingBinder_;
    GpuSortBinders::CompBinders tileMergedScanScatterBinder_;
    GpuSortBinders::CompBinders tileMergedSortSizeCalcBinder_;
    GpuSortBinders::CompBinders tileMergedInitPreSortBinder_;
    GsRenderBinders::CompBinders tileMergedPrecomputeBinder_;

    BASE_NS::vector<GsTileWorkgroupMetadataStruct> mergedWgInfos_;
    RENDER_NS::RenderHandleReference tilePreSortWgInfoBuffer_;
    uint32_t tilePreSortWgInfoCapacity_ { 0u };

    // tile-specific shader handles
    RENDER_NS::RenderHandle tilePrecomputeProjectionShader_;
    RENDER_NS::RenderHandle tilePrecomputeProjectionPackedShader_;

    BASE_NS::unordered_map<uint64_t, PsoData> shaderToPsoData_;
    RENDER_NS::PipelineLayout pl_;
    RENDER_NS::RenderNodeHandles::InputRenderPass inputRenderPass_;
    RENDER_NS::RenderPass gsRenderPass_;
    RENDER_NS::RenderHandleReference cameraUniformBufferHandle_;
    RENDER_NS::RenderHandleReference gsDynamicDataUniformBufferHandle_;
    RENDER_NS::RenderHandle shaderHandle_;

    RENDER_NS::RenderHandle sceneDepthBufferHandle_;
    RENDER_NS::ImageAspectFlags sceneDepthBufferAspectFlags_ { 0 };
    uint32_t cameraIdx_ { CORE3D_NS::RenderSceneDataConstants::INVALID_INDEX };

    static constexpr uint64_t INVALID_CAM_ID { 0xFFFFFFFFffffffff };
    struct JsonInputs {
        RENDER_NS::RenderNodeGraphInputs::InputRenderPass renderPass;
        BASE_NS::string customCameraName;
        uint64_t customCameraId { INVALID_CAM_ID };
    };
    JsonInputs jsonInputs_;

    RENDER_NS::ViewportDesc viewportDesc_;
    RENDER_NS::ViewportDesc viewportDescPrevious_;
    RENDER_NS::ScissorDesc scissorDesc_;

    BASE_NS::Math::Mat4X4 cameraProjMatrix_;
    BASE_NS::Math::Mat4X4 cameraViewMatrix_;
    BASE_NS::Math::Mat4X4 cameraViewMatrixPrev_;

    std::array<float, 3> lastNearPlane {};
    std::array<float, 3> currentNearPlane {};
    bool lastDynamicDrawing = true;

    uint64_t frameIdx_ { 0 };
    uint32_t jitterRendered_ { 0 };

    size_t primitiveCountPrevious_ { 0 };

    std::chrono::high_resolution_clock::time_point animationTimePrevious_;
    bool animationRenderFrame_ { false };
    bool resourcesAreLoadedStateValid_ { false };

    bool indexPingPong_ { false };
    bool isMoving_ { true };
    bool uboEmpty_ { true };

    bool isSortingCpu_ { false };
    bool radixDone_ { false };

    bool camerasValidCanRender_ { false };

    BASE_NS::Math::Vec3 lastSortForwardDirection_;
    BASE_NS::Math::Vec3 lastSortPosition_;

    bool shDirtyFlag_ { false };
    uint32_t shComputationOffset_ { 0u };
    uint32_t shFramesRequired_ { 1u };
    uint32_t shFramesRendered_ { 0u };
    uint32_t currentRadixPass_ { 0 };

    RENDER_NS::RenderHandleReference gpuSortIndirectDrawBuffer_;
    RENDER_NS::RenderHandleReference gpuDummyBuffer_;
    RENDER_NS::RenderHandleReference edgesBuffer_;
    RENDER_NS::RenderHandleReference preComputeIndirectDispatchBuffer_;

    static constexpr float needsSortDistance_ { 1.0f };
    static constexpr float needsSortAngle_ { 0.01f };

    bool gpuSortInProgress_ { false };
    bool gsFramePrecomputedThisFrame_ { false };

    inline void BeginGpuSort()
    {
        gpuSortInProgress_ = true;
        currentRadixPass_ = 0u;
        radixDone_ = false;
        jitterRendered_ = 0u;
    }

    /**
     *  If set to true, will only render the gaussians if something has changed.
     *  Preferable for mobile.
     */
    static constexpr bool useRenderIfDirty_ { true };

    Render::DeviceBackendType backendType_ { Render::DeviceBackendType::VULKAN };

    // XEngine 3DGS / spatial-upscale backend, resolved at runtime via dlopen in
    // xengine_loader.cpp. Owned via the neutral IXEngineGsBackend interface so no
    // XEngine SDK type leaks into LumeGS. Null when libxengine_gs.z.so is absent
    // or the platform capability queries return false; every XEngine code path
    // guards on `if (xegBackend_)` and falls back to the native render path.
    BASE_NS::unique_ptr<IXEngineGsBackend> xegBackend_;

    BASE_NS::unique_ptr<GsEditor> gsEditor_;
    uint32_t ringIndex_ {};
    bool firstLoad = true;

};
} // namespace GS

#endif // RENDER_NODE_GS_SIMULATION_H