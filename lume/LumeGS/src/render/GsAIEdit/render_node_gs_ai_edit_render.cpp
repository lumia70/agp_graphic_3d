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

#include "render_node_gs_ai_edit_render.h"

#include <algorithm>
#include <util/gs_serialization.h>

#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <base/containers/array_view.h>
#include <base/math/matrix.h>
#include <core/perf/cpu_perf_scope.h>
#include <render/datastore/intf_render_data_store_pod.h>
#include <render/datastore/render_data_store_render_pods.h>
#include "render_data_store_gs_ai_edit.h"
#include "../render_data_store_gs_edit.h"

namespace {
#include "app/shaders/common/gs_common.h"
} // namespace

#if RENDER_HAS_VULKAN_BACKEND
#include <render/vulkan/intf_device_vk.h>

#endif

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;

namespace GS {
namespace {
constexpr DynamicStateEnum DYNAMIC_STATES[] = {CORE_DYNAMIC_STATE_ENUM_VIEWPORT, CORE_DYNAMIC_STATE_ENUM_SCISSOR};

void InBarrier(IRenderCommandList &cmdList)
{
    // might be read in fragment or compute
    constexpr GeneralBarrier src{AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT |
                                     AccessFlagBits::CORE_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                 PipelineStageFlagBits::CORE_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                     PipelineStageFlagBits::CORE_PIPELINE_STAGE_VERTEX_SHADER_BIT};
    constexpr GeneralBarrier dst{AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT |
                                     AccessFlagBits::CORE_ACCESS_SHADER_WRITE_BIT,
                                 PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT};

    cmdList.CustomMemoryBarrier(src, dst);
    cmdList.AddCustomBarrierPoint();
}

void OutBarrier(IRenderCommandList &cmdList)
{
    // might be read next in vertex or compute
    constexpr GeneralBarrier src{AccessFlagBits::CORE_ACCESS_SHADER_WRITE_BIT,
                                 PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT};
    constexpr GeneralBarrier dst{AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT |
                                     AccessFlagBits::CORE_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
                                 PipelineStageFlagBits::CORE_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                                     PipelineStageFlagBits::CORE_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                     PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT};

    cmdList.CustomMemoryBarrier(src, dst);
    cmdList.AddCustomBarrierPoint();
}

} // namespace

inline void CalculateDescriptorCounts(const PipelineLayout &pl, const uint32_t setIdx, const uint32_t setCount,
                                      DescriptorCounts &descriptorCounts)
{
    const auto &bindings = pl.descriptorSetLayouts[setIdx].bindings;
    descriptorCounts.counts.reserve(descriptorCounts.counts.size() + bindings.size());
    for (const auto &bindingRef : bindings) {
        descriptorCounts.counts.emplace_back(
            DescriptorCounts::TypedCount{bindingRef.descriptorType, bindingRef.descriptorCount * setCount});
    }
}

template <typename RenderDataStoreType>
RenderDataStoreType* GetRenderDataStore(const IRenderNodeRenderDataStoreManager &renderDataStoreManager,
                                        const string_view name)
{
    return static_cast<RenderDataStoreType *>(renderDataStoreManager.GetRenderDataStore(name));
}
void RenderNodeGsAIEditRender::CreateRenderPassInternal()
{
    const auto &renderNodeUtil = renderNodeContextMgr_->GetRenderNodeUtil();

    // Color-only render pass
    {
        RENDER_NS::RenderNodeHandles::InputRenderPass inputRenderPass;
        RenderNodeAttachment colorAttachment;
        colorAttachment.loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.clearValue = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);
        inputRenderPass.attachments.push_back(colorAttachment);
        inputRenderPass.subpassCount = 1;
        inputRenderPass.subpassContents = SubpassContents::CORE_SUBPASS_CONTENTS_INLINE;
        inputRenderPass.subpassIndex = 0;
        inputRenderPass.colorAttachmentIndices.push_back(0);
        gsRenderPass_ = renderNodeUtil.CreateRenderPass(inputRenderPass);
    }

    // Depth variant render pass (dual color attachment: color + depth)
    {
        RENDER_NS::RenderNodeHandles::InputRenderPass inputRenderPass;
        // color
        {
            RenderNodeAttachment colorAttachment;
            colorAttachment.loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);
            inputRenderPass.attachments.push_back(colorAttachment);
        }
        // depth (second color attachment, not depth-stencil)
        {
            RenderNodeAttachment depthAttachment;
            depthAttachment.loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
            depthAttachment.storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.clearValue = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);
            inputRenderPass.attachments.push_back(depthAttachment);
        }
        inputRenderPass.subpassCount = 1;
        inputRenderPass.subpassContents = SubpassContents::CORE_SUBPASS_CONTENTS_INLINE;
        inputRenderPass.subpassIndex = 0;
        inputRenderPass.colorAttachmentIndices.push_back(0);
        inputRenderPass.colorAttachmentIndices.push_back(1);
        gsDepthRenderPass_ = renderNodeUtil.CreateRenderPass(inputRenderPass);
    }
}

void RenderNodeGsAIEditRender::CreateUniformBuffers(const BASE_NS::string &uniqueName)
{
    GpuBufferDesc desc;
    desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                               MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_RING_BUFFER;
    desc.byteSize = sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS;
    gsDynamicDataUniformBufferHandle_ =
        renderNodeContextMgr_->GetGpuResourceManager().Create("GSDynamicDataUBO_" + uniqueName, desc);
}

RENDER_NS::RenderHandleReference RenderNodeGsAIEditRender::CreateGpuBuffer(const BASE_NS::string &name,
                                                                           uint32_t usageFlags, uint32_t byteSize)
{
    GpuBufferDesc desc;
    desc.usageFlags = usageFlags;
    desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
    desc.byteSize = byteSize;
    return renderNodeContextMgr_->GetGpuResourceManager().Create(name, desc);
}

void RenderNodeGsAIEditRender::CreateGpuBuffers(const BASE_NS::string &uniqueName)
{
    const uint32_t storageBit = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const uint32_t vertexBit = BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    const uint32_t indirectBit = BufferUsageFlagBits::CORE_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    const uint32_t transferDstBit = BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT;

    gpuSortIndirectDrawBuffer_ = CreateGpuBuffer("GsDrawDispatchIndirect_" + uniqueName, storageBit | indirectBit,
                                                 sizeof(GsIndirectDrawDispatch) * 2u);

    gpuDummyBuffer_ = CreateGpuBuffer("GsDrawDispatchIndirectDummy_" + uniqueName, storageBit, 1u);

    gsAIEditIndicesRef_ =
        CreateGpuBuffer("GsAIEditIndices0_" + uniqueName, storageBit | vertexBit | transferDstBit, 1u);

    gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0 =
        CreateGpuBuffer("GsAIEditSortKey0_" + uniqueName, storageBit, 1u);
    gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer1 =
        CreateGpuBuffer("GsAIEditSortKey1_" + uniqueName, storageBit, 1u);
    gsHandlesCache_.gpuSortBuffers.sortCountBuffer = CreateGpuBuffer("GsAIEditSortCount_" + uniqueName, storageBit, 1u);

    gsHandlesCache_.dataStorageCacheHandle =
        CreateGpuBuffer("GsAIEditDataStorageCache_" + uniqueName, storageBit | vertexBit, 1u);

    gsHandlesCache_.viewZStoreCacheHandle =
        CreateGpuBuffer("GsAIEditViewZStore_" + uniqueName, vertexBit | storageBit, 1u);
}

void RenderNodeGsAIEditRender::LoadShaders()
{
    const IRenderNodeShaderManager &shaderMgr = renderNodeContextMgr_->GetShaderManager();

    gsRenderShaders_.precomputeProjection =
        shaderMgr.GetShaderHandle("gsshaders://"
                                  "computeshader/gs3d_precompute_projection.shader");
    gsRenderShaders_.precomputeProjectionPack =
        shaderMgr.GetShaderHandle("gsshaders://"
                                  "computeshader/gs3d_precompute_projection_pack.shader");
    gsRenderShaders_.precomputeProjectionFg =
        shaderMgr.GetShaderHandle("gsshaders://"
                                  "computeshader/gs3d_precompute_projection_fg.shader");
    gsRenderShaders_.precomputeProjectionPackFg =
        shaderMgr.GetShaderHandle("gsshaders://"
                                  "computeshader/gs3d_precompute_projection_pack_fg.shader");
    gsRenderShaders_.updateRenderStatusUbo =
        shaderMgr.GetShaderHandle("gsshaders://"
                                  "computeshader/gs3d_update_render_status_ubo.shader");

    gpuSortShaders_.sortedDataCopy = shaderMgr.GetShaderHandle("gsshaders://"
                                                               "computeshader/gs3d_rx_sorted_data_copy.shader");
    gpuSortShaders_.preSort = shaderMgr.GetShaderHandle("gsshaders://"
                                                        "computeshader/gs3d_rx_pre_sort.shader");
    gpuSortShaders_.counting = shaderMgr.GetShaderHandle("gsshaders://"
                                                         "computeshader/gs3d_rx_counting.shader");
    gpuSortShaders_.scanScatter = shaderMgr.GetShaderHandle("gsshaders://"
                                                            "computeshader/gs3d_rx_scan_scatter.shader");

    gsRenderHandle_ = shaderMgr.GetShaderHandle("gsshaders://"
                                                "shader/gs3d_gaussian_splatting_render.shader");
    gsRenderDepthHandle_ = shaderMgr.GetShaderHandle("gsshaders://"
                                                     "shader/gs3d_gaussian_splatting_render_depth.shader");
}

