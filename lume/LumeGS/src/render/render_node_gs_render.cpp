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
#include "render_node_gs_render.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <plugin_gs/implementation_uids.h>
#include <thread>

#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <3d/implementation_uids.h>
#include <3d/namespace.h>
#include <3d/render/default_material_constants.h>
#include <3d/render/intf_render_data_store_default_camera.h>
#include <3d/render/intf_render_node_scene_util.h>
#include <base/containers/array_view.h>
#include <base/containers/string.h>
#include <base/containers/vector.h>
#include <base/math/matrix.h>
#include <base/math/matrix_util.h>
#include <base/math/vector.h>
#include <core/log.h>
#include <core/perf/cpu_perf_scope.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/datastore/intf_render_data_store_pod.h>
#include <render/datastore/render_data_store_render_pods.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_node_context_pso_manager.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/nodecontext/intf_render_node_parser_util.h>
#include <render/nodecontext/intf_render_node_util.h>



#include "util/gs_serialization.h"

namespace {
#include "app/shaders/common/gs_common.h"
std::atomic<uint32_t> gRenderNodeGsRenderCounter{0};
} // namespace

#include <algorithm>

#include "xengine_gs_backend.h"
#include "xengine_loader.h"

#if RENDER_HAS_GLES_BACKEND
#include <render/gles/intf_device_gles.h>
#endif

#if RENDER_HAS_GL_BACKEND
#include <render/gles/intf_device_gles.h>
#endif

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;

namespace GS {
namespace {
constexpr DynamicStateEnum DYNAMIC_STATES[] = { CORE_DYNAMIC_STATE_ENUM_VIEWPORT, CORE_DYNAMIC_STATE_ENUM_SCISSOR };

constexpr bool USE_CUSTOM_BARRIERS { false };
constexpr double ROTATE_THRESHOLD = 0.0008;
constexpr bool USE_DEBUG_DRAW { false };

inline uint64_t FindFirstSetBitIndex(uint64_t mask)
{
    for (uint64_t ii = 0; ii < 64u; ii++) {
        if (mask & (1uLL << ii)) {
            return ii;
        }
    }

    return 64u;
}

inline void CalculateDescriptorCounts(
    const PipelineLayout& pl, const uint32_t setIdx, const uint32_t setCount, DescriptorCounts& descriptorCounts)
{
    const auto& bindings = pl.descriptorSetLayouts[setIdx].bindings;
    descriptorCounts.counts.reserve(descriptorCounts.counts.size() + bindings.size());
    for (const auto& bindingRef : bindings) {
        descriptorCounts.counts.emplace_back(
            DescriptorCounts::TypedCount { bindingRef.descriptorType, bindingRef.descriptorCount * setCount });
    }
}

template<typename RenderDataStoreType>
RenderDataStoreType* GetRenderDataStore(
    const IRenderNodeRenderDataStoreManager& renderDataStoreManager, const string_view name)
{
    return static_cast<RenderDataStoreType*>(renderDataStoreManager.GetRenderDataStore(name));
}
} // namespace

void RenderNodeGsRender::InitNode(IRenderNodeContextManager& renderNodeContextMgr)
{
    renderNodeContextMgr_ = &renderNodeContextMgr;
    backendType_ = renderNodeContextMgr_->GetRenderContext().GetDevice().GetBackendType();
    ParseRenderNodeInputs();

    shaderToPsoData_.clear();

#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    xegBackend_ = XEngineGsFactory::CreateBackend();
    if (xegBackend_) {
        xegBackend_->InitCapabilities(renderNodeContextMgr);
    } else {
        CORE_LOG_I("InitNode: xegBackend_ is null (libxengine_gs.z.so not present or dlsym failed; "
            "XEngine disabled at runtime)");
    }
#endif

    const auto& renderNodeUtil = renderNodeContextMgr.GetRenderNodeUtil();

    // construct the render pass
    {
        RENDER_NS::RenderNodeHandles::InputRenderPass inputRenderPass;

        // color attachment
        {
            RenderNodeAttachment colorAttachment;
            colorAttachment.loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
            colorAttachment.storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue = ClearValue(0.0f, 0.0f, 0.0f, 0.0f);

            inputRenderPass.attachments.push_back(colorAttachment);
        }

        inputRenderPass.subpassCount = 1;
        inputRenderPass.subpassContents = SubpassContents::CORE_SUBPASS_CONTENTS_INLINE;
        inputRenderPass.subpassIndex = 0;

        inputRenderPass.colorAttachmentIndices.push_back(0);

        gsRenderPass_ = renderNodeUtil.CreateRenderPass(inputRenderPass);
    }

    // construct the depth variant render pass
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

        // depth
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

    IClassRegister* classRegister = renderNodeContextMgr_->GetRenderContext().GetInterface<IClassRegister>();
    if (classRegister) {
        if (auto* renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
                *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
            renderNodeSceneUtil) {
            const auto& renderNodeGraphData = renderNodeContextMgr_->GetRenderNodeGraphData();
            stores_ = renderNodeSceneUtil->GetSceneRenderDataStores(
                renderNodeContextMgr, renderNodeGraphData.renderNodeGraphDataStoreName);
        }
    }

    const auto uniqueName = BASE_NS::to_string(gRenderNodeGsRenderCounter.fetch_add(1, std::memory_order_relaxed));
    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();

    // create the fade animation, dense object output, and camera data UBOs
    {
        GpuBufferDesc desc;
        desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                   MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

        desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_RING_BUFFER;

        // camera UBO
        desc.byteSize = sizeof(GsCameraUboStruct);
        cameraUniformBufferHandle_ =
            renderNodeContextMgr_->GetGpuResourceManager().Create("GSCameraUBO_" + uniqueName, desc);

        // dynamic datas
        desc.byteSize = sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS;
        gsDynamicDataUniformBufferHandle_ =
            renderNodeContextMgr_->GetGpuResourceManager().Create("GSDynamicDataUBO_" + uniqueName, desc);

        // dense object output buffer
        desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                   MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        desc.byteSize = sizeof(GsObjectHitResult);
        denseObjectOutputBufferHandle_ =
            renderNodeContextMgr_->GetGpuResourceManager().Create("GSDenseObjectOutputBuffer" + uniqueName, desc);
    }

    // create indirect buffer for GPU sort data copy and draw
    {
        GpuBufferDesc desc;
        desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          BufferUsageFlagBits::CORE_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

        desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;

        desc.byteSize = sizeof(GsIndirectDrawDispatch) * 2u;
        gpuSortIndirectDrawBuffer_ =
            renderNodeContextMgr_->GetGpuResourceManager().Create("GsDrawDispatchIndirect_" + uniqueName, desc);
    }

    // create indirect buffer for GPU pre compute
    {
        GpuBufferDesc desc;
        desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          BufferUsageFlagBits::CORE_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

        desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

        desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;

        desc.byteSize = sizeof(GsDispatchIndirectCommand);
        preComputeIndirectDispatchBuffer_ =
            renderNodeContextMgr_->GetGpuResourceManager().Create("GsPreCompDispatchIndirect_" + uniqueName, desc);
    }

    // create indirect buffer for GPU sort data copy and draw
    {
        GpuBufferDesc desc;
        desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;

        desc.byteSize = 1u;
        gpuDummyBuffer_ =
            renderNodeContextMgr_->GetGpuResourceManager().Create("GsDrawDispatchIndirectDummy_" + uniqueName, desc);
    }

    // edges buffer
    {
        GpuBufferDesc desc;
        desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT |
                          BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT;

        desc.byteSize = sizeof(GsEdgesBufferLayout);
        desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                   MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT;

        desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;

        edgesBuffer_ = renderNodeContextMgr_->GetGpuResourceManager().Create("GSEdgesBuffer_" + uniqueName, desc);
    }

    // debug draw
    if (USE_DEBUG_DRAW) {
        GpuBufferDesc desc;
        desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                          BufferUsageFlagBits::CORE_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS |
                                   EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER |
                                   EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE;

        // dynamic datas
        desc.byteSize = sizeof(GsDebugLinesVertexStruct) * 100;
        gaussianDebugDrawBufferHandle_ = gpuResourceMgr.Create("GsDebugDrawLineSSBO_" + uniqueName, desc);
    }

    shComputationOffset_ = 0;

    // load gs data copy shader
    const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr.GetShaderManager();

    {
        gsRenderShaders_.animationPass =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_animation_default.shader");

        gsRenderShaders_.precomputeProjection =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_precompute_projection.shader");

        gsRenderShaders_.precomputeProjectionPack =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_precompute_projection_pack.shader");

        gsRenderShaders_.precomputeProjectionFg =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_precompute_projection_fg.shader");

        gsRenderShaders_.precomputeProjectionPackFg =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_precompute_projection_pack_fg.shader");

        gsRenderShaders_.computeSphericalHarmonics =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_compute_spherical_harmonics.shader");

        gsRenderShaders_.computeSphericalHarmonicsPack =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_compute_spherical_harmonics_pack.shader");

        gsRenderShaders_.updateRenderStatusUbo =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_update_render_status_ubo.shader");
    }

    // accurate sort shaders
    {
        gpuSortShaders_.sortedDataCopy =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_sorted_data_copy.shader");
        gpuSortShaders_.preSort = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_pre_sort.shader");
        gpuSortShaders_.counting = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_counting.shader");
        gpuSortShaders_.scanScatter =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_scan_scatter.shader");
    }

    // tile-specific shaders
    {
        tilePrecomputeProjectionShader_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_tile_precompute_projection.shader");
        tilePrecomputeProjectionPackedShader_ =
                shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_tile_precompute_projection_pack.shader");
        gpuSortShaders_.preSortTiled =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_pre_sort_tiled.shader");
        gpuSortShaders_.preSortPackedTiled =
                shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_pre_sort_tiled_pack.shader");
        gpuSortShaders_.sortSizeCalcTiled =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_sort_size_calc.shader");
        gpuSortShaders_.initPreSortTiled =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_init_pre_sort.shader");
        gpuSortShaders_.countingTiled =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_counting_tiled.shader");
        gpuSortShaders_.scanScatterTiled =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_rx_scan_scatter_tiled.shader");
    }

    // prefilter depth shader
    {
        prefilterDepthShaderHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_prefilter_depth.shader");
    }

    // world space normals shader
    {
        normalsShaderHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_compute_worldspace_normals.shader");
    }

    // edges shader
    {
        edgesShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_compute_edges.shader");
    }

    // dilate edges shader
    {
        dilateEdgesShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_dilate_edges.shader");
    }

    // min max edges shader
    {
        minmaxEdgesShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_find_minmax_edges.shader");
    }

    // dense object shader
    {
        denseObjectShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_find_dense_object.shader");
    }

    // debug draw
    if (USE_DEBUG_DRAW) {
        debugDrawShaderHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://shader/gs3d_gaussian_splatting_debug_draw.shader");
    }

    // prefilter depth shader
    {
        prefilterDepthShaderHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_prefilter_depth.shader");
    }

    // world space normals shader
    {
        normalsShaderHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_compute_worldspace_normals.shader");
    }

    // edges shader
    {
        edgesShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_compute_edges.shader");
    }

    // dilate edges shader
    {
        dilateEdgesShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_dilate_edges.shader");
    }

    // min max edges shader
    {
        minmaxEdgesShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_find_minmax_edges.shader");
    }

    // dense object shader
    {
        denseObjectShaderHandle_ = shaderMgr.GetShaderHandle("gsshaders://computeshader/gs3d_find_dense_object.shader");
    }

    // debug draw
    if (USE_DEBUG_DRAW) {
        debugDrawShaderHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://shader/gs3d_gaussian_splatting_debug_draw.shader");
    }

    RecreateDescriptorSets();
}

void RenderNodeGsRender::ParseRenderNodeInputs()
{
    const IRenderNodeParserUtil& parserUtil = renderNodeContextMgr_->GetRenderNodeParserUtil();
    const auto jsonVal = renderNodeContextMgr_->GetNodeJson();
    jsonInputs_.renderPass = parserUtil.GetInputRenderPass(jsonVal, "renderPass");
    jsonInputs_.customCameraName = parserUtil.GetStringValue(jsonVal, "customCameraName");
    jsonInputs_.customCameraId = parserUtil.GetUintValue(jsonVal, "customCameraId");
}

bool RenderNodeGsRender::GSNeedsSort(const BASE_NS::Math::Mat4X4& viewMatrix, const GsRenderData* renderData)
{
    bool anyLods = false;
    const bool canGpuSort = renderData->CanUseGpuSort();

    if (canGpuSort) {
        anyLods = dataStore_->GetGsRenderData()->anyComputeLods;
    }

    if (isSortingCpu_) {
        return false;
    }

    const Math::Vec3 currentCameraForward =
        Math::Normalize(Math::Vec3(viewMatrix[2u][0u], viewMatrix[2u][1u], viewMatrix[2u][2u]));
    const Math::Vec3 currentCameraPosition(viewMatrix[3u][0u], viewMatrix[3u][1u], viewMatrix[3u][2u]);

    if (Math::Dot(lastSortForwardDirection_, lastSortForwardDirection_) != 0.0f) {
        const float dotP = Math::Dot(currentCameraForward, lastSortForwardDirection_);
        const float positionDifferenceSq = Math::Distance2(currentCameraPosition, lastSortPosition_);

        if ((Math::abs(dotP - 1.0f) <= needsSortAngle_) &&
            (positionDifferenceSq <= needsSortDistance_ * needsSortDistance_)) {
            isMoving_ = false;
            return false;
        }
    }

    if (!canGpuSort || anyLods) {
        dataStore_->SetNeedsSort();
        isSortingCpu_ = true;
    } else {
        BeginGpuSort();
    }

    lastSortForwardDirection_ = currentCameraForward;
    lastSortPosition_ = currentCameraPosition;

    isMoving_ = true;
    return true;
}

void RenderNodeGsRender::checkDRS(const float* viewMatrix, GsRenderData* renderData)
{
    std::copy(currentNearPlane.begin(), currentNearPlane.end(), lastNearPlane.begin());
    currentNearPlane[0] = viewMatrix[2];
    currentNearPlane[1] = viewMatrix[6];
    currentNearPlane[2] = viewMatrix[10];
    double sum = lastNearPlane[0] * currentNearPlane[0] + lastNearPlane[1] * currentNearPlane[1] +
                 lastNearPlane[2] * currentNearPlane[2];
    double sum1 = std::sqrt(lastNearPlane[0] * lastNearPlane[0] + lastNearPlane[1] * lastNearPlane[1] +
                            lastNearPlane[2] * lastNearPlane[2]);
    double sum2 = std::sqrt(currentNearPlane[0] * currentNearPlane[0] + currentNearPlane[1] * currentNearPlane[1] +
                            currentNearPlane[2] * currentNearPlane[2]);
    // Simplify calculation for efficiency
    double cosine = sum / (sum1 * sum2);
    double angle = std::acos(std::clamp(cosine, double(-1.0), double(1.0)));
    dataStore_->SetDRSScale(renderData->dynamicResolutionScale);
    if (abs(angle) < ROTATE_THRESHOLD) {
        if (dataStore_->GetDynamicRendering() != false) {
            CORE_LOG_D("change dynamic rending state to false");
        }
        dataStore_->SetDynamicRendering(false);
    } else {
        if (dataStore_->GetDynamicRendering() != true) {
            CORE_LOG_D("change dynamic rending state to true");
        }
        dataStore_->SetDynamicRendering(true);
    }
    CORE_LOG_D("in sum:%lf sum1:%lf sum2:%lf cosine:%lf angle:%lf isMoving_:%d ScaleFactor:%f", sum, sum1, sum2, cosine,
        angle, isMoving_, dataStore_->GetDynamicRendering() ? dataStore_->GetDRSScale() : -1);
}

uint32_t RenderNodeGsRender::GetCameraIdx()
{
    if (dataStore_->GetGsPrimitives().empty()) {
        return RenderSceneDataConstants::INVALID_INDEX;
    }

    const auto& renderDataStoreMgr = renderNodeContextMgr_->GetRenderDataStoreManager();
    auto dataStoreScene =
        static_cast<IRenderDataStoreDefaultScene*>(renderDataStoreMgr.GetRenderDataStore(stores_.dataStoreNameScene));

    auto dataStoreCamera =
        static_cast<IRenderDataStoreDefaultCamera*>(renderDataStoreMgr.GetRenderDataStore(stores_.dataStoreNameCamera));
    if (!dataStoreScene || !dataStoreCamera) {
        return RenderSceneDataConstants::INVALID_INDEX;
    }

    const auto& scene = dataStoreScene->GetScene();
    uint32_t cameraIdx = scene.cameraIndex;
    if (jsonInputs_.customCameraId != INVALID_CAM_ID) {
        cameraIdx = dataStoreCamera->GetCameraIndex(jsonInputs_.customCameraId);
    } else if (!(jsonInputs_.customCameraName.empty())) {
        cameraIdx = dataStoreCamera->GetCameraIndex(jsonInputs_.customCameraName);
    }

    const auto& cameras = dataStoreCamera->GetCameras();
    if (cameraIdx >= cameras.size()) {
        return RenderSceneDataConstants::INVALID_INDEX;
    }
    auto& cam = cameras[cameraIdx];

    // Not interesed in shadow or reflection camera.
    if (cam.flags & (RenderCamera::CameraFlagBits::CAMERA_FLAG_SHADOW_BIT |
                        RenderCamera::CameraFlagBits::CAMERA_FLAG_REFLECTION_BIT)) {
        return RenderSceneDataConstants::INVALID_INDEX;
    }

    // Hackish: Require the "allow reflection" bit to recognize the actual active camera in editor.
    if ((cam.flags & RenderCamera::CameraFlagBits::CAMERA_FLAG_ALLOW_REFLECTION_BIT) == 0uLL) {
        return RenderSceneDataConstants::INVALID_INDEX;
    }

    uint64_t commonMask = ~0uLL;
    for (const auto& prim : dataStore_->GetGsPrimitives()) {
        commonMask &= prim.layerMask;
    }

    const bool haveCommonBit = (commonMask != 0uLL);
    const uint64_t cameraActiveLayerBit = haveCommonBit ? FindFirstSetBitIndex(commonMask) : 0uLL;
    if (!haveCommonBit) {
        return RenderSceneDataConstants::INVALID_INDEX;
    }

    if (!(cam.layerMask & (1uLL << cameraActiveLayerBit))) {
        return RenderSceneDataConstants::INVALID_INDEX;
    }
    return cameraIdx;
}

void RenderNodeGsRender::UpdateImagesOnResize()
{
    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    const auto uniqueName = BASE_NS::to_string(gRenderNodeGsRenderCounter.fetch_add(1, std::memory_order_relaxed));

    // prefilter
    {
        GpuImageDesc imageDesc { ImageType::CORE_IMAGE_TYPE_2D, ImageViewType::CORE_IMAGE_VIEW_TYPE_2D,
            Format::BASE_FORMAT_R16_SFLOAT, ImageTiling::CORE_IMAGE_TILING_OPTIMAL,
            ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_STORAGE_BIT |
                ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT,
            MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
            EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS,
            static_cast<uint32_t>(viewportDesc_.width), static_cast<uint32_t>(viewportDesc_.height), 1U, 1U, 1U,
            SampleCountFlagBits::CORE_SAMPLE_COUNT_1_BIT, {} };

        prefilterDepthTex_ = gpuResourceMgr.Create("GsPrefilterDepthTex" + uniqueName, imageDesc);
    }

    // world space normals
    {
        GpuImageDesc imageDesc { ImageType::CORE_IMAGE_TYPE_2D, ImageViewType::CORE_IMAGE_VIEW_TYPE_2D,
            Format::BASE_FORMAT_R16G16B16A16_SFLOAT, ImageTiling::CORE_IMAGE_TILING_OPTIMAL,
            ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_STORAGE_BIT |
                ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT,
            MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
            EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS,
            static_cast<uint32_t>(viewportDesc_.width), static_cast<uint32_t>(viewportDesc_.height), 1U, 1U, 1U,
            SampleCountFlagBits::CORE_SAMPLE_COUNT_1_BIT, {} };

        normalsTex_ = gpuResourceMgr.Create("GsScreenSpaceNormalsTex" + uniqueName, imageDesc);
    }

    // edge image
    {
        GpuImageDesc imageDesc { ImageType::CORE_IMAGE_TYPE_2D, ImageViewType::CORE_IMAGE_VIEW_TYPE_2D,
            Format::BASE_FORMAT_R8_UNORM, ImageTiling::CORE_IMAGE_TILING_OPTIMAL,
            ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_STORAGE_BIT,
            MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0,
            EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS,
            static_cast<uint32_t>(viewportDesc_.width), static_cast<uint32_t>(viewportDesc_.height), 1U, 1U, 1U,
            SampleCountFlagBits::CORE_SAMPLE_COUNT_1_BIT, {} };

        edgeTex_ = gpuResourceMgr.Create("GsEdgeTex" + uniqueName, imageDesc);

        // dilate edges image
        dilatedEdgesTex_ = gpuResourceMgr.Create("GsDilateEdgesTex" + uniqueName, imageDesc);
    }
}

bool RenderNodeGsRender::UpdateCameraUBO(GsRenderData* renderData)
{
    if (!dataStore_) {
        return false;
    }
    const IRenderDataStoreDefaultCamera* dsCamera = static_cast<IRenderDataStoreDefaultCamera*>(
        renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(stores_.dataStoreNameCamera));
    if (!dsCamera) {
        return false;
    }
    auto cameras = dsCamera->GetCameras();

    if (cameraIdx_ >= cameras.size()) {
        return false;
    }
    auto& cam = cameras[cameraIdx_];
    Math::Mat4X4 viewMatrix = cam.matrices.view;
    Math::Mat4X4 projMatrix = cam.matrices.proj;

    IRenderNodeSceneUtil* renderNodeSceneUtil = nullptr;
    IClassRegister* classRegister = renderNodeContextMgr_->GetRenderContext().GetInterface<IClassRegister>();
    if (classRegister) {
        renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
            *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
    }
    if (renderNodeSceneUtil) {
        stores_ = renderNodeSceneUtil->GetSceneRenderDataStores(
            *renderNodeContextMgr_, renderNodeContextMgr_->GetRenderNodeGraphData().renderNodeGraphDataStoreName);
        viewportDesc_ = renderNodeSceneUtil->CreateViewportFromCamera(cam);
        scissorDesc_ = renderNodeSceneUtil->CreateScissorFromCamera(cam);
    }

    GsCameraUboStruct cameraUboStruct;
    cameraUboStruct.view = viewMatrix;
    cameraUboStruct.viewProj = projMatrix * viewMatrix;
    cameraUboStruct.proj = projMatrix;
    cameraUboStruct.viewInv = Math::Inverse(viewMatrix);

    if (renderData->enableDynamicResolution) {
        checkDRS(cameraUboStruct.view.data, renderData);
    } else {
        dataStore_->SetDynamicRendering(false);
    }

    dataStore_->SetXegUpscaleEnabled(renderData->enableXegUpscale);
    dataStore_->SetXegUpscaleFactor(renderData->xegUpscaleFactor);

    cameraProjMatrix_ = projMatrix;
    cameraViewMatrix_ = viewMatrix;

#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    if (xegBackend_) {
        xegBackend_->SetCameraMatrices(cameraViewMatrix_, cameraProjMatrix_);
        xegBackend_->SetViewport(viewportDesc_);
        xegBackend_->SetRenderParams(dataStore_->GetSingleFrameGenEnable(),
                                     dataStore_->GetRenderCovarianceStddev(),
                                     dataStore_->GetXegUpscaleSharpnessInternal());
    }
#endif

