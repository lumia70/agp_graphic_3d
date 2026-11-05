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

#ifndef RENDER_NODE_GS_AI_EDIT_RENDER_H
#define RENDER_NODE_GS_AI_EDIT_RENDER_H

#include <3d/implementation_uids.h>
#include <3d/namespace.h>
#include <3d/render/default_material_constants.h>
#include <3d/render/intf_render_data_store_default_camera.h>
#include <3d/render/intf_render_data_store_default_scene.h>
#include <3d/render/intf_render_node_scene_util.h>

#include <plugin_gs/render/intf_render_data_store_gs_ai_edit.h>
#include <plugin_gs/render/intf_render_data_store_gs_edit.h>
#include <plugin_gs/render/intf_render_node_gs_ai_edit.h>

#include <base/containers/string.h>
#include <base/containers/unique_ptr.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/matrix_util.h>
#include <base/math/vector.h>
#include <base/util/uid.h>
#include <core/log.h>
#include <core/namespace.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/device/pipeline_layout_desc.h>
#include <plugin_gs/render/gs_ai_edit_factory.h>
#include <plugin_gs/render/gs_multiview_types.h>
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
#include "app/shaders/common/gs_struct_common.h"

namespace RENDER_NS {
class IRenderCommandList;
class IRenderNodeContextManager;
class IPipelineDescriptorSetBinder;
} // namespace RENDER_NS

namespace GS {
struct MultiviewRenderContext {
    int index;
    MultiviewRenderDataPack* optionalMVHandle;
};
class RenderNodeGsAIEditRender : public RENDER_NS::IRenderNode, public IRenderNodeGsAIEdit {
public:
    RenderNodeGsAIEditRender() = default;
    ~RenderNodeGsAIEditRender() override
    {
        gsAIEditor_.reset();
        hasInit_ = false;
    };

    IRenderDataStoreGsAIEdit* GetDataStoreAIEdit() const override
    {
        return dataStoreAIEdit_;
    }
    const CORE3D_NS::SceneRenderDataStores &GetStores() const override
    {
        return stores_;
    }

    void InitNode(RENDER_NS::IRenderNodeContextManager &renderNodeContextMgr) override;
    void InitNodeLazy(RENDER_NS::IRenderNodeContextManager &renderNodeContextMgr);
    void PreExecuteFrame() override;
    void ExecuteFrame(RENDER_NS::IRenderCommandList &cmdList) override;
    void ExecuteGpuSort(RENDER_NS::IRenderCommandList &cmdList, const BASE_NS::Math::Mat4X4 viewMatrix,
                        MultiviewRenderDataPack* optionalMVHandle = nullptr);
    void CalculateMultiviewDescriptorCounts(RENDER_NS::DescriptorCounts &dc, uint32_t count) const;

    void ExecuteGaussianRenderPassWithAttachment(RENDER_NS::IRenderCommandList &cmdList,
                                                 const BASE_NS::Math::Mat4X4 viewMatrix, GS::GsRenderData* renderData,
                                                 const MultiviewRenderContext &mvContext);

    void BeginMultiviewDepthRenderPass(RENDER_NS::IRenderCommandList &cmdList, const MultiviewRenderContext &mvContext,
                                       GS::GsRenderData* renderData, RENDER_NS::ViewportDesc &outViewportDesc,
                                       RENDER_NS::ScissorDesc &outScissorDesc);

    void PrecomputeGsFrameData(RENDER_NS::IRenderCommandList &cmdList, GS::GsRenderData* renderData,
                               const BASE_NS::Math::Mat4X4 &viewMatrix,
                               MultiviewRenderDataPack* optionalMVHandle = nullptr);
    void ExecuteGpuSortPopulate(RENDER_NS::IRenderCommandList &cmdList, MultiviewRenderDataPack* optionalMVHandle);

    ExecuteFlags GetExecuteFlags() const override;

    void ExecuteFrameWithViewport(RENDER_NS::IRenderCommandList &cmdList, int index,
                                  MultiviewRenderDataPack* optionalMVHandle = nullptr) override;

    void CreateDescriptorSet(RENDER_NS::INodeContextDescriptorSetManager &descriptorSetMgr,
                             RENDER_NS::PipelineLayout &pl, const uint32_t bufferSetIndex,
                             RENDER_NS::IDescriptorSetBinder::Ptr &setBinder);
    RENDER_NS::IDescriptorSetBinder::Ptr CreateCameraDescriptorSet() override;
    RENDER_NS::IDescriptorSetBinder::Ptr CreatePresortDescriptorSet() override;
    RENDER_NS::IDescriptorSetBinder::Ptr CreatePreComputeDescriptorSet() override;
    RENDER_NS::IDescriptorSetBinder::Ptr CreateDepthRenderDescriptorSet() override;
    // for plugin / factory interface
    static constexpr BASE_NS::Uid UID{"4ce00e60-68c9-46c8-aea8-87c4e7994d29"};
    static constexpr char const *TYPE_NAME = "RenderNodeGsAIEditRender";
    static constexpr RENDER_NS::IRenderNode::BackendFlags BACKEND_FLAGS =
        RENDER_NS::IRenderNode::BackendFlagBits::BACKEND_FLAG_BITS_DEFAULT;

