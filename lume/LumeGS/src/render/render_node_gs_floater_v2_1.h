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

#ifndef RENDER_NODE_GS_FLOATER_V2_1_H
#define RENDER_NODE_GS_FLOATER_V2_1_H

#include <array>
#include <cstdint>

#include <base/containers/string.h>
#include <base/math/matrix.h>
#include <base/math/matrix_util.h>
#include <base/math/vector.h>
#include <base/util/uid.h>

#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <plugin_gs/render/intf_render_data_store_gs_edit.h>
#include <plugin_gs/render/intf_render_data_store_gs_floater_v2_1.h>
#include <3d/render/intf_render_data_store_default_camera.h>

#include <render/device/pipeline_layout_desc.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_node.h>
#include <render/resource_handle.h>

#include "gs_floater_v2_1_gpu_types.h"
#include "render_data_store_default_gs.h"
#include "render_data_store_gs_edit.h"
#include "render_data_store_gs_floater_v2_1.h"

namespace RENDER_NS {
class IRenderCommandList;
class IRenderNodeContextManager;
struct RenderPassDesc;
struct RenderPassSubpassDesc;
} // namespace RENDER_NS

namespace GS {

/*
 * Synchronous v2.1 floater-removal RenderNode.
 *
 * Design decisions (from graph metadata):
 *   D3:  Registered AFTER RenderNodeGsRender, no .rng file, no new System.
 *   D4:  DataStore is a passive buffer holder; all state lives here.
 *   D10: Multi-frame state machine: one stage per ExecuteFrame call.
 *   D11: FloaterRemoval pushed LAST (stack top) for O(1) undo check.
 *   D12: All config thresholds baked into source; no external param.
 *   D16: SelectByIndex + v2_1_compact_indices.comp replaces SelectByMask.
 *   D17: Zero-Gaussian guard -- if count==0, push FloaterRemoval marker only.
 *   D19: GpuFrameParams kept as private member, uploaded per peel frame.
 *   CC3:  ExecuteFrame is a multi-frame state machine (PREPARE->SORT->PEEL x15->COMPACT).
 *   CC4:  Buffer creation uses GsSplatData stride (64 bytes).
 *   RC1:  Zero-Gaussian guard.
 *   RC2:  No rollback -- on failure only push FloaterRemoval marker.
 */
class RenderNodeGsFloaterV21 final : public RENDER_NS::IRenderNode {
public:
    RenderNodeGsFloaterV21() = default;
    ~RenderNodeGsFloaterV21() override = default;

    void InitNode(RENDER_NS::IRenderNodeContextManager& context) override;
    void PreExecuteFrame() override;
    void ExecuteFrame(RENDER_NS::IRenderCommandList& commandList) override;
    RENDER_NS::IRenderNode::ExecuteFlags GetExecuteFlags() const override
    {
        return 0u;
    }

    // for plugin / factory interface
    static constexpr BASE_NS::Uid UID{"f3c7a1d2-9e84-4b6a-bf52-2d1e8a3f5c70"};
    static constexpr char const *TYPE_NAME = "RenderNodeGsFloaterV21";
    static constexpr RENDER_NS::IRenderNode::BackendFlags BACKEND_FLAGS =
        RENDER_NS::IRenderNode::BackendFlagBits::BACKEND_FLAG_BITS_DEFAULT;
    static constexpr RENDER_NS::IRenderNode::ClassType CLASS_TYPE =
        RENDER_NS::IRenderNode::ClassType::CLASS_TYPE_NODE;
    // Registered AFTER RenderNodeGsRender (D3).
    static constexpr BASE_NS::Uid AFTER{"4ce00e60-68c9-46c8-aea8-87c4e7994d27"};
    static constexpr BASE_NS::Uid BEFORE{};
    static RENDER_NS::IRenderNode* Create();
    static void Destroy(RENDER_NS::IRenderNode* instance);

private:
    enum class ComputePipeline : uint32_t {
        CLEAR = 0u,
        CLEAR_AUX,
        PROJECT_SORT_INIT,
        SH_RANGE,
        SH_HISTOGRAM,
        SH_MEDIAN,
        SH_MAD_HISTOGRAM,
        SH_MAD,
        RADIX_COUNT,
        RADIX_PREFIX,
        RADIX_SCATTER,
        DEPTH_RESOLVE,
        FOOTPRINT,
        PRE_SCORE,
        COMPACT_HASH,
        KNN_SCORE,
        SH_PROPAGATE,
        MASK_UPDATE,
        COMPACT_INDICES,
        COUNT,
    };