    cameraUboStruct.viewportSize = Math::Vec2(viewportDesc_.width, viewportDesc_.height);
    auto dataStoreScene = GetRenderDataStore<IRenderDataStoreDefaultScene>(
        renderNodeContextMgr_->GetRenderDataStoreManager(), stores_.dataStoreNameScene);
    const auto scene = dataStoreScene->GetScene();

    if (renderData->animationSeekTimestamp != -1.0f) {
        dataStore_->SetCurrentAnimationTime(renderData->animationSeekTimestamp);
        animationRenderFrame_ = true;
        renderData->animationSeekTimestamp = -1.0f;
    }

    cameraUboStruct.frameIdx = scene.frameIndex;
    cameraUboStruct.totalTickTimeSeconds = scene.totalTime;
    cameraUboStruct.deltaTime = scene.deltaTime;
    cameraUboStruct.animationRunningTimeSeconds = dataStore_->GetCurrentAnimationTime();

    const auto now = std::chrono::high_resolution_clock::now();

    // animationTimePrevious_ needs update for RENDER_IF_DIRTY adaption
    if (renderData->isAnimationRunningChanged) {
        animationTimePrevious_ = now;
    }

    if (!renderData->isAnimationPaused && resourcesAreLoadedStateValid_) {
        const float deltaTime =
            static_cast<float>(
                std::chrono::duration_cast<std::chrono::milliseconds>(now - animationTimePrevious_).count()) /
            1000.0f;

        dataStore_->SetCurrentAnimationTime(dataStore_->GetCurrentAnimationTime() + deltaTime);
    }
    animationTimePrevious_ = now;

    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    uint8_t* const uboMappedData =
        reinterpret_cast<uint8_t*>(gpuResourceMgr.MapBuffer(cameraUniformBufferHandle_.GetHandle()));

    if (uboMappedData) {
        if (!CloneData(uboMappedData, sizeof(GsCameraUboStruct), &cameraUboStruct, sizeof(GsCameraUboStruct))) {
            CORE_LOG_E("CloneData failed for Camera UBO");
        }
        gpuResourceMgr.UnmapBuffer(cameraUniformBufferHandle_.GetHandle());
    } else {
        CORE_LOG_E("RenderNodeGsRender::UpdateCameraUBO: Failed to map Camera UBO!");
    }

    return true;
}

bool RenderNodeGsRender::UpdateDynamicDataUBO(RENDER_NS::IRenderCommandList& cmdList)
{
    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();

    if (gsDynamicDatas_.size() != GS_MAX_NUM_GS_OBJECTS) {
        gsDynamicDatas_ = BASE_NS::vector<GsDynamicDataStruct>(GS_MAX_NUM_GS_OBJECTS);

        uint8_t* const mappedData =
            static_cast<uint8_t*>(gpuResourceMgr.MapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle()));
        if (mappedData) {
            if (!CloneData(mappedData, sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS, gsDynamicDatas_.data(),
                sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS)) {
                CORE_LOG_E("CloneData failed for dynamic data UBO reset");
            }
            gpuResourceMgr.UnmapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle());
        }
    }

    auto& gsPrimitives = dataStore_->GetGsPrimitives();
    const GS::GsRenderData* renderData = dataStore_->GetGsRenderData();

    if (!renderData || !renderData->areResourcesLoaded) {
        return false;
    }

    const size_t primitiveCount = Math::min(static_cast<size_t>(GS_MAX_NUM_GS_OBJECTS), gsPrimitives.size());
    bool anyChange = uboEmpty_ || primitiveCountPrevious_ != primitiveCount;

    primitiveCountPrevious_ = primitiveCount;

    if (primitiveCount >= GS_MAX_NUM_GS_OBJECTS) {
        // previous validation should make sure this branch is never hit, but just in case
        CORE_LOG_E("RenderNodeGsRender::UpdateDynamicDataUBO: primitiveCount >= GS_MAX_NUM_GS_OBJECTS, aborting!");
        return false;
    }

    // collect changes
    for (size_t ii = 0; !anyChange && (ii < primitiveCount); ii++) {
        if (gsPrimitives[ii].prevUpdateWorldMatrix != gsPrimitives[ii].worldMatrix) {
            anyChange = true;
        }
        if (gsPrimitives[ii].animationFadeProperties.useFadingCopyFlag) {
            anyChange = true;
        }
        if (gsPrimitives[ii].cropBoxProperties.cropBoxChanged) {
            anyChange = true;
        }
        if (gsPrimitives[ii].enabled != gsPrimitives[ii].prevEnabled) {
            anyChange = true;
        }
    }

    // nothing has changed, don't bother updating
    if (!anyChange) {
        return false;
    }

    // apply changes
    for (size_t ii = 0; ii < primitiveCount; ii++) {
        GsDynamicDataStruct dynamicData;

        dynamicData.fadeSpeed = gsPrimitives[ii].animationFadeProperties.fadeSpeed;
        dynamicData.useFadeAnimation = gsPrimitives[ii].animationFadeProperties.useFadeAnimation ? 1u : 0u;
        dynamicData.fadeWaitOffset = gsPrimitives[ii].animationFadeProperties.fadeWaitOffset;

        dynamicData.fadeStartPosition = gsPrimitives[ii].animationFadeProperties.fadeStartPosition;
        dynamicData.timeToFadeFullPoints = gsPrimitives[ii].animationFadeProperties.timeToFadeFullPoints;
        dynamicData.fadeDistanceOffset = gsPrimitives[ii].animationFadeProperties.fadeDistanceOffset;

        dynamicData.aabbMin = gsPrimitives[ii].cropBoxProperties.aabbMin;
        dynamicData.aabbMax = gsPrimitives[ii].cropBoxProperties.aabbMax;
        dynamicData.useCropBox = gsPrimitives[ii].cropBoxProperties.useCropBox ? 1u : 0u;

        dynamicData.worldMat = gsPrimitives[ii].worldMatrix;
        gsPrimitives[ii].prevUpdateWorldMatrix = gsPrimitives[ii].worldMatrix;

        dynamicData.computeDepthPass = renderData->gsEnableDepthPass ? 1u : 0u;

        dynamicData.startOffset = gsPrimitives[ii].startOffset;
        dynamicData.startShOffset = gsPrimitives[ii].startShOffset;
        dynamicData.endOffset = gsPrimitives[ii].startOffset + gsPrimitives[ii].gsBufferSize;

        if (gsPrimitives[ii].animationFadeProperties.useFadingCopyFlag) {
            dataStore_->DisableUBOCopyFlagForGsModel(static_cast<uint32_t>(ii));
        }

        gsPrimitives[ii].cropBoxProperties.cropBoxChanged = false;

        dynamicData.enabled = gsPrimitives[ii].enabled ? 1u : 0u;
        gsPrimitives[ii].prevEnabled = gsPrimitives[ii].enabled;

        dynamicData.shDegree = gsPrimitives[ii].shDegree;

        // codec related data
        dynamicData.codecVersion = gsPrimitives[ii].codecVersion;
        dynamicData.compressedShWidth = gsPrimitives[ii].compressedShWidth;
        dynamicData.compressedShHeight = gsPrimitives[ii].compressedShHeight;
        dynamicData.attribute2DSingleHeight = gsPrimitives[ii].attribute2DSingleHeight;
        dynamicData.impMin = gsPrimitives[ii].impMin;
        dynamicData.impMax = gsPrimitives[ii].impMax;
        for (int jj = 0; jj < 4; jj++) {
            dynamicData.shRestMins_R[jj] =
                Math::Vec4(gsPrimitives[ii].shRestMins_R[4 * jj], gsPrimitives[ii].shRestMins_R[4 * jj + 1],
                    gsPrimitives[ii].shRestMins_R[4 * jj + 2], gsPrimitives[ii].shRestMins_R[4 * jj + 3]);
            dynamicData.shRestMaxs_R[jj] =
                Math::Vec4(gsPrimitives[ii].shRestMaxs_R[4 * jj], gsPrimitives[ii].shRestMaxs_R[4 * jj + 1],
                    gsPrimitives[ii].shRestMaxs_R[4 * jj + 2], gsPrimitives[ii].shRestMaxs_R[4 * jj + 3]);
            dynamicData.shRestMins_G[jj] =
                Math::Vec4(gsPrimitives[ii].shRestMins_G[4 * jj], gsPrimitives[ii].shRestMins_G[4 * jj + 1],
                    gsPrimitives[ii].shRestMins_G[4 * jj + 2], gsPrimitives[ii].shRestMins_G[4 * jj + 3]);
            dynamicData.shRestMaxs_G[jj] =
                Math::Vec4(gsPrimitives[ii].shRestMaxs_G[4 * jj], gsPrimitives[ii].shRestMaxs_G[4 * jj + 1],
                    gsPrimitives[ii].shRestMaxs_G[4 * jj + 2], gsPrimitives[ii].shRestMaxs_G[4 * jj + 3]);
            dynamicData.shRestMins_B[jj] =
                Math::Vec4(gsPrimitives[ii].shRestMins_B[4 * jj], gsPrimitives[ii].shRestMins_B[4 * jj + 1],
                    gsPrimitives[ii].shRestMins_B[4 * jj + 2], gsPrimitives[ii].shRestMins_B[4 * jj + 3]);
            dynamicData.shRestMaxs_B[jj] =
                Math::Vec4(gsPrimitives[ii].shRestMaxs_B[4 * jj], gsPrimitives[ii].shRestMaxs_B[4 * jj + 1],
                    gsPrimitives[ii].shRestMaxs_B[4 * jj + 2], gsPrimitives[ii].shRestMaxs_B[4 * jj + 3]);
        }

        dynamicData.compressedShAstcBlock = gsPrimitives[ii].compressedShAstcBlock;
        dynamicData.attribute2DSingleWidth = gsPrimitives[ii].attribute2DSingleWidth;
        dynamicData.shRestMins = gsPrimitives[ii].shRestMins;
        dynamicData.shRestMaxs = gsPrimitives[ii].shRestMaxs;
        dynamicData.shDcMins = gsPrimitives[ii].shDcMins;
        dynamicData.shDcMaxs = gsPrimitives[ii].shDcMaxs;
        dynamicData.textIdOffset = gsPrimitives[ii].textIdOffset;
        dynamicData.attribute2DTexNum = gsPrimitives[ii].attribute2DTexNum;
        dynamicData.frameGenerationEnabled = renderData->enableFrameGeneration ? 1u : 0u;
        gsDynamicDatas_[ii] = dynamicData;
    }

    // clear the rest of the elements
    for (size_t ii = primitiveCount; ii < GS_MAX_NUM_GS_OBJECTS; ii++) {
        gsDynamicDatas_[ii] = {};
    }

    uint8_t* const uboMappedData =
        static_cast<uint8_t*>(gpuResourceMgr.MapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle()));
    if (uboMappedData) {
        uboEmpty_ = false;
        if (!CloneData(uboMappedData, sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS, gsDynamicDatas_.data(),
            sizeof(GsDynamicDataStruct) * GS_MAX_NUM_GS_OBJECTS)) {
            CORE_LOG_E("CloneData failed for dynamic data UBO update");
        }
        gpuResourceMgr.UnmapBuffer(gsDynamicDataUniformBufferHandle_.GetHandle());
    }

    return true;
}

void RenderNodeGsRender::UpdateRenderStatusUbo(RENDER_NS::IRenderCommandList& cmdList)
{
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "UpdateRenderStatusUbo", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (!renderData) {
        return;
    }

    auto& binders = bindersGsRender_.updateRenderStatusUbo;
    PsoData psoData = GetPsoData(binders.pl, gsRenderShaders_.updateRenderStatusUbo, 0xFFFFffff);

    RenderHandle pso;

    if (pso != psoData.psoHandle) {
        pso = psoData.psoHandle;
        cmdList.BindPipeline(psoData.psoHandle);
    }

    auto& binder0 = *binders.set0;
    binder0.BindBuffer(0u, gsHandlesCache_.gaussianRenderTargetStatusUbo.GetHandle(), 0u);

    cmdList.UpdateDescriptorSet(binder0.GetDescriptorSetHandle(), binder0.GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0u, binder0.GetDescriptorSetHandle());

    const GsRenderStatusUbo pc = { jitterRendered_ + 1u, static_cast<uint32_t>(frameIdx_),
        dataStore_->GetRenderScaleFactor(), renderData->convertGsColorSrgb ? 1u : 0u, dataStore_->GetDRSScale(),
        renderData->gsEnableDepthPass ? 1u : 0u, dataStore_->GetRenderSharpeningCoefficient(),
        dataStore_->GetXegUpscaleEnabled() ? 1u : 0u };
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));
    cmdList.Dispatch(1u, 1u, 1u);
}

bool RenderNodeGsRender::IsGsAnimationRunning(const uint32_t gsIdx)
{
    const GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (renderData && renderData->useAnimations && !renderData->isAnimationPaused) {
        return true;
    }

    const auto& gsPrimitives = dataStore_->GetGsPrimitives();

    if (gsPrimitives[gsIdx].animationFadeProperties.useFadeAnimation && useRenderIfDirty_) {
        const auto& renderDataStoreMgr = renderNodeContextMgr_->GetRenderDataStoreManager();
        auto dataStoreScene =
            GetRenderDataStore<IRenderDataStoreDefaultScene>(renderDataStoreMgr, stores_.dataStoreNameScene);
        if (!dataStoreScene) {
            return false;
        }
        const auto scene = dataStoreScene->GetScene();

        const Math::Vec3 aabbMin =
            Math::Vec3(gsPrimitives[gsIdx].worldMatrix * Math::Vec4(gsPrimitives[gsIdx].aabbMin, 1.0f));
        const Math::Vec3 aabbMax =
            Math::Vec3(gsPrimitives[gsIdx].worldMatrix * Math::Vec4(gsPrimitives[gsIdx].aabbMax, 1.0f));

        float maxFadeDistance = 0.0f;
        for (int xx = 0; xx <= 1; xx++) {
            for (int yy = 0; yy <= 1; yy++) {
                for (int zz = 0; zz <= 1; zz++) {
                    const Math::Vec3 corner(
                        xx ? aabbMax.x : aabbMin.x, yy ? aabbMax.y : aabbMin.y, zz ? aabbMax.z : aabbMin.z);

                    const float distance =
                        Math::Magnitude(gsPrimitives[gsIdx].animationFadeProperties.fadeStartPosition - corner);
                    maxFadeDistance = Math::max(maxFadeDistance, distance);
                }
            }
        }

        // worst-case linear time
        const float maxTimeToFade = gsPrimitives[gsIdx].animationFadeProperties.fadeWaitOffset +
                                    (maxFadeDistance / gsPrimitives[gsIdx].animationFadeProperties.fadeSpeed);

        const float sceneTime = scene.totalTime;
        if (sceneTime < maxTimeToFade) {
            return true;
        } else {
            // this will just disable the checking code above after the animation has been completed.
            dataStore_->DisableFadeForGsModel(gsIdx);
        }
    }

    return false;
}

void RenderNodeGsRender::CreateDescriptorSet(INodeContextDescriptorSetManager& descriptorSetMgr, PipelineLayout& pl,
    const uint32_t bufferSetIndex, IDescriptorSetBinder::Ptr& setBinder)
{
    const RenderHandle setDescHandle = descriptorSetMgr.CreateDescriptorSet(bufferSetIndex, pl);
    setBinder =
        descriptorSetMgr.CreateDescriptorSetBinder(setDescHandle, pl.descriptorSetLayouts[bufferSetIndex].bindings);
}

RENDER_NS::IDescriptorSetBinder::Ptr RenderNodeGsRender::CreateCameraDescriptorSet()
{
    auto& descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();
    RENDER_NS::IDescriptorSetBinder::Ptr cameraSet {};
    CreateDescriptorSet(descriptorSetMgr, gsRenderPipelineLayout_, 0u, cameraSet);
    return cameraSet;
}

RENDER_NS::IDescriptorSetBinder::Ptr RenderNodeGsRender::CreatePresortDescriptorSet()
{
    auto& descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();
    RENDER_NS::IDescriptorSetBinder::Ptr presortSet {};
    CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.preSort.pl, 0u, presortSet);
    return presortSet;
}

RENDER_NS::IDescriptorSetBinder::Ptr RenderNodeGsRender::CreatePreComputeDescriptorSet()
{
    auto& descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();
    RENDER_NS::IDescriptorSetBinder::Ptr precomputeProjectionSet {};
    CreateDescriptorSet(descriptorSetMgr, bindersGsRender_.precomputeProjection.pl, 0u, precomputeProjectionSet);
    return precomputeProjectionSet;
}



void RenderNodeGsRender::SetupPipelineLayout(
    RENDER_NS::PipelineLayout& pl, const char* handle, const uint32_t maxCount, DescriptorCounts& dc)
{
    const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr_->GetShaderManager();
    pl = shaderMgr.GetPipelineLayout(shaderMgr.GetPipelineLayoutHandle(handle));
    CalculateDescriptorCounts(pl, 0u, maxCount, dc);
}

bool RenderNodeGsRender::IsTiledMode() const
{
    if (!dataStore_) {
        return false;
    }
    GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    return renderData && renderData->isTiled && renderData->tilePool;
}

void RenderNodeGsRender::CalculateAnimationPassCounts(DescriptorCounts& dc)
{
    const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr_->GetShaderManager();
    auto& pl = bindersGsRender_.animationPass.pl;
    pl = shaderMgr.GetReflectionPipelineLayout(gsRenderShaders_.animationPass);
    for (size_t ii = 0; ii < PipelineLayoutConstants::MAX_DESCRIPTOR_SET_COUNT; ii++) {
        if (!pl.descriptorSetLayouts[ii].bindings.empty()) {
            CalculateDescriptorCounts(pl, static_cast<uint32_t>(ii), GS_MAX_NUM_GS_OBJECTS, dc);
        }
    }
}