    static constexpr RENDER_NS::IRenderNode::ClassType CLASS_TYPE = RENDER_NS::IRenderNode::ClassType::CLASS_TYPE_NODE;

    // RenderNodeDefaultCameraController
    static constexpr BASE_NS::Uid AFTER{"4ce00e60-68c9-46c8-aea8-87c4e7994d27"};
    static constexpr BASE_NS::Uid BEFORE{};
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

        CompBinders precomputeProjection;
        CompBinders updateRenderStatusUbo;
    };
    BASE_NS::Math::Mat4X4 GetCamProjMatrix() const override
    {
        return cameraProjMatrix_;
    }
    BASE_NS::Math::Mat4X4 GetCamViewMatrix() const override
    {
        return cameraViewMatrix_;
    }
    bool UpdateDynamicDataUBO(RENDER_NS::IRenderCommandList &cmdList, bool forceUpdate = false) override;
    size_t GetCameraUboSize() const override
    {
        return sizeof(GsCameraUboStruct);
    }

private:
    void ParseRenderNodeInputs();

    void UpdateCameraUBO(GS::GsRenderData* renderData);

    void UpdateRenderStatusUbo(RENDER_NS::IRenderCommandList &cmdList);

    void CreateRenderPassInternal();
    void CreateUniformBuffers(const BASE_NS::string &uniqueName);
    void CreateGpuBuffers(const BASE_NS::string &uniqueName);
    void LoadShaders();
    struct PsoData {
        RENDER_NS::RenderHandle psoHandle;
        RENDER_NS::PushConstant pushConstant;
    };
    void CalculateGsRenderDescriptorCounts(const RENDER_NS::IRenderNodeShaderManager &shaderMgr,
                                           RENDER_NS::DescriptorCounts &dc);
    void CalculateGpuSortDescriptorCounts(const RENDER_NS::IRenderNodeShaderManager &shaderMgr,
                                          RENDER_NS::DescriptorCounts &dc);
    void CalculateAllDescriptorCounts(const RENDER_NS::IRenderNodeShaderManager &shaderMgr,
                                      RENDER_NS::DescriptorCounts &dc);
    void FillSpatialCodecData(GsDynamicDataStruct &dynamicData, const GsPrimitive &primitive);
    void FillDynamicDataForPrimitive(GsDynamicDataStruct &dynamicData, GsPrimitive &primitive,
                                     const GS::GsRenderData* renderData, size_t primitiveIndex);
    bool EnsureNodeInitialized();
    void ResizeAIEditIndexBuffer(const GS::GsRenderData* renderData);
    RENDER_NS::RenderHandleReference CreateGpuBuffer(const BASE_NS::string &name, uint32_t usageFlags,
                                                     uint32_t byteSize);
    void CacheReadOnlyHandles(GS::GsRenderData* renderData);
    void ResizeSortKeyBuffers(GS::GsRenderData* renderData, const BASE_NS::string &uniqueName);
    void ResizeDataStorageBuffers(GS::GsRenderData* renderData, const BASE_NS::string &uniqueName);
    void CacheGsRenderHandles(GS::GsRenderData* renderData);
    void AcquireSceneDepthBuffer(GS::GsRenderData* renderData);
    void ExecuteRadixCountingPass(RENDER_NS::IRenderCommandList &cmdList, uint32_t bytePos, uint32_t totalGs);
    void ExecuteRadixScanScatterPass(RENDER_NS::IRenderCommandList &cmdList, uint32_t bytePos, uint32_t totalGs);
    void BeginMultiviewRenderPass(RENDER_NS::IRenderCommandList &cmdList, const MultiviewRenderContext &mvContext,
                                  GS::GsRenderData* renderData, RENDER_NS::ViewportDesc &outViewportDesc,
                                  RENDER_NS::ScissorDesc &outScissorDesc);
    void DispatchPrecomputeProjection(RENDER_NS::IRenderCommandList &cmdList, const PsoData &psoData,
                                      MultiviewRenderDataPack* optionalMVHandle, uint32_t numGs);
    void ExecutePreSortPass(RENDER_NS::IRenderCommandList &cmdList, const BASE_NS::Math::Vec2 &depthRange,
                            uint32_t totalGs, MultiviewRenderDataPack* optionalMVHandle);
    void BindGsRenderDescriptorSet0(RENDER_NS::IRenderCommandList &cmdList, bool useDepth,
                                    const MultiviewRenderContext &mvContext);
    void UpdateViewportCameraUBO(MultiviewRenderDataPack* optionalMVHandle);

    void RecreateDescriptorSets();

    void UpdateRenderResolution();

    BASE_NS::vector<GsDynamicDataStruct> gsDynamicDatas_;