    enum class GraphicsPipeline : uint32_t {
        WEIGHTED_DEPTH = 0u,
        OPAQUE_DISC,
        COUNT,
    };

    // D19: per-frame uniform uploaded once. D12: all thresholds baked in.
    struct alignas(16) GpuFrameParams {
        BASE_NS::Math::Mat4X4 viewProjection{BASE_NS::Math::IDENTITY_4X4};
        BASE_NS::Math::Mat4X4 view{BASE_NS::Math::IDENTITY_4X4};
        BASE_NS::Math::Vec4 cameraPosition{0.0f, 0.0f, 0.0f, 1.0f};
        BASE_NS::Math::Vec4 intrinsicsViewport{1.0f, 1.0f, 1.0f, 1.0f};
        BASE_NS::Math::Vec4 principalNearFar{0.5f, 0.5f, 0.01f, 300.0f};
        BASE_NS::Math::Vec4 discAlphaGap{5.991f, 1.0f / 255.0f, 0.05f, 0.35f};
        BASE_NS::Math::Vec4 weights0{0.34f, 0.24f, 0.20f, 0.14f};
        BASE_NS::Math::Vec4 weights1{0.08f, 0.55f, 0.72f, 0.03f};
        BASE_NS::Math::Vec4 candidate0{0.62f, 0.52f, 0.18f, 0.055f};
        BASE_NS::Math::Vec4 candidate1{6.0f, 0.10f, 0.035f, 0.04f};
        BASE_NS::Math::Vec4 seed0{0.92f, 0.98f, 0.98f, 0.35f};
        BASE_NS::Math::Vec4 seed1{0.10f, 24.0f, 0.20f, 0.0f};
        BASE_NS::Math::Vec4 weak0{0.25f, 0.02f, 0.10f, 0.05f};
        BASE_NS::Math::Vec4 weak1{1.0f, 0.0f, 0.0f, 0.0f};
        BASE_NS::Math::Vec4 graph0{0.25f, 3.0f, 0.02f, 0.08f};
        BASE_NS::Math::Vec4 graph1{1.5f, 2.0f, 0.35f, 16.0f};
        BASE_NS::Math::Vec4 shWeights{0.50f, 0.30f, 0.15f, 0.05f};
        BASE_NS::Math::Vec4 shLimits{8.0f, 0.75f, 1.25f, 2.2239f};
        BASE_NS::Math::Vec4 sampleGates{5.0f, 3.0f, 0.04f, 0.03f};
        BASE_NS::Math::Vec4 adaptive0{0.06f, 0.05f, 0.60f, 2.0f};
        BASE_NS::Math::Vec4 protection0{3.0f, 0.65f, 2.0f, 0.25f};
        BASE_NS::Math::Vec4 protection1{2.0f, 2.0f, 0.0f, 0.0f};
        uint32_t counts[4]{0u, 0u, 0u, 0u}; // N, width, height, SH degree
        uint32_t peel[4]{0u, 15u, 1u, 1u};  // round, max, fixed scale, hash capacity
        uint32_t revisions[4]{0u, 0u, 0u, 0u};
    };
    static_assert((sizeof(GpuFrameParams) & 15u) == 0u);

    struct SortPushConstant {
        uint32_t count{0u};
        uint32_t paddedCount{0u};
        uint32_t radixShift{0u};
        uint32_t groupCount{0u};
    };
    static_assert(sizeof(SortPushConstant) == 16u);

    struct DispatchPushConstant {
        uint32_t mode{0u};
        uint32_t count{0u};
        uint32_t round{0u};
        uint32_t reserved{0u};
    };
    static_assert(sizeof(DispatchPushConstant) == 16u);

    struct GraphicsPushConstant {
        uint32_t applyCumulativeMask{1u};
        uint32_t reserved[3]{0u, 0u, 0u};
    };
    static_assert(sizeof(GraphicsPushConstant) == 16u);

    struct Resources {
        RENDER_NS::RenderHandleReference frameParams;
        RENDER_NS::RenderHandleReference projected;
        RENDER_NS::RenderHandleReference analysis;
        RENDER_NS::RenderHandleReference state;
        RENDER_NS::RenderHandleReference sortEntries;
        RENDER_NS::RenderHandleReference sortScratch;
        RENDER_NS::RenderHandleReference histogram;
        RENDER_NS::RenderHandleReference compactEntries;
        RENDER_NS::RenderHandleReference globals;
        RENDER_NS::RenderHandleReference hashBuckets;
        RENDER_NS::RenderHandleReference shStats;