void RenderNodeGsRender::CalculateRenderPassCounts(DescriptorCounts& dc)
{
    SetupPipelineLayout(bindersGsRender_.precomputeProjection.pl,
        "gspipelinelayouts://gs3d_precompute_projection.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);
    SetupPipelineLayout(bindersGsRender_.computeSphericalHarmonics.pl,
        "gspipelinelayouts://gs3d_compute_spherical_harmonics.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);
    SetupPipelineLayout(bindersGsRender_.updateRenderStatusUbo.pl,
        "gspipelinelayouts://gs3d_update_render_status_ubo.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);
    SetupPipelineLayout(gsRenderPipelineLayout_, "gspipelinelayouts://gs3d_gaussian_splatting_render.shaderpl",
        GS_MAX_NUM_GS_OBJECTS, dc);
    SetupPipelineLayout(gsDepthRenderPipelineLayout_,
        "gspipelinelayouts://gs3d_gaussian_splatting_render_depth.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);
}

void RenderNodeGsRender::CalculateGpuSortCounts(DescriptorCounts& dc)
{
    SetupPipelineLayout(
        bindersGpuSort_.copy.pl, "gspipelinelayouts://gs3d_rx_sorted_data_copy.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);
    SetupPipelineLayout(
        bindersGpuSort_.preSort.pl, "gspipelinelayouts://gs3d_rx_pre_sort.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);
    SetupPipelineLayout(
        bindersGpuSort_.counting.pl, "gspipelinelayouts://gs3d_rx_counting.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);
    SetupPipelineLayout(
        bindersGpuSort_.scanScatter.pl, "gspipelinelayouts://gs3d_rx_scan_scatter.shaderpl", GS_MAX_NUM_GS_OBJECTS, dc);

    if (IsTiledMode()) {
        SetupPipelineLayout(tileMergedPreSortBinder_.pl, "gspipelinelayouts://gs3d_rx_pre_sort_tiled.shaderpl", 1u, dc);
        SetupPipelineLayout(
            tileMergedCountingBinder_.pl, "gspipelinelayouts://gs3d_rx_counting_tiled.shaderpl", 1u, dc);
        SetupPipelineLayout(
            tileMergedScanScatterBinder_.pl, "gspipelinelayouts://gs3d_rx_scan_scatter_tiled.shaderpl", 1u, dc);
    }
}

void RenderNodeGsRender::CalculateTileMergedCounts(DescriptorCounts& dc)
{
    if (!IsTiledMode()) {
        return;
    }
    SetupPipelineLayout(
        tileMergedPrecomputeBinder_.pl, "gspipelinelayouts://gs3d_tile_precompute_projection.shaderpl", 1u, dc);
    SetupPipelineLayout(
        tileMergedSortSizeCalcBinder_.pl, "gspipelinelayouts://gs3d_rx_sort_size_calc.shaderpl", 1u, dc);
    SetupPipelineLayout(tileMergedInitPreSortBinder_.pl, "gspipelinelayouts://gs3d_rx_init_pre_sort.shaderpl", 1u, dc);
}

void RenderNodeGsRender::CreateRenderPassDescriptorSets(RENDER_NS::INodeContextDescriptorSetManager& descriptorSetMgr)
{
    auto& anim = bindersGsRender_.animationPass;
    CreateDescriptorSet(descriptorSetMgr, anim.pl, 0u, anim.set0);
    if (!anim.pl.descriptorSetLayouts[1u].bindings.empty()) {
        CreateDescriptorSet(descriptorSetMgr, anim.pl, 1u, anim.set1);
    }

    CreateDescriptorSet(
        descriptorSetMgr, bindersGsRender_.precomputeProjection.pl, 0u, bindersGsRender_.precomputeProjection.set0);
    CreateDescriptorSet(descriptorSetMgr, bindersGsRender_.computeSphericalHarmonics.pl, 0u,
        bindersGsRender_.computeSphericalHarmonics.set0);
    CreateDescriptorSet(
        descriptorSetMgr, bindersGsRender_.updateRenderStatusUbo.pl, 0u, bindersGsRender_.updateRenderStatusUbo.set0);
    CreateDescriptorSet(descriptorSetMgr, gsRenderPipelineLayout_, 0u, gsRenderSet0_);
    CreateDescriptorSet(descriptorSetMgr, gsDepthRenderPipelineLayout_, 0u, gsDepthRenderSet0_);
}

void RenderNodeGsRender::CreateGpuSortDescriptorSets(RENDER_NS::INodeContextDescriptorSetManager& descriptorSetMgr)
{
    CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.copy.pl, 0u, bindersGpuSort_.copy.set0);
    CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.preSort.pl, 0u, bindersGpuSort_.preSort.set0);
    CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.counting.pl, 0u, bindersGpuSort_.counting.set0);
    CreateDescriptorSet(descriptorSetMgr, bindersGpuSort_.scanScatter.pl, 0u, bindersGpuSort_.scanScatter.set0);
}

void RenderNodeGsRender::CreateTileMergedDescriptorSets(RENDER_NS::INodeContextDescriptorSetManager& descriptorSetMgr)
{
    if (!IsTiledMode()) {
        return;
    }

    CreateDescriptorSet(descriptorSetMgr, tileMergedPrecomputeBinder_.pl, 0u, tileMergedPrecomputeBinder_.set0);
    CreateDescriptorSet(descriptorSetMgr, tileMergedPreSortBinder_.pl, 0u, tileMergedPreSortBinder_.set0);
    CreateDescriptorSet(descriptorSetMgr, tileMergedSortSizeCalcBinder_.pl, 0u, tileMergedSortSizeCalcBinder_.set0);
    CreateDescriptorSet(descriptorSetMgr, tileMergedInitPreSortBinder_.pl, 0u, tileMergedInitPreSortBinder_.set0);
    CreateDescriptorSet(descriptorSetMgr, tileMergedCountingBinder_.pl, 0u, tileMergedCountingBinder_.set0);
    CreateDescriptorSet(descriptorSetMgr, tileMergedScanScatterBinder_.pl, 0u, tileMergedScanScatterBinder_.set0);
}

void RenderNodeGsRender::RecreateDescriptorSets()
{
    if (!renderNodeContextMgr_) {
        return;
    }

    const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr_->GetShaderManager();

    DescriptorCounts dc;
    CalculateAnimationPassCounts(dc);
    CalculateRenderPassCounts(dc);
    CalculateGpuSortCounts(dc);
    CalculateTileMergedCounts(dc);

    // prefilter depth compute
    {
        prefilterDepthPipelineLayout_ = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_prefilter_depth.shaderpl"));
        CalculateDescriptorCounts(prefilterDepthPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
    }

    // world space normals compute
    {
        normalsPipelineLayout_ = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_compute_worldspace_normals.shaderpl"));
        CalculateDescriptorCounts(normalsPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
    }

    // edges compute
    {
        edgesPipelineLayout_ = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_compute_edges.shaderpl"));
        CalculateDescriptorCounts(edgesPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
    }

    // dilate edges compute
    {
        dilateEdgesPipelineLayout_ = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_dilate_edges.shaderpl"));
        CalculateDescriptorCounts(dilateEdgesPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
    }

    // min max edges compute
    {
        minmaxEdgesPipelineLayout_ = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_find_minmax_edges.shaderpl"));
        CalculateDescriptorCounts(minmaxEdgesPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
    }

    // dense object
    {
        denseObjectPipelineLayout_ = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_find_dense_object.shaderpl"));
        CalculateDescriptorCounts(denseObjectPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
    }

    // debug draw
    if (USE_DEBUG_DRAW) {
        debugDrawPipelineLayout_ = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gs3d_gaussian_splatting_debug_draw.shaderpl"));
        CalculateDescriptorCounts(debugDrawPipelineLayout_, 0u, GS_MAX_NUM_GS_OBJECTS, dc);
    }

    if (gsEditor_) {
        gsEditor_->CalculateDescriptorCounts(dc);
    }
    
    auto& descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();
    descriptorSetMgr.ResetAndReserve(dc);

    CreateRenderPassDescriptorSets(descriptorSetMgr);
    CreateGpuSortDescriptorSets(descriptorSetMgr);
    CreateTileMergedDescriptorSets(descriptorSetMgr);

    // prefilter depth
    CreateDescriptorSet(descriptorSetMgr, prefilterDepthPipelineLayout_, 0u, prefilterDepthSet0_);

    // world space normals
    CreateDescriptorSet(descriptorSetMgr, normalsPipelineLayout_, 0u, normalsSet0_);

    // edges
    CreateDescriptorSet(descriptorSetMgr, edgesPipelineLayout_, 0u, edgesSet0_);

    // dilate edges
    CreateDescriptorSet(descriptorSetMgr, dilateEdgesPipelineLayout_, 0u, dilateEdgesSet0_);

    // min max edges
    CreateDescriptorSet(descriptorSetMgr, minmaxEdgesPipelineLayout_, 0u, minmaxEdgesSet0_);

    // dense object
    CreateDescriptorSet(descriptorSetMgr, denseObjectPipelineLayout_, 0u, denseObjectSet0_);

    // debug draw
    if (USE_DEBUG_DRAW) {
        CreateDescriptorSet(descriptorSetMgr, debugDrawPipelineLayout_, 0u, debugDrawRenderSet0_);
    }

    if (gsEditor_) {
        gsEditor_->CreateDescriptorSets();
    }
}

void RenderNodeGsRender::UpdateRenderResolution(GsRenderData* renderData)
{
    if (!dataStore_) {
        return;
    }

    const IRenderDataStoreDefaultCamera* dsCamera = static_cast<IRenderDataStoreDefaultCamera*>(
        renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(stores_.dataStoreNameCamera));
    if (!dsCamera) {
        return;
    }

    auto cameras = dsCamera->GetCameras();
    if (cameraIdx_ >= cameras.size()) {
        return;
    }

    IRenderNodeSceneUtil* renderNodeSceneUtil = nullptr;
    if (IClassRegister* classRegister = renderNodeContextMgr_->GetRenderContext().GetInterface<IClassRegister>()) {
        renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
            *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
    }
    if (!renderNodeSceneUtil) {
        return;
    }
    auto& cam = cameras[cameraIdx_];
    const auto viewportDesc = renderNodeSceneUtil->CreateViewportFromCamera(cam);
    dataStore_->SetRenderResolution(
        Math::UVec2(static_cast<uint32_t>(viewportDesc.width), static_cast<uint32_t>(viewportDesc.height)));
}

uint32_t RenderNodeGsRender::GetNumEffectiveSplats()
{
    if (!dataStore_) {
        CORE_LOG_E("RenderNodeGsRender::GetNumEffectiveSplats: DataStore is null!");
        return 0;
    }

    if (!gsEditor_) {
        return dataStore_->GetNumActiveGs();
    }

    if (gsEditor_->GetNumModifyingPasses() == 0u) {
        return dataStore_->GetNumActiveGs();
    } else {
        return gsEditor_->GetEditorNumSplats();
    }
}

void RenderNodeGsRender::SetRecommended3DBox()
{
    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();

    void* mapped = gpuResourceMgr.MapBufferMemory(edgesBuffer_.GetHandle());

    GsEdgesBufferLayout result {};
    if (mapped) {
        if (!CloneData(&result, sizeof(result), mapped, sizeof(result))) {
            CORE_LOG_E("SetRecommended3DBox: CloneData failed for edges buffer!");
        }
        gpuResourceMgr.UnmapBuffer(edgesBuffer_.GetHandle());
    } else {
        CORE_LOG_E("SetRecommended3DBox: failed to map edges buffer!");
    }

    const int minX = result.minX;
    const int minY = result.minY;
    const int maxX = result.maxX;
    const int maxY = result.maxY;

    if (minX == 0x7FFFFFFF || maxX < 0 || minX > maxX || minY > maxY) {
        CORE_LOG_W("SetRecommended3DBox: no foreground region found");
        return;
    }

    const Math::Vec3 cameraForward = Math::Inverse(cameraViewMatrix_)[2];

    Math::Vec3 tL = Math::Vec3(result.worldMinX.x, result.worldMaxY.y, result.worldMinX.z);
    Math::Vec3 tR = Math::Vec3(result.worldMaxX.x, result.worldMaxY.y, result.worldMaxX.z);
    Math::Vec3 bR = Math::Vec3(result.worldMaxX.x, result.worldMinY.y, result.worldMaxX.z);
    Math::Vec3 bL = Math::Vec3(result.worldMinX.x, result.worldMinY.y, result.worldMinX.z);

    const float worldWidth = Math::Magnitude(tR - tL);
    const float worldHeight = Math::Magnitude(tL - bL);

    // The dense object hit position is reached when an opacity theshold is reached.
    // Assuming that this point will end up somewhere in the middle, we use the halfMinDim offset to displace front and
    // back planes
    const float halfMinDim = Math::min(worldWidth, worldHeight) * 0.5f;

    const Math::Vec3 forwardOffset = Math::Vec3(cameraForward.x, 0.0, cameraForward.z) * halfMinDim;

    Math::Vec3 frontTopLeft = tL + forwardOffset;
    Math::Vec3 frontTopRight = tR + forwardOffset;
    Math::Vec3 frontBottomRight = bR + forwardOffset;
    Math::Vec3 frontBottomLeft = bL + forwardOffset;

    const Math::Vec3 edgeRight = frontTopRight - frontTopLeft;
    const Math::Vec3 edgeDown = frontBottomLeft - frontTopLeft;
    const Math::Vec3 planeNormal = Math::Normalize(Math::Cross(edgeRight, edgeDown));
    const Math::Vec3 backOffset = planeNormal * halfMinDim;

    Math::Vec3 backTopLeft = frontTopLeft + backOffset;
    Math::Vec3 backTopRight = frontTopRight + backOffset;
    Math::Vec3 backBottomRight = frontBottomRight + backOffset;
    Math::Vec3 backBottomLeft = frontBottomLeft + backOffset;

    if (USE_DEBUG_DRAW) {
        gaussianDebugLines_.clear();

        auto addLine = [this](BASE_NS::Math::Vec3 a, BASE_NS::Math::Vec3 b) {
            gaussianDebugLines_.push_back(BASE_NS::Math::Vec4(a, 1.0f));
            gaussianDebugLines_.push_back(BASE_NS::Math::Vec4(b, 1.0f));
        };

        addLine({ frontTopLeft }, { frontTopRight });
        addLine({ frontTopLeft }, { frontBottomLeft });
        addLine({ frontBottomLeft }, { frontBottomRight });
        addLine({ frontBottomRight }, { frontTopRight });

        addLine({ backTopLeft }, { backTopRight });
        addLine({ backTopLeft }, { backBottomLeft });
        addLine({ backBottomLeft }, { backBottomRight });
        addLine({ backBottomRight }, { backTopRight });

        addLine({ frontTopLeft }, { backTopLeft });
        addLine({ frontTopRight }, { backTopRight });
        addLine({ frontBottomLeft }, { backBottomLeft });
        addLine({ frontBottomRight }, { backBottomRight });

        const uint32_t byteSize = static_cast<uint32_t>(gaussianDebugLines_.size()) * sizeof(GsDebugLinesVertexStruct);

        if (auto* dst =
                static_cast<uint8_t*>(gpuResourceMgr.MapBufferMemory(gaussianDebugDrawBufferHandle_.GetHandle()))) {
            if (!CloneData(dst, byteSize, gaussianDebugLines_.data(), byteSize)) {
                CORE_LOG_E("CloneData failed for debug draw buffer");
            }
            gpuResourceMgr.UnmapBuffer(gaussianDebugDrawBufferHandle_.GetHandle());
        }
    }

    dataStoreEdit_->SetRecommended3DBox(BASE_NS::pair(frontTopLeft, backBottomRight));
}

void RenderNodeGsRender::CheckEnsureEditorState()
{
    if (!dataStoreEdit_) {
        return;
    }

    if (dataStoreEdit_->GetEditState() && !gsEditor_) {
        gsEditor_ = BASE_NS::make_unique<GsEditor>(renderNodeContextMgr_);
        RecreateDescriptorSets();
        CORE_LOG_I("RenderNodeGsRender: GS Editor Created");
    } else if (!dataStoreEdit_->GetEditState() && gsEditor_) {
        gsEditor_.reset();
        // clear the stack for future use
        dataStoreEdit_->ClearEditActionStack();
        CORE_LOG_I("RenderNodeGsRender: GS Editor Destroyed");
    }
}

RenderNodeGsRender::PsoData RenderNodeGsRender::GetRenderPsoData(
    IRenderNodeContextManager& renderNodeContextMgr, const RenderHandle& shaderHandle)
{
    if (const auto iter = shaderToPsoData_.find(shaderHandle.id); iter != shaderToPsoData_.cend()) {
        return iter->second;
    } else { // new
        const auto& shaderMgr = renderNodeContextMgr.GetShaderManager();
        const RenderHandle plHandle = shaderMgr.GetPipelineLayoutHandleByShaderHandle(shaderHandle);
        const PipelineLayout pipelineLayout = shaderMgr.GetPipelineLayout(plHandle);
        const RenderHandle vidHandle = shaderMgr.GetVertexInputDeclarationHandleByShaderHandle(shaderHandle);
        const VertexInputDeclarationView vertexInputDeclaration = shaderMgr.GetVertexInputDeclarationView(vidHandle);
        const PipelineLayout& reflPipelineLayout = shaderMgr.GetReflectionPipelineLayout(shaderHandle);
        const RenderHandle graphicsStateHandle = shaderMgr.GetGraphicsStateHandleByShaderHandle(shaderHandle);

        const RenderHandle psoHandle =
            renderNodeContextMgr.GetPsoManager().GetGraphicsPsoHandle(shaderHandle, graphicsStateHandle, pipelineLayout,
                vertexInputDeclaration, {}, { DYNAMIC_STATES, countof(DYNAMIC_STATES) });
        const PsoData psoData = { psoHandle, reflPipelineLayout.pushConstant };
        shaderToPsoData_[shaderHandle.id] = psoData;
        return psoData;
    }
}

RenderNodeGsRender::PsoData RenderNodeGsRender::GetPsoData(
    const PipelineLayout& pl, const RenderHandle& shaderHandle, const uint32_t flags)
{
    uint64_t seed = shaderHandle.id;
    HashCombine(seed, static_cast<uint64_t>(flags));
    if (const auto iter = shaderToPsoData_.find(seed); iter != shaderToPsoData_.cend()) {
        return iter->second;
    } else { // new
        const auto& shaderMgr = renderNodeContextMgr_->GetShaderManager();
        constexpr size_t maxSpecializationFlagCount { 4u };
        uint32_t specializationFlags[maxSpecializationFlagCount];
        const ShaderSpecializationConstantView& sscv = shaderMgr.GetReflectionSpecialization(shaderHandle);
        const size_t maxSpecializations = Math::min(maxSpecializationFlagCount, sscv.constants.size());
        for (size_t idx = 0; idx < maxSpecializations; ++idx) {
            const auto& ref = sscv.constants[idx];
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

        const ShaderSpecializationConstantDataView spec { { sscv.constants.data(), maxSpecializations },
            { specializationFlags, maxSpecializations } };
        const RenderHandle psoHandle =
            renderNodeContextMgr_->GetPsoManager().GetComputePsoHandle(shaderHandle, pl, spec);
        const PsoData psoData = { psoHandle, pl.pushConstant };
        shaderToPsoData_[seed] = psoData;
        return psoData;
    }
}

void RenderNodeGsRender::PreExecuteFrame()
{
    bool recreateDescriptorSets = !bindersGpuSort_.copy.set0;
    if (!dataStore_ && renderNodeContextMgr_) {
        dataStore_ = static_cast<RenderDataStoreDefaultGS*>(
            renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(
                stores_.dataStoreNamePrefix + DEFAULT_GS_RENDER_DATA_STORE_NAME_POST_FIX));
        recreateDescriptorSets = true;
    }
    if (!dataStoreEdit_ && renderNodeContextMgr_) {
        dataStoreEdit_ =
            static_cast<RenderDataStoreGsEdit*>(renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(
                stores_.dataStoreNamePrefix + GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX));
        CORE_LOG_E("RenderNodeGsRender: dataStoreEdit_ Created");
    }

    if (!dataStore_) {
        gsHandlesCache_.Invalidate();
        return;
    }

    cameraIdx_ = GetCameraIdx();
    if (cameraIdx_ == RenderSceneDataConstants::INVALID_INDEX) {
        camerasValidCanRender_ = false;
        return;
    }

    if (dataStoreEdit_) {
        CheckEnsureEditorState();
    }


    auto* renderData = dataStore_->GetGsRenderData();
    if (firstLoad) {
        const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr_->GetShaderManager();
        string_view path = "gsshaders://shader/gs3d_gaussian_splatting_render.shader";
        if (renderData->enableGsOpt != 0) {
            path = "gsshaders://shader/gs3d_gaussian_splatting_render_opt.shader";
        }
        gsRenderHandle_ = shaderMgr.GetShaderHandle(path);
        gsRenderDepthHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://shader/gs3d_gaussian_splatting_render_depth.shader");
        firstLoad = false;
    }

    UpdateRenderResolution(renderData);

    // cache handles to keep them alive
    if (renderData) {
        animationRenderFrame_ = renderData->useAnimations && !renderData->isAnimationPaused;

        auto lg = dataStore_->GetDataLockGuard();

        gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0 = renderData->gpuSortBuffers.sortKeyIndexBuffer0;
        gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer1 = renderData->gpuSortBuffers.sortKeyIndexBuffer1;
        gsHandlesCache_.gpuSortBuffers.sortCountBuffer = renderData->gpuSortBuffers.sortCountBuffer;

        gsHandlesCache_.dataStorageCacheHandle = renderData->dataStorageCacheHandle;
        gsHandlesCache_.viewZStoreCacheHandle = renderData->viewZStoreCacheHandle;
        if (renderData->isTiled && renderData->tilePool) {
            const GsTilePool& pool = *renderData->tilePool;

            gsHandlesCache_.tileSplatPoolBuffer = pool.splatPoolBuffer;
            gsHandlesCache_.tileSortIndexPool0 = pool.sortIndexPool0;
            gsHandlesCache_.tileSortIndexPool1 = pool.sortIndexPool1;
            gsHandlesCache_.tileSortCountPoolBuffer = pool.sortCountPoolBuffer;
            gsHandlesCache_.tileMetaBuffer = pool.tileMetaBuffer;
            gsHandlesCache_.tileIndirectDrawBuffer = pool.tileIndirectDrawBuffer;

            if (RENDER_NS::RenderHandleUtil::IsValid(pool.tileDataStorageCacheHandle.GetHandle())) {
                gsHandlesCache_.dataStorageCacheHandle = pool.tileDataStorageCacheHandle;
            }

            if (RENDER_NS::RenderHandleUtil::IsValid(pool.tileViewZStoreCacheHandle.GetHandle())) {
                gsHandlesCache_.viewZStoreCacheHandle = pool.tileViewZStoreCacheHandle;
            }
#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
            if (xegBackend_) {
                xegBackend_->OnTiledModeActive();
            }
#endif
        }
        gsHandlesCache_.gsIndicesRef = renderData->gsIndicesRef;
        gsHandlesCache_.gsIndicesRef2 = renderData->gsIndicesRef2;
        gsHandlesCache_.gsSplatData = renderData->gsSplatData;

        if (!gsEditor_) {
            // only update if not using the editor
            gsHandlesCache_.gsSplatDataRender = renderData->gsSplatDataRender;
        }
        gsHandlesCache_.gsHarmonicsDataRef = renderData->gsHarmonicsDataRef;

        gsHandlesCache_.gaussianRenderTarget0 = renderData->gaussianRenderTarget0;
        gsHandlesCache_.gaussianRenderTarget1 = renderData->gaussianRenderTarget1;
        gsHandlesCache_.gaussianRenderTarget2 = renderData->gaussianRenderTarget2;
        gsHandlesCache_.gaussianRenderTarget3 = renderData->gaussianRenderTarget3;
        gsHandlesCache_.gaussianRenderTargetBB = renderData->gaussianRenderTargetBB;

        gsHandlesCache_.gaussianRenderTargetDepth = renderData->gaussianRenderTargetDepth1;
        gsHandlesCache_.depthStoreCacheHandle = renderData->depthStoreCacheHandle;

        gsHandlesCache_.gaussianRenderTargetStatusUbo = renderData->gaussianRenderTargetStatusUbo;

        gsHandlesCache_.customAnimationShader = renderData->customAnimationShader;
        gsHandlesCache_.customAnimationResources = renderData->customAnimationResources;

        gsHandlesCache_.gsShASTCDataRefs = renderData->gsShASTCDataRefs;
        gsHandlesCache_.sampler = renderData->sampler;

        if (renderData->tilePool && !tileMergedPreSortBinder_.set0) {
            recreateDescriptorSets = true;
        }
    } else {
        gsHandlesCache_.Invalidate();
    }

    // check if we have a custom animation shader
    if (RenderHandleUtil::IsValid(gsHandlesCache_.customAnimationShader.GetHandle())) {
        if (gsRenderShaders_.animationPass != gsHandlesCache_.customAnimationShader.GetHandle()) {
            // replace the default shader with the custom animation shader
            gsRenderShaders_.animationPass = gsHandlesCache_.customAnimationShader.GetHandle();

            const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr_->GetShaderManager();

            bindersGsRender_.animationPass.pl = shaderMgr.GetReflectionPipelineLayout(gsRenderShaders_.animationPass);

            recreateDescriptorSets = true;
        }
    }

    // acquire scene depth buffer handle
    if (!renderNodeContextMgr_->GetGpuResourceManager().IsValid(sceneDepthBufferHandle_) && renderData &&
        renderData->gsEnableDepthPass) {
        const auto& renderDataStoreMgr = renderNodeContextMgr_->GetRenderDataStoreManager();

        auto dataStoreScene = static_cast<IRenderDataStoreDefaultScene*>(
            renderDataStoreMgr.GetRenderDataStore(stores_.dataStoreNameScene));

        auto dataStoreCamera = static_cast<IRenderDataStoreDefaultCamera*>(
            renderDataStoreMgr.GetRenderDataStore(stores_.dataStoreNameCamera));

        IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();

        if (dataStoreScene && dataStoreCamera) {
            const auto& cameras = dataStoreCamera->GetCameras();
            if (cameraIdx_ < cameras.size()) {
                const auto& sceneCam = cameras[cameraIdx_];
                if (sceneCam.depthTarget) {
                    sceneDepthBufferHandle_ = sceneCam.depthTarget.GetHandle();
                } else {
                    BASE_NS::string camIdName = BASE_NS::string(sceneCam.name);
                    if (camIdName.empty()) {
                        camIdName = BASE_NS::string(to_hex(sceneCam.id));
                    } else {
                        camIdName += "_" + BASE_NS::string(to_hex(sceneCam.id));
                    }

                    const auto depthTargetName = stores_.dataStoreNameScene +
                                                 DefaultMaterialCameraConstants::CAMERA_DEPTH_PREFIX_NAME + camIdName;

                    sceneDepthBufferHandle_ = gpuResourceMgr.GetImageHandle(depthTargetName);

                    if (!gpuResourceMgr.IsValid(sceneDepthBufferHandle_)) {
                        CORE_LOG_E("Failed to get scene depth handle with name %s", depthTargetName.c_str());
                    }
                }

                if (gpuResourceMgr.IsValid(sceneDepthBufferHandle_)) {
                    const GpuImageDesc& desc = gpuResourceMgr.GetImageDescriptor(sceneDepthBufferHandle_);
                    sceneDepthBufferAspectFlags_ = gpuResourceMgr.GetImageAspectFlags(desc.format);
                }
            }
        }
    }

    if (!renderData || !UpdateCameraUBO(renderData)) {
        camerasValidCanRender_ = false;
    } else {
        camerasValidCanRender_ = true;
    }

    if ((dataStoreEdit_ && dataStoreEdit_->IsGetRecommended3DBoxFlagEnabled()) ||
        (viewportDescPrevious_.width != viewportDesc_.width || viewportDescPrevious_.height != viewportDesc_.height)) {
        UpdateImagesOnResize();
    }
    // add edit actions to editor
    if (gsEditor_ && dataStoreEdit_ && dataStoreEdit_->HasActionStackBeendUpdated()) {
        dataStoreEdit_->UpdateCameras(cameraViewMatrix_, cameraProjMatrix_);
        gsEditor_->ClearActionStack();
        for (const GsEditAction& editAction : dataStoreEdit_->GetEditActionStack()) {
            gsEditor_->PushActionToStack(editAction);
        }
    }
    if (recreateDescriptorSets) {
        RecreateDescriptorSets();
    }

    if (auto* rData = dataStore_->GetGsRenderData(); rData && rData->tilePool) {
        const GsTilePool& pool = *rData->tilePool;
        constexpr uint32_t wgElements = NUM_BLOCKS_PER_GROUP_RADIX * RADIX_SORT_BINS;

        const uint64_t wgElements64 = BASE_NS::Math::max(static_cast<uint64_t>(wgElements), static_cast<uint64_t>(1u));
        const uint64_t splatDrivenWg = (static_cast<uint64_t>(pool.sortCapacity) + wgElements64 - 1u) / wgElements64;
        const uint64_t worstCaseWg = splatDrivenWg + static_cast<uint64_t>(pool.maxTiles);
        const uint64_t uintMax = static_cast<uint64_t>(std::numeric_limits<uint32_t>::max());
        const uint32_t cappedWorstCaseWg = static_cast<uint32_t>(BASE_NS::Math::min(worstCaseWg, uintMax));

        if (!EnsureTilePreSortWgInfoCapacity(BASE_NS::Math::max(cappedWorstCaseWg, 1u))) {
            CORE_LOG_E("EnsureTilePreSortWgInfoCapacity failed");
            return;
        }
    }
}

void RenderNodeGsRender::ExecuteFrame(RENDER_NS::IRenderCommandList& cmdList)
{
    /**
     *  NOTE: Currently support for multi-gs rendering is not guaranteed to always
     *        be correct if objects with ASTC spherical harmonics textuers are used.
     *        This can probably be fixed in the future if necessary.
     */

    CORE_CPU_PERF_SCOPE("LUMEGS", "RenderNodeGsRender::ExecuteFrame", "", 0xFF00FF);

    if (!dataStore_ || !camerasValidCanRender_) {
        return;
    }

    auto& primitives = dataStore_->GetGsPrimitives();
    if (primitives.empty()) {
        dataStore_->SetDynamicRendering(false);
        return;
    }

    GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (!renderData) {
        dataStore_->SetDynamicRendering(false);
        return;
    }

    const bool gpuSortAvailable = renderData->CanUseGpuSort() && !renderData->anyComputeLods;
    const bool useCpuSort = !gpuSortAvailable;
    const bool tiledMode = renderData->isTiled && gpuSortAvailable && renderData->tilePool;

    if (!gsHandlesCache_.IsValid(gpuSortAvailable)) {
        dataStore_->SetDynamicRendering(false);
        return;
    }

    const bool resourcesWereLoaded = renderData->areResourcesLoaded;
    const bool resourcesNeedInit = !resourcesWereLoaded;
    const bool cameraMoved = cameraViewMatrix_ != cameraViewMatrixPrev_;
    const bool viewportChanged =
        viewportDescPrevious_.width != viewportDesc_.width || viewportDescPrevious_.height != viewportDesc_.height;
    const bool dynamicRenderingActive = dataStore_->GetDynamicRendering();
    const bool dynamicRenderingWasActive = lastDynamicDrawing;

    const size_t dynamicPrimitiveCount = Math::min(static_cast<size_t>(GS_MAX_NUM_GS_OBJECTS), primitives.size());
    bool sortInputChanged = uboEmpty_ || primitiveCountPrevious_ != dynamicPrimitiveCount;
    for (size_t idx = 0; !sortInputChanged && (idx < dynamicPrimitiveCount); idx++) {
        sortInputChanged = primitives[idx].prevUpdateWorldMatrix != primitives[idx].worldMatrix ||
                           primitives[idx].cropBoxProperties.cropBoxChanged ||
                           primitives[idx].enabled != primitives[idx].prevEnabled;
    }

    const bool dynamicUboUpdated = UpdateDynamicDataUBO(cmdList);

    if (gsEditor_) {
        gsEditor_->SetRenderNodeBuffers(cameraUniformBufferHandle_, gsDynamicDataUniformBufferHandle_);
    }

    if (dataStore_->GetGsScaleFactor() == 0xFFFFFFFFu) {
        // no valid data yet
        return;
    }
#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    if (xegBackend_ && xegBackend_->IsFrameGenClosed()) {
        renderData->enableFrameGeneration = false;
    }
#endif
    // Two scenarios we don't genenerate frame: a) camera has no motion; b) render resolution changes
    if (renderData->enableFrameGeneration) {
        const float scaleFactor = dataStore_->GetRenderScaleFactor();
        dataStore_->UpdateFrameGenerationState(cameraMoved, cameraViewMatrix_,
                                               Math::UVec2(static_cast<uint32_t>(viewportDesc_.width / scaleFactor),
                                                           static_cast<uint32_t>(viewportDesc_.height / scaleFactor)));
    } else {
        dataStore_->SetSingleFrameGenEnable(false);
    }

    const bool editorActive = gsEditor_ != nullptr;
    const bool editorActionPending = editorActive && dataStoreEdit_ && dataStoreEdit_->HasActionStackBeendUpdated();
    const bool animationActive = renderData->useAnimations && !renderData->isAnimationPaused;

    bool sceneDataChanged = dynamicUboUpdated || renderData->isAnimationRunningChanged || editorActionPending;
    bool sceneAnimationActive = animationActive;
    bool hasSphericalHarmonics = false;
    bool shInputsSupported = true;
    for (size_t idx = 0; idx < primitives.size(); idx++) {
        const bool primitiveAnimationActive = IsGsAnimationRunning(static_cast<uint32_t>(idx));
        sceneAnimationActive = sceneAnimationActive || primitiveAnimationActive;
        sceneDataChanged = sceneDataChanged || primitiveAnimationActive;

        if (primitives[idx].shDegree != 0u) {
            hasSphericalHarmonics = true;
            shInputsSupported = shInputsSupported && primitives[idx].platformsSupportsBlockFormat;
        }
    }

    // NOTE: this means we don't currently support SHs for LSGS, but can fix in the future!
    const bool canComputeSh = !renderData->isTiled && hasSphericalHarmonics && shInputsSupported &&
                              RENDER_NS::RenderHandleUtil::IsValid(gsRenderShaders_.computeSphericalHarmonics);

    if (!canComputeSh) {
        shComputationOffset_ = 0u;
        shFramesRendered_ = 0u;
        shFramesRequired_ = 0u;
    }

    // For generated frames, no sort or precompute needed
#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    const bool bypassGpuSort = !useCpuSort && xegBackend_ &&
        xegBackend_->ShouldBypassGpuSort(renderData->enableFrameGeneration,
                                         dataStore_->GetSingleFrameGenEnable());
#else
    const bool bypassGpuSort = false;
#endif

    bool sortViewChanged = false;
    bool sortResourcesChanged = false;
    if (!bypassGpuSort) {
        sortViewChanged = GSNeedsSort(cameraViewMatrix_, renderData);
        sortResourcesChanged = resourcesNeedInit || viewportChanged || sortInputChanged ||
                               renderData->isAnimationRunningChanged || editorActionPending;
    }
    bool cpuSortFinished = false;
    if (useCpuSort) {
        const bool requestCpuSort = sortViewChanged || sortResourcesChanged || (sceneAnimationActive && !isSortingCpu_);
        if (requestCpuSort) {
            dataStore_->SetNeedsSort();
            isSortingCpu_ = true;
        }

        if (isSortingCpu_) {
            // datastore handles the asynchronous CPU sorting and we need
            // to poll for whether the sort is done or not before we can dispatch
            // the GS data buffer population step.
            const float sortScaleFactor = dataStore_->GetRenderScaleFactor();
            cpuSortFinished =
                dataStore_->SortGaussians(cameraViewMatrix_,
                                          cameraProjMatrix_,
                                          Math::IVec2(static_cast<int32_t>(viewportDesc_.width / sortScaleFactor),
                                                      static_cast<int32_t>(viewportDesc_.height / sortScaleFactor)),
                                          indexPingPong_);

            if (cpuSortFinished) {
                isSortingCpu_ = false;
            }
        }
    } else {
        const bool requestGpuSort = sortResourcesChanged || (sceneAnimationActive && !gpuSortInProgress_);
        if (!sortViewChanged && requestGpuSort) {
            BeginGpuSort();
        }
    }

    const bool frameInputsChanged = resourcesNeedInit || cameraMoved || viewportChanged || sceneDataChanged ||
                                    sortViewChanged || editorActionPending || dynamicRenderingActive ||
                                    dynamicRenderingWasActive;
    if (frameInputsChanged || cpuSortFinished) {
        jitterRendered_ = 0u;
        if (tiledMode) {
            mergedPrecomputedJitterIndex_ = 0xFFFFFFFFu;
        }
    }

    const bool shInvalidated = canComputeSh && frameInputsChanged;
    if (shInvalidated) {
        shDirtyFlag_ = true;
        shFramesRendered_ = 0u;
    }

    lastDynamicDrawing = dynamicRenderingActive;

    if (gsEditor_ && dataStoreEdit_) {
        if (useCpuSort || renderData->anyComputeLods) {
            CORE_LOG_E("Cannot use GS editor with LODs enabled or CPU sort disabled!");
        } else {
            bool aiEditDirty = dataStoreEdit_->IsAIEditDirtyFlag();
            if (dataStoreEdit_->HasAIEditResultAvailable()) {
                dataStoreEdit_->ClearAIEditResultAvailable();
            }

            if (dataStoreEdit_->HasActionStackBeendUpdated()) {
                const auto& editStack = dataStoreEdit_->GetEditActionStack();
                if (!editStack.empty()) {
                    if (gsEditor_->ExecuteStack(cmdList)) {
                        dataStoreEdit_->MarkStackAsExecuted();
                    }
                } else {
                    gsEditor_->ExecuteStack(cmdList);
                    dataStoreEdit_->MarkStackAsExecuted();
                }
            }

            gsHandlesCache_.gsSplatDataRender = gsEditor_->GetGsEditedBuffer();
            bool buffersDifferent = dataStore_->GetGsRenderData()->gsSplatData.GetHandle() !=
                                    gsHandlesCache_.gsSplatDataRender.GetHandle();
            bool shouldExport = (buffersDifferent || aiEditDirty) && dataStoreEdit_->IsExportModelFlagEnabled();
            if (dataStoreEdit_->IsFloaterRemovalInProgress()) {
                // Floater removal pipeline is still running (PREPARE -> SORT ->
                // PEEL x15 -> COMPACT). Defer the export unconditionally -- even
                // if shouldExport is true (e.g. stale aiEditDirty from a prior
                // edit), exporting now would capture an intermediate buffer
                // state. Keep the flag set so the next frame retries once
                // COMPACT has pushed edits and ExecuteStack has replayed them.
            } else if (shouldExport) {
                if (ringIndex_ == 0) {
                    gsEditor_->BeginGsModelExport(cmdList);
                }
                if (ringIndex_ >=
                    renderNodeContextMgr_->GetRenderContext().GetDevice().GetDeviceConfiguration().bufferingCount) {
                    gsEditor_->EndGsModelExport();
                    ringIndex_ = 0;
                    if (aiEditDirty) {
                        dataStoreEdit_->ClearAIEditDirtyFlag();
                    }
                } else {
                    ++ringIndex_;
                }
            } else {
                dataStoreEdit_->UpdateExportModelStatus();
                dataStoreEdit_->TriggerExportModelCallback(false, "same model");
                dataStoreEdit_->SetExportModelFlag(false);
                dataStoreEdit_->UpdateExportModelStatus();
            }
        }
    }

    if (tiledMode && gpuSortInProgress_) {
        mergedPrecomputeInProgress_ = false;
        mergedPrecomputedJitterIndex_ = 0xFFFFFFFFu;
    }

    if (!useRenderIfDirty_ && jitterRendered_ >= 4u) {
        jitterRendered_ = 0u;
    }

    bool preprocSubmitted = false;
    bool gsFramePrecomputed = false;
    bool tiledPrecomputeCompleted = false;
    bool tiledCanRenderCurrentJitter = !tiledMode;

    const auto isShPending = [this, canComputeSh]() -> bool { return canComputeSh && (shComputationOffset_ > 0u); };

    const bool shWorkReq = canComputeSh && (shInvalidated || shDirtyFlag_ || isShPending());
    if (shWorkReq) {
        if (!shInvalidated && shDirtyFlag_) {
            // refresh all SH from zero and reset flag
            shComputationOffset_ = 0u;
            shDirtyFlag_ = false;
        }
        ExecuteComputeSphericalHarmonics(cmdList, renderData, false);
        preprocSubmitted = true;
    }
    const bool shWorkPending = isShPending();

    if (tiledMode && !bypassGpuSort) {
        const uint32_t currentPoolVersion = renderData->tilePool->resourceVersion;
        const bool poolVersionChanged = currentPoolVersion != lastSeenPoolVersion_;
        lastSeenPoolVersion_ = currentPoolVersion;

        const bool jitterFrameNeeded = !useRenderIfDirty_ || jitterRendered_ < 4u;
        const bool tiledWorkNeeded = gpuSortInProgress_ || mergedSortInitializing_ || mergedPrecomputeInProgress_ ||
                                     frameInputsChanged || cpuSortFinished || jitterFrameNeeded || poolVersionChanged;

        const bool tileSetMayHaveChanged = frameInputsChanged || cpuSortFinished || gpuSortInProgress_ ||
                                           mergedSortInitializing_ || poolVersionChanged;

        if (tiledWorkNeeded) {
            const GsTilePool& pool = *renderData->tilePool;

            if (tileSetMayHaveChanged) {
                const auto tileLock = dataStore_->GetDataLockGuard();
                BuildMergedTileBatch(cameraViewMatrix_);
            }

            if (mergedTotalActiveSplats_ > 0u) {
                bool sortFailedThisFrame = false;
                if (gpuSortInProgress_ || mergedSortInitializing_) {
                    bool sortMadeProgress = false;
                    {
                        const auto tileLock = dataStore_->GetDataLockGuard();
                        sortMadeProgress = ExecuteMergedTiledGpuSort(cmdList, pool);
                    }
                    preprocSubmitted = preprocSubmitted || sortMadeProgress;
                    if (!sortMadeProgress) {
                        ResetMergedTiledSortState();
                        sortFailedThisFrame = true;
                    }
                }

                if (radixDone_) {
                    radixDone_ = false;
                    gpuSortInProgress_ = false;
                    jitterRendered_ = 0u;
                    RestartMergedTiledPrecompute(true);
                    mergedPrecomputedJitterIndex_ = 0xFFFFFFFFu;
                    preprocSubmitted = true;
                }

                const uint32_t tiledJitterIndex = Math::clamp(jitterRendered_, 0u, 3u);
                if (!sortFailedThisFrame && !gpuSortInProgress_ &&
                    (mergedPrecomputeInProgress_ || mergedPrecomputedJitterIndex_ != tiledJitterIndex)) {
                    if (!mergedPrecomputeInProgress_) {
                        RestartMergedTiledPrecompute(false);
                    }

                    const bool precomputeNeedsMoreFrames = ExecuteMergedTiledPrecompute(cmdList, pool);
                    preprocSubmitted = true;
                    if (!precomputeNeedsMoreFrames) {
                        mergedPrecomputedJitterIndex_ = tiledJitterIndex;
                        tiledPrecomputeCompleted = true;
                    }
                }

                const bool tiledPrecomputed =
                    !mergedPrecomputeInProgress_ && mergedPrecomputedJitterIndex_ == tiledJitterIndex;
                tiledCanRenderCurrentJitter = !sortFailedThisFrame && !gpuSortInProgress_ && tiledPrecomputed;
            } else {
                ResetMergedTiledSortState();
                mergedPrecomputedJitterIndex_ = Math::clamp(jitterRendered_, 0u, 3u);
                tiledCanRenderCurrentJitter = true;
            }
        } else {
            const uint32_t tiledJitterIndex = Math::clamp(jitterRendered_, 0u, 3u);
            tiledCanRenderCurrentJitter = mergedPrecomputedJitterIndex_ == tiledJitterIndex;
        }
    }

    const bool sortWorkPendingTiled = gpuSortAvailable ? gpuSortInProgress_ : isSortingCpu_;
    const bool tiledPrecompPending = tiledMode && mergedPrecomputeInProgress_;
    const bool finalAccumBlocked = sortWorkPendingTiled || shWorkPending || tiledPrecompPending;

    const bool previewRenderReq = !useRenderIfDirty_ || cameraMoved || viewportChanged || sceneDataChanged ||
                                  resourcesNeedInit || sceneAnimationActive || dynamicRenderingActive ||
                                  dynamicRenderingWasActive || editorActive || shWorkPending;
    const bool accumFrameReq =
        !finalAccumBlocked &&
        (!useRenderIfDirty_ || (jitterRendered_ < 4u && !cameraMoved && !sceneAnimationActive &&
                                   !dynamicRenderingActive && !dynamicRenderingWasActive && !editorActive));

    const bool precompNeeded =
        !tiledMode && (gpuSortInProgress_ || resourcesNeedInit || accumFrameReq || previewRenderReq);
    if (precompNeeded) {
        PrecomputeGsFrameData(cmdList, renderData, cameraViewMatrix_);
        gsFramePrecomputed = true;
        preprocSubmitted = true;
    }

    if (resourcesNeedInit &&
        (gsFramePrecomputed || tiledPrecomputeCompleted || tiledMode || preprocSubmitted || !canComputeSh)) {
        dataStore_->SetResourcesAsLoaded();
    }

    const bool sortWorkPendingAfterPrecompute = gpuSortAvailable ? gpuSortInProgress_ : isSortingCpu_;
    const bool preprocessingStillPending =
        sortWorkPendingAfterPrecompute || isShPending() || (tiledMode && mergedPrecomputeInProgress_);

    const bool canRenderTiled = !tiledMode || tiledCanRenderCurrentJitter || bypassGpuSort;
    const bool renderAccum =
        !preprocessingStillPending && canRenderTiled &&
        (!useRenderIfDirty_ || (jitterRendered_ < 4u && !cameraMoved && !sceneAnimationActive &&
                                   !dynamicRenderingActive && !dynamicRenderingWasActive && !editorActive));
    const bool renderPreviewFrame = !renderAccum && previewRenderReq && canRenderTiled;
    bool renderedFrame = false;

    if (renderAccum || renderPreviewFrame) {
        if (renderPreviewFrame) {
            // preview frames are not part of jitter accum
            jitterRendered_ = 0u;
        } else if (!useRenderIfDirty_ && jitterRendered_ >= 4u) {
            jitterRendered_ = 0u;
        }

        if (!tiledMode && !gsFramePrecomputed) {
            PrecomputeGsFrameData(cmdList, renderData, cameraViewMatrix_);
            gsFramePrecomputed = true;
        }

        ExecuteGaussianRenderPass(cmdList, cameraViewMatrix_, renderData, cpuSortFinished);
        renderedFrame = true;

        UpdateRenderStatusUbo(cmdList);
    }

    if (renderedFrame) {
        if (renderAccum) {
            jitterRendered_++;
            if (!useRenderIfDirty_ && jitterRendered_ >= 4u) {
                jitterRendered_ = 0u;
            }
        } else {
            jitterRendered_ = 0u;
        }
    }

    const bool jitterWorkRemaining = useRenderIfDirty_ && jitterRendered_ < 4u;
    const bool tiledWaitingForTiles = tiledMode && mergedTotalActiveSplats_ == 0u;
    const bool keepDirty = !useRenderIfDirty_ || sortWorkPendingAfterPrecompute || isShPending() ||
                           (tiledMode && mergedPrecomputeInProgress_) || jitterWorkRemaining || sceneAnimationActive ||
                           dynamicRenderingActive || editorActive || tiledWaitingForTiles;
    dataStore_->SetDirtyForGsModel(0u, keepDirty);

    frameIdx_++;
    cameraViewMatrixPrev_ = cameraViewMatrix_;
    viewportDescPrevious_ = viewportDesc_;

    if (resourcesAreLoadedStateValid_ != renderData->areResourcesLoaded) {
        animationTimePrevious_ = std::chrono::high_resolution_clock::now();
    }

    if (renderData->areResourcesLoaded) {
        resourcesAreLoadedStateValid_ = renderData->areResourcesLoaded;
    }

    if (dataStoreEdit_ && dataStoreEdit_->IsGetRecommended3DBoxFlagEnabled()) {
        FindDenseObject(cmdList, renderData);
        GetDenseObjectResults(cmdList, renderData);

        ExecutePrefilterDepthPass(cmdList);
        ExecuteWorldSpaceNormalsPass(cmdList);
        ExecuteEdgesPass(cmdList, renderData);
        ExecuteDilateEdgesPass(cmdList);
        ExecuteMinMaxEdgesPass(cmdList);
        if (USE_DEBUG_DRAW) {
            ExecuteGaussianDebugDrawRenderPass(cmdList, renderData);
        }
        dataStoreEdit_->SetRecommended3DBoxFlag(false);
    }
}

void RenderNodeGsRender::PrecomputeGsFrameData(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData,
    const BASE_NS::Math::Mat4X4& viewMatrix)
{
#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    if (xegBackend_ && xegBackend_->ShouldSkipPrecompute(renderData->enableFrameGeneration,
                                                         dataStore_->GetSingleFrameGenEnable())) {
        return;
    }
#endif
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "PrecomputeGsFrameData", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    ExecutePrecomputePrePasses(cmdList, renderData, viewMatrix);

    if constexpr (USE_CUSTOM_BARRIERS) {
        constexpr GeneralBarrier src { AccessFlagBits::CORE_ACCESS_MEMORY_WRITE_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_TRANSFER_BIT };
        constexpr GeneralBarrier dst { AccessFlagBits::CORE_ACCESS_MEMORY_READ_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT };
        cmdList.CustomMemoryBarrier(src, dst);
        cmdList.AddCustomBarrierPoint();
    }

    const RenderHandle shaderHandles[] = { gsRenderShaders_.precomputeProjection,
        gsRenderShaders_.precomputeProjectionFg, gsRenderShaders_.precomputeProjectionPack,
        gsRenderShaders_.precomputeProjectionPackFg };
    const uint32_t shaderIndex =
        (renderData->CanUsePackedData() ? 2u : 0u) + (renderData->enableFrameGeneration ? 1u : 0u);
    auto& prepassBinders = bindersGsRender_.precomputeProjection;
    const PsoData psoData = GetPsoData(prepassBinders.pl, shaderHandles[shaderIndex], 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    BindPrecomputeDescriptorSet(cmdList, renderData);

    GsRenderPushConstantStruct pc {};
    const float renderScale = static_cast<float>(dataStore_->GetRenderScaleFactor());
    pc.viewportSize.x = static_cast<float>(scissorDesc_.extentWidth) / renderScale;
    pc.viewportSize.y = static_cast<float>(scissorDesc_.extentHeight) / renderScale;
    // When XEG upscale is enabled, no subpixel jittering is needed (jitterIndex = 0)
    // Otherwise, use normal jitterRendered_ for subpixel jittering
    pc.jitterIndex = dataStore_->GetXegUpscaleEnabled() ? 0u : Math::clamp(jitterRendered_, 0u, 4u);
    pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
    pc.renderScaleFactor = dataStore_->GetRenderScaleFactor();
    pc.useIndirectDispatch = (gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0) ? 1u : 0u;
    pc.covarianceStddev = dataStore_->GetRenderCovarianceStddev();
    pc.adhocCullingScale = renderData->adhocCullingScale;
    pc.enableGsOpt = renderData->enableGsOpt;

    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

    if (renderData->CanUseGpuSort() && !renderData->anyComputeLods) {
        cmdList.DispatchIndirect(gpuSortIndirectDrawBuffer_.GetHandle(), sizeof(uint32_t) * 4);
    } else {
        const uint32_t numGs = GetNumEffectiveSplats();
        const uint32_t numWorkGroups = (numGs + (GS_PROCESS_DATA_TGS - 1)) / GS_PROCESS_DATA_TGS;
        cmdList.Dispatch(numWorkGroups, 1, 1);
    }
}

void RenderNodeGsRender::ExecutePrecomputePrePasses(RENDER_NS::IRenderCommandList& cmdList,
    GS::GsRenderData* renderData, const BASE_NS::Math::Mat4X4& viewMatrix)
{
    if (renderData->CanUsePackedData()) {
        return;
    }

    ExecuteAnimationPass(cmdList, renderData);
    if (!renderData->CanUseGpuSort() || renderData->anyComputeLods || !gpuSortInProgress_) {
        return;
    }

    ExecuteGpuSort(cmdList, viewMatrix);

    if (radixDone_) {
        ExecuteGpuSortPopulate(cmdList);
        radixDone_ = false;
    }
}

void RenderNodeGsRender::BindPrecomputeDescriptorSet(
    RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData)
{
    auto& prepassBinders = bindersGsRender_.precomputeProjection;


    auto& prepassSet = prepassBinders.set0;
    auto camHandle = cameraUniformBufferHandle_;
    auto indexBufferHandle =
        (indexPingPong_ ? gsHandlesCache_.gsIndicesRef2.GetHandle() : gsHandlesCache_.gsIndicesRef.GetHandle());


    const bool useRenderSplats = (renderData->useAnimations && !renderData->CanUsePackedData()) || gsEditor_;
    const auto splatHandle =
        useRenderSplats ? gsHandlesCache_.gsSplatDataRender.GetHandle() : gsHandlesCache_.gsSplatData.GetHandle();
    const bool useIndirect = gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0;
    const auto dispatchHandle =
        useIndirect ? gsEditor_->GetIndirectDispatchBuffer().GetHandle() : gpuDummyBuffer_.GetHandle();

    prepassSet->BindBuffer(0u, splatHandle, 0);
    prepassSet->BindBuffer(1u, indexBufferHandle, 0);
    prepassSet->BindBuffer(2u, gsHandlesCache_.dataStorageCacheHandle.GetHandle(), 0);
    prepassSet->BindBuffer(3u, camHandle.GetHandle(), 0);
    prepassSet->BindBuffer(4u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
    prepassSet->BindImage(5u, gsHandlesCache_.depthStoreCacheHandle.GetHandle());
    prepassSet->BindBuffer(6u, gsHandlesCache_.viewZStoreCacheHandle.GetHandle(), 0);
    prepassSet->BindBuffer(7u, dispatchHandle, 0);

    const auto& set0Handle = prepassSet->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, prepassSet->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, set0Handle);
}

void RenderNodeGsRender::ExecuteGpuSort(
    RENDER_NS::IRenderCommandList& cmdList, const Math::Mat4X4 viewMatrix)
{
    GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (!renderData) {
        return;
    }
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteGpuSort", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    // depth range in view-world
    const BASE_NS::Math::Vec2 range = dataStore_->GetDepthRange(viewMatrix);
    const BASE_NS::Math::Vec2 depthRange = BASE_NS::Math::Vec2(Math::min(range.x, 0.0f), Math::min(range.y, 0.0f));

    RenderHandle pso;

    const uint32_t globalInvocationSize = (renderData->GetTotalGsCount() / NUM_BLOCKS_PER_GROUP_RADIX) +
                                          (renderData->GetTotalGsCount() % NUM_BLOCKS_PER_GROUP_RADIX > 0 ? 1 : 0);
    const uint32_t numWorkGroups = (globalInvocationSize + (RADIX_SORT_BINS - 1)) / RADIX_SORT_BINS;

    const uint32_t editUseIndirect = (gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0) ? 1u : 0u;

    if (currentRadixPass_ == 0) {
        auto& presortBinders = bindersGpuSort_.preSort;

        auto& preSortSet = presortBinders.set0;

        PsoData psoData = GetPsoData(presortBinders.pl, gpuSortShaders_.preSort, 0xFFFFffff);
        if (pso != psoData.psoHandle) {
            pso = psoData.psoHandle;
            cmdList.BindPipeline(psoData.psoHandle);
        }

        if (renderData->useAnimations || gsEditor_) {
            preSortSet->BindBuffer(0u, gsHandlesCache_.gsSplatDataRender.GetHandle(), 0);
        } else {
            preSortSet->BindBuffer(0u, gsHandlesCache_.gsSplatData.GetHandle(), 0);
        }


        auto camHandle = cameraUniformBufferHandle_;


        preSortSet->BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0);
        preSortSet->BindBuffer(2u, camHandle.GetHandle(), 0);
        preSortSet->BindBuffer(3u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
        preSortSet->BindBuffer(4u,
            (gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0) ? gsEditor_->GetIndirectDispatchBuffer().GetHandle()
                                                                  : gpuDummyBuffer_.GetHandle(),
            0);

        cmdList.UpdateDescriptorSet(
            preSortSet->GetDescriptorSetHandle(), preSortSet->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, preSortSet->GetDescriptorSetHandle());

        GsSortPushConstantStruct pc {};
        pc.depthRange = depthRange;
        pc.numGs = renderData->GetTotalGsCount();
        pc.renderScaleFactor = dataStore_->GetRenderScaleFactor();
        pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
        pc.useIndirectDispatch = editUseIndirect;
        pc.gsObjectIndex = 0xFFFFffff;
        pc.enableGsOpt = dataStore_->GetGsRenderData()->enableGsOpt;

        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));
        cmdList.Dispatch(numWorkGroups, 1, 1);
    }

    const uint32_t bytePos = currentRadixPass_;

    // count
    {
        auto& binders = bindersGpuSort_.counting;
        PsoData psoData = GetPsoData(binders.pl, gpuSortShaders_.counting, 0xFFFFffff);
        if (pso != psoData.psoHandle) {
            pso = psoData.psoHandle;
            cmdList.BindPipeline(psoData.psoHandle);
        }

        auto& binder0 = *binders.set0;
        binder0.BindBuffer(0u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0);
        binder0.BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer1.GetHandle(), 0);
        binder0.BindBuffer(2u, gsHandlesCache_.gpuSortBuffers.sortCountBuffer.GetHandle(), 0);
        binder0.BindBuffer(3u,
            (gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0) ? gsEditor_->GetIndirectDispatchBuffer().GetHandle()
                                                                  : gpuDummyBuffer_.GetHandle(),
            0);

        cmdList.UpdateDescriptorSet(binder0.GetDescriptorSetHandle(), binder0.GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, binder0.GetDescriptorSetHandle());

        const GsRadixSortPushConstantStruct pc = { bytePos, renderData->GetTotalGsCount(), 0xFFFFffff,
            editUseIndirect };
        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));
        cmdList.Dispatch(numWorkGroups, 1, 1);
    }

    // scan + scatter
    {
        auto& binders = bindersGpuSort_.scanScatter;
        PsoData psoData = GetPsoData(binders.pl, gpuSortShaders_.scanScatter, 0xFFFFffff);
        if (pso != psoData.psoHandle) {
            pso = psoData.psoHandle;
            cmdList.BindPipeline(psoData.psoHandle);
        }

        auto& binder0 = *binders.set0;
        binder0.BindBuffer(0u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0);
        binder0.BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer1.GetHandle(), 0);
        binder0.BindBuffer(2u, gsHandlesCache_.gpuSortBuffers.sortCountBuffer.GetHandle(), 0);
        binder0.BindBuffer(3u,
            (gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0) ? gsEditor_->GetIndirectDispatchBuffer().GetHandle()
                                                                  : gpuDummyBuffer_.GetHandle(),
            0);

        cmdList.UpdateDescriptorSet(binder0.GetDescriptorSetHandle(), binder0.GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, binder0.GetDescriptorSetHandle());

        const GsRadixSortPushConstantStruct pc = { bytePos, renderData->GetTotalGsCount(), 0xFFFFffff,
            editUseIndirect };
        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));
        cmdList.Dispatch(numWorkGroups, 1, 1);
    }

    if (currentRadixPass_ == NUM_RADIX_PASSES - 1) {
        radixDone_ = true;
    }

    currentRadixPass_ = (currentRadixPass_ + 1) % NUM_RADIX_PASSES;
}

bool RenderNodeGsRender::SetGsColorAttachment(RENDER_NS::RenderPass& activeRenderPass)
{
    if (dataStore_->GetGsScaleFactor() == 1u) {
        if (frameIdx_ == 0) {
            activeRenderPass.renderPassDesc.attachmentHandles[0] = gsHandlesCache_.gaussianRenderTarget0.GetHandle();
        }
        return true;
    } else if (dataStore_->GetXegUpscaleEnabled()) {
        gsRenderPass_.renderPassDesc.attachmentHandles[0] = gsHandlesCache_.gaussianRenderTarget0.GetHandle();
        return true;
    }

    switch (jitterRendered_) {
        case 0u: {
            activeRenderPass.renderPassDesc.attachmentHandles[0] = gsHandlesCache_.gaussianRenderTarget0.GetHandle();
            return true;
        }
        case 1u: {
            activeRenderPass.renderPassDesc.attachmentHandles[0] = gsHandlesCache_.gaussianRenderTarget1.GetHandle();
            return true;
        }
        case 2u: {
            activeRenderPass.renderPassDesc.attachmentHandles[0] = gsHandlesCache_.gaussianRenderTarget2.GetHandle();
            return true;
        }
        case 3u: {
            activeRenderPass.renderPassDesc.attachmentHandles[0] = gsHandlesCache_.gaussianRenderTarget3.GetHandle();
            return true;
        }
        default: {
            return false;
        }
    }
}

void RenderNodeGsRender::PrepareGsViewportAndScissor(RENDER_NS::RenderPass& activeRenderPass,
    RENDER_NS::ViewportDesc& viewportDesc, RENDER_NS::ScissorDesc& scissorDesc) const
{
    const float scaleFactor = dataStore_->GetRenderScaleFactor();
    viewportDesc = viewportDesc_;
    viewportDesc.height = ceil(viewportDesc.height / scaleFactor);
    viewportDesc.width = ceil(viewportDesc.width / scaleFactor);

    scissorDesc = scissorDesc_;
    scissorDesc.extentHeight = ceil(scissorDesc.extentHeight / scaleFactor);
    scissorDesc.extentWidth = ceil(scissorDesc.extentWidth / scaleFactor);
    activeRenderPass.renderPassDesc.renderArea.extentWidth = scissorDesc.extentWidth;
    activeRenderPass.renderPassDesc.renderArea.extentHeight = scissorDesc.extentHeight;
}

void RenderNodeGsRender::RecordGaussianDraw(
    RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData, const uint32_t numGs)
{
    if (renderData->isTiled && renderData->tilePool && mergedTotalActiveSplats_ > 0u) {
        const uint64_t cacheBytes = static_cast<uint64_t>(mergedTotalActiveSplats_) * sizeof(Math::UVec4);
        const auto dataCacheDesc = renderNodeContextMgr_->GetGpuResourceManager().GetBufferDescriptor(
            gsHandlesCache_.dataStorageCacheHandle.GetHandle());

        if (static_cast<uint64_t>(dataCacheDesc.byteSize) < cacheBytes) {
            return;
        }

        VertexBuffer vb;
        vb.bufferHandle = gsHandlesCache_.dataStorageCacheHandle.GetHandle();
        vb.byteSize = static_cast<uint32_t>(cacheBytes);
        cmdList.BindVertexBuffers({ vb });
        cmdList.DrawIndirect(renderData->tilePool->tileIndirectDrawBuffer.GetHandle(), 0u, 1u,
            static_cast<uint32_t>(sizeof(GsDrawIndirectCommand)));
        return;
    }

    VertexBuffer vb;
    vb.bufferHandle = gsHandlesCache_.dataStorageCacheHandle.GetHandle();
    vb.byteSize = numGs * sizeof(Math::UVec4);
    cmdList.BindVertexBuffers({ vb });

    if (renderData->CanUseGpuSort() && !renderData->anyComputeLods) {
        cmdList.DrawIndirect(gpuSortIndirectDrawBuffer_.GetHandle(), 0u, 1u, 0u);
    } else {
        cmdList.Draw(4u, numGs, 0, 0);
    }
}

void RenderNodeGsRender::BindGsRenderSet0(RENDER_NS::IRenderCommandList& cmdList, const bool useDepth)
{
    if (useDepth) {
        if (!RenderHandleUtil::IsValid(cameraUniformBufferHandle_.GetHandle()) ||
            !RenderHandleUtil::IsValid(gsDynamicDataUniformBufferHandle_.GetHandle()) ||
            !RenderHandleUtil::IsValid(gsHandlesCache_.depthStoreCacheHandle.GetHandle())) {
            return;
        }
        gsDepthRenderSet0_->BindBuffer(0u, cameraUniformBufferHandle_.GetHandle(), 0);
        gsDepthRenderSet0_->BindBuffer(1u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
        gsDepthRenderSet0_->BindImage(2u, gsHandlesCache_.depthStoreCacheHandle.GetHandle());
        gsDepthRenderSet0_->BindSampler(3u, gsHandlesCache_.sampler.GetHandle());
        const auto& set0Handle = gsDepthRenderSet0_->GetDescriptorSetHandle();

        cmdList.UpdateDescriptorSet(set0Handle, gsDepthRenderSet0_->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);
    } else {
        if (!RenderHandleUtil::IsValid(cameraUniformBufferHandle_.GetHandle()) ||
            !RenderHandleUtil::IsValid(gsDynamicDataUniformBufferHandle_.GetHandle())) {
            return;
        }
        gsRenderSet0_->BindBuffer(0u, cameraUniformBufferHandle_.GetHandle(), 0);
        gsRenderSet0_->BindBuffer(1u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
        const auto& set0Handle = gsRenderSet0_->GetDescriptorSetHandle();

        cmdList.UpdateDescriptorSet(set0Handle, gsRenderSet0_->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);
    }
}

void RenderNodeGsRender::ExecuteGaussianRenderPass(RENDER_NS::IRenderCommandList& cmdList,
    const Math::Mat4X4 viewMatrix, GS::GsRenderData* renderData, const bool isCpuSortDone)
{
    if (!renderData || !renderNodeContextMgr_ || !dataStore_) {
        return;
    }
    const bool useDepth = renderData->gsEnableDepthPass && gsHandlesCache_.gaussianRenderTargetDepth &&
                          gsHandlesCache_.depthStoreCacheHandle;
    RenderPass& activeRenderPass = useDepth ? gsDepthRenderPass_ : gsRenderPass_;
    const RenderHandle activeShader = useDepth ? gsRenderDepthHandle_ : gsRenderHandle_;

    if (!SetGsColorAttachment(activeRenderPass)) {
        return;
    }

    if (useDepth) {
        activeRenderPass.renderPassDesc.attachmentHandles[1] = gsHandlesCache_.gaussianRenderTargetDepth.GetHandle();
    }
    const uint32_t numGs = GetNumEffectiveSplats();

#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    if (xegBackend_ && xegBackend_->Is3DGSEnabled()) {
        const bool tookOver = xegBackend_->OnGaussianRenderPass(
            cmdList, activeRenderPass, *dataStore_, numGs, gsEditor_ != nullptr);
        if (tookOver) {
            return;
        }
    }
#endif
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteGaussianRenderPass", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    ViewportDesc viewportDesc;
    ScissorDesc scissorDesc;
    PrepareGsViewportAndScissor(activeRenderPass, viewportDesc, scissorDesc);

    cmdList.BeginRenderPass(
        activeRenderPass.renderPassDesc, activeRenderPass.subpassStartIndex, activeRenderPass.subpassDesc);
    cmdList.SetDynamicStateScissor(scissorDesc);
    cmdList.SetDynamicStateViewport(viewportDesc);
    const PsoData psoData = GetRenderPsoData(*renderNodeContextMgr_, activeShader);
    cmdList.BindPipeline(psoData.psoHandle);

    GsRenderPushConstantStruct pc {};
    pc.viewportSize.x = static_cast<float>(scissorDesc.extentWidth);
    pc.viewportSize.y = static_cast<float>(scissorDesc.extentHeight);
    pc.jitterIndex = Math::clamp(jitterRendered_, 0u, 4u);
    pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
    pc.renderScaleFactor = dataStore_->GetRenderScaleFactor();
    pc.covarianceStddev = dataStore_->GetRenderCovarianceStddev();
    pc.adhocCullingScale = 1.2f;
    pc.enableGsOpt = renderData->enableGsOpt;

    BindGsRenderSet0(cmdList, useDepth);
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

    RecordGaussianDraw(cmdList, renderData, numGs);

    cmdList.EndRenderPass();

#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    // Software render + XEG upscale only path
    if (xegBackend_) {
        const bool entered = xegBackend_->IsUpscaleEnabled() && !xegBackend_->Is3DGSEnabled() &&
                             dataStore_->GetXegUpscaleEnabled();
        if (entered) {
            xegBackend_->TryEnterUpscaleOnlyMode(cmdList, activeRenderPass, *dataStore_);
            cmdList.SetExecuteBackendFramePosition();
        }
    }
#endif
}

void RenderNodeGsRender::ExecuteGaussianDebugDrawRenderPass(
    RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData)
{
    if (!RenderHandleUtil::IsValid(gsDepthRenderPass_.renderPassDesc.attachmentHandles[0])) {
        return;
    }

    const auto& renderNodeUtil = renderNodeContextMgr_->GetRenderNodeUtil();

    {
        RENDER_NS::RenderNodeHandles::InputRenderPass inputRenderPass;

        // color attachment
        {
            RenderNodeAttachment colorAttachment;
            colorAttachment.loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_LOAD;
            colorAttachment.storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.clearValue = { 0.0f, 0.0f, 0.0f, 1.0f };

            inputRenderPass.attachments.push_back(colorAttachment);
        }

        inputRenderPass.subpassCount = 1u;
        inputRenderPass.subpassContents = SubpassContents::CORE_SUBPASS_CONTENTS_INLINE;
        inputRenderPass.subpassIndex = 0u;

        inputRenderPass.colorAttachmentIndices.push_back(0);

        debugDrawRenderPass_ = renderNodeUtil.CreateRenderPass(inputRenderPass);
    }

    debugDrawRenderPass_.renderPassDesc.attachmentHandles[0] = gsDepthRenderPass_.renderPassDesc.attachmentHandles[0];

    const uint32_t scaleFactor = dataStore_->GetGsScaleFactor();
    if (scaleFactor == 0) {
        return;
    }

    ViewportDesc viewportDesc = viewportDesc_;
    viewportDesc.height /= static_cast<float>(scaleFactor);
    viewportDesc.width /= static_cast<float>(scaleFactor);

    ScissorDesc scissorDesc = scissorDesc_;
    scissorDesc.extentHeight /= scaleFactor;
    scissorDesc.extentWidth /= scaleFactor;

    debugDrawRenderPass_.renderPassDesc.renderArea.extentWidth = scissorDesc.extentWidth;
    debugDrawRenderPass_.renderPassDesc.renderArea.extentHeight = scissorDesc.extentHeight;

    cmdList.BeginRenderPass(
        debugDrawRenderPass_.renderPassDesc, debugDrawRenderPass_.subpassStartIndex, debugDrawRenderPass_.subpassDesc);

    cmdList.SetDynamicStateScissor(scissorDesc);
    cmdList.SetDynamicStateViewport(viewportDesc);

    const PsoData psoData = GetRenderPsoData(*renderNodeContextMgr_, debugDrawShaderHandle_);
    cmdList.BindPipeline(psoData.psoHandle);

    debugDrawRenderSet0_->BindBuffer(0u, cameraUniformBufferHandle_.GetHandle(), 0u);
    debugDrawRenderSet0_->BindBuffer(1u, gaussianDebugDrawBufferHandle_.GetHandle(), 0u);
    debugDrawRenderSet0_->BindBuffer(2u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0u);

    const auto& set0Handle = debugDrawRenderSet0_->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, debugDrawRenderSet0_->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0u, set0Handle);

    GsRenderPushConstantStruct pc {};
    pc.viewportSize.x = static_cast<float>(scissorDesc.extentWidth);
    pc.viewportSize.y = static_cast<float>(scissorDesc.extentHeight);

    pc.jitterIndex = Math::clamp(jitterRendered_, 0u, 4u);
    pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
    pc.renderScaleFactor = static_cast<float>(dataStore_->GetGsScaleFactor());
    pc.adhocCullingScale = 1.2f;
    pc.enableGsOpt = renderData->enableGsOpt;

    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

    cmdList.Draw(static_cast<uint32_t>(gaussianDebugLines_.size()), 1u, 0u, 0u);
    cmdList.EndRenderPass();
}

void RenderNodeGsRender::ExecutePrefilterDepthPass(RENDER_NS::IRenderCommandList& cmdList)
{
    if (!gsEditor_) {
        return;
    }

    const PsoData psoData = GetPsoData(prefilterDepthPipelineLayout_, prefilterDepthShaderHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    {
        prefilterDepthSet0_->BindImage(0u, sceneDepthBufferHandle_);
        prefilterDepthSet0_->BindImage(1u, prefilterDepthTex_.GetHandle());

        const auto& set0Handle = prefilterDepthSet0_->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, prefilterDepthSet0_->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0u, set0Handle);
    }

    const Math::UVec3 targetSize = { static_cast<uint32_t>(viewportDesc_.width),
        static_cast<uint32_t>(viewportDesc_.height), 1u };
    const uint32_t tgcX = (targetSize.x + 8u - 1u) / 8u;
    const uint32_t tgcY = (targetSize.y + 8u - 1u) / 8u;

    cmdList.Dispatch(tgcX, tgcY, 1u);
}

void RenderNodeGsRender::ExecuteWorldSpaceNormalsPass(RENDER_NS::IRenderCommandList& cmdList)
{
    if (!gsEditor_) {
        return;
    }

    const PsoData psoData = GetPsoData(normalsPipelineLayout_, normalsShaderHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    {
        normalsSet0_->BindImage(0u, prefilterDepthTex_.GetHandle());
        normalsSet0_->BindImage(1u, normalsTex_.GetHandle());

        const auto& set0Handle = normalsSet0_->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, normalsSet0_->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0u, set0Handle);
    }

    constexpr PushConstant pc { ShaderStageFlagBits::CORE_SHADER_STAGE_COMPUTE_BIT, sizeof(Math::Mat4X4) };
    Math::Mat4X4 uPc = Math::Inverse(cameraProjMatrix_ * cameraViewMatrix_);
    cmdList.PushConstantData(pc, arrayviewU8(uPc));

    const Math::UVec3 targetSize = { static_cast<uint32_t>(viewportDesc_.width),
        static_cast<uint32_t>(viewportDesc_.height), 1u };
    const uint32_t tgcX = (targetSize.x + 8u - 1u) / 8u;
    const uint32_t tgcY = (targetSize.y + 8u - 1u) / 8u;

    cmdList.Dispatch(tgcX, tgcY, 1u);
}

void RenderNodeGsRender::ExecuteEdgesPass(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData)
{
    if (!renderData || !gsEditor_ || !edgesSet0_ || !dataStoreEdit_ || !renderNodeContextMgr_) {
        return;
    }
    const IRenderDataStoreDefaultCamera* dsCamera = static_cast<IRenderDataStoreDefaultCamera*>(
        renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(stores_.dataStoreNameCamera));
    if (!dsCamera) {
        return;
    }

    const BASE_NS::Math::Vec3 rayOrigin = dataStoreEdit_->GetRecommended3DBoxRay().first;
    const BASE_NS::Math::Vec3 rayDir = dataStoreEdit_->GetRecommended3DBoxRay().second;

    const BASE_NS::Math::Vec3 point = rayOrigin + rayDir * 0.1f; // any point
    const BASE_NS::Math::Vec4 clip = cameraProjMatrix_ * cameraViewMatrix_ * BASE_NS::Math::Vec4(point, 1.0f);
    const BASE_NS::Math::Vec2 ndc = BASE_NS::Math::Vec2(clip.x, clip.y) / clip.w;

    mousePos_ = (ndc * 0.5f + 0.5f) * BASE_NS::Math::Vec2(viewportDesc_.width, viewportDesc_.height);

    struct GsEdgePassPushConstantStruct uPc;

    const PsoData psoData = GetPsoData(edgesPipelineLayout_, edgesShaderHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    {
        edgesSet0_->BindImage(0u, prefilterDepthTex_.GetHandle());
        edgesSet0_->BindImage(1u, normalsTex_.GetHandle());
        edgesSet0_->BindImage(2u, edgeTex_.GetHandle());

        const auto& set0Handle = edgesSet0_->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, edgesSet0_->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0u, set0Handle);
    }

    constexpr PushConstant pc { ShaderStageFlagBits::CORE_SHADER_STAGE_COMPUTE_BIT,
        sizeof(GsEdgePassPushConstantStruct) };

    auto cameras = dsCamera->GetCameras();

    uPc.cameraParams = Math::Vec4(cameras[cameraIdx_].zNear, cameras[cameraIdx_].zFar, mousePos_.x, mousePos_.y);
    uPc.rayHitPos = Math::Vec4(denseObjectWorldPos_, 0.0f);
    uPc.invViewProj = Math::Inverse(cameraProjMatrix_ * cameraViewMatrix_);

    cmdList.PushConstantData(pc, arrayviewU8(uPc));

    const Math::UVec3 targetSize = { static_cast<uint32_t>(viewportDesc_.width),
        static_cast<uint32_t>(viewportDesc_.height), 1u };
    const uint32_t tgcX = (targetSize.x + 8u - 1u) / 8u;
    const uint32_t tgcY = (targetSize.y + 8u - 1u) / 8u;

    cmdList.Dispatch(tgcX, tgcY, 1u);
}

void RenderNodeGsRender::ExecuteDilateEdgesPass(RENDER_NS::IRenderCommandList& cmdList)
{
    if (!gsEditor_) {
        return;
    }

    const PsoData psoData = GetPsoData(dilateEdgesPipelineLayout_, dilateEdgesShaderHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    dilateEdgesSet0_->BindImage(0u, edgeTex_.GetHandle());
    dilateEdgesSet0_->BindImage(1u, dilatedEdgesTex_.GetHandle());
    const auto& setHandle = dilateEdgesSet0_->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(setHandle, dilateEdgesSet0_->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0u, setHandle);

    const uint32_t tgcX = static_cast<uint32_t>((viewportDesc_.width + 8u - 1u) / 8u);
    const uint32_t tgcY = static_cast<uint32_t>((viewportDesc_.height + 8u - 1u) / 8u);
    cmdList.Dispatch(tgcX, tgcY, 1u);
}

void RenderNodeGsRender::ExecuteMinMaxEdgesPass(RENDER_NS::IRenderCommandList& cmdList)
{
    if (!gsEditor_) {
        return;
    }

    const PsoData psoData = GetPsoData(minmaxEdgesPipelineLayout_, minmaxEdgesShaderHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    struct GsEdgeMinMaxPushConstantStruct uPc;

    minmaxEdgesSet0_->BindImage(0u, dilatedEdgesTex_.GetHandle());
    minmaxEdgesSet0_->BindImage(1u, prefilterDepthTex_.GetHandle());
    minmaxEdgesSet0_->BindBuffer(2u, edgesBuffer_.GetHandle(), 0u);
    const auto& setHandle = minmaxEdgesSet0_->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(setHandle, minmaxEdgesSet0_->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0u, setHandle);

    constexpr PushConstant pcDesc { ShaderStageFlagBits::CORE_SHADER_STAGE_COMPUTE_BIT,
        sizeof(GsEdgeMinMaxPushConstantStruct) };

    // Clear
    {
        uPc.invViewProj = Math::Inverse(cameraProjMatrix_ * cameraViewMatrix_);
        uPc.isFirstPass = 1.0f;
        uPc.rayHitPos = { 0.0f, 0.0f, 0.0f };
        cmdList.PushConstantData(pcDesc, arrayviewU8(uPc));
        cmdList.Dispatch(1u, 1u, 1u);
    }

    // Reduction
    {
        uPc.invViewProj = Math::Inverse(cameraProjMatrix_ * cameraViewMatrix_);
        uPc.isFirstPass = 0.0f;
        uPc.rayHitPos = { denseObjectWorldPos_ };
        cmdList.PushConstantData(pcDesc, arrayviewU8(uPc));
        const uint32_t tgcX = static_cast<uint32_t>((viewportDesc_.width + 8u - 1u) / 8u);
        const uint32_t tgcY = static_cast<uint32_t>((viewportDesc_.height + 8u - 1u) / 8u);
        cmdList.Dispatch(tgcX, tgcY, 1u);
    }

    SetRecommended3DBox();
}

void RenderNodeGsRender::GetDenseObjectResults(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData)
{
    if (!renderNodeContextMgr_) {
        return;
    }
    if constexpr (USE_CUSTOM_BARRIERS) {
        constexpr GeneralBarrier src { AccessFlagBits::CORE_ACCESS_SHADER_WRITE_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT };

        constexpr GeneralBarrier dst { AccessFlagBits::CORE_ACCESS_HOST_READ_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_HOST_BIT };

        cmdList.CustomMemoryBarrier(src, dst);
        cmdList.AddCustomBarrierPoint();
    }

    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    uint8_t* const mappedData =
        reinterpret_cast<uint8_t*>(gpuResourceMgr.MapBuffer(denseObjectOutputBufferHandle_.GetHandle()));

    GsObjectHitResult objectHitResult { 0xFFFFFFFFu, {} };
    if (mappedData) {
        if (!CloneData(&objectHitResult, sizeof(GsObjectHitResult), mappedData, sizeof(GsObjectHitResult))) {
            CORE_LOG_E("GetDenseObjectResults: CloneData failed for dense object output buffer!");
        }
        denseObjectWorldPos_ = objectHitResult.worldPos;
        gpuResourceMgr.UnmapBuffer(denseObjectOutputBufferHandle_.GetHandle());
    }
}

void RenderNodeGsRender::FindDenseObject(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData)
{
    const uint32_t numGs = dataStore_->GetNumActiveGs();

    if (!RenderHandleUtil::IsValid(renderData->gsSplatData.GetHandle())) {
        return;
    }

    const PsoData psoData = GetPsoData(denseObjectPipelineLayout_, denseObjectShaderHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    denseObjectSet0_->BindBuffer(0u, renderData->gsSplatData.GetHandle(), 0u);
    denseObjectSet0_->BindBuffer(
        1u, indexPingPong_ ? renderData->gsIndicesRef2.GetHandle() : renderData->gsIndicesRef.GetHandle(), 0u);
    denseObjectSet0_->BindBuffer(2u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0u);
    denseObjectSet0_->BindBuffer(3u, denseObjectOutputBufferHandle_.GetHandle(), 0u);
    denseObjectSet0_->BindBuffer(4u, cameraUniformBufferHandle_.GetHandle(), 0u);

    const auto& set0Handle = denseObjectSet0_->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, denseObjectSet0_->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0u, set0Handle);

    // reset the buffer
    {
        GsDenseObjectPushConstantStruct pc;
        pc.numSplats = numGs;
        pc.stage = 0u; // reset stage
        pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());

        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));
        cmdList.Dispatch(1u, 1u, 1u);
    }

    // dense object
    {
        auto rayOriginDir = dataStoreEdit_->GetRecommended3DBoxRay();

        GsDenseObjectPushConstantStruct pc;
        pc.rayOrigin = rayOriginDir.first;
        pc.rayDirection = rayOriginDir.second;
        pc.numSplats = numGs;
        pc.stage = 1u; // dense object trace stage
        pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());

        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

        const uint32_t numWorkGroups = (numGs + (GS_DENSE_OBJECT_TGS - 1u)) / GS_DENSE_OBJECT_TGS;
        cmdList.Dispatch(numWorkGroups, 1u, 1u);
    }
}