void RenderNodeGsAIEditRender::InitNodeLazy(RENDER_NS::IRenderNodeContextManager &renderNodeContextMgr)
{
    renderNodeContextMgr_ = &renderNodeContextMgr;
    const BASE_NS::string uniqueName(BASE_NS::to_hex(reinterpret_cast<uintptr_t>(this)));

    CreateRenderPassInternal();
    CreateUniformBuffers(uniqueName);
    CreateGpuBuffers(uniqueName);
    LoadShaders();
    RecreateDescriptorSets();
}
void RenderNodeGsAIEditRender::InitNode(IRenderNodeContextManager &renderNodeContextMgr)
{
    renderNodeContextMgr_ = &renderNodeContextMgr;
    ParseRenderNodeInputs();

    shaderToPsoData_.clear();

    IClassRegister* classRegister = renderNodeContextMgr.GetRenderContext().GetInterface<IClassRegister>();
    if (classRegister) {
        if (auto* renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
                *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
            renderNodeSceneUtil) {
            const auto &renderNodeGraphData = renderNodeContextMgr.GetRenderNodeGraphData();
            stores_ = renderNodeSceneUtil->GetSceneRenderDataStores(renderNodeContextMgr,
                                                                    renderNodeGraphData.renderNodeGraphDataStoreName);
        }
    }
}

void RenderNodeGsAIEditRender::ParseRenderNodeInputs()
{
    const IRenderNodeParserUtil &parserUtil = renderNodeContextMgr_->GetRenderNodeParserUtil();
    const auto jsonVal = renderNodeContextMgr_->GetNodeJson();
    jsonInputs_.renderPass = parserUtil.GetInputRenderPass(jsonVal, "renderPass");
    jsonInputs_.customCameraName = parserUtil.GetStringValue(jsonVal, "customCameraName");
    jsonInputs_.customCameraId = parserUtil.GetUintValue(jsonVal, "customCameraId");
}

void RenderNodeGsAIEditRender::UpdateCameraUBO(GsRenderData* renderData)
{
    if (!dataStore_) {
        return;
    }
    const IRenderDataStoreDefaultCamera* dsCamera = static_cast<IRenderDataStoreDefaultCamera*>(
        renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(stores_.dataStoreNameCamera));

    if (dsCamera == nullptr) {
        return;
    }
    auto cameras = dsCamera->GetCameras();
    Math::Mat4X4 viewMatrix;
    Math::Mat4X4 projMatrix;

    for (auto &cam : cameras) {
        if (cam.flags & RenderCamera::CAMERA_FLAG_MAIN_BIT) {
            viewMatrix = cam.matrices.view;
            projMatrix = cam.matrices.proj;

            const auto &renderNodeGraphData = renderNodeContextMgr_->GetRenderNodeGraphData();

            IRenderNodeSceneUtil* renderNodeSceneUtil = nullptr;
            IClassRegister* classRegister = renderNodeContextMgr_->GetRenderContext().GetInterface<IClassRegister>();
            if (classRegister) {
                renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
                    *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
            }
            if (renderNodeSceneUtil) {
                stores_ = renderNodeSceneUtil->GetSceneRenderDataStores(
                    *renderNodeContextMgr_, renderNodeGraphData.renderNodeGraphDataStoreName);
                viewportDesc_ = renderNodeSceneUtil->CreateViewportFromCamera(cam);
                scissorDesc_ = renderNodeSceneUtil->CreateScissorFromCamera(cam);
            }

            break;
        }
    }
    cameraProjMatrix_ = projMatrix;
    cameraViewMatrix_ = viewMatrix;
}

RENDER_NS::IRenderNode::ExecuteFlags RenderNodeGsAIEditRender::GetExecuteFlags() const
{
    if (dataStoreEdit_ && !dataStoreEdit_->GetEditState()) {
        return ExecuteFlagBits::EXECUTE_FLAG_BITS_DO_NOT_EXECUTE;
    }
    return ExecuteFlagBits::EXECUTE_FLAG_BITS_DEFAULT;
}

void RenderNodeGsAIEditRender::FillSpatialCodecData(GsDynamicDataStruct &dynamicData, const GsPrimitive &gsPrimitive)
{
    dynamicData.compressedShHeight = gsPrimitive.compressedShHeight;
    dynamicData.attribute2DSingleHeight = gsPrimitive.attribute2DSingleHeight;
    for (int jj = 0; jj < 4; jj++) {
        dynamicData.shRestMins_R[jj] =
            Math::Vec4(gsPrimitive.shRestMins_R[4 * jj], gsPrimitive.shRestMins_R[4 * jj + 1],
                       gsPrimitive.shRestMins_R[4 * jj + 2], gsPrimitive.shRestMins_R[4 * jj + 3]);
        dynamicData.shRestMaxs_R[jj] =
            Math::Vec4(gsPrimitive.shRestMaxs_R[4 * jj], gsPrimitive.shRestMaxs_R[4 * jj + 1],
                       gsPrimitive.shRestMaxs_R[4 * jj + 2], gsPrimitive.shRestMaxs_R[4 * jj + 3]);
        dynamicData.shRestMins_G[jj] =
            Math::Vec4(gsPrimitive.shRestMins_G[4 * jj], gsPrimitive.shRestMins_G[4 * jj + 1],
                       gsPrimitive.shRestMins_G[4 * jj + 2], gsPrimitive.shRestMins_G[4 * jj + 3]);
        dynamicData.shRestMaxs_G[jj] =
            Math::Vec4(gsPrimitive.shRestMaxs_G[4 * jj], gsPrimitive.shRestMaxs_G[4 * jj + 1],
                       gsPrimitive.shRestMaxs_G[4 * jj + 2], gsPrimitive.shRestMaxs_G[4 * jj + 3]);
        dynamicData.shRestMins_B[jj] =
            Math::Vec4(gsPrimitive.shRestMins_B[4 * jj], gsPrimitive.shRestMins_B[4 * jj + 1],
                       gsPrimitive.shRestMins_B[4 * jj + 2], gsPrimitive.shRestMins_B[4 * jj + 3]);
        dynamicData.shRestMaxs_B[jj] =
            Math::Vec4(gsPrimitive.shRestMaxs_B[4 * jj], gsPrimitive.shRestMaxs_B[4 * jj + 1],
                       gsPrimitive.shRestMaxs_B[4 * jj + 2], gsPrimitive.shRestMaxs_B[4 * jj + 3]);
    }
}