        RENDER_NS::RenderHandleReference currentPreview;
        RENDER_NS::RenderHandleReference weightedAccumulator;
        RENDER_NS::RenderHandleReference opaqueLinearDepth;
        RENDER_NS::RenderHandleReference opaqueOwnerId;
        RENDER_NS::RenderHandleReference opaqueHardwareDepth;
        // D16: compact index buffer for SelectByIndex action.
        RENDER_NS::RenderHandleReference compactIndexBuffer;
        // Scratch progress buffer written by clear_aux and mask_update shaders.
        RENDER_NS::RenderHandleReference progress;
    };

    struct Pipeline {
        RENDER_NS::RenderHandle shader;
        RENDER_NS::RenderHandle pso;
        RENDER_NS::PipelineLayout layout;
        RENDER_NS::IDescriptorSetBinder::Ptr binder;
    };

    // LumeGS handle cache (D3/D4). Refreshed every PreExecuteFrame.
    struct GsHandlesCache {
        RENDER_NS::RenderHandleReference gsSplatData;
        RENDER_NS::RenderHandleReference gsHarmonicsDataRef;
        RENDER_NS::RenderHandleReference gsIndicesRef;
        RENDER_NS::RenderHandleReference sampler;

        bool IsValid() const
        {
            return gsSplatData && gsHarmonicsDataRef && gsIndicesRef && sampler;
        }

        void Invalidate()
        {
            gsSplatData = {};
            gsHarmonicsDataRef = {};
            gsIndicesRef = {};
            sampler = {};
        }
    };

    static constexpr uint32_t LOCAL_SIZE = 256u;
    static constexpr uint32_t RADIX_BUCKETS = 256u;
    static constexpr uint32_t RADIX_PASSES = 4u;
    static constexpr uint32_t MAX_PEEL_COUNT = 15u;
    static constexpr uint32_t MINIMUM_PEELS = 2u;
    static constexpr uint32_t INVALID_GAUSSIAN_ID = 0xffffffffu;

    // Multi-frame state machine: each ExecuteFrame call advances one stage.
    // Frame 1: PREPARE_SCENE  Frame 2: SORT_CAMERA  Frame 3..17: PEEL  Frame 18: COMPACT
    enum class Stage : uint32_t {
        IDLE = 0u,
        PREPARE_SCENE = 1u,
        SORT_CAMERA = 2u,
        PEEL = 3u,
        COMPACT = 4u,
        DONE = 5u,
    };
    // CC4: GsSplatData is 4xvec4 = 64 bytes.
    static constexpr uint32_t GS_SPLAT_DATA_STRIDE = 64u;
    // GsHarmonicsData is 24 uints = 96 bytes.
    static constexpr uint32_t GS_HARMONICS_DATA_STRIDE = 96u;

    void ParseInputs();
    void CacheReadOnlyHandles();
    bool ResolvePendingFloaterRemoval();
    bool CreateTaskResources();
    void ReleaseTaskResources();
    bool CreateBuffer(RENDER_NS::RenderHandleReference& target, uint32_t bytes, uint32_t usage,
        uint32_t memoryProperties, const char* name,
        RENDER_NS::EngineBufferCreationFlags creationFlags =
            RENDER_NS::EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS);
    bool CreateImage(RENDER_NS::RenderHandleReference& target, BASE_NS::Format format, uint32_t usage,
        const char* name);
    void FillGpuParams();
    bool UploadGpuParams();

    void LoadPipelines();
    void CreatePipelineBinders();
    void CreatePipelineStates();
    bool PipelinesReady() const;
    void BindResources(Pipeline& pipeline, RENDER_NS::IRenderCommandList& commandList,
        uint32_t pipelineIndex, bool graphicsPipeline);
    void BindCompute(ComputePipeline pipelineId, RENDER_NS::IRenderCommandList& commandList);
    void BindGraphics(GraphicsPipeline pipelineId, RENDER_NS::IRenderCommandList& commandList);
    void Dispatch(ComputePipeline pipelineId, uint32_t x, const DispatchPushConstant& push,
        RENDER_NS::IRenderCommandList& commandList);