void RenderNodeGsRender::ExecuteComputeSphericalHarmonics(RENDER_NS::IRenderCommandList& cmdList,
    GS::GsRenderData* renderData, const bool computeAll)
{
#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
    if (xegBackend_ && xegBackend_->ShouldSkipSphericalHarmonics(renderData->enableFrameGeneration,
                                                                 dataStore_->GetSingleFrameGenEnable())) {
        return;
    }
#endif
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteComputeSphericalHarmonics", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    auto& shBinders = bindersGsRender_.computeSphericalHarmonics;

    // introduce a barrier to prevent flickering
    if constexpr (USE_CUSTOM_BARRIERS) {
        constexpr GeneralBarrier src { AccessFlagBits::CORE_ACCESS_MEMORY_WRITE_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_TRANSFER_BIT };

        constexpr GeneralBarrier dst { AccessFlagBits::CORE_ACCESS_MEMORY_READ_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT };

        cmdList.CustomMemoryBarrier(src, dst);
        cmdList.AddCustomBarrierPoint();
    }

    PsoData psoData;
    if (renderData->CanUsePackedData()) {
        psoData = GetPsoData(shBinders.pl, gsRenderShaders_.computeSphericalHarmonicsPack, 0xFFFFffff);
    } else {
        psoData = GetPsoData(shBinders.pl, gsRenderShaders_.computeSphericalHarmonics, 0xFFFFffff);
    }
    cmdList.BindPipeline(psoData.psoHandle);

    const uint32_t numGsTotal = renderData->GetTotalGsCount();
    if (renderData->numFramesToComputeSh == 0) {
        return;
    }
    const uint32_t numShPerFrame = Math::max(GS_SH_MIN_COMP_PER_FRAME, numGsTotal / renderData->numFramesToComputeSh);

    shFramesRequired_ =
        computeAll ? 1u : static_cast<uint32_t>(Math::ceilToInt((float)numGsTotal / (float)numShPerFrame));

    {

        auto camHandle = cameraUniformBufferHandle_;


        shBinders.set0->BindBuffer(0u, gsHandlesCache_.gsSplatData.GetHandle(), 0);
        shBinders.set0->BindBuffer(1u, camHandle.GetHandle(), 0);
        shBinders.set0->BindBuffer(2u, gsHandlesCache_.gsHarmonicsDataRef.GetHandle(), 0);
        shBinders.set0->BindBuffer(3u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);

        // ASTC
        BASE_NS::vector<BindableImage> shASTCImages;
        shASTCImages.resize(gsHandlesCache_.gsShASTCDataRefs.size());
        for (size_t ii = 0; ii < gsHandlesCache_.gsShASTCDataRefs.size(); ii++) {
            shASTCImages[ii].handle = gsHandlesCache_.gsShASTCDataRefs[ii].GetHandle();
        }

        shBinders.set0->BindImages(4u, shASTCImages);
        shBinders.set0->BindSampler(5u, gsHandlesCache_.sampler.GetHandle()); // seem not support multi-gs

        const auto& set0Handle = shBinders.set0->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, shBinders.set0->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);

        GsShPushConstantStruct pc {};
        const float shScaleFactor = dataStore_->GetRenderScaleFactor();
        pc.viewportSize.x = static_cast<float>(scissorDesc_.extentWidth) / shScaleFactor;
        pc.viewportSize.y = static_cast<float>(scissorDesc_.extentHeight) / shScaleFactor;

        pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());

        if (computeAll) {
            pc.baseOffset = 0;
        } else {
            // progressively update the spherical harmonics
            pc.baseOffset = shComputationOffset_;
        }

        pc.numGsTotal = numGsTotal;
        pc.convertGsColorSrgb = renderData->convertGsColorSrgb ? 1u : 0u;

        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

        uint32_t numWorkGroups = 0u;
        if (computeAll) {
            numWorkGroups = (numGsTotal + (GS_PROCESS_DATA_TGS - 1)) / GS_PROCESS_DATA_TGS;
        } else {
            numWorkGroups = (numShPerFrame + (GS_PROCESS_DATA_TGS - 1)) / GS_PROCESS_DATA_TGS;
        }
        cmdList.Dispatch(numWorkGroups, 1u, 1u);
    }

    if (!computeAll) {
        shComputationOffset_ += numShPerFrame;
        if (shComputationOffset_ >= numGsTotal) {
            shComputationOffset_ = 0u;
        }
    }

    shFramesRendered_++;
}