void RenderNodeGsAIEditRender::FillDynamicDataForPrimitive(GsDynamicDataStruct &dynamicData, GsPrimitive &gsPrimitive,
                                                           const GS::GsRenderData* renderData, size_t primitiveIndex)
{
    dynamicData.fadeSpeed = gsPrimitive.animationFadeProperties.fadeSpeed;
    dynamicData.useFadeAnimation = gsPrimitive.animationFadeProperties.useFadeAnimation ? 1u : 0u;
    dynamicData.fadeWaitOffset = gsPrimitive.animationFadeProperties.fadeWaitOffset;
    dynamicData.fadeStartPosition = gsPrimitive.animationFadeProperties.fadeStartPosition;
    dynamicData.timeToFadeFullPoints = gsPrimitive.animationFadeProperties.timeToFadeFullPoints;
    dynamicData.fadeDistanceOffset = gsPrimitive.animationFadeProperties.fadeDistanceOffset;

    dynamicData.aabbMin = gsPrimitive.cropBoxProperties.aabbMin;
    dynamicData.aabbMax = gsPrimitive.cropBoxProperties.aabbMax;
    dynamicData.useCropBox = gsPrimitive.cropBoxProperties.useCropBox ? 1u : 0u;
    dynamicData.worldMat = gsPrimitive.worldMatrix;
    gsPrimitive.prevUpdateWorldMatrix = gsPrimitive.worldMatrix;
    dynamicData.computeDepthPass = renderData->gsEnableDepthPass ? 1u : 0u;
    dynamicData.startOffset = gsPrimitive.startOffset;
    dynamicData.startShOffset = gsPrimitive.startShOffset;
    dynamicData.endOffset = gsPrimitive.startOffset + gsPrimitive.gsBufferSize;

    if (gsPrimitive.animationFadeProperties.useFadingCopyFlag) {
        dataStore_->DisableUBOCopyFlagForGsModel(static_cast<uint32_t>(primitiveIndex));
    }

    gsPrimitive.cropBoxProperties.cropBoxChanged = false;

    dynamicData.enabled = gsPrimitive.enabled ? 1u : 0u;
    gsPrimitive.prevEnabled = gsPrimitive.enabled;

    dynamicData.shDegree = gsPrimitive.shDegree;

    // codec related data
    dynamicData.codecVersion = gsPrimitive.codecVersion;
    dynamicData.compressedShWidth = gsPrimitive.compressedShWidth;
    FillSpatialCodecData(dynamicData, gsPrimitive);

    dynamicData.compressedShAstcBlock = gsPrimitive.compressedShAstcBlock;
    dynamicData.attribute2DSingleWidth = gsPrimitive.attribute2DSingleWidth;
    dynamicData.shRestMins = gsPrimitive.shRestMins;
    dynamicData.shRestMaxs = gsPrimitive.shRestMaxs;
    dynamicData.shDcMins = gsPrimitive.shDcMins;
    dynamicData.shDcMaxs = gsPrimitive.shDcMaxs;
    dynamicData.textIdOffset = gsPrimitive.textIdOffset;
    dynamicData.attribute2DTexNum = gsPrimitive.attribute2DTexNum;
    dynamicData.frameGenerationEnabled = renderData->enableFrameGeneration ? 1u : 0u;
}

bool RenderNodeGsAIEditRender::UpdateDynamicDataUBO(IRenderCommandList &cmdList, bool forceUpdate)
{
    if (!dataStore_ || !renderNodeContextMgr_) {
        return false;
    }
    IRenderNodeGpuResourceManager &gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    if (gsDynamicDatas_.size() != GS_MAX_NUM_GS_OBJECTS) {
        gsDynamicDatas_ = BASE_NS::vector<GsDynamicDataStruct>(GS_MAX_NUM_GS_OBJECTS);
        uint8_t* const mappedData =
            static_cast<uint8_t *>(gpuResourceMgr.MapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle()));
        if (mappedData) {
            CloneData(mappedData, sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS, gsDynamicDatas_.data(),
                      sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS);
            gpuResourceMgr.UnmapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle());
        }
    }

    auto &gsPrimitives = dataStore_->GetGsPrimitives();
    const GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (!renderData->areResourcesLoaded) {
        return false;
    }

    const size_t primitiveCount = Math::min(static_cast<size_t>(GS_MAX_NUM_GS_OBJECTS), gsPrimitives.size());

    if (primitiveCount >= GS_MAX_NUM_GS_OBJECTS) {
        CORE_LOG_E("RenderNodeGsAIEditRender::UpdateDynamicDataUBO: "
                   "primitiveCount >= GS_MAX_NUM_GS_OBJECTS, aborting!");
        return false;
    }

    for (size_t ii = 0; ii < primitiveCount; ii++) {
        GsDynamicDataStruct dynamicData;
        FillDynamicDataForPrimitive(dynamicData, gsPrimitives[ii], renderData, ii);
        gsDynamicDatas_[ii] = dynamicData;
    }
    for (size_t ii = primitiveCount; ii < GS_MAX_NUM_GS_OBJECTS; ii++) {
        gsDynamicDatas_[ii] = {};
    }

    uint8_t* const uboMappedData =
        static_cast<uint8_t *>(gpuResourceMgr.MapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle()));
    if (uboMappedData) {
        CloneData(uboMappedData, sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS, gsDynamicDatas_.data(),
                  sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS);
        gpuResourceMgr.UnmapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle());
    }
    return true;
}

void RenderNodeGsAIEditRender::UpdateRenderStatusUbo(IRenderCommandList &cmdList)
{
    if (!dataStore_) {
        return;
    }
    GS::GsRenderData* renderData = dataStore_->GetGsRenderData();

    auto &binders = bindersGsRender_.updateRenderStatusUbo;
    PsoData psoData = GetPsoData(binders.pl, gsRenderShaders_.updateRenderStatusUbo, 0xFFFFffff);

    RenderHandle pso;

    if (pso != psoData.psoHandle) {
        pso = psoData.psoHandle;
        cmdList.BindPipeline(psoData.psoHandle);
    }

    auto &binder0 = *binders.set0;
    binder0.BindBuffer(0, gsHandlesCache_.gaussianRenderTargetStatusUbo.GetHandle(), 0);

    cmdList.UpdateDescriptorSet(binder0.GetDescriptorSetHandle(), binder0.GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, binder0.GetDescriptorSetHandle());

    const GsRenderStatusUbo pc = {jitterRendered_ + 1u,
                                  static_cast<uint32_t>(frameIdx_),
                                  dataStore_->GetRenderScaleFactor(),
                                  renderData->convertGsColorSrgb ? 1u : 0u,
                                  dataStore_->GetDRSScale(),
                                  renderData->gsEnableDepthPass ? 1u : 0u,
                                  dataStore_->GetRenderSharpeningCoefficient(),
                                  dataStore_->GetXegUpscaleEnabled() ? 1u : 0u};
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t *>(&pc));
    cmdList.Dispatch(1, 1, 1);
}