    void RecordPrepareScene(RENDER_NS::IRenderCommandList& commandList);
    void RecordSortCamera(RENDER_NS::IRenderCommandList& commandList);
    void RecordRadixSort(RENDER_NS::IRenderCommandList& commandList);
    void RecordPeel(RENDER_NS::IRenderCommandList& commandList);
    void RecordCompactPass(RENDER_NS::IRenderCommandList& commandList);
    void PushEditActions([[maybe_unused]] RENDER_NS::IRenderCommandList& commandList);

    // Read stopReason from the globals buffer (host-visible). Returns the
    // stop reason value (0 = NONE = keep peeling). Called from PreExecuteFrame
    // during PEEL stage -- the previous frame's GPU work is guaranteed complete.
    uint32_t ObserveStopReason();
    void PushFloaterRemovalMarkerOnly();

    void RecordWeightedPass(RENDER_NS::IRenderCommandList& commandList, bool applyCumulativeMask = true);
    void RecordOpaquePass(RENDER_NS::IRenderCommandList& commandList, bool applyCumulativeMask = true);
    void RecordDepthResolve(RENDER_NS::IRenderCommandList& commandList, bool accumulateEvidence);
    void RecordComputeBarrier(RENDER_NS::IRenderCommandList& commandList) const;
    void RecordGraphicsToComputeBarrier(RENDER_NS::IRenderCommandList& commandList) const;
    void RecordComputeToGraphicsBarrier(RENDER_NS::IRenderCommandList& commandList) const;

    void BuildWeightedRenderPass(RENDER_NS::RenderPassDesc& pass, RENDER_NS::RenderPassSubpassDesc& subpass) const;
    void BuildOpaqueRenderPass(RENDER_NS::RenderPassDesc& pass, RENDER_NS::RenderPassSubpassDesc& subpass) const;
    void SetViewportAndScissor(RENDER_NS::IRenderCommandList& commandList) const;

    RENDER_NS::IRenderNodeContextManager* context_{nullptr};
    BASE_NS::string dataStoreName_{"RenderDataStoreGsFloaterV21"};
    BASE_NS::string gsEditDataStoreName_{"RenderDataStoreGsEdit"};
    BASE_NS::string defaultGsDataStoreName_{"RenderDataStoreDefaultGs"};
    BASE_NS::string cameraDataStoreName_{"RenderDataStoreDefaultCamera"};

    // LumeGS data store references.
    RenderDataStoreGsFloaterV21* floaterStore_{nullptr};
    RenderDataStoreGsEdit* gsEditStore_{nullptr};
    RenderDataStoreDefaultGS* defaultGsStore_{nullptr};
    CORE3D_NS::IRenderDataStoreDefaultCamera* cameraStore_{nullptr};

    GsHandlesCache gsHandlesCache_{};
    GpuFrameParams gpuParams_{};
    Resources resources_{};
    std::array<Pipeline, static_cast<uint32_t>(ComputePipeline::COUNT)> computePipelines_{};
    std::array<Pipeline, static_cast<uint32_t>(GraphicsPipeline::COUNT)> graphicsPipelines_{};
    RENDER_NS::RenderHandle nearestSampler_{};

    // Per-frame state populated in PreExecuteFrame.
    uint32_t gaussianCount_{0u};
    uint32_t renderWidth_{0u};
    uint32_t renderHeight_{0u};
    float intrinsicsFx_{1.0f};
    float intrinsicsFy_{1.0f};
    float intrinsicsCx_{0.5f};
    float intrinsicsCy_{0.5f};
    BASE_NS::Math::Mat4X4 cameraViewMatrix_{BASE_NS::Math::IDENTITY_4X4};
    BASE_NS::Math::Mat4X4 cameraProjMatrix_{BASE_NS::Math::IDENTITY_4X4};
    BASE_NS::Math::Vec4 cameraPosition_{0.0f, 0.0f, 0.0f, 1.0f};

    bool resourcesReady_{false};
    bool pipelinesReady_{false};
    bool hasPendingFloaterRemoval_{false};
    bool errorState_{false};

    // Multi-frame pipeline state.
    Stage stage_{Stage::IDLE};
    uint32_t currentPeel_{0u};
    uint32_t peelsCompleted_{0u};
};

} // namespace GS

#endif // RENDER_NODE_GS_FLOATER_V2_1_H