void RenderNodeGsRender::ExecuteAnimationPass(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData)
{
    if (!renderData || !renderData->useAnimations || gsEditor_) {
        return;
    }

    if (!animationRenderFrame_) {
        // animation is paused, no need to render. We can just read from the previous state of gsSplatDataRender
        return;
    }
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteAnimationPass", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    if (bindersGsRender_.animationPass.pl.descriptorSetLayouts[1u].bindings.size() !=
        gsHandlesCache_.customAnimationResources.size()) {
        CORE_LOG_E("RenderNodeGsRender::ExecuteAnimationPass custom animation shader resources size %u does not "
                   "match the compute shader reflection pipeline layout number of resources %u!",
            static_cast<uint32_t>(gsHandlesCache_.customAnimationResources.size()),
            static_cast<uint32_t>(bindersGsRender_.animationPass.pl.descriptorSetLayouts[1u].bindings.size()));
        return;
    }

    if (!gsHandlesCache_.customAnimationResources.empty() && !bindersGsRender_.animationPass.set1) {
        return;
    }

    auto& animationBinders = bindersGsRender_.animationPass;
    if (!animationBinders.set0) {
        return;
    }

    const uint32_t numGs = renderData->GetTotalGsCount();

    // introduce a barrier to prevent flickering
    if constexpr (USE_CUSTOM_BARRIERS) {
        constexpr GeneralBarrier src { AccessFlagBits::CORE_ACCESS_MEMORY_WRITE_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_TRANSFER_BIT };

        constexpr GeneralBarrier dst { AccessFlagBits::CORE_ACCESS_MEMORY_READ_BIT,
            PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT };

        cmdList.CustomMemoryBarrier(src, dst);
        cmdList.AddCustomBarrierPoint();
    }

    const PsoData psoData = GetPsoData(animationBinders.pl, gsRenderShaders_.animationPass, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    // set0 is bound always
    {
        animationBinders.set0->BindBuffer(0u, gsHandlesCache_.gsSplatData.GetHandle(), 0);
        animationBinders.set0->BindBuffer(1u, gsHandlesCache_.gsSplatDataRender.GetHandle(), 0);

        animationBinders.set0->BindBuffer(2u, cameraUniformBufferHandle_.GetHandle(), 0);
        animationBinders.set0->BindBuffer(3u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);

        const auto& set0Handle = animationBinders.set0->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, animationBinders.set0->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);
    }

    // bind set1 if any
    {
        for (uint32_t set1binding = 0;
             set1binding < static_cast<uint32_t>(gsHandlesCache_.customAnimationResources.size()); set1binding++) {
            const auto bType =
                bindersGsRender_.animationPass.pl.descriptorSetLayouts[1u].bindings[set1binding].descriptorType;

            // buffers
            if (bType == DescriptorType::CORE_DESCRIPTOR_TYPE_STORAGE_BUFFER ||
                bType == DescriptorType::CORE_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
                bType == DescriptorType::CORE_DESCRIPTOR_TYPE_UNIFORM_BUFFER ||
                bType == DescriptorType::CORE_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
                animationBinders.set1->BindBuffer(
                    set1binding, gsHandlesCache_.customAnimationResources[set1binding].GetHandle(), 0);
            }

            // images
            else if (bType == DescriptorType::CORE_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
                     bType == DescriptorType::CORE_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER) {
                animationBinders.set1->BindImage(
                    set1binding, gsHandlesCache_.customAnimationResources[set1binding].GetHandle());
            }
            // samplers
            else if (bType == DescriptorType::CORE_DESCRIPTOR_TYPE_SAMPLER) {
                animationBinders.set1->BindSampler(
                    set1binding, gsHandlesCache_.customAnimationResources[set1binding].GetHandle());
            }
        }

        if (!gsHandlesCache_.customAnimationResources.empty()) {
            const auto& set1Handle = animationBinders.set1->GetDescriptorSetHandle();
            cmdList.UpdateDescriptorSet(set1Handle, animationBinders.set1->GetDescriptorSetLayoutBindingResources());
            cmdList.BindDescriptorSet(1u, set1Handle);
        }
    }

    GsRenderPushConstantStruct pc {};
    const float animScaleFactor = dataStore_->GetRenderScaleFactor();
    pc.viewportSize.x = static_cast<float>(scissorDesc_.extentWidth) / animScaleFactor;
    pc.viewportSize.y = static_cast<float>(scissorDesc_.extentHeight) / animScaleFactor;

    pc.jitterIndex = Math::clamp(jitterRendered_, 0u, 4u);
    pc.numGsObjects = static_cast<uint32_t>(dataStore_->GetGsPrimitives().size());
    pc.renderScaleFactor = animScaleFactor;
    pc.covarianceStddev = dataStore_->GetRenderCovarianceStddev();
    pc.adhocCullingScale = 1.2f;
    pc.enableGsOpt = renderData->enableGsOpt;

    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

    const uint32_t numWorkGroups = (numGs + (GS_PROCESS_DATA_TGS - 1)) / GS_PROCESS_DATA_TGS;
    cmdList.Dispatch(numWorkGroups, 1, 1);
}