    RenderNodeGsAIEditRender::PsoData GetPsoData(const RENDER_NS::PipelineLayout &pl,
                                                 const RENDER_NS::RenderHandle &shaderHandle, const uint32_t flags);

    RenderNodeGsAIEditRender::PsoData GetRenderPsoData(RENDER_NS::IRenderNodeContextManager &renderNodeContextMgr,
                                                       const RENDER_NS::RenderHandle &shaderHandle);

    RENDER_NS::IRenderNodeContextManager* renderNodeContextMgr_{nullptr};
    CORE3D_NS::SceneRenderDataStores stores_;
    RenderDataStoreDefaultGS* dataStore_{nullptr};

    IRenderDataStoreGsAIEdit* dataStoreAIEdit_{nullptr};
    IRenderDataStoreGsEdit* dataStoreEdit_{nullptr};

    struct GpuSortShaders {
        RENDER_NS::RenderHandle sortedDataCopy;
        RENDER_NS::RenderHandle preSort;
        RENDER_NS::RenderHandle counting;
        RENDER_NS::RenderHandle scanScatter;
    } gpuSortShaders_;

    struct GsRenderShaders {
        RENDER_NS::RenderHandle precomputeProjection;
        RENDER_NS::RenderHandle precomputeProjectionPack;
        RENDER_NS::RenderHandle precomputeProjectionFg;
        RENDER_NS::RenderHandle precomputeProjectionPackFg;
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

        RENDER_NS::RenderHandleReference depthStoreCacheHandle;

        RENDER_NS::RenderHandleReference gaussianRenderTargetStatusUbo;

        BASE_NS::vector<RENDER_NS::RenderHandleReference> gsShASTCDataRefs;
        RENDER_NS::RenderHandleReference sampler;

        bool IsValid(const bool useGpuSort) const
        {
            bool allAstcValid = true;
            for (const auto &astc : gsShASTCDataRefs) {
                allAstcValid = allAstcValid && astc;
            }

            const bool gpuSortBuffersValid =
                useGpuSort ? (gpuSortBuffers.sortCountBuffer && gpuSortBuffers.sortKeyIndexBuffer0 &&
                              gpuSortBuffers.sortKeyIndexBuffer1)
                           : true;

            return gpuSortBuffersValid && dataStorageCacheHandle && viewZStoreCacheHandle && gsIndicesRef &&
                   gsIndicesRef2 && gsSplatData && gsHarmonicsDataRef && gaussianRenderTarget0 &&
                   gaussianRenderTarget1 && gaussianRenderTarget2 && gaussianRenderTarget3 && gaussianRenderTargetBB &&
                   depthStoreCacheHandle && gaussianRenderTargetStatusUbo && sampler && allAstcValid;
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
            depthStoreCacheHandle = {};
            gaussianRenderTargetStatusUbo = {};

            gsShASTCDataRefs.clear();
            sampler = {};
        }
    } gsHandlesCache_;

    RENDER_NS::RenderHandle gsRenderHandle_;
    RENDER_NS::RenderHandle gsRenderDepthHandle_;
    RENDER_NS::PipelineLayout gsRenderPipelineLayout_;
    RENDER_NS::PipelineLayout gsDepthRenderPipelineLayout_;
    RENDER_NS::RenderPass gsDepthRenderPass_;

    GpuSortBinders bindersGpuSort_;
    GsRenderBinders bindersGsRender_;

    BASE_NS::unordered_map<uint64_t, PsoData> shaderToPsoData_;
    RENDER_NS::RenderPass gsRenderPass_;
    // RENDER_NS::RenderHandleReference cameraUniformBufferHandle_;
    RENDER_NS::RenderHandleReference gsDynamicDataUniformBufferHandle_;

    RENDER_NS::RenderHandle sceneDepthBufferHandle_;

    static constexpr uint64_t INVALID_CAM_ID{0xFFFFFFFFffffffff};
    struct JsonInputs {
        RENDER_NS::RenderNodeGraphInputs::InputRenderPass renderPass;
        BASE_NS::string customCameraName;
        uint64_t customCameraId{INVALID_CAM_ID};
    };
    JsonInputs jsonInputs_;

    RENDER_NS::ViewportDesc viewportDesc_;
    RENDER_NS::ScissorDesc scissorDesc_;

    BASE_NS::Math::Mat4X4 cameraProjMatrix_;
    BASE_NS::Math::Mat4X4 cameraViewMatrix_;

    uint64_t frameIdx_{0};
    uint32_t jitterRendered_{0};

    RENDER_NS::RenderHandleReference gsAIEditIndicesRef_;

    RENDER_NS::RenderHandleReference gpuSortIndirectDrawBuffer_;
    RENDER_NS::RenderHandleReference gpuDummyBuffer_;

    RENDER_NS::RenderPass mvRenderPassDescs_[4]{};

    BASE_NS::unique_ptr<IGsAIEditor> gsAIEditor_;
    bool hasInit_{false};
};
} // namespace GS

#endif // RENDER_NODE_GS_AI_EDIT_RENDER_H