void RenderNodeGsAIEditRender::CreateDescriptorSet(INodeContextDescriptorSetManager &descriptorSetMgr,
                                                   PipelineLayout &pl, const uint32_t bufferSetIndex,
                                                   IDescriptorSetBinder::Ptr &setBinder)
{
    const RenderHandle setDescHandle = descriptorSetMgr.CreateDescriptorSet(bufferSetIndex, pl);
    setBinder =
        descriptorSetMgr.CreateDescriptorSetBinder(setDescHandle, pl.descriptorSetLayouts[bufferSetIndex].bindings);
}
void RenderNodeGsAIEditRender::CalculateGsRenderDescriptorCounts(const IRenderNodeShaderManager &shaderMgr,
                                                                 RENDER_NS::DescriptorCounts &dc)
{
    bindersGsRender_.precomputeProjection.pl = shaderMgr.GetPipelineLayout(
        shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_precompute_projection.shaderpl"));
    CalculateDescriptorCounts(bindersGsRender_.precomputeProjection.pl, 0u, GS_MAX_NUM_GS_OBJECTS, dc);

    bindersGsRender_.updateRenderStatusUbo.pl = shaderMgr.GetPipelineLayout(
        shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_update_render_status_ubo.shaderpl"));
    CalculateDescriptorCounts(bindersGsRender_.updateRenderStatusUbo.pl, 0u, GS_MAX_NUM_GS_OBJECTS, dc);

    gsDepthRenderPipelineLayout_ = shaderMgr.GetPipelineLayout(
        shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_gaussian_splatting_render_depth.shaderpl"));
    CalculateDescriptorCounts(gsDepthRenderPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
}

void RenderNodeGsAIEditRender::CalculateGpuSortDescriptorCounts(const IRenderNodeShaderManager &shaderMgr,
                                                                RENDER_NS::DescriptorCounts &dc)
{
    bindersGpuSort_.copy.pl = shaderMgr.GetPipelineLayout(
        shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_rx_sorted_data_copy.shaderpl"));
    CalculateDescriptorCounts(bindersGpuSort_.copy.pl, 0u, GS_MAX_NUM_GS_OBJECTS, dc);

    bindersGpuSort_.preSort.pl =
        shaderMgr.GetPipelineLayout(shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_rx_pre_sort.shaderpl"));
    CalculateDescriptorCounts(bindersGpuSort_.preSort.pl, 0u, GS_MAX_NUM_GS_OBJECTS, dc);

    bindersGpuSort_.counting.pl =
        shaderMgr.GetPipelineLayout(shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_rx_counting.shaderpl"));
    CalculateDescriptorCounts(bindersGpuSort_.counting.pl, 0u, GS_MAX_NUM_GS_OBJECTS, dc);

    bindersGpuSort_.scanScatter.pl = shaderMgr.GetPipelineLayout(
        shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_rx_scan_scatter.shaderpl"));
    CalculateDescriptorCounts(bindersGpuSort_.scanScatter.pl, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
}

void RenderNodeGsAIEditRender::CalculateAllDescriptorCounts(const IRenderNodeShaderManager &shaderMgr,
                                                            RENDER_NS::DescriptorCounts &dc)
{
    CalculateGsRenderDescriptorCounts(shaderMgr, dc);
    CalculateGpuSortDescriptorCounts(shaderMgr, dc);

    gsRenderPipelineLayout_ = shaderMgr.GetPipelineLayout(
        shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_gaussian_splatting_render.shaderpl"));
    CalculateDescriptorCounts(gsRenderPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);

    CalculateMultiviewDescriptorCounts(dc, MAX_GS_VIEWPORT_COUNT);
}

void RenderNodeGsAIEditRender::RecreateDescriptorSets()
{
    if (!renderNodeContextMgr_) {
        return;
    }
    const IRenderNodeShaderManager &shaderMgr = renderNodeContextMgr_->GetShaderManager();
    auto &descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();

    {
        DescriptorCounts dc;
        CalculateAllDescriptorCounts(shaderMgr, dc);
        descriptorSetMgr.ResetAndReserve(dc);

        {
            bindersGsRender_.precomputeProjection.set0 = CreatePreComputeDescriptorSet();
            CreateDescriptorSet(descriptorSetMgr, bindersGsRender_.updateRenderStatusUbo.pl, 0u,
                                bindersGsRender_.updateRenderStatusUbo.set0);
        }

        {
            CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.copy.pl, 0u, bindersGpuSort_.copy.set0);
            bindersGpuSort_.preSort.set0 = CreatePresortDescriptorSet();
            CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.counting.pl, 0u, bindersGpuSort_.counting.set0);
            CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.scanScatter.pl, 0u, bindersGpuSort_.scanScatter.set0);
        }
    }
}
RENDER_NS::IDescriptorSetBinder::Ptr RenderNodeGsAIEditRender::CreateCameraDescriptorSet()
{
    const IRenderNodeShaderManager &shaderMgr = renderNodeContextMgr_->GetShaderManager();
    auto &descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();

    RENDER_NS::IDescriptorSetBinder::Ptr cameraSet{};
    CreateDescriptorSet(descriptorSetMgr, gsRenderPipelineLayout_, 0u, cameraSet);
    return cameraSet;
}
RENDER_NS::IDescriptorSetBinder::Ptr RenderNodeGsAIEditRender::CreatePresortDescriptorSet()
{
    const IRenderNodeShaderManager &shaderMgr = renderNodeContextMgr_->GetShaderManager();
    auto &descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();

    RENDER_NS::IDescriptorSetBinder::Ptr presortSet{};
    CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.preSort.pl, 0u, presortSet);
    return presortSet;
}
RENDER_NS::IDescriptorSetBinder::Ptr RenderNodeGsAIEditRender::CreatePreComputeDescriptorSet()
{
    const IRenderNodeShaderManager &shaderMgr = renderNodeContextMgr_->GetShaderManager();
    auto &descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();

    RENDER_NS::IDescriptorSetBinder::Ptr precomputeProjectionSet{};
    CreateDescriptorSet(descriptorSetMgr, bindersGsRender_.precomputeProjection.pl, 0u, precomputeProjectionSet);
    return precomputeProjectionSet;
}
RENDER_NS::IDescriptorSetBinder::Ptr RenderNodeGsAIEditRender::CreateDepthRenderDescriptorSet()
{
    auto &descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();

    RENDER_NS::IDescriptorSetBinder::Ptr depthRenderSet{};
    CreateDescriptorSet(descriptorSetMgr, gsDepthRenderPipelineLayout_, 0u, depthRenderSet);
    return depthRenderSet;
}
void RenderNodeGsAIEditRender::UpdateRenderResolution()
{
    if (!dataStore_ || !renderNodeContextMgr_) {
        return;
    }
    const IRenderDataStoreDefaultCamera* dsCamera = static_cast<IRenderDataStoreDefaultCamera*>(
        renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(stores_.dataStoreNameCamera));
    auto cameras = dsCamera->GetCameras();

    for (auto &cam : cameras) {
        if (cam.flags & RenderCamera::CAMERA_FLAG_MAIN_BIT) {
            IRenderNodeSceneUtil* renderNodeSceneUtil = nullptr;
            IClassRegister* classRegister = renderNodeContextMgr_->GetRenderContext().GetInterface<IClassRegister>();
            if (classRegister) {
                renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
                    *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
            }
            if (renderNodeSceneUtil) {
                const auto viewportDesc = renderNodeSceneUtil->CreateViewportFromCamera(cam);

                dataStore_->SetRenderResolution(
                    Math::UVec2(static_cast<uint32_t>(viewportDesc.width), static_cast<uint32_t>(viewportDesc.height)));
            }

            break;
        }
    }
}

RenderNodeGsAIEditRender::PsoData
RenderNodeGsAIEditRender::GetRenderPsoData(IRenderNodeContextManager &renderNodeContextMgr,
                                           const RenderHandle &shaderHandle)
{
    if (const auto iter = shaderToPsoData_.find(shaderHandle.id); iter != shaderToPsoData_.cend()) {
        return iter->second;
    } else { // new
        const auto &shaderMgr = renderNodeContextMgr.GetShaderManager();
        const RenderHandle plHandle = shaderMgr.GetPipelineLayoutHandleByShaderHandle(shaderHandle);
        const PipelineLayout pipelineLayout = shaderMgr.GetPipelineLayout(plHandle);
        const RenderHandle vidHandle = shaderMgr.GetVertexInputDeclarationHandleByShaderHandle(shaderHandle);
        const VertexInputDeclarationView vertexInputDeclaration = shaderMgr.GetVertexInputDeclarationView(vidHandle);
        const PipelineLayout &reflPipelineLayout = shaderMgr.GetReflectionPipelineLayout(shaderHandle);
        const RenderHandle graphicsStateHandle = shaderMgr.GetGraphicsStateHandleByShaderHandle(shaderHandle);

        const RenderHandle psoHandle = renderNodeContextMgr.GetPsoManager().GetGraphicsPsoHandle(
            shaderHandle, graphicsStateHandle, pipelineLayout, vertexInputDeclaration, {},
            {DYNAMIC_STATES, countof(DYNAMIC_STATES)});
        const PsoData psoData = {psoHandle, reflPipelineLayout.pushConstant};
        shaderToPsoData_[shaderHandle.id] = psoData;
        return psoData;
    }
}

RenderNodeGsAIEditRender::PsoData
RenderNodeGsAIEditRender::GetPsoData(const PipelineLayout &pl, const RenderHandle &shaderHandle, const uint32_t flags)
{
    uint64_t seed = shaderHandle.id;
    HashCombine(seed, static_cast<uint64_t>(flags));
    if (const auto iter = shaderToPsoData_.find(seed); iter != shaderToPsoData_.cend()) {
        return iter->second;
    } else { // new
        const auto &shaderMgr = renderNodeContextMgr_->GetShaderManager();
        constexpr size_t maxSpecializationFlagCount{4u};
        uint32_t specializationFlags[maxSpecializationFlagCount];
        const ShaderSpecializationConstantView &sscv = shaderMgr.GetReflectionSpecialization(shaderHandle);
        const size_t maxSpecializations = Math::min(maxSpecializationFlagCount, sscv.constants.size());
        for (size_t idx = 0; idx < maxSpecializations; ++idx) {
            const auto &ref = sscv.constants[idx];
            const uint32_t constantId = ref.offset / sizeof(uint32_t);

            // only compute shader stage
            switch (ref.id) {
                case 0u:
                    specializationFlags[constantId] = flags;
                    break;
                default:
                    break;
            }
        }

        const ShaderSpecializationConstantDataView spec{{sscv.constants.data(), maxSpecializations},
                                                        {specializationFlags, maxSpecializations}};
        const RenderHandle psoHandle =
            renderNodeContextMgr_->GetPsoManager().GetComputePsoHandle(shaderHandle, pl, spec);
        const PsoData psoData = {psoHandle, pl.pushConstant};
        shaderToPsoData_[seed] = psoData;
        return psoData;
    }
}

bool RenderNodeGsAIEditRender::EnsureNodeInitialized()
{
    if (!dataStoreEdit_ && renderNodeContextMgr_) {
        dataStoreEdit_ =
            static_cast<RenderDataStoreGsEdit*>(renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(
                stores_.dataStoreNamePrefix + GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX));
    }
    if (dataStoreEdit_ == nullptr) {
        return false;
    }
    if (!dataStoreEdit_->GetEditState()) {
        return false;
    }
    if (!hasInit_) {
        InitNodeLazy(*renderNodeContextMgr_);
        hasInit_ = true;
    }

    if (!dataStore_ && renderNodeContextMgr_) {
        dataStore_ = static_cast<RenderDataStoreDefaultGS*>(
            renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(
                stores_.dataStoreNamePrefix + DEFAULT_GS_RENDER_DATA_STORE_NAME_POST_FIX));
    }

    if (!dataStore_) {
        gsHandlesCache_.Invalidate();
        return false;
    }

    if (gsAIEditor_ && dataStoreAIEdit_ && !dataStoreAIEdit_->checkOperationQueueEmpty()) {
        auto op = dataStoreAIEdit_->GetFrontOperation();
        dataStoreAIEdit_->PopOperation();
        gsAIEditor_->PushOperation(op);
    }

    if (!dataStoreAIEdit_ && renderNodeContextMgr_) {
        dataStoreAIEdit_ = static_cast<RenderDataStoreGsAIEdit*>(
            renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(
                stores_.dataStoreNamePrefix + GS_AI_EDIT_RENDER_DATA_STORE_NAME_POST_FIX));
        gsAIEditor_ = GS::GsAIEditFactory::Create(renderNodeContextMgr_, this);
    }
    return true;
}

void RenderNodeGsAIEditRender::ResizeAIEditIndexBuffer(const GS::GsRenderData* renderData)
{
    IRenderNodeGpuResourceManager &gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    if (!RENDER_NS::RenderHandleUtil::IsValid(renderData->gsIndicesRef.GetHandle())) {
        return;
    }
    auto srcDesc = gpuResourceMgr.GetBufferDescriptor(renderData->gsIndicesRef.GetHandle());
    auto desc0 = gpuResourceMgr.GetBufferDescriptor(gsAIEditIndicesRef_.GetHandle());
    if (desc0.byteSize == srcDesc.byteSize) {
        return;
    }
    GpuBufferDesc newDesc;
    newDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                         BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                         BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
    newDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    newDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
    newDesc.byteSize = srcDesc.byteSize;
    const BASE_NS::string uniqueName(BASE_NS::to_hex(reinterpret_cast<uintptr_t>(this)));
    gsAIEditIndicesRef_ = gpuResourceMgr.Create("GsAIEditIndices0_" + uniqueName, newDesc);
}

void RenderNodeGsAIEditRender::CacheReadOnlyHandles(GS::GsRenderData* renderData)
{
    gsHandlesCache_.gsIndicesRef = renderData->gsIndicesRef;
    gsHandlesCache_.gsIndicesRef2 = renderData->gsIndicesRef2;
    gsHandlesCache_.gsSplatData = renderData->gsSplatData;
    gsHandlesCache_.gsHarmonicsDataRef = renderData->gsHarmonicsDataRef;
    gsHandlesCache_.gaussianRenderTarget0 = renderData->gaussianRenderTarget0;
    gsHandlesCache_.gaussianRenderTarget1 = renderData->gaussianRenderTarget1;
    gsHandlesCache_.gaussianRenderTarget2 = renderData->gaussianRenderTarget2;
    gsHandlesCache_.gaussianRenderTarget3 = renderData->gaussianRenderTarget3;
    gsHandlesCache_.gaussianRenderTargetBB = renderData->gaussianRenderTargetBB;
    gsHandlesCache_.depthStoreCacheHandle = renderData->depthStoreCacheHandle;
    gsHandlesCache_.gaussianRenderTargetStatusUbo = renderData->gaussianRenderTargetStatusUbo;
    gsHandlesCache_.gsShASTCDataRefs = renderData->gsShASTCDataRefs;
    gsHandlesCache_.sampler = renderData->sampler;
}

void RenderNodeGsAIEditRender::ResizeSortKeyBuffers(GS::GsRenderData* renderData, const BASE_NS::string &uniqueName)
{
    if (!renderData->CanUseGpuSort()) {
        return;
    }
    IRenderNodeGpuResourceManager &gpuMgr = renderNodeContextMgr_->GetGpuResourceManager();
    const auto srcSortKey0Desc = gpuMgr.GetBufferDescriptor(renderData->gpuSortBuffers.sortKeyIndexBuffer0.GetHandle());
    const auto srcSortKey1Desc = gpuMgr.GetBufferDescriptor(renderData->gpuSortBuffers.sortKeyIndexBuffer1.GetHandle());
    const auto srcSortCountDesc = gpuMgr.GetBufferDescriptor(renderData->gpuSortBuffers.sortCountBuffer.GetHandle());

    auto &sortKey0 = gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0;
    auto &sortKey1 = gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer1;
    auto &sortCount = gsHandlesCache_.gpuSortBuffers.sortCountBuffer;

    const auto curSortKey0Desc = gpuMgr.GetBufferDescriptor(sortKey0.GetHandle());
    const auto curSortKey1Desc = gpuMgr.GetBufferDescriptor(sortKey1.GetHandle());
    const auto curSortCountDesc = gpuMgr.GetBufferDescriptor(sortCount.GetHandle());

    const uint32_t storageBit = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if (curSortKey0Desc.byteSize != srcSortKey0Desc.byteSize && srcSortKey0Desc.byteSize > 0) {
        sortKey0 = CreateGpuBuffer("GsAIEditSortKey0_" + uniqueName, storageBit, srcSortKey0Desc.byteSize);
    }
    if (curSortKey1Desc.byteSize != srcSortKey1Desc.byteSize && srcSortKey1Desc.byteSize > 0) {
        sortKey1 = CreateGpuBuffer("GsAIEditSortKey1_" + uniqueName, storageBit, srcSortKey1Desc.byteSize);
    }
    if (curSortCountDesc.byteSize != srcSortCountDesc.byteSize && srcSortCountDesc.byteSize > 0) {
        sortCount = CreateGpuBuffer("GsAIEditSortCount_" + uniqueName, storageBit, srcSortCountDesc.byteSize);
    }
}

void RenderNodeGsAIEditRender::ResizeDataStorageBuffers(GS::GsRenderData* renderData, const BASE_NS::string &uniqueName)
{
    IRenderNodeGpuResourceManager &gpuMgr = renderNodeContextMgr_->GetGpuResourceManager();
    const auto srcDataStorageDesc = gpuMgr.GetBufferDescriptor(renderData->dataStorageCacheHandle.GetHandle());
    const auto srcViewZDesc = gpuMgr.GetBufferDescriptor(renderData->viewZStoreCacheHandle.GetHandle());
    auto &dataStorage = gsHandlesCache_.dataStorageCacheHandle;
    auto &viewZStore = gsHandlesCache_.viewZStoreCacheHandle;
    const auto curDataStorageDesc = gpuMgr.GetBufferDescriptor(dataStorage.GetHandle());
    const auto curViewZDesc = gpuMgr.GetBufferDescriptor(viewZStore.GetHandle());

    const uint32_t storageBit = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const uint32_t vertexBit = BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    if (curDataStorageDesc.byteSize != srcDataStorageDesc.byteSize && srcDataStorageDesc.byteSize > 0) {
        dataStorage = CreateGpuBuffer("GsAIEditDataStorageCache_" + uniqueName, storageBit | vertexBit,
                                      srcDataStorageDesc.byteSize);
    }
    if (curViewZDesc.byteSize != srcViewZDesc.byteSize && srcViewZDesc.byteSize > 0) {
        viewZStore = CreateGpuBuffer("GsAIEditViewZStore_" + uniqueName, vertexBit | storageBit, srcViewZDesc.byteSize);
    }
}

void RenderNodeGsAIEditRender::CacheGsRenderHandles(GS::GsRenderData* renderData)
{
    auto lg = dataStore_->GetDataLockGuard();
    const BASE_NS::string uniqueName(BASE_NS::to_hex(reinterpret_cast<uintptr_t>(this)));
    CacheReadOnlyHandles(renderData);
    ResizeSortKeyBuffers(renderData, uniqueName);
    ResizeDataStorageBuffers(renderData, uniqueName);
    ResizeAIEditIndexBuffer(renderData);
}

void RenderNodeGsAIEditRender::AcquireSceneDepthBuffer(GS::GsRenderData* renderData)
{
    IRenderNodeGpuResourceManager &gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    if (gpuResourceMgr.IsValid(sceneDepthBufferHandle_) || !renderData || !renderData->gsEnableDepthPass) {
        return;
    }

    const auto &renderDataStoreMgr = renderNodeContextMgr_->GetRenderDataStoreManager();

    auto dataStoreScene =
        static_cast<IRenderDataStoreDefaultScene*>(renderDataStoreMgr.GetRenderDataStore(stores_.dataStoreNameScene));

    auto dataStoreCamera = static_cast<IRenderDataStoreDefaultCamera*>(
        renderDataStoreMgr.GetRenderDataStore(stores_.dataStoreNameCamera));

    if (!dataStoreScene || !dataStoreCamera) {
        return;
    }

    const auto &scene = dataStoreScene->GetScene();
    uint32_t cameraIdx = scene.cameraIndex;
    if (jsonInputs_.customCameraId != INVALID_CAM_ID) {
        cameraIdx = dataStoreCamera->GetCameraIndex(jsonInputs_.customCameraId);
    } else if (!(jsonInputs_.customCameraName.empty())) {
        cameraIdx = dataStoreCamera->GetCameraIndex(jsonInputs_.customCameraName);
    }

    const auto &cameras = dataStoreCamera->GetCameras();
    if (cameraIdx >= cameras.size()) {
        return;
    }
    const auto &sceneCam = cameras[cameraIdx];
    if (sceneCam.depthTarget) {
        sceneDepthBufferHandle_ = sceneCam.depthTarget.GetHandle();
    } else {
        BASE_NS::string camIdName = BASE_NS::string(sceneCam.name);
        if (camIdName.empty()) {
            camIdName = BASE_NS::string(to_hex(sceneCam.id));
        }
        const auto depthTargetName =
            stores_.dataStoreNameScene + DefaultMaterialCameraConstants::CAMERA_DEPTH_PREFIX_NAME + camIdName;
        sceneDepthBufferHandle_ = gpuResourceMgr.GetImageHandle(depthTargetName);
        if (!gpuResourceMgr.IsValid(sceneDepthBufferHandle_)) {
            CORE_LOG_E("Failed to get scene depth handle with name %s", depthTargetName.c_str());
        }
    }
}

void RenderNodeGsAIEditRender::PreExecuteFrame()
{
    if (!EnsureNodeInitialized()) {
        return;
    }

    auto* renderData = dataStore_->GetGsRenderData();
    UpdateRenderResolution();

    if (renderData) {
        CacheGsRenderHandles(renderData);
    } else {
        gsHandlesCache_.Invalidate();
    }

    AcquireSceneDepthBuffer(renderData);

    UpdateCameraUBO(renderData);
}
void RenderNodeGsAIEditRender::UpdateViewportCameraUBO(MultiviewRenderDataPack* optionalMVHandle)
{
    auto inputViewMat = optionalMVHandle->camMat.view;
    auto inputProjMat = optionalMVHandle->camMat.proj;

    GsCameraUboStruct cameraUboStruct{};
    cameraUboStruct.view = inputViewMat;
    cameraUboStruct.proj = inputProjMat;
    cameraUboStruct.viewProj = cameraUboStruct.proj * cameraUboStruct.view;
    cameraUboStruct.viewInv = Math::Inverse(cameraUboStruct.view);
    cameraUboStruct.viewportSize = Math::Vec2(viewportDesc_.width, viewportDesc_.height);

    const auto &renderDataStoreMgr = renderNodeContextMgr_->GetRenderDataStoreManager();
    auto dataStoreScene =
        GetRenderDataStore<IRenderDataStoreDefaultScene>(renderDataStoreMgr, stores_.dataStoreNameScene);
    const auto scene = dataStoreScene->GetScene();

    cameraUboStruct.frameIdx = scene.frameIndex;
    cameraUboStruct.totalTickTimeSeconds = scene.totalTime;
    cameraUboStruct.deltaTime = scene.deltaTime;

    auto camHandle = optionalMVHandle->camBuffer;
    IRenderNodeGpuResourceManager &gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    uint8_t* const uboMappedData = reinterpret_cast<uint8_t *>(gpuResourceMgr.MapBuffer(camHandle.GetHandle()));

    if (uboMappedData) {
        CloneData(uboMappedData, sizeof(GsCameraUboStruct), &cameraUboStruct, sizeof(GsCameraUboStruct));
        gpuResourceMgr.UnmapBuffer(camHandle.GetHandle());
    } else {
        CORE_LOG_E("RenderNodeGsAIEditRender::ExecuteFrameWithViewport: Failed to map Camera UBO!");
    }
}

void RenderNodeGsAIEditRender::ExecuteFrameWithViewport(RENDER_NS::IRenderCommandList &cmdList, int index,
                                                        MultiviewRenderDataPack* optionalMVHandle)
{
    if (!dataStore_) {
        return;
    }

    UpdateViewportCameraUBO(optionalMVHandle);

    GsCameraUboStruct cameraUboStruct{};
    cameraUboStruct.view = optionalMVHandle->camMat.view;

    ExecuteGpuSort(cmdList, cameraUboStruct.view, optionalMVHandle);
    OutBarrier(cmdList);
    ExecuteGpuSortPopulate(cmdList, optionalMVHandle);
    OutBarrier(cmdList);

    GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    PrecomputeGsFrameData(cmdList, renderData, cameraUboStruct.view, optionalMVHandle);
    OutBarrier(cmdList);
    GS::MultiviewRenderContext mvContext{index, optionalMVHandle};
    ExecuteGaussianRenderPassWithAttachment(cmdList, cameraUboStruct.view, renderData, mvContext);
}
void RenderNodeGsAIEditRender::ExecuteFrame(IRenderCommandList &cmdList)
{

    if (!dataStoreEdit_ || !dataStoreEdit_->GetEditState()) {
        return;
    }

    if (gsAIEditor_ && dataStoreAIEdit_) {
        gsAIEditor_->Update(cmdList);
    }
}

void RenderNodeGsAIEditRender::DispatchPrecomputeProjection(RENDER_NS::IRenderCommandList &cmdList,
                                                            const PsoData &psoData,
                                                            MultiviewRenderDataPack* optionalMVHandle, uint32_t numGs)
{
    auto camHandle = optionalMVHandle->camBuffer;
    RENDER_NS::IDescriptorSetBinder::Ptr &prepassSet = optionalMVHandle->preComputeProjectionSet;
    auto indexBufferHandle = gsAIEditIndicesRef_.GetHandle();
    auto depthStoreCacheHandle = optionalMVHandle->depthStoreTexture;

    prepassSet->BindBuffer(0u, gsHandlesCache_.gsSplatData.GetHandle(), 0);
    prepassSet->BindBuffer(1u, indexBufferHandle, 0);
    prepassSet->BindBuffer(2u, gsHandlesCache_.dataStorageCacheHandle.GetHandle(), 0);
    prepassSet->BindBuffer(3u, camHandle.GetHandle(), 0);
    prepassSet->BindBuffer(4u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
    prepassSet->BindImage(5u, depthStoreCacheHandle.GetHandle());
    prepassSet->BindBuffer(6u, gsHandlesCache_.viewZStoreCacheHandle.GetHandle(), 0);
    prepassSet->BindBuffer(7u, gpuDummyBuffer_.GetHandle(), 0);

    const auto &set0Handle = prepassSet->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, prepassSet->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, set0Handle);

    GsRenderPushConstantStruct pc{};
    pc.viewportSize.x =
        static_cast<float>(scissorDesc_.extentWidth) / static_cast<float>(dataStore_->GetRenderScaleFactor());
    pc.viewportSize.y =
        static_cast<float>(scissorDesc_.extentHeight) / static_cast<float>(dataStore_->GetRenderScaleFactor());
    pc.jitterIndex = Math::clamp(jitterRendered_, 0u, 4u);
    pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
    pc.renderScaleFactor = dataStore_->GetRenderScaleFactor();
    pc.useIndirectDispatch = 0u;
    pc.covarianceStddev = dataStore_->GetRenderCovarianceStddev();

    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t *>(&pc));

    const uint32_t numWorkGroups = (numGs + (GS_PROCESS_DATA_TGS - 1)) / GS_PROCESS_DATA_TGS;
    cmdList.Dispatch(numWorkGroups, 1, 1);
}

void RenderNodeGsAIEditRender::PrecomputeGsFrameData(RENDER_NS::IRenderCommandList &cmdList,
                                                     GS::GsRenderData* renderData,
                                                     const BASE_NS::Math::Mat4X4 &viewMatrix,
                                                     MultiviewRenderDataPack* optionalMVHandle)
{
    auto &prepassBinders = bindersGsRender_.precomputeProjection;

    const uint32_t numGs = renderData->GetTotalGsCount();

    const RenderHandle projectionShaders[] = {
        gsRenderShaders_.precomputeProjection, gsRenderShaders_.precomputeProjectionFg,
        gsRenderShaders_.precomputeProjectionPack, gsRenderShaders_.precomputeProjectionPackFg};
    uint32_t shaderIndex = 0;
    if (renderData->CanUsePackedData()) {
        shaderIndex += 2;
    }

    PsoData psoData = GetPsoData(prepassBinders.pl, projectionShaders[shaderIndex], 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    DispatchPrecomputeProjection(cmdList, psoData, optionalMVHandle, numGs);
}

void RenderNodeGsAIEditRender::ExecuteRadixCountingPass(RENDER_NS::IRenderCommandList &cmdList, uint32_t bytePos,
                                                        uint32_t totalGs)
{
    auto &binders = bindersGpuSort_.counting;
    PsoData psoData = GetPsoData(binders.pl, gpuSortShaders_.counting, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    auto &binder0 = *binders.set0;
    binder0.BindBuffer(0u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0);
    binder0.BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer1.GetHandle(), 0);
    binder0.BindBuffer(2u, gsHandlesCache_.gpuSortBuffers.sortCountBuffer.GetHandle(), 0);
    binder0.BindBuffer(3u, gpuDummyBuffer_.GetHandle(), 0);

    cmdList.UpdateDescriptorSet(binder0.GetDescriptorSetHandle(), binder0.GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, binder0.GetDescriptorSetHandle());

    const GsRadixSortPushConstantStruct pc = {bytePos, totalGs, 0xFFFFffff, false};
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t *>(&pc));

    const uint32_t globalInvocationSize =
        (totalGs / NUM_BLOCKS_PER_GROUP_RADIX) + (totalGs % NUM_BLOCKS_PER_GROUP_RADIX > 0 ? 1 : 0);
    const uint32_t numWorkGroups = (globalInvocationSize + (RADIX_SORT_BINS - 1)) / RADIX_SORT_BINS;
    cmdList.Dispatch(numWorkGroups, 1, 1);
}

void RenderNodeGsAIEditRender::ExecuteRadixScanScatterPass(RENDER_NS::IRenderCommandList &cmdList, uint32_t bytePos,
                                                           uint32_t totalGs)
{
    auto &binders = bindersGpuSort_.scanScatter;
    PsoData psoData = GetPsoData(binders.pl, gpuSortShaders_.scanScatter, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    auto &binder0 = *binders.set0;
    binder0.BindBuffer(0u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0);
    binder0.BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer1.GetHandle(), 0);
    binder0.BindBuffer(2u, gsHandlesCache_.gpuSortBuffers.sortCountBuffer.GetHandle(), 0);
    binder0.BindBuffer(3u, gpuDummyBuffer_.GetHandle(), 0);

    cmdList.UpdateDescriptorSet(binder0.GetDescriptorSetHandle(), binder0.GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, binder0.GetDescriptorSetHandle());

    const GsRadixSortPushConstantStruct pc = {bytePos, totalGs, 0xFFFFffff, false};
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t *>(&pc));

    const uint32_t globalInvocationSize =
        (totalGs / NUM_BLOCKS_PER_GROUP_RADIX) + (totalGs % NUM_BLOCKS_PER_GROUP_RADIX > 0 ? 1 : 0);
    const uint32_t numWorkGroups = (globalInvocationSize + (RADIX_SORT_BINS - 1)) / RADIX_SORT_BINS;
    cmdList.Dispatch(numWorkGroups, 1, 1);
}

void RenderNodeGsAIEditRender::ExecutePreSortPass(RENDER_NS::IRenderCommandList &cmdList,
                                                  const BASE_NS::Math::Vec2 &depthRange, uint32_t totalGs,
                                                  MultiviewRenderDataPack* optionalMVHandle)
{
    auto &preSortSetHandle = optionalMVHandle->preSortSet;
    PsoData psoData = GetPsoData(bindersGpuSort_.preSort.pl, gpuSortShaders_.preSort, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);
    preSortSetHandle->BindBuffer(0u, gsHandlesCache_.gsSplatData.GetHandle(), 0);
    preSortSetHandle->BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0);
    preSortSetHandle->BindBuffer(2u, optionalMVHandle->camBuffer.GetHandle(), 0);
    preSortSetHandle->BindBuffer(3u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
    preSortSetHandle->BindBuffer(4u, gpuDummyBuffer_.GetHandle(), 0);
    cmdList.UpdateDescriptorSet(preSortSetHandle->GetDescriptorSetHandle(),
                                preSortSetHandle->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, preSortSetHandle->GetDescriptorSetHandle());
    GsSortPushConstantStruct pc{};
    pc.depthRange = depthRange;
    pc.numGs = totalGs;
    pc.renderScaleFactor = dataStore_->GetRenderScaleFactor();
    pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
    pc.useIndirectDispatch = 0u;
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t *>(&pc));
    const uint32_t globalInvocationSize =
        (totalGs / NUM_BLOCKS_PER_GROUP_RADIX) + (totalGs % NUM_BLOCKS_PER_GROUP_RADIX > 0 ? 1 : 0);
    const uint32_t numWorkGroups = (globalInvocationSize + (RADIX_SORT_BINS - 1)) / RADIX_SORT_BINS;
    cmdList.Dispatch(numWorkGroups, 1, 1);
}

void RenderNodeGsAIEditRender::ExecuteGpuSort(RENDER_NS::IRenderCommandList &cmdList, const Math::Mat4X4 viewMatrix,
                                              MultiviewRenderDataPack* optionalMVHandle)
{
    if (!dataStore_) {
        return;
    }
    GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (!renderData) {
        return;
    }
    const BASE_NS::Math::Vec2 range = dataStore_->GetDepthRange(viewMatrix);
    const BASE_NS::Math::Vec2 depthRange = BASE_NS::Math::Vec2(Math::min(range.x, 0.0f), Math::min(range.y, 0.0f));
    const uint32_t totalGs = renderData->GetTotalGsCount();

    for (uint32_t pass = 0; pass < NUM_RADIX_PASSES; ++pass) {
        if (pass == 0) {
            ExecutePreSortPass(cmdList, depthRange, totalGs, optionalMVHandle);
            InBarrier(cmdList);
        } else {
            InBarrier(cmdList);
        }
        ExecuteRadixCountingPass(cmdList, pass, totalGs);
        InBarrier(cmdList);
        ExecuteRadixScanScatterPass(cmdList, pass, totalGs);
    }
}

void RenderNodeGsAIEditRender::BeginMultiviewRenderPass(RENDER_NS::IRenderCommandList &cmdList,
                                                        const MultiviewRenderContext &mvContext,
                                                        GS::GsRenderData* renderData, ViewportDesc &outViewportDesc,
                                                        ScissorDesc &outScissorDesc)
{
    auto &attachment = mvContext.optionalMVHandle->colorAttachment;
    mvRenderPassDescs_[mvContext.index] = gsRenderPass_;
    mvRenderPassDescs_[mvContext.index].renderPassDesc.attachmentHandles[0] = attachment.GetHandle();
    const RENDER_NS::GpuImageDesc imageDesc =
        renderNodeContextMgr_->GetGpuResourceManager().GetImageDescriptor(attachment.GetHandle());

    outViewportDesc = viewportDesc_;
    outViewportDesc.height = imageDesc.height;
    outViewportDesc.width = imageDesc.width;

    outScissorDesc = scissorDesc_;
    outScissorDesc.extentHeight = imageDesc.height;
    outScissorDesc.extentWidth = imageDesc.width;

    mvRenderPassDescs_[mvContext.index].renderPassDesc.renderArea.extentWidth = outScissorDesc.extentWidth;
    mvRenderPassDescs_[mvContext.index].renderPassDesc.renderArea.extentHeight = outScissorDesc.extentHeight;
    const uint32_t idx = static_cast<uint32_t>(mvContext.index);
    mvRenderPassDescs_[mvContext.index].renderPassDesc.attachments[0].clearValue = {
        float(idx & 1u), float(idx & 2u), float(idx & 3u), 1.f};
    cmdList.BeginRenderPass(mvRenderPassDescs_[mvContext.index].renderPassDesc,
                            mvRenderPassDescs_[mvContext.index].subpassStartIndex,
                            mvRenderPassDescs_[mvContext.index].subpassDesc);

    cmdList.SetDynamicStateScissor(outScissorDesc);
    cmdList.SetDynamicStateViewport(outViewportDesc);
}

void RenderNodeGsAIEditRender::BeginMultiviewDepthRenderPass(RENDER_NS::IRenderCommandList &cmdList,
                                                             const MultiviewRenderContext &mvContext,
                                                             GS::GsRenderData* renderData,
                                                             ViewportDesc &outViewportDesc, ScissorDesc &outScissorDesc)
{
    auto &colorAttachment = mvContext.optionalMVHandle->colorAttachment;
    auto &depthAttachment = mvContext.optionalMVHandle->depthAttachment;

    mvRenderPassDescs_[mvContext.index] = gsDepthRenderPass_;
    mvRenderPassDescs_[mvContext.index].renderPassDesc.attachmentHandles[0] = colorAttachment.GetHandle();
    mvRenderPassDescs_[mvContext.index].renderPassDesc.attachmentHandles[1] = depthAttachment.GetHandle();

    const RENDER_NS::GpuImageDesc imageDesc =
        renderNodeContextMgr_->GetGpuResourceManager().GetImageDescriptor(colorAttachment.GetHandle());

    outViewportDesc = viewportDesc_;
    outViewportDesc.height = imageDesc.height;
    outViewportDesc.width = imageDesc.width;

    outScissorDesc = scissorDesc_;
    outScissorDesc.extentHeight = imageDesc.height;
    outScissorDesc.extentWidth = imageDesc.width;

    mvRenderPassDescs_[mvContext.index].renderPassDesc.renderArea.extentWidth = outScissorDesc.extentWidth;
    mvRenderPassDescs_[mvContext.index].renderPassDesc.renderArea.extentHeight = outScissorDesc.extentHeight;
    const uint32_t idx = static_cast<uint32_t>(mvContext.index);
    mvRenderPassDescs_[mvContext.index].renderPassDesc.attachments[0].clearValue = {
        float(idx & 1u), float(idx & 2u), float(idx & 3u), 1.f};
    mvRenderPassDescs_[mvContext.index].renderPassDesc.attachments[1].clearValue = {0.0f, 0.0f, 0.0f, 0.0f};

    cmdList.BeginRenderPass(mvRenderPassDescs_[mvContext.index].renderPassDesc,
                            mvRenderPassDescs_[mvContext.index].subpassStartIndex,
                            mvRenderPassDescs_[mvContext.index].subpassDesc);
    cmdList.SetDynamicStateScissor(outScissorDesc);
    cmdList.SetDynamicStateViewport(outViewportDesc);
}

void RenderNodeGsAIEditRender::BindGsRenderDescriptorSet0(RENDER_NS::IRenderCommandList &cmdList, bool useDepth,
                                                          const MultiviewRenderContext &mvContext)
{
    if (useDepth) {
        auto &depthSet = mvContext.optionalMVHandle->depthRenderSet;
        depthSet->BindBuffer(0u, mvContext.optionalMVHandle->camBuffer.GetHandle(), 0);
        depthSet->BindBuffer(1u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
        depthSet->BindImage(2u, mvContext.optionalMVHandle->depthStoreTexture.GetHandle());
        depthSet->BindSampler(3u, gsHandlesCache_.sampler.GetHandle());
        const auto &set0Handle = depthSet->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, depthSet->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);
    } else {
        auto &camSetHandle = mvContext.optionalMVHandle->camSet;
        camSetHandle->BindBuffer(0u, mvContext.optionalMVHandle->camBuffer.GetHandle(), 0);
        camSetHandle->BindBuffer(1u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
        const auto &set0Handle = camSetHandle->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, camSetHandle->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);
    }
}

void RenderNodeGsAIEditRender::ExecuteGaussianRenderPassWithAttachment(RENDER_NS::IRenderCommandList &cmdList,
                                                                       const BASE_NS::Math::Mat4X4 viewMatrix,
                                                                       GS::GsRenderData* renderData,
                                                                       const MultiviewRenderContext &mvContext)
{
    if (!dataStore_) {
        return;
    }
    const bool useDepth = renderData && renderData->gsEnableDepthPass && mvContext.optionalMVHandle->depthAttachment &&
                          mvContext.optionalMVHandle->depthStoreTexture;
    ViewportDesc viewportDesc;
    ScissorDesc scissorDesc;
    if (useDepth) {
        BeginMultiviewDepthRenderPass(cmdList, mvContext, renderData, viewportDesc, scissorDesc);
    } else {
        BeginMultiviewRenderPass(cmdList, mvContext, renderData, viewportDesc, scissorDesc);
    }
    const RenderHandle activeShader = useDepth ? gsRenderDepthHandle_ : gsRenderHandle_;
    const PsoData psoData = GetRenderPsoData(*renderNodeContextMgr_, activeShader);
    cmdList.BindPipeline(psoData.psoHandle);
    VertexBuffer vb;
    vb.bufferHandle = gsHandlesCache_.dataStorageCacheHandle.GetHandle();
    vb.byteSize = dataStore_->GetNumActiveGs() * sizeof(Math::UVec4);
    cmdList.BindVertexBuffers({vb});
    BindGsRenderDescriptorSet0(cmdList, useDepth, mvContext);
    GsRenderPushConstantStruct pc{};
    pc.viewportSize.x = static_cast<float>(scissorDesc.extentWidth);
    pc.viewportSize.y = static_cast<float>(scissorDesc.extentHeight);
    pc.jitterIndex = 0;
    pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
    pc.renderScaleFactor = dataStore_->GetRenderScaleFactor();
    pc.covarianceStddev = dataStore_->GetRenderCovarianceStddev();
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t *>(&pc));
    cmdList.DrawIndirect(gpuSortIndirectDrawBuffer_.GetHandle(), 0u, 1u, 0u);
    cmdList.EndRenderPass();
}

void RenderNodeGsAIEditRender::ExecuteGpuSortPopulate(RENDER_NS::IRenderCommandList &cmdList,
                                                      MultiviewRenderDataPack* optionalMVHandle)
{
    const GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (!renderData) {
        return;
    }

    auto &copyBinders = bindersGpuSort_.copy;

    const PsoData psoData = GetPsoData(copyBinders.pl, gpuSortShaders_.sortedDataCopy, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    const uint32_t numGs = renderData->GetTotalGsCount();

    {
        copyBinders.set0->BindBuffer(0u, gsAIEditIndicesRef_.GetHandle(), 0);
        copyBinders.set0->BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0u);
        copyBinders.set0->BindBuffer(2u, gpuSortIndirectDrawBuffer_.GetHandle(), 0u);
        copyBinders.set0->BindBuffer(3u, gpuDummyBuffer_.GetHandle(), 0u);

        const auto &set0Handle = copyBinders.set0->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, copyBinders.set0->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0u, set0Handle);
    }

    constexpr PushConstant pc{ShaderStageFlagBits::CORE_SHADER_STAGE_COMPUTE_BIT, sizeof(BASE_NS::Math::Vec4)};
    const BASE_NS::Math::Vec4 uPc(static_cast<float>(numGs), 0, 0, 0);
    cmdList.PushConstantData(pc, arrayviewU8(uPc));

    const uint32_t globalInvocationSize = (numGs / NUM_BLOCKS_PER_GROUP) + (numGs % NUM_BLOCKS_PER_GROUP > 0 ? 1 : 0);
    const uint32_t numWorkGroups = (globalInvocationSize + (GS_PROCESS_DATA_TGS - 1)) / GS_PROCESS_DATA_TGS;

    cmdList.Dispatch(numWorkGroups, 1, 1);

    jitterRendered_ = 0;
}

// for plugin / factory interface
IRenderNode* RenderNodeGsAIEditRender::Create()
{
    return new RenderNodeGsAIEditRender();
}

void RenderNodeGsAIEditRender::Destroy(IRenderNode* instance)
{
    CORE_LOG_E("RenderNodeGsAIEditRender::Destroy");
    delete static_cast<RenderNodeGsAIEditRender *>(instance);
}

void RenderNodeGsAIEditRender::CalculateMultiviewDescriptorCounts(RENDER_NS::DescriptorCounts &dc, uint32_t count) const
{
    CalculateDescriptorCounts(gsRenderPipelineLayout_, 0u, count, dc);
    CalculateDescriptorCounts(gsDepthRenderPipelineLayout_, 0u, count, dc);
    CalculateDescriptorCounts(bindersGsRender_.precomputeProjection.pl, 0u, count, dc);
    CalculateDescriptorCounts(bindersGpuSort_.preSort.pl, 0u, count, dc);
}
} // namespace GS