void RenderNodeGsRender::ExecuteGpuSortPopulate(RENDER_NS::IRenderCommandList& cmdList)
{
    const GS::GsRenderData* renderData = dataStore_->GetGsRenderData();
    if (!renderData) {
        return;
    }
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteGpuSortPopulate", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    auto& copyBinders = bindersGpuSort_.copy;

    const PsoData psoData = GetPsoData(copyBinders.pl, gpuSortShaders_.sortedDataCopy, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    const uint32_t numGs = renderData->GetTotalGsCount();

    {
        if (indexPingPong_) {
            copyBinders.set0->BindBuffer(0u, gsHandlesCache_.gsIndicesRef.GetHandle(), 0);
        } else {
            copyBinders.set0->BindBuffer(0u, gsHandlesCache_.gsIndicesRef2.GetHandle(), 0);
        }

        copyBinders.set0->BindBuffer(1u, gsHandlesCache_.gpuSortBuffers.sortKeyIndexBuffer0.GetHandle(), 0u);
        copyBinders.set0->BindBuffer(2u, gpuSortIndirectDrawBuffer_.GetHandle(), 0u);
        copyBinders.set0->BindBuffer(3u,
            (gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0) ? gsEditor_->GetIndirectDispatchBuffer().GetHandle()
                                                                  : gpuDummyBuffer_.GetHandle(),
            0);

        const auto& set0Handle = copyBinders.set0->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, copyBinders.set0->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0u, set0Handle);
    }

    constexpr PushConstant pc { ShaderStageFlagBits::CORE_SHADER_STAGE_COMPUTE_BIT, sizeof(BASE_NS::Math::Vec4) };
    const BASE_NS::Math::Vec4 uPc(
        static_cast<float>(numGs), (gsEditor_ && gsEditor_->GetNumModifyingPasses() > 0), 0, 0);
    cmdList.PushConstantData(pc, arrayviewU8(uPc));

    const uint32_t globalInvocationSize = (numGs / NUM_BLOCKS_PER_GROUP) + (numGs % NUM_BLOCKS_PER_GROUP > 0 ? 1 : 0);
    const uint32_t numWorkGroups = (globalInvocationSize + (GS_PROCESS_DATA_TGS - 1)) / GS_PROCESS_DATA_TGS;

    cmdList.Dispatch(numWorkGroups, 1, 1);

    jitterRendered_ = 0;
    indexPingPong_ = !indexPingPong_;
    gpuSortInProgress_ = false;
}

void RenderNodeGsRender::ResetMergedTiledSortState()
{
    gpuSortInProgress_ = false;
    radixDone_ = false;
    mergedSortInitializing_ = false;
    currentRadixPass_ = 0u;
    mergedPrecomputeInProgress_ = false;
    mergedPrecomputedJitterIndex_ = 0xFFFFFFFFu;
}

bool RenderNodeGsRender::EnsureTilePreSortWgInfoCapacity(uint32_t capacity)
{
    if (!renderNodeContextMgr_) {
        CORE_LOG_E("RenderNodeGsRender: render node context manager is nullptr.");
        return false;
    }
    capacity = Math::max(capacity, 1u);
    if (tilePreSortWgInfoBuffer_ && capacity <= tilePreSortWgInfoCapacity_) {
        return true;
    }

    const uint64_t grown =
        Math::max(static_cast<uint64_t>(capacity), (static_cast<uint64_t>(tilePreSortWgInfoCapacity_) * 3u) / 2u);
    const uint64_t maxCapacity =
        static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) / sizeof(GsTileWorkgroupMetadataStruct);
    capacity = static_cast<uint32_t>(Math::min(grown, maxCapacity));

    const uint64_t byteSize64 = static_cast<uint64_t>(capacity) * sizeof(GsTileWorkgroupMetadataStruct);
    if (byteSize64 > std::numeric_limits<uint32_t>::max()) {
        CORE_LOG_E("RenderNodeGsRender: tile pre-sort metadata buffer is too large (%llu bytes).",
            static_cast<unsigned long long>(byteSize64));
        return false;
    }

    GpuBufferDesc desc;
    desc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    desc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    desc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_RING_BUFFER;
    desc.byteSize = Math::max(static_cast<uint32_t>(byteSize64), 16u);

    const auto uniqueName = BASE_NS::to_string(gRenderNodeGsRenderCounter.fetch_add(1, std::memory_order_relaxed)) +
                            "_" + BASE_NS::to_string(capacity);
    tilePreSortWgInfoBuffer_ =
        renderNodeContextMgr_->GetGpuResourceManager().Create("GsTilePreSortWgInfo_" + uniqueName, desc);
    tilePreSortWgInfoCapacity_ = capacity;

    return RenderHandleUtil::IsValid(tilePreSortWgInfoBuffer_.GetHandle());
}

namespace {
bool ComputeTileCullAndDepthRange(
    const Math::Mat4X4& vwMat, const Math::Mat4X4& projMat, const GsTileMetadata& tile, Math::Vec2& depthRange)
{
    constexpr float kAabbExpand = 1.1f;
    const Math::Vec3 center = (tile.aabbMin + tile.aabbMax) * 0.5f;
    const Math::Vec3 halfExt = (tile.aabbMax - tile.aabbMin) * (0.5f * kAabbExpand);
    const Math::Vec3 minE = center - halfExt;
    const Math::Vec3 maxE = center + halfExt;

    const Math::Vec4 corners[8u] = {
        Math::Vec4(minE.x, minE.y, minE.z, 1.0f),
        Math::Vec4(maxE.x, minE.y, minE.z, 1.0f),
        Math::Vec4(minE.x, maxE.y, minE.z, 1.0f),
        Math::Vec4(maxE.x, maxE.y, minE.z, 1.0f),
        Math::Vec4(minE.x, minE.y, maxE.z, 1.0f),
        Math::Vec4(maxE.x, minE.y, maxE.z, 1.0f),
        Math::Vec4(minE.x, maxE.y, maxE.z, 1.0f),
        Math::Vec4(maxE.x, maxE.y, maxE.z, 1.0f),
    };

    uint8_t cullMaskAll = 0x3Fu;
    float tileMinZ = std::numeric_limits<float>::infinity();
    float tileMaxZ = -std::numeric_limits<float>::infinity();

    for (const auto& corner : corners) {
        const Math::Vec4 viewPos = vwMat * corner;
        tileMinZ = BASE_NS::Math::min(tileMinZ, viewPos.z);
        tileMaxZ = BASE_NS::Math::max(tileMaxZ, viewPos.z);

        const Math::Vec4 clipPos = projMat * viewPos;
        uint8_t mask = 0u;
        if (clipPos.x + clipPos.w < 0.0f)
            mask |= 0x01u;
        if (clipPos.w - clipPos.x < 0.0f)
            mask |= 0x02u;
        if (clipPos.y + clipPos.w < 0.0f)
            mask |= 0x04u;
        if (clipPos.w - clipPos.y < 0.0f)
            mask |= 0x08u;
        if (clipPos.z < 0.0f)
            mask |= 0x10u;
        if (clipPos.w - clipPos.z < 0.0f)
            mask |= 0x20u;
        cullMaskAll &= mask;
    }

    if (cullMaskAll != 0u) {
        return true;
    }

    depthRange.x = BASE_NS::Math::min(depthRange.x, tileMinZ);
    depthRange.y = BASE_NS::Math::max(depthRange.y, tileMaxZ);
    return false;
}

bool IsPrimitiveAabbFullyOutsideFrustum(
    const Math::Mat4X4& vpwMat, const Math::Vec3& aabbMin, const Math::Vec3& aabbMax)
{
    constexpr float kAabbExpand = 1.1f;
    const Math::Vec3 center = (aabbMin + aabbMax) * 0.5f;
    const Math::Vec3 halfExt = (aabbMax - aabbMin) * (0.5f * kAabbExpand);
    const Math::Vec3 minE = center - halfExt;
    const Math::Vec3 maxE = center + halfExt;

    const Math::Vec4 corners[8u] = {
        Math::Vec4(minE.x, minE.y, minE.z, 1.0f),
        Math::Vec4(maxE.x, minE.y, minE.z, 1.0f),
        Math::Vec4(minE.x, maxE.y, minE.z, 1.0f),
        Math::Vec4(maxE.x, maxE.y, minE.z, 1.0f),
        Math::Vec4(minE.x, minE.y, maxE.z, 1.0f),
        Math::Vec4(maxE.x, minE.y, maxE.z, 1.0f),
        Math::Vec4(minE.x, maxE.y, maxE.z, 1.0f),
        Math::Vec4(maxE.x, maxE.y, maxE.z, 1.0f),
    };

    uint8_t cullMaskAll = 0x3Fu;
    for (const auto& corner : corners) {
        const Math::Vec4 clipPos = vpwMat * corner;
        uint8_t mask = 0u;
        if (clipPos.x + clipPos.w < 0.0f)
            mask |= 0x01u;
        if (clipPos.w - clipPos.x < 0.0f)
            mask |= 0x02u;
        if (clipPos.y + clipPos.w < 0.0f)
            mask |= 0x04u;
        if (clipPos.w - clipPos.y < 0.0f)
            mask |= 0x08u;
        if (clipPos.z < 0.0f)
            mask |= 0x10u;
        if (clipPos.w - clipPos.z < 0.0f)
            mask |= 0x20u;
        cullMaskAll &= mask;
        if (cullMaskAll == 0u) {
            return false;
        }
    }
    return cullMaskAll != 0u;
}
} // namespace

void RenderNodeGsRender::BuildMergedWorkgroupInfos()
{
    const uint32_t wgElements = NUM_BLOCKS_PER_GROUP_RADIX * RADIX_SORT_BINS;
    mergedWgInfos_.clear();
    mergedWgInfos_.reserve(mergedTiles_.size() * 2u);

    for (const MergedTileEntry& e : mergedTiles_) {
        const uint32_t paddedCount = (e.splatCount + MERGED_SLOT_ALIGNMENT - 1u) & ~(MERGED_SLOT_ALIGNMENT - 1u);
        const uint32_t wgPerTile = (paddedCount + wgElements - 1u) / wgElements;

        for (uint32_t ww = 0u; ww < wgPerTile; ww++) {
            mergedWgInfos_.push_back(GsTileWorkgroupMetadataStruct { e.poolOffset, e.mergedOffset, e.splatCount, ww });
        }
    }
}

uint64_t RenderNodeGsRender::ComputeMergedTileSetHash(const uint32_t poolVersion) const
{
    uint64_t hash = 1469598103934665603uLL;
    hash ^= static_cast<uint64_t>(poolVersion);
    hash *= 1099511628211uLL;
    for (const MergedTileEntry& e : mergedTiles_) {
        hash ^= static_cast<uint64_t>(e.slot) | (static_cast<uint64_t>(e.splatCount) << 32);
        hash *= 0x100000001B3uLL;
        hash ^= static_cast<uint64_t>(e.poolOffset) | (static_cast<uint64_t>(e.mergedOffset) << 32);
        hash *= 1099511628211uLL;
        hash ^= static_cast<uint64_t>(e.primIdx) | (static_cast<uint64_t>(e.tileIdx) << 32);
        hash *= 1099511628211uLL;
    }

    return hash;
}

void RenderNodeGsRender::ProcessPrimitiveTiles(const GsPrimitive& prim, const uint32_t primIdx,
    const BASE_NS::Math::Mat4X4& viewMatrix, const BASE_NS::Math::Mat4X4& projMatrix, const GsTilePool* pool,
    BASE_NS::Math::Vec2& globalDepthRange)
{
    const Math::Mat4X4 vwMat = viewMatrix * prim.worldMatrix;
    const Math::Mat4X4 vpwMat = projMatrix * vwMat;

    if (IsPrimitiveAabbFullyOutsideFrustum(vpwMat, prim.aabbMin, prim.aabbMax)) {
        return;
    }

    for (size_t ti = 0; ti < prim.tileGrid.tiles.size(); ti++) {
        const GsTileMetadata& tile = prim.tileGrid.tiles[ti];
        if (tile.state != GsTileState::RESIDENT || tile.gpuPoolSlot < 0 || tile.splatCount == 0u) {
            continue;
        }

        if (tile.splatCount > tile.gpuPoolCapacity) {
            continue;
        }

        if (pool && static_cast<uint64_t>(tile.gpuPoolOffset) + tile.splatCount > pool->splatCapacity) {
            continue;
        }

        if (ComputeTileCullAndDepthRange(vwMat, projMatrix, tile, globalDepthRange)) {
            continue;
        }

        mergedTiles_.push_back({ static_cast<uint32_t>(tile.gpuPoolSlot), tile.gpuPoolOffset, tile.splatCount, 0u,
            primIdx, static_cast<uint32_t>(ti) });
    }
}

void RenderNodeGsRender::BuildMergedTileBatch(const Math::Mat4X4& viewMatrix)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "RenderNodeGsRender::BuildMergedTileBatch", "", 0x00FF88);

    mergedTiles_.clear();
    mergedTotalActiveSplats_ = 0u;
    mergedGsObjectIndex_ = 0u;
    mergedDepthRange_ = Math::Vec2(0.0f, 0.0f);

    if (!dataStore_) {
        return;
    }

    const GsRenderData* renderData = dataStore_->GetGsRenderData();
    const GsTilePool* pool = (renderData && renderData->tilePool.get()) ? renderData->tilePool.get() : nullptr;
    const uint32_t poolVersion = pool ? pool->resourceVersion : 0u;
    BASE_NS::vector<GsPrimitive>& primitives = dataStore_->GetGsPrimitives();
    Math::Vec2 globalDepthRange(std::numeric_limits<float>::max(), std::numeric_limits<float>::lowest());

    for (size_t pi = 0; pi < primitives.size(); pi++) {
        const GsPrimitive& prim = primitives[pi];
        if (!prim.isValid || !prim.enabled || prim.tileGrid.tiles.empty()) {
            continue;
        }

        const size_t prevTileCount = mergedTiles_.size();
        ProcessPrimitiveTiles(prim, static_cast<uint32_t>(pi), viewMatrix, cameraProjMatrix_, pool, globalDepthRange);
        if (mergedTiles_.size() != prevTileCount) {
            mergedGsObjectIndex_ = static_cast<uint32_t>(pi);
        }
    }

    for (MergedTileEntry& e : mergedTiles_) {
        constexpr uint32_t mergedSlotAlignment = 32u;
        const uint32_t padded = (e.splatCount + mergedSlotAlignment - 1u) & ~(mergedSlotAlignment - 1u);
        if (static_cast<uint64_t>(mergedTotalActiveSplats_) + padded > std::numeric_limits<uint32_t>::max()) {
            CORE_LOG_E("RenderNodeGsRender: merged tiled splat count exceeds uint32_t range.");
            mergedTiles_.clear();
            mergedTotalActiveSplats_ = 0u;
            break;
        }
        e.mergedOffset = mergedTotalActiveSplats_;
        mergedTotalActiveSplats_ += padded;
    }

    constexpr float kDepthRangeEpsilon = 1e-4f;
    Math::Vec2 depthRange(Math::min(globalDepthRange.x, 0.0f), Math::min(globalDepthRange.y, 0.0f));
    if (depthRange.y - depthRange.x < kDepthRangeEpsilon) {
        depthRange.x = depthRange.y - kDepthRangeEpsilon;
    }
    mergedDepthRange_ = depthRange;

    BuildMergedWorkgroupInfos();

    const uint64_t hash = ComputeMergedTileSetHash(poolVersion);
    if (hash != mergedTileSetHash_) {
        mergedTileSetHash_ = hash;
        mergedSortInitializing_ = true;
        BeginGpuSort();
    }
}

bool RenderNodeGsRender::ExecuteMergedTiledGpuSort(RENDER_NS::IRenderCommandList& cmdList, const GsTilePool& pool)
{
    if (mergedTotalActiveSplats_ == 0u || mergedTiles_.empty() || !dataStore_) {
        return false;
    }
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteMergedTiledGpuSort", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    const uint32_t totalGs = mergedTotalActiveSplats_;
    if (pool.sortCapacity < totalGs) {
        CORE_LOG_E("RenderNodeGsRender: tile sort capacity %u is smaller than active splat count %u.",
            pool.sortCapacity, totalGs);
        return false;
    }

    const uint64_t sortByteSize64 = static_cast<uint64_t>(totalGs) * sizeof(SortKeyIndex);
    if (sortByteSize64 > std::numeric_limits<uint32_t>::max()) {
        CORE_LOG_E("RenderNodeGsRender: tile sort range is too large.");
        return false;
    }
    const uint32_t sortByteSize = static_cast<uint32_t>(sortByteSize64);
    const uint32_t globalInv =
        (totalGs / NUM_BLOCKS_PER_GROUP_RADIX) + (totalGs % NUM_BLOCKS_PER_GROUP_RADIX > 0 ? 1u : 0u);
    const uint32_t numWorkGroups = (globalInv + (RADIX_SORT_BINS - 1)) / RADIX_SORT_BINS;

    const bool runPreSort = mergedSortInitializing_ || currentRadixPass_ == 0u;
    if (runPreSort) {
        const bool preSortOk = ExecuteTilePreSortPass(cmdList, pool, sortByteSize);
        if (!preSortOk) {
            return false;
        }
        ExecuteSortSizeCalcPass(cmdList, pool);
    }

    const uint32_t sortByteCount = dataStore_->GetGpuSortByteCount();
    for (uint32_t pp = currentRadixPass_; pp < sortByteCount; pp++) {
        ExecuteTileRadixPass(cmdList, pool, sortByteSize, totalGs, numWorkGroups, pp);
    }

    currentRadixPass_ = 0u;
    radixDone_ = true;
    mergedSortInitializing_ = false;
    forceFullPrecompute_ = true;
    precomputeChunkIdx_ = 0u;

    return true;
}

bool RenderNodeGsRender::ExecuteTilePreSortPass(
    IRenderCommandList& cmdList, const GsTilePool& pool, const uint32_t sortByteSize)
{
    const uint32_t totalWg = static_cast<uint32_t>(mergedWgInfos_.size());
    if (totalWg == 0u || !EnsureTilePreSortWgInfoCapacity(totalWg)) {
        return false;
    }

    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    auto* dst = static_cast<uint8_t*>(gpuResourceMgr.MapBuffer(tilePreSortWgInfoBuffer_.GetHandle()));
    if (!dst) {
        return false;
    }
    const size_t bytes = mergedWgInfos_.size() * sizeof(GsTileWorkgroupMetadataStruct);
    if (!CloneData(dst, bytes, mergedWgInfos_.data(), bytes)) {
        CORE_LOG_E("CloneData failed for tile pre-sort wg info");
    }
    gpuResourceMgr.UnmapBuffer(tilePreSortWgInfoBuffer_.GetHandle());

    // init pre sort
    {
        auto& binder = tileMergedInitPreSortBinder_;
        const PsoData psoData = GetPsoData(binder.pl, gpuSortShaders_.initPreSortTiled, 0xFFFFffff);
        cmdList.BindPipeline(psoData.psoHandle);

        binder.set0->BindBuffer(0u, pool.tileIndirectDrawBuffer.GetHandle(), 0);
        cmdList.UpdateDescriptorSet(
            binder.set0->GetDescriptorSetHandle(), binder.set0->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, binder.set0->GetDescriptorSetHandle());
        cmdList.Dispatch(1, 1, 1);
    }

    auto& binder = tileMergedPreSortBinder_;
    PsoData psoData;
    if (dataStore_->GetGsRenderData()->CanUseTilePackedData()) {
        psoData = GetPsoData(binder.pl, gpuSortShaders_.preSortPackedTiled, 0xFFFFffff);
    } else {
        psoData = GetPsoData(binder.pl, gpuSortShaders_.preSortTiled, 0xFFFFffff);
    }
    cmdList.BindPipeline(psoData.psoHandle);

    binder.set0->BindBuffer(0u, pool.splatPoolBuffer.GetHandle(), 0);
    binder.set0->BindBuffer(1u, pool.sortIndexPool0.GetHandle(), 0, sortByteSize);
    binder.set0->BindBuffer(2u, cameraUniformBufferHandle_.GetHandle(), 0);
    binder.set0->BindBuffer(3u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
    binder.set0->BindBuffer(4u, tilePreSortWgInfoBuffer_.GetHandle(), 0,
        totalWg * static_cast<uint32_t>(sizeof(GsTileWorkgroupMetadataStruct)));
    binder.set0->BindBuffer(5u, pool.tileIndirectDrawBuffer.GetHandle(), 0);

    cmdList.UpdateDescriptorSet(
        binder.set0->GetDescriptorSetHandle(), binder.set0->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, binder.set0->GetDescriptorSetHandle());

    GsTiledPreSortPushConstantStruct pc {};
    pc.depthRange = mergedDepthRange_;
    pc.numGs = 1u;
    pc.gsObjectIndex = mergedGsObjectIndex_;
    pc.gsIndexBase = 0u;
    pc.sortKeyBits = dataStore_->GetGpuSortKeyBits();
    pc.enableGsOpt = dataStore_->GetGsRenderData()->enableGsOpt;
    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));
    cmdList.Dispatch(totalWg, 1, 1);

    auto& primitivesRef = dataStore_->GetGsPrimitives();
    for (const MergedTileEntry& mte : mergedTiles_) {
        if (mte.primIdx < primitivesRef.size() && mte.tileIdx < primitivesRef[mte.primIdx].tileGrid.tiles.size()) {
            primitivesRef[mte.primIdx].tileGrid.tiles[mte.tileIdx].sortOutputValid = true;
        }
    }

    return true;
}

void RenderNodeGsRender::ExecuteSortSizeCalcPass(IRenderCommandList& cmdList, const GsTilePool& pool)
{
    if (mergedTotalActiveSplats_ == 0u) {
        return;
    }
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteSortSizeCalcPass", Math::Vec4(0.8f, 0.6f, 0.2f, 1.0f));

    auto& binder = tileMergedSortSizeCalcBinder_;
    const PsoData psoData = GetPsoData(binder.pl, gpuSortShaders_.sortSizeCalcTiled, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    binder.set0->BindBuffer(0u, pool.tileIndirectDrawBuffer.GetHandle(), 0);
    binder.set0->BindBuffer(1u, preComputeIndirectDispatchBuffer_.GetHandle(), 0);
    cmdList.UpdateDescriptorSet(
        binder.set0->GetDescriptorSetHandle(), binder.set0->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, binder.set0->GetDescriptorSetHandle());
    cmdList.Dispatch(1, 1, 1);
}

void RenderNodeGsRender::ExecuteTileRadixPass(IRenderCommandList& cmdList, const GsTilePool& pool,
    const uint32_t sortByteSize, const uint32_t totalGs, const uint32_t numWorkGroups, const uint32_t bytePos)
{
    if (!dataStore_ || bytePos >= dataStore_->GetGpuSortByteCount()) {
        return;
    }

    {
        auto& binders = tileMergedCountingBinder_;
        const PsoData psoData = GetPsoData(binders.pl, gpuSortShaders_.countingTiled, 0xFFFFffff);
        cmdList.BindPipeline(psoData.psoHandle);

        auto& set0 = *binders.set0;
        set0.BindBuffer(0u, pool.sortIndexPool0.GetHandle(), 0, sortByteSize);
        set0.BindBuffer(1u, pool.sortIndexPool1.GetHandle(), 0, sortByteSize);
        set0.BindBuffer(2u, pool.sortCountPoolBuffer.GetHandle(), 0);
        set0.BindBuffer(3u, pool.tileIndirectDrawBuffer.GetHandle(), 0);

        cmdList.UpdateDescriptorSet(set0.GetDescriptorSetHandle(), set0.GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0.GetDescriptorSetHandle());

        const GsRadixSortPushConstantStruct pc = { bytePos, totalGs, 0xFFFFffffu };
        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

        cmdList.DispatchIndirect(pool.tileIndirectDrawBuffer.GetHandle(), sizeof(uint32_t) * 4);
    }

    {
        auto& binders = tileMergedScanScatterBinder_;
        const PsoData psoData = GetPsoData(binders.pl, gpuSortShaders_.scanScatterTiled, 0xFFFFffff);
        cmdList.BindPipeline(psoData.psoHandle);

        auto& set0 = *binders.set0;
        set0.BindBuffer(0u, pool.sortIndexPool0.GetHandle(), 0, sortByteSize);
        set0.BindBuffer(1u, pool.sortIndexPool1.GetHandle(), 0, sortByteSize);
        set0.BindBuffer(2u, pool.sortCountPoolBuffer.GetHandle(), 0);
        set0.BindBuffer(3u, pool.tileIndirectDrawBuffer.GetHandle(), 0);

        cmdList.UpdateDescriptorSet(set0.GetDescriptorSetHandle(), set0.GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0.GetDescriptorSetHandle());

        const GsRadixSortPushConstantStruct pc = { bytePos, totalGs, 0xFFFFffffu };
        cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&pc));

        cmdList.DispatchIndirect(pool.tileIndirectDrawBuffer.GetHandle(), sizeof(uint32_t) * 4);
    }
}

uint32_t RenderNodeGsRender::GetMergedTiledPrecomputeChunkCount(const uint32_t totalGs) const
{
    if (totalGs == 0u || forceFullPrecompute_) {
        return 1u;
    }

    const uint32_t fullThreshold = kAmortFrames * kPrecomputeChunkAlignmentElems;
    if (totalGs <= fullThreshold) {
        return 1u;
    }

    const uint32_t rough = (totalGs + kAmortFrames - 1u) / kAmortFrames;
    const uint32_t chunkSize = (rough + kPrecomputeChunkAlignmentElems - 1u) & ~(kPrecomputeChunkAlignmentElems - 1u);
    return Math::max((totalGs + chunkSize - 1u) / chunkSize, 1u);
}

void RenderNodeGsRender::RestartMergedTiledPrecompute(const bool forceFull)
{
    forceFullPrecompute_ = forceFull;
    precomputeChunkIdx_ = 0u;
    mergedPrecomputeInProgress_ = true;
    mergedPrecomputedJitterIndex_ = 0xFFFFFFFFu;
}

bool RenderNodeGsRender::ExecuteMergedTiledPrecompute(RENDER_NS::IRenderCommandList& cmdList, const GsTilePool& pool)
{
    if (mergedTotalActiveSplats_ == 0u) {
        mergedPrecomputeInProgress_ = false;
        return false;
    }
    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "ExecuteMergedTiledPrecompute", Math::Vec4(0.5f, 0.0f, 1.0f, 1.0f));

    const uint32_t totalGs = mergedTotalActiveSplats_;
    if (pool.sortCapacity < totalGs) {
        mergedPrecomputeInProgress_ = false;
        return false;
    }

    uint32_t chunkStart = 0u;
    uint32_t chunkCount = totalGs;
    const uint32_t precomputeChunkCount = GetMergedTiledPrecomputeChunkCount(totalGs);

    if (forceFullPrecompute_ || precomputeChunkCount == 1u) {
        forceFullPrecompute_ = false;
        precomputeChunkIdx_ = 0u;
        mergedPrecomputeInProgress_ = false;
    } else {
        const uint32_t rough = (totalGs + kAmortFrames - 1u) / kAmortFrames;
        const uint32_t chunkSize =
            (rough + kPrecomputeChunkAlignmentElems - 1u) & ~(kPrecomputeChunkAlignmentElems - 1u);
        const uint32_t idx = Math::min(precomputeChunkIdx_, precomputeChunkCount - 1u);

        chunkStart = idx * chunkSize;
        chunkCount = Math::min(chunkSize, totalGs - chunkStart);

        precomputeChunkIdx_ = idx + 1u;
        mergedPrecomputeInProgress_ = precomputeChunkIdx_ < precomputeChunkCount;
        if (!mergedPrecomputeInProgress_) {
            precomputeChunkIdx_ = 0u;
        }
    }
    const uint64_t indexOffset64 = static_cast<uint64_t>(chunkStart) * sizeof(SortKeyIndex);
    const uint64_t indexBytes64 = static_cast<uint64_t>(chunkCount) * sizeof(SortKeyIndex);
    const uint64_t cacheOffset64 = static_cast<uint64_t>(chunkStart) * sizeof(Math::UVec4);
    const uint64_t cacheBytes64 = static_cast<uint64_t>(chunkCount) * sizeof(Math::UVec4);

    if (indexOffset64 > std::numeric_limits<uint32_t>::max() || indexBytes64 > std::numeric_limits<uint32_t>::max() ||
        cacheOffset64 > std::numeric_limits<uint32_t>::max() || cacheBytes64 > std::numeric_limits<uint32_t>::max()) {
        mergedPrecomputeInProgress_ = false;
        return false;
    }

    const auto tileSortIndexDesc =
        renderNodeContextMgr_->GetGpuResourceManager().GetBufferDescriptor(pool.sortIndexPool0.GetHandle());
    const auto dataCacheDesc = renderNodeContextMgr_->GetGpuResourceManager().GetBufferDescriptor(
        gsHandlesCache_.dataStorageCacheHandle.GetHandle());
    if (static_cast<uint64_t>(tileSortIndexDesc.byteSize) < indexOffset64 + indexBytes64 ||
        static_cast<uint64_t>(dataCacheDesc.byteSize) < cacheOffset64 + cacheBytes64) {
        mergedPrecomputeInProgress_ = false;
        return false;
    }
    const bool finalSortOutputInPool1 = (dataStore_->GetGpuSortByteCount() % 2u) != 0u;
    const auto sortedPoolHandle =
        finalSortOutputInPool1 ? pool.sortIndexPool1.GetHandle() : pool.sortIndexPool0.GetHandle();

    auto& binder = tileMergedPrecomputeBinder_;
    PsoData psoData;
    if (dataStore_->GetGsRenderData()->CanUseTilePackedData()) {
        psoData = GetPsoData(binder.pl, tilePrecomputeProjectionPackedShader_, 0xFFFFffff);
    } else {
        psoData = GetPsoData(binder.pl, tilePrecomputeProjectionShader_, 0xFFFFffff);
    }
    cmdList.BindPipeline(psoData.psoHandle);

    binder.set0->BindBuffer(0u, pool.splatPoolBuffer.GetHandle(), 0);
    binder.set0->BindBuffer(1u, sortedPoolHandle, 0);
    binder.set0->BindBuffer(2u, gsHandlesCache_.dataStorageCacheHandle.GetHandle(), 0);
    binder.set0->BindBuffer(3u, cameraUniformBufferHandle_.GetHandle(), 0);
    binder.set0->BindBuffer(4u, gsDynamicDataUniformBufferHandle_.GetHandle(), 0);
    binder.set0->BindImage(5u, gsHandlesCache_.depthStoreCacheHandle.GetHandle());
    binder.set0->BindBuffer(6u, gsHandlesCache_.viewZStoreCacheHandle.GetHandle(), 0);
    binder.set0->BindBuffer(7u, pool.tileIndirectDrawBuffer.GetHandle(), 0);

    cmdList.UpdateDescriptorSet(
        binder.set0->GetDescriptorSetHandle(), binder.set0->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, binder.set0->GetDescriptorSetHandle());

    GsTilePrecomputePushConstantStruct uPc {};
    const float tileScaleFactor = dataStore_->GetRenderScaleFactor();
    uPc.viewportSize = {static_cast<float>(scissorDesc_.extentWidth) / tileScaleFactor,
                        static_cast<float>(scissorDesc_.extentHeight) / tileScaleFactor};
    uPc.jitterIndex = Math::clamp(jitterRendered_, 0u, 4u);
    uPc.gsObjectIndex = mergedGsObjectIndex_;
    uPc.renderScaleFactor = tileScaleFactor;
    uPc.elementOffset = chunkStart;
    uPc.covarianceStddev = dataStore_->GetRenderCovarianceStddev();
    uPc.enableGsOpt = dataStore_->GetGsRenderData()->enableGsOpt;

    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&uPc));
    cmdList.DispatchIndirect(preComputeIndirectDispatchBuffer_.GetHandle(), 0);

    return mergedPrecomputeInProgress_;
}


#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
void RenderNodeGsRender::ExecuteBackendFrame(
    const RENDER_NS::ILowLevelDevice& device, const RENDER_NS::RenderBackendRecordingState& recordingState)
{
    if (xegBackend_) {
        xegBackend_->ExecuteBackendFrame(device, recordingState, *dataStore_, mergedTotalActiveSplats_);
    }
}
#endif

#if RENDER_HAS_VULKAN_BACKEND || RENDER_HAS_MALEOON_BACKEND
// Destructor defined out-of-line so the IXEngineGsBackend forward declaration in
// the header is sufficient (pimpl).
RenderNodeGsRender::~RenderNodeGsRender() = default;
#endif

// for plugin / factory interface
IRenderNode* RenderNodeGsRender::Create()
{
    return new RenderNodeGsRender();
}

void RenderNodeGsRender::Destroy(IRenderNode* instance)
{
    delete (RenderNodeGsRender*)instance;
}
} // namespace GS