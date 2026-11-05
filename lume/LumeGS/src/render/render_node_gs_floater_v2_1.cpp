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

#include "render_node_gs_floater_v2_1.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <base/containers/shared_ptr.h>
#include <base/containers/string_view.h>
#include <base/math/matrix_util.h>
#include <base/util/formats.h>
#include <core/log.h>
#include <core/plugin/intf_class_register.h>
#include <3d/implementation_uids.h>
#include <3d/render/intf_render_node_scene_util.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/device/gpu_resource_desc.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_node_context_pso_manager.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/nodecontext/intf_render_node_parser_util.h>
#include <render/nodecontext/intf_render_node_util.h>

using namespace BASE_NS;
using namespace RENDER_NS;

namespace GS {
namespace {

constexpr string_view DEFAULT_FLOATER_DATA_STORE_NAME{"RenderDataStoreGsFloaterV21"};
constexpr string_view DEFAULT_GS_EDIT_DATA_STORE_NAME{"RenderDataStoreGsEdit"};
constexpr string_view DEFAULT_GS_DATA_STORE_NAME{"RenderDataStoreDefaultGs"};
constexpr uint32_t SH_DIMENSIONS = 48u;
constexpr uint32_t SH_HISTOGRAM_BINS = 256u;
constexpr uint32_t UINT_BYTES = sizeof(uint32_t);
constexpr uint32_t LOCAL_SIZE = 256u;
constexpr uint32_t RADIX_BUCKETS = 256u;

constexpr BufferUsageFlags STORAGE_BUFFER_USAGE =
    BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
    BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT |
    BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr MemoryPropertyFlags DEVICE_MEMORY = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
constexpr MemoryPropertyFlags HOST_UPLOAD_MEMORY = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;

constexpr ImageUsageFlags COLOR_SAMPLED_USAGE =
    ImageUsageFlagBits::CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
    ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT |
    ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_SRC_BIT |
    ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT;
constexpr ImageUsageFlags DEPTH_USAGE =
    ImageUsageFlagBits::CORE_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
    ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT;

constexpr GeneralBarrier COMPUTE_WRITE{
    AccessFlagBits::CORE_ACCESS_SHADER_WRITE_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
};
constexpr GeneralBarrier COMPUTE_READ_WRITE{
    AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT | AccessFlagBits::CORE_ACCESS_SHADER_WRITE_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
};
constexpr GeneralBarrier GRAPHICS_ATTACHMENT_WRITE{
    AccessFlagBits::CORE_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        AccessFlagBits::CORE_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        PipelineStageFlagBits::CORE_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        PipelineStageFlagBits::CORE_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
};
constexpr GeneralBarrier COMPUTE_SAMPLED_READ{
    AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
};
constexpr GeneralBarrier VERTEX_READ{
    AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        PipelineStageFlagBits::CORE_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
};

// D10/D12: multi-frame state machine with baked thresholds. One stage per
// ExecuteFrame: PREPARE_SCENE -> SORT_CAMERA -> PEEL x15 -> COMPACT -> DONE.
constexpr const char* COMPUTE_SHADER_URIS[] = {
    "gsshaders://computeshader/v2_1_clear.shader",
    "gsshaders://computeshader/v2_1_clear_aux.shader",
    "gsshaders://computeshader/v2_1_project_sort_init.shader",
    "gsshaders://computeshader/v2_1_sh_range.shader",
    "gsshaders://computeshader/v2_1_sh_histogram.shader",
    "gsshaders://computeshader/v2_1_sh_median.shader",
    "gsshaders://computeshader/v2_1_sh_mad_histogram.shader",
    "gsshaders://computeshader/v2_1_sh_mad.shader",
    "gsshaders://computeshader/v2_1_radix_count.shader",
    "gsshaders://computeshader/v2_1_radix_prefix.shader",
    "gsshaders://computeshader/v2_1_radix_scatter.shader",
    "gsshaders://computeshader/v2_1_depth_resolve.shader",
    "gsshaders://computeshader/v2_1_footprint.shader",
    "gsshaders://computeshader/v2_1_pre_score.shader",
    "gsshaders://computeshader/v2_1_compact_hash.shader",
    "gsshaders://computeshader/v2_1_knn_score.shader",
    "gsshaders://computeshader/v2_1_sh_propagate.shader",
    "gsshaders://computeshader/v2_1_mask_update.shader",
    "gsshaders://computeshader/v2_1_compact_indices.shader",
};
constexpr const char* COMPUTE_LAYOUT_URIS[] = {
    "gspipelinelayouts://v2_1_clear_main.shaderpl",
    "gspipelinelayouts://v2_1_clear_aux.shaderpl",
    "gspipelinelayouts://v2_1_project.shaderpl",
    "gspipelinelayouts://v2_1_sh_range.shaderpl",
    "gspipelinelayouts://v2_1_sh_histogram.shaderpl",
    "gspipelinelayouts://v2_1_sh_resolve.shaderpl",
    "gspipelinelayouts://v2_1_sh_histogram.shaderpl",
    "gspipelinelayouts://v2_1_sh_resolve.shaderpl",
    "gspipelinelayouts://v2_1_radix.shaderpl",
    "gspipelinelayouts://v2_1_radix.shaderpl",
    "gspipelinelayouts://v2_1_radix.shaderpl",
    "gspipelinelayouts://v2_1_depth.shaderpl",
    "gspipelinelayouts://v2_1_footprint.shaderpl",
    "gspipelinelayouts://v2_1_pre_score.shaderpl",
    "gspipelinelayouts://v2_1_compact.shaderpl",
    "gspipelinelayouts://v2_1_knn.shaderpl",
    "gspipelinelayouts://v2_1_sh_propagate.shaderpl",
    "gspipelinelayouts://v2_1_mask.shaderpl",
    "gspipelinelayouts://v2_1_compact_indices.shaderpl",
};
static_assert(sizeof(COMPUTE_SHADER_URIS) == sizeof(COMPUTE_LAYOUT_URIS));

constexpr const char* GRAPHICS_SHADER_URIS[] = {
    "gsshaders://shader/v2_1_weighted_depth.shader",
    "gsshaders://shader/v2_1_opaque_disc.shader",
};
constexpr const char* GRAPHICS_LAYOUT_URI = "gspipelinelayouts://v2_1_graphics.shaderpl";

// HARMONICS binding slot is per-profile: it is the next binding after all
// other resources in each pipeline layout. The binding maps below include
// HARMONICS at the correct position for each profile that needs it.

enum class GpuResourceKind : uint8_t {
    FRAME_PARAMS,
    INPUT,
    PROJECTED,
    ANALYSIS,
    STATE,
    SORT_ENTRIES,
    SORT_SCRATCH,
    HISTOGRAM,
    COMPACT_ENTRIES,
    GLOBALS,
    HASH_BUCKETS,
    SH_STATS,
    PROGRESS,
    HARMONICS,
    WEIGHTED_TEXTURE,
    OPAQUE_DEPTH_TEXTURE,
    OPAQUE_OWNER_TEXTURE,
};

struct PipelineBindingMap {
    uint32_t count;
    GpuResourceKind resources[12];
};

constexpr PipelineBindingMap COMPUTE_BINDING_MAPS[] = {
    {3u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::ANALYSIS, GpuResourceKind::STATE}},
    {6u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::HISTOGRAM, GpuResourceKind::GLOBALS,
        GpuResourceKind::HASH_BUCKETS, GpuResourceKind::PROGRESS, GpuResourceKind::SH_STATS}},
    {5u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::PROJECTED,
        GpuResourceKind::SORT_ENTRIES, GpuResourceKind::HARMONICS}},
    {5u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::GLOBALS,
        GpuResourceKind::SH_STATS, GpuResourceKind::HARMONICS}},
    {5u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::HISTOGRAM,
        GpuResourceKind::SH_STATS, GpuResourceKind::HARMONICS}},
    {4u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::HISTOGRAM, GpuResourceKind::GLOBALS,
        GpuResourceKind::SH_STATS}},
    {5u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::HISTOGRAM,
        GpuResourceKind::SH_STATS, GpuResourceKind::HARMONICS}},
    {4u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::HISTOGRAM, GpuResourceKind::GLOBALS,
        GpuResourceKind::SH_STATS}},
    {4u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::SORT_ENTRIES, GpuResourceKind::SORT_SCRATCH,
        GpuResourceKind::HISTOGRAM}},
    {4u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::SORT_ENTRIES, GpuResourceKind::SORT_SCRATCH,
        GpuResourceKind::HISTOGRAM}},
    {4u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::SORT_ENTRIES, GpuResourceKind::SORT_SCRATCH,
        GpuResourceKind::HISTOGRAM}},
    {9u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::STATE,
        GpuResourceKind::ANALYSIS, GpuResourceKind::WEIGHTED_TEXTURE, GpuResourceKind::OPAQUE_DEPTH_TEXTURE,
        GpuResourceKind::OPAQUE_OWNER_TEXTURE,
        GpuResourceKind::GLOBALS, GpuResourceKind::HARMONICS}},
    {7u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::PROJECTED,
        GpuResourceKind::STATE, GpuResourceKind::ANALYSIS, GpuResourceKind::WEIGHTED_TEXTURE,
        GpuResourceKind::HARMONICS}},
    {6u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::PROJECTED,
        GpuResourceKind::STATE, GpuResourceKind::ANALYSIS, GpuResourceKind::HARMONICS}},
    {9u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::PROJECTED,
        GpuResourceKind::STATE, GpuResourceKind::ANALYSIS, GpuResourceKind::COMPACT_ENTRIES,
        GpuResourceKind::GLOBALS, GpuResourceKind::HASH_BUCKETS, GpuResourceKind::HARMONICS}},
    {9u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::PROJECTED,
        GpuResourceKind::STATE, GpuResourceKind::ANALYSIS, GpuResourceKind::COMPACT_ENTRIES,
        GpuResourceKind::GLOBALS, GpuResourceKind::HASH_BUCKETS, GpuResourceKind::HARMONICS}},
    {10u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::PROJECTED,
        GpuResourceKind::STATE, GpuResourceKind::ANALYSIS, GpuResourceKind::COMPACT_ENTRIES,
        GpuResourceKind::GLOBALS, GpuResourceKind::HASH_BUCKETS, GpuResourceKind::SH_STATS,
        GpuResourceKind::HARMONICS}},
    {8u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::STATE,
        GpuResourceKind::ANALYSIS, GpuResourceKind::COMPACT_ENTRIES, GpuResourceKind::GLOBALS,
        GpuResourceKind::PROGRESS, GpuResourceKind::HARMONICS}},
    // D16: compact_indices -- binding 0 = frame params, binding 1 = state,
    // binding 2 = index buffer. Bound manually in RecordCompactPass.
    {3u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::STATE, GpuResourceKind::COMPACT_ENTRIES}},
};
constexpr PipelineBindingMap GRAPHICS_BINDING_MAP{
    6u, {GpuResourceKind::FRAME_PARAMS, GpuResourceKind::INPUT, GpuResourceKind::PROJECTED,
        GpuResourceKind::STATE, GpuResourceKind::SORT_ENTRIES, GpuResourceKind::HARMONICS}
};
static_assert(std::size(COMPUTE_BINDING_MAPS) == std::size(COMPUTE_SHADER_URIS));

uint32_t CeilDivide(uint32_t value, uint32_t divisor)
{
    return value == 0u ? 0u : 1u + ((value - 1u) / divisor);
}

bool CheckedMultiply(uint32_t left, uint32_t right, uint32_t& result)
{
    if (left != 0u && right > (std::numeric_limits<uint32_t>::max() / left)) {
        return false;
    }
    result = left * right;
    return true;
}

uint32_t AlignCount(uint32_t count)
{
    const uint32_t remainder = count % LOCAL_SIZE;
    if (remainder == 0u) {
        return count;
    }
    const uint32_t add = LOCAL_SIZE - remainder;
    return count <= std::numeric_limits<uint32_t>::max() - add ? count + add : 0u;
}

uint32_t HashCapacity(uint32_t count)
{
    if (count > 0x40000000u) {
        return 0u;
    }
    const uint32_t wanted = std::max(256u, count * 2u);
    uint32_t capacity = 1u;
    while (capacity < wanted && capacity <= 0x40000000u) {
        capacity <<= 1u;
    }
    return capacity >= wanted ? capacity : 0u;
}

uint32_t RadixHistogramBytes(uint32_t count)
{
    const uint32_t groups = CeilDivide(count, LOCAL_SIZE);
    if (groups >= (std::numeric_limits<uint32_t>::max() / RADIX_BUCKETS)) {
        return 0u;
    }
    uint32_t bytes = 0u;
    return CheckedMultiply((groups + 1u) * RADIX_BUCKETS, UINT_BYTES, bytes) ? bytes : 0u;
}

bool IsValid(const RenderHandleReference& handle)
{
    return RenderHandleUtil::IsValid(handle.GetHandle());
}

} // namespace

void RenderNodeGsFloaterV21::InitNode(IRenderNodeContextManager& context)
{
    context_ = &context;
    ParseInputs();

    // Resolve scene data store prefix (same pattern as render_node_gs_render.cpp).
    // The data stores are created with a scene-specific prefix, so we must
    // prepend it to our default names.
    if (auto* classRegister = context.GetRenderContext().GetInterface<CORE_NS::IClassRegister>()) {
        if (auto* renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
                *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
            renderNodeSceneUtil) {
            const auto& graphData = context.GetRenderNodeGraphData();
            const auto stores = renderNodeSceneUtil->GetSceneRenderDataStores(
                context, graphData.renderNodeGraphDataStoreName);
            const auto prefix = string(stores.dataStoreNamePrefix);
            if (!prefix.empty()) {
                dataStoreName_ = prefix + dataStoreName_;
                gsEditDataStoreName_ = prefix + gsEditDataStoreName_;
                defaultGsDataStoreName_ = prefix + defaultGsDataStoreName_;
            }
            // Use the scene-resolved camera data store name if available.
            if (!stores.dataStoreNameCamera.empty()) {
                cameraDataStoreName_ = string(stores.dataStoreNameCamera);
            }
        }
    }
    LoadPipelines();
    CreatePipelineBinders();
    CreatePipelineStates();
    nearestSampler_ = context.GetGpuResourceManager().GetSamplerHandle("CORE_DEFAULT_SAMPLER_NEAREST_CLAMP");
    pipelinesReady_ = PipelinesReady() && RenderHandleUtil::IsValid(nearestSampler_);

    // Create FrameParams UBO once in InitNode (same pattern as
    // render_node_gs_render.cpp camera UBO). Reused every frame, never
    // released. This avoids first-frame MapBuffer failure caused by lazy
    // GPU memory allocation when creating in PreExecuteFrame.
    {
        const uint32_t uniformUsage = static_cast<uint32_t>(
            BufferUsageFlagBits::CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT);
        const uint32_t hostUpload = static_cast<uint32_t>(HOST_UPLOAD_MEMORY);
        [[maybe_unused]] const bool ok = CreateBuffer(
            resources_.frameParams, sizeof(GpuFrameParams), uniformUsage, hostUpload, "V21.FrameParams",
            EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_RING_BUFFER);
    }
}

void RenderNodeGsFloaterV21::ParseInputs()
{
    if (!context_) {
        return;
    }
    const auto json = context_->GetNodeJson();
    const auto& parser = context_->GetRenderNodeParserUtil();
    const string parsedFloaterName = parser.GetStringValue(json, "dataStoreName");
    dataStoreName_ = parsedFloaterName.empty() ? string(DEFAULT_FLOATER_DATA_STORE_NAME) : parsedFloaterName;
    const string parsedEditName = parser.GetStringValue(json, "gsEditDataStoreName");
    gsEditDataStoreName_ = parsedEditName.empty() ? string(DEFAULT_GS_EDIT_DATA_STORE_NAME) : parsedEditName;
    const string parsedDefaultGsName = parser.GetStringValue(json, "defaultGsDataStoreName");
    defaultGsDataStoreName_ = parsedDefaultGsName.empty() ? string(DEFAULT_GS_DATA_STORE_NAME) : parsedDefaultGsName;
}

void RenderNodeGsFloaterV21::PreExecuteFrame()
{
    // Multi-frame state machine: while pipeline is active (stage_ != IDLE),
    // preserve hasPendingFloaterRemoval_ and skip re-initialization.
    if (stage_ != Stage::IDLE) {
        // Report progress to the data store for the JS layer to poll.
        if (gsEditStore_ && hasPendingFloaterRemoval_) {
            const float progress = static_cast<float>(currentPeel_) / static_cast<float>(MAX_PEEL_COUNT);
            gsEditStore_->SetFloaterRemovalProgress(progress);
        }
        // Re-upload params for the current peel round (updated in RecordPeel).
        if (stage_ == Stage::PEEL) {
            // Auto-stop: check if the GPU wrote a stop reason for the
            // previous peel. The globals buffer is host-visible and the
            // previous frame's GPU work is guaranteed complete by now.
            if (peelsCompleted_ >= MINIMUM_PEELS) {
                const uint32_t reason = ObserveStopReason();
                if (reason != 0u) {
                    CORE_LOG_I("RenderNodeGsFloaterV21: auto-stop after %u peel(s), reason=%u",
                        peelsCompleted_, reason);
                    stage_ = Stage::COMPACT;
                    return;
                }
            }
            FillGpuParams();
            if (!UploadGpuParams()) {
                errorState_ = true;
            }
        }
        return;
    }

    hasPendingFloaterRemoval_ = false;
    errorState_ = false;
    gsHandlesCache_.Invalidate();

    if (!context_) {
        return;
    }

    // Resolve data stores.
    auto& manager = context_->GetRenderDataStoreManager();
    if (!floaterStore_) {
        floaterStore_ = static_cast<RenderDataStoreGsFloaterV21*>(
            manager.GetRenderDataStore(dataStoreName_));
    }
    if (!gsEditStore_) {
        gsEditStore_ = static_cast<RenderDataStoreGsEdit*>(
            manager.GetRenderDataStore(gsEditDataStoreName_));
    }
    if (!defaultGsStore_) {
        defaultGsStore_ = static_cast<RenderDataStoreDefaultGS*>(
            manager.GetRenderDataStore(defaultGsDataStoreName_));
    }
    if (!cameraStore_) {
        cameraStore_ = static_cast<CORE3D_NS::IRenderDataStoreDefaultCamera*>(
            manager.GetRenderDataStore(cameraDataStoreName_));
    }
    if (!defaultGsStore_ || !gsEditStore_) {
        return;
    }

    // D3/D4: cache LumeGS handles from RenderDataStoreDefaultGS.
    CacheReadOnlyHandles();
    if (!gsHandlesCache_.IsValid()) {
        return;
    }

    // D11/D17: read pending FloaterRemoval actions from the edit stack.
    if (!ResolvePendingFloaterRemoval()) {
        return;
    }

    // Extract camera intrinsics and viewport. Prefer CameraIntrinsics from the
    // FloaterRemoval action (already set in ResolvePendingFloaterRemoval);
    // fall back to the default GS render resolution.
    auto* renderData = defaultGsStore_->GetGsRenderData();
    if (!renderData) {
        return;
    }
    if (renderWidth_ == 0u || renderHeight_ == 0u) {
        const Math::UVec2 resolution = defaultGsStore_->GetRenderResolution();
        renderWidth_ = resolution.x;
        renderHeight_ = resolution.y;
    }
    if (renderWidth_ == 0u || renderHeight_ == 0u) {
        return;
    }

    // CC4: splat count derived from gsSplatData buffer byte size / 64.
    auto& gpu = context_->GetGpuResourceManager();
    const GpuBufferDesc splatDesc = gpu.GetBufferDescriptor(gsHandlesCache_.gsSplatData.GetHandle());
    gaussianCount_ = splatDesc.byteSize / GS_SPLAT_DATA_STRIDE;

    // D17: zero-Gaussian guard -- push FloaterRemoval marker only, return early.
    if (gaussianCount_ == 0u || gaussianCount_ == INVALID_GAUSSIAN_ID) {
        hasPendingFloaterRemoval_ = true;
        return;
    }

    // Create or resize buffers for this frame's pipeline.
    if (!CreateTaskResources()) {
        errorState_ = true;
        return;
    }

    // D19: fill and upload GpuFrameParams once at pipeline start.
    FillGpuParams();
    if (!UploadGpuParams()) {
        errorState_ = true;
        return;
    }

    // Start the multi-frame state machine.
    stage_ = Stage::PREPARE_SCENE;
    currentPeel_ = 0u;
    peelsCompleted_ = 0u;
    // Mark pipeline in progress so SaveToPLY defers export until completion.
    gsEditStore_->SetFloaterRemovalInProgress(true);
}

void RenderNodeGsFloaterV21::CacheReadOnlyHandles()
{
    if (!defaultGsStore_) {
        return;
    }
    auto* renderData = defaultGsStore_->GetGsRenderData();
    if (!renderData) {
        return;
    }
    auto lg = defaultGsStore_->GetDataLockGuard();
    gsHandlesCache_.gsSplatData = renderData->gsSplatData;
    gsHandlesCache_.gsHarmonicsDataRef = renderData->gsHarmonicsDataRef;
    gsHandlesCache_.gsIndicesRef = renderData->gsIndicesRef;
    gsHandlesCache_.sampler = renderData->sampler;
}

bool RenderNodeGsFloaterV21::ResolvePendingFloaterRemoval()
{
    if (!gsEditStore_) {
        return false;
    }
    // D11: Only check the TOP of the stack (O(1)). A user-initiated
    // FloaterRemoval has FloaterRemovalOptions params and sits at the top
    // when first pushed. After we process it, we push SelectByIndex +
    // RemoveSelected + FloaterRemoval(NoParams marker), so the top becomes
    // the NoParams marker -- which we skip.
    const auto& stack = gsEditStore_->GetEditActionStack();
    if (stack.empty()) {
        return false;
    }
    const auto& top = stack.back();
    if (top.type != GsEditActionType::FloaterRemoval) {
        return false;
    }
    // User-initiated has FloaterRemovalOptions params; marker has NoParams.
    if (!top.params || !top.params->IsFloaterRemovalOptions()) {
        return false;
    }
    auto options = static_pointer_cast<GS_EDIT_PARAMS::FloaterRemovalOptions>(top.params);
    hasPendingFloaterRemoval_ = true;

    // Write the task ID so the JS layer can correlate progress with its task.
    gsEditStore_->SetFloaterRemovalTaskId(options->taskId);

    // Read camera from the current rendering camera data store.
    if (!cameraStore_) {
        CORE_LOG_E("RenderNodeGsFloaterV21: camera data store not available");
        return false;
    }
    const auto& cameras = cameraStore_->GetCameras();
    if (cameras.empty()) {
        CORE_LOG_E("RenderNodeGsFloaterV21: no cameras available");
        return false;
    }
    const auto& cam = cameras[0u];
    const Math::Mat4X4 viewMatrix = cam.matrices.view;
    const Math::Mat4X4 projMatrix = cam.matrices.proj;

    // Viewport from camera render resolution.
    const uint32_t width = cam.renderResolution.x;
    const uint32_t height = cam.renderResolution.y;
    if (width > 0u && height > 0u) {
        renderWidth_ = width;
        renderHeight_ = height;
    }

    // Compute intrinsics from projection matrix (same formula as the old JS layer).
    intrinsicsFx_ = projMatrix.x.x * static_cast<float>(renderWidth_) * 0.5f;
    intrinsicsFy_ = BASE_NS::Math::abs(projMatrix.y.y) * static_cast<float>(renderHeight_) * 0.5f;
    intrinsicsCx_ = static_cast<float>(renderWidth_) * 0.5f;
    intrinsicsCy_ = static_cast<float>(renderHeight_) * 0.5f;

    cameraViewMatrix_ = viewMatrix;
    cameraProjMatrix_ = projMatrix;
    const Math::Mat4X4 inverseView = BASE_NS::Math::Inverse(viewMatrix);
    cameraPosition_ = {
        inverseView.data[12u], inverseView.data[13u], inverseView.data[14u], 1.0f};
    // Bake the GS object world matrix into the view matrix (same as before).
    if (defaultGsStore_) {
        const auto& primitives = defaultGsStore_->GetGsPrimitives();
        if (!primitives.empty()) {
            const Math::Mat4X4 worldMat = primitives[0u].worldMatrix;
            cameraViewMatrix_ = cameraViewMatrix_ * worldMat;
            const Math::Mat4X4 inverseViewWorld = BASE_NS::Math::Inverse(cameraViewMatrix_);
            cameraPosition_ = { inverseViewWorld.data[12u], inverseViewWorld.data[13u],
                inverseViewWorld.data[14u], 1.0f };
        }
    }
    return true;
}

bool RenderNodeGsFloaterV21::CreateBuffer(RenderHandleReference& target, uint32_t bytes, uint32_t usage,
    uint32_t memoryProperties, const char* name,
    EngineBufferCreationFlags creationFlags)
{
    if (!context_ || bytes == 0u) {
        return false;
    }
    const GpuBufferDesc desc{
        static_cast<BufferUsageFlags>(usage),
        static_cast<MemoryPropertyFlags>(memoryProperties),
        creationFlags,
        bytes,
        Format::BASE_FORMAT_UNDEFINED,
    };
    target = context_->GetGpuResourceManager().Create(name, desc);
    const bool valid = IsValid(target);
    if (!valid) {
        CORE_LOG_E("RenderNodeGsFloaterV21: failed to create buffer '%s' (%u bytes)",
            name ? name : "<unnamed>", bytes);
    }
    return valid;
}

uint32_t RenderNodeGsFloaterV21::ObserveStopReason()
{
    if (!context_ || !IsValid(resources_.globals)) {
        return 0u;
    }
    auto& gpu = context_->GetGpuResourceManager();
    void* mapped = gpu.MapBufferMemory(resources_.globals.GetHandle());
    if (!mapped) {
        return 0u;
    }
    GpuGlobalCounters counters;
    BASE_NS::CloneData(&counters, sizeof(GpuGlobalCounters), mapped, sizeof(GpuGlobalCounters));
    gpu.UnmapBuffer(resources_.globals.GetHandle());
    return counters.stopReason;
}

bool RenderNodeGsFloaterV21::CreateImage(RenderHandleReference& target, Format format, uint32_t usage,
    const char* name)
{
    if (!context_) {
        return false;
    }
    const GpuImageDesc desc{
        ImageType::CORE_IMAGE_TYPE_2D,
        ImageViewType::CORE_IMAGE_VIEW_TYPE_2D,
        format,
        ImageTiling::CORE_IMAGE_TILING_OPTIMAL,
        static_cast<ImageUsageFlags>(usage),
        MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
        0u,
        EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS,
        renderWidth_,
        renderHeight_,
        1u,
        1u,
        1u,
        SampleCountFlagBits::CORE_SAMPLE_COUNT_1_BIT,
        {},
    };
    target = context_->GetGpuResourceManager().Create(name, desc);
    const bool valid = IsValid(target);
    if (!valid) {
        CORE_LOG_E("RenderNodeGsFloaterV21: failed to create image '%s' (%ux%u)",
            name ? name : "<unnamed>", renderWidth_, renderHeight_);
    }
    return valid;
}

bool RenderNodeGsFloaterV21::CreateTaskResources()
{
    const auto& gpuManager = context_->GetGpuResourceManager();
    const auto supports = [&gpuManager](Format format, FormatFeatureFlags required) {
        return (gpuManager.GetFormatProperties(format).optimalTilingFeatures & required) == required;
    };
    const FormatFeatureFlags weightedFeatures = FormatFeatureFlagBits::CORE_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        FormatFeatureFlagBits::CORE_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
        FormatFeatureFlagBits::CORE_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        FormatFeatureFlagBits::CORE_FORMAT_FEATURE_TRANSFER_SRC_BIT |
        FormatFeatureFlagBits::CORE_FORMAT_FEATURE_TRANSFER_DST_BIT;
    const FormatFeatureFlags floatFeatures = FormatFeatureFlagBits::CORE_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
        FormatFeatureFlagBits::CORE_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (!supports(Format::BASE_FORMAT_R16G16B16A16_SFLOAT, weightedFeatures) ||
        !supports(Format::BASE_FORMAT_R32_SFLOAT, floatFeatures) ||
        !supports(Format::BASE_FORMAT_R32_UINT,
            FormatFeatureFlagBits::CORE_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                FormatFeatureFlagBits::CORE_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !supports(Format::BASE_FORMAT_D32_SFLOAT,
            FormatFeatureFlagBits::CORE_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        CORE_LOG_E("RenderNodeGsFloaterV21: required attachment format is unsupported");
        return false;
    }
    const uint32_t count = gaussianCount_;
    const uint32_t alignedCount = AlignCount(count);
    const uint32_t hashCapacity = HashCapacity(count);
    if (alignedCount == 0u || hashCapacity == 0u) {
        return false;
    }

    uint32_t projectedBytes = 0u;
    uint32_t analysisBytes = 0u;
    uint32_t stateBytes = 0u;
    uint32_t sortBytes = 0u;
    uint32_t compactBytes = 0u;
    uint32_t hashBytes = 0u;
    uint32_t shStatsBytes = 0u;
    uint32_t shHistogramBytes = 0u;
    uint32_t progressBytes = 0u;
    if (!CheckedMultiply(count, sizeof(GpuProjectedGaussian), projectedBytes) ||
        !CheckedMultiply(count, sizeof(GpuGaussianAnalysis), analysisBytes) ||
        !CheckedMultiply(count, sizeof(GpuGaussianState), stateBytes) ||
        !CheckedMultiply(alignedCount, sizeof(GpuSortEntry), sortBytes) ||
        !CheckedMultiply(count, sizeof(GpuCompactEntry), compactBytes) ||
        !CheckedMultiply(hashCapacity, sizeof(GpuHashBucket), hashBytes) ||
        !CheckedMultiply(SH_DIMENSIONS, sizeof(GpuShRobustStats), shStatsBytes) ||
        !CheckedMultiply(SH_DIMENSIONS * SH_HISTOGRAM_BINS,
            sizeof(GpuHistogramBin), shHistogramBytes) ||
        !CheckedMultiply(count, sizeof(GpuProgressRecord), progressBytes)) {
        return false;
    }
    const uint32_t radixBytes = RadixHistogramBytes(count);
    const uint32_t histogramBytes = std::max(radixBytes, shHistogramBytes);
    if (radixBytes == 0u || histogramBytes == 0u) {
        return false;
    }

    // D16: compact index buffer = (count + 1) * uint32_t.
    // [0] = compacted count, [1..N] = gaussian indices.
    uint32_t compactIndexBytes = 0u;
    if (!CheckedMultiply(count + 1u, UINT_BYTES, compactIndexBytes)) {
        return false;
    }

    const uint32_t storage = static_cast<uint32_t>(STORAGE_BUFFER_USAGE);
    const uint32_t device = static_cast<uint32_t>(DEVICE_MEMORY);
    const uint32_t hostUpload = static_cast<uint32_t>(HOST_UPLOAD_MEMORY);
    const uint32_t uniformUsage = static_cast<uint32_t>(
        BufferUsageFlagBits::CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
        BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT);

    bool ok = true;
    // FrameParams UBO is created once in InitNode and reused. Skip if valid.
    if (!IsValid(resources_.frameParams)) {
        ok &= CreateBuffer(
            resources_.frameParams, sizeof(GpuFrameParams), uniformUsage, hostUpload, "V21.FrameParams",
            EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_RING_BUFFER);
        if (!ok) {
            return false;
        }
    }
    ok &= CreateBuffer(resources_.projected, projectedBytes, storage, device, "V21.Projected");
    ok &= CreateBuffer(resources_.analysis, analysisBytes, storage, device, "V21.GaussianAnalysis");
    ok &= CreateBuffer(resources_.state, stateBytes, storage, device, "V21.GaussianState");
    ok &= CreateBuffer(resources_.sortEntries, sortBytes, storage, device, "V21.SortEntries");
    ok &= CreateBuffer(resources_.sortScratch, sortBytes, storage, device, "V21.SortScratch");
    ok &= CreateBuffer(resources_.histogram, histogramBytes, storage, device, "V21.HistogramScratch");
    ok &= CreateBuffer(resources_.compactEntries, compactBytes, storage, device, "V21.CompactEntries");
    ok &= CreateBuffer(resources_.globals, sizeof(GpuGlobalCounters), storage, hostUpload,
        "V21.GlobalCounters",
        EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS |
        EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE);
    ok &= CreateBuffer(resources_.hashBuckets, hashBytes, storage, device, "V21.HashBuckets");
    ok &= CreateBuffer(resources_.shStats, shStatsBytes, storage, device, "V21.ShRobustStats");
    ok &= CreateBuffer(
        resources_.compactIndexBuffer, compactIndexBytes, storage, hostUpload, "V21.CompactIndexBuffer",
        EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS |
        EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE);
    ok &= CreateBuffer(resources_.progress, progressBytes, storage, device, "V21.ProgressRecords");
    if (!ok) {
        return false;
    }

    const uint32_t colorSampled = static_cast<uint32_t>(COLOR_SAMPLED_USAGE);
    const uint32_t depthUsage = static_cast<uint32_t>(DEPTH_USAGE);
    ok &= CreateImage(resources_.currentPreview, Format::BASE_FORMAT_R16G16B16A16_SFLOAT,
        colorSampled, "V21.CurrentPreview");
    ok &= CreateImage(resources_.weightedAccumulator, Format::BASE_FORMAT_R16G16B16A16_SFLOAT,
        colorSampled, "V21.WeightedAccumulator");
    ok &= CreateImage(resources_.opaqueLinearDepth, Format::BASE_FORMAT_R32_SFLOAT,
        colorSampled, "V21.OpaqueLinearAttachment");
    ok &= CreateImage(resources_.opaqueOwnerId, Format::BASE_FORMAT_R32_UINT,
        colorSampled, "V21.OpaqueOwner");
    ok &= CreateImage(resources_.opaqueHardwareDepth, Format::BASE_FORMAT_D32_SFLOAT,
        depthUsage, "V21.OpaqueHardwareDepth");

    resourcesReady_ = ok;
    return ok;
}

void RenderNodeGsFloaterV21::ReleaseTaskResources()
{
    // Preserve FrameParams UBO (created once in InitNode, reused every frame).
    auto savedFrameParams = std::move(resources_.frameParams);
    resources_ = {};
    resources_.frameParams = std::move(savedFrameParams);
    resourcesReady_ = false;
}

void RenderNodeGsFloaterV21::FillGpuParams()
{
    // D12: all thresholds are baked -- no external config struct.
    gpuParams_ = {};
    gpuParams_.viewProjection = cameraProjMatrix_ * cameraViewMatrix_;
    gpuParams_.view = cameraViewMatrix_;
    gpuParams_.cameraPosition = cameraPosition_;
    gpuParams_.intrinsicsViewport = {
        intrinsicsFx_, intrinsicsFy_,
        static_cast<float>(renderWidth_), static_cast<float>(renderHeight_)};
    gpuParams_.principalNearFar = {intrinsicsCx_, intrinsicsCy_, 0.01f, 300.0f};
    // D12: baked thresholds (matching GsFloaterV21Config defaults).
    gpuParams_.discAlphaGap = {5.991f, 1.0f / 255.0f, 0.05f, 0.35f};
    gpuParams_.weights0 = {0.34f, 0.24f, 0.20f, 0.14f};
    gpuParams_.weights1 = {0.08f, 0.55f, 0.72f, 0.03f};
    gpuParams_.candidate0 = {0.62f, 0.52f, 0.18f, 0.055f};
    gpuParams_.candidate1 = {6.0f, 0.10f, 0.035f, 0.04f};
    gpuParams_.seed0 = {0.92f, 0.98f, 0.98f, 0.35f};
    gpuParams_.seed1 = {0.10f, 24.0f, 0.20f, 0.0f};
    gpuParams_.weak0 = {0.25f, 0.02f, 0.10f, 0.05f};
    gpuParams_.weak1 = {1.0f, 0.0f, 0.0f, 0.0f};
    gpuParams_.graph0 = {0.25f, 3.0f, 0.02f, 0.08f};
    gpuParams_.graph1 = {1.5f, 2.0f, 0.35f, 16.0f};
    gpuParams_.shWeights = {0.50f, 0.30f, 0.15f, 0.05f};
    gpuParams_.shLimits = {8.0f, 0.75f, 1.25f, 2.2239f};
    gpuParams_.sampleGates = {5.0f, 3.0f, 0.04f, 0.03f};
    gpuParams_.adaptive0 = {0.06f, 0.05f, 0.60f, 2.0f};
    gpuParams_.protection0 = {3.0f, 0.65f, 2.0f, 0.25f};
    gpuParams_.protection1 = {2.0f, 2.0f, 0.0f, 0.0f};
    gpuParams_.counts[0] = gaussianCount_;
    gpuParams_.counts[1] = renderWidth_;
    gpuParams_.counts[2] = renderHeight_;
    gpuParams_.counts[3] = 3u; // D12: SH degree 3 baked.
    gpuParams_.peel[0] = currentPeel_;
    gpuParams_.peel[1] = MAX_PEEL_COUNT;

    uint32_t pixelCount = 1u;
    (void)CheckedMultiply(renderWidth_, renderHeight_, pixelCount);
    const float safeScale = std::floor(4294967040.0f / std::max(1.0f, static_cast<float>(pixelCount)));
    gpuParams_.peel[2] = static_cast<uint32_t>(std::max(1.0f, std::min(65535.0f, safeScale)));
    gpuParams_.peel[3] = HashCapacity(gaussianCount_);
}

bool RenderNodeGsFloaterV21::UploadGpuParams()
{
    if (!context_) {
        return false;
    }
    if (!IsValid(resources_.frameParams)) {
        return false;
    }
    auto& gpu = context_->GetGpuResourceManager();
    void* destination = gpu.MapBuffer(resources_.frameParams.GetHandle());
    if (!destination) {
        return false;
    }
    // AGENTS.md: use BASE_NS::CloneData, not std::memcpy.
    BASE_NS::CloneData(destination, sizeof(gpuParams_), &gpuParams_, sizeof(gpuParams_));
    gpu.UnmapBuffer(resources_.frameParams.GetHandle());
    return true;
}

void RenderNodeGsFloaterV21::LoadPipelines()
{
    if (!context_) {
        return;
    }
    auto& shaderManager = context_->GetShaderManager();
    for (uint32_t index = 0u; index < computePipelines_.size(); ++index) {
        computePipelines_[index].shader = shaderManager.GetShaderHandle(COMPUTE_SHADER_URIS[index]);
        computePipelines_[index].layout = shaderManager.GetPipelineLayout(
            shaderManager.GetPipelineLayoutHandle(COMPUTE_LAYOUT_URIS[index]));
    }
    for (uint32_t index = 0u; index < graphicsPipelines_.size(); ++index) {
        graphicsPipelines_[index].shader = shaderManager.GetShaderHandle(GRAPHICS_SHADER_URIS[index]);
        graphicsPipelines_[index].layout = shaderManager.GetPipelineLayout(
            shaderManager.GetPipelineLayoutHandle(GRAPHICS_LAYOUT_URI));
    }
}

void RenderNodeGsFloaterV21::CreatePipelineBinders()
{
    if (!context_) {
        return;
    }
    DescriptorCounts reserve;
    for (const auto& pipeline : computePipelines_) {
        const DescriptorCounts counts = context_->GetRenderNodeUtil().GetDescriptorCounts(pipeline.layout);
        for (const auto& count : counts.counts) {
            reserve.counts.push_back(count);
        }
    }
    for (const auto& pipeline : graphicsPipelines_) {
        const DescriptorCounts counts = context_->GetRenderNodeUtil().GetDescriptorCounts(pipeline.layout);
        for (const auto& count : counts.counts) {
            reserve.counts.push_back(count);
        }
    }
    auto& descriptorManager = context_->GetDescriptorSetManager();
    descriptorManager.ResetAndReserve(reserve);
    for (auto& pipeline : computePipelines_) {
        pipeline.binder = descriptorManager.CreateDescriptorSetBinder(
            descriptorManager.CreateDescriptorSet(0u, pipeline.layout),
            pipeline.layout.descriptorSetLayouts[0].bindings);
    }
    for (auto& pipeline : graphicsPipelines_) {
        pipeline.binder = descriptorManager.CreateDescriptorSetBinder(
            descriptorManager.CreateDescriptorSet(0u, pipeline.layout),
            pipeline.layout.descriptorSetLayouts[0].bindings);
    }
}

void RenderNodeGsFloaterV21::CreatePipelineStates()
{
    if (!context_) {
        return;
    }
    auto& psoManager = context_->GetPsoManager();
    for (auto& pipeline : computePipelines_) {
        pipeline.pso = psoManager.GetComputePsoHandle(pipeline.shader, pipeline.layout, {});
    }
    constexpr DynamicStateEnum dynamicStates[] = {
        DynamicStateEnum::CORE_DYNAMIC_STATE_ENUM_VIEWPORT,
        DynamicStateEnum::CORE_DYNAMIC_STATE_ENUM_SCISSOR,
    };
    auto& shaderManager = context_->GetShaderManager();
    for (auto& pipeline : graphicsPipelines_) {
        const auto shaderData = shaderManager.GetGraphicsShaderDataByShaderHandle(pipeline.shader);
        pipeline.pso = psoManager.GetGraphicsPsoHandle(shaderData, {}, {dynamicStates, 2u});
    }
}

bool RenderNodeGsFloaterV21::PipelinesReady() const
{
    for (const auto& pipeline : computePipelines_) {
        if (!RenderHandleUtil::IsValid(pipeline.shader) ||
            !RenderHandleUtil::IsValid(pipeline.pso) || !pipeline.binder) {
            return false;
        }
    }
    for (const auto& pipeline : graphicsPipelines_) {
        if (!RenderHandleUtil::IsValid(pipeline.shader) ||
            !RenderHandleUtil::IsValid(pipeline.pso) || !pipeline.binder) {
            return false;
        }
    }
    return true;
}

void RenderNodeGsFloaterV21::BindResources(Pipeline& pipeline, IRenderCommandList& commandList,
    uint32_t pipelineIndex, bool graphicsPipeline)
{
    auto& binder = *pipeline.binder;
    binder.ClearBindings();
    const PipelineBindingMap& map = graphicsPipeline ? GRAPHICS_BINDING_MAP : COMPUTE_BINDING_MAPS[pipelineIndex];
    const auto bindBuffer = [&binder](uint32_t binding, RenderHandle handle) {
        binder.BindBuffer(binding, handle, 0u);
    };
    for (uint32_t localBinding = 0u; localBinding < map.count; ++localBinding) {
        switch (map.resources[localBinding]) {
            case GpuResourceKind::FRAME_PARAMS:
                bindBuffer(localBinding, resources_.frameParams.GetHandle()); break;
            case GpuResourceKind::INPUT:
                bindBuffer(localBinding, gsHandlesCache_.gsSplatData.GetHandle()); break;
            case GpuResourceKind::PROJECTED:
                bindBuffer(localBinding, resources_.projected.GetHandle()); break;
            case GpuResourceKind::ANALYSIS:
                bindBuffer(localBinding, resources_.analysis.GetHandle()); break;
            case GpuResourceKind::STATE:
                bindBuffer(localBinding, resources_.state.GetHandle()); break;
            case GpuResourceKind::SORT_ENTRIES:
                bindBuffer(localBinding, resources_.sortEntries.GetHandle()); break;
            case GpuResourceKind::SORT_SCRATCH:
                bindBuffer(localBinding, resources_.sortScratch.GetHandle()); break;
            case GpuResourceKind::HISTOGRAM:
                bindBuffer(localBinding, resources_.histogram.GetHandle()); break;
            case GpuResourceKind::COMPACT_ENTRIES:
                bindBuffer(localBinding, resources_.compactEntries.GetHandle()); break;
            case GpuResourceKind::GLOBALS:
                bindBuffer(localBinding, resources_.globals.GetHandle()); break;
            case GpuResourceKind::HASH_BUCKETS:
                bindBuffer(localBinding, resources_.hashBuckets.GetHandle()); break;
            case GpuResourceKind::SH_STATS:
                bindBuffer(localBinding, resources_.shStats.GetHandle()); break;
            case GpuResourceKind::PROGRESS:
                bindBuffer(localBinding, resources_.progress.GetHandle()); break;
            case GpuResourceKind::HARMONICS:
                bindBuffer(localBinding, gsHandlesCache_.gsHarmonicsDataRef.GetHandle()); break;
            case GpuResourceKind::WEIGHTED_TEXTURE:
                binder.BindImage(localBinding, resources_.weightedAccumulator.GetHandle(), nearestSampler_); break;
            case GpuResourceKind::OPAQUE_DEPTH_TEXTURE:
                binder.BindImage(localBinding, resources_.opaqueLinearDepth.GetHandle(), nearestSampler_); break;
            case GpuResourceKind::OPAQUE_OWNER_TEXTURE:
                binder.BindImage(localBinding, resources_.opaqueOwnerId.GetHandle(), nearestSampler_); break;
            default: break;
        }
    }
    commandList.UpdateDescriptorSet(binder.GetDescriptorSetHandle(), binder.GetDescriptorSetLayoutBindingResources());
    commandList.BindDescriptorSet(0u, binder.GetDescriptorSetHandle());
}

void RenderNodeGsFloaterV21::BindCompute(ComputePipeline pipelineId, IRenderCommandList& commandList)
{
    auto& pipeline = computePipelines_[static_cast<uint32_t>(pipelineId)];
    commandList.BindPipeline(pipeline.pso);
    BindResources(pipeline, commandList, static_cast<uint32_t>(pipelineId), false);
}

void RenderNodeGsFloaterV21::BindGraphics(GraphicsPipeline pipelineId, IRenderCommandList& commandList)
{
    auto& pipeline = graphicsPipelines_[static_cast<uint32_t>(pipelineId)];
    commandList.BindPipeline(pipeline.pso);
    BindResources(pipeline, commandList, static_cast<uint32_t>(pipelineId), true);
}

void RenderNodeGsFloaterV21::Dispatch(ComputePipeline pipelineId, uint32_t x,
    const DispatchPushConstant& push, IRenderCommandList& commandList)
{
    if (x == 0u) {
        return;
    }
    const uint32_t pipelineIndex = static_cast<uint32_t>(pipelineId);
    auto& pipeline = computePipelines_[pipelineIndex];
    BindCompute(pipelineId, commandList);
    commandList.PushConstant(pipeline.layout.pushConstant, reinterpret_cast<const uint8_t*>(&push));
    commandList.Dispatch(x, 1u, 1u);
}

void RenderNodeGsFloaterV21::RecordComputeBarrier(IRenderCommandList& commandList) const
{
    commandList.CustomMemoryBarrier(COMPUTE_WRITE, COMPUTE_READ_WRITE);
    commandList.AddCustomBarrierPoint();
}

void RenderNodeGsFloaterV21::RecordGraphicsToComputeBarrier(IRenderCommandList& commandList) const
{
    commandList.CustomMemoryBarrier(GRAPHICS_ATTACHMENT_WRITE, COMPUTE_SAMPLED_READ);
    commandList.AddCustomBarrierPoint();
}

void RenderNodeGsFloaterV21::RecordComputeToGraphicsBarrier(IRenderCommandList& commandList) const
{
    commandList.CustomMemoryBarrier(COMPUTE_WRITE, VERTEX_READ);
    commandList.AddCustomBarrierPoint();
}

void RenderNodeGsFloaterV21::BuildWeightedRenderPass(
    RenderPassDesc& pass, RenderPassSubpassDesc& subpass) const
{
    pass = {};
    subpass = {};
    pass.attachmentCount = 2u;
    pass.attachmentHandles[0u] = resources_.currentPreview.GetHandle();
    pass.attachmentHandles[1u] = resources_.weightedAccumulator.GetHandle();
    for (uint32_t index = 0u; index < pass.attachmentCount; ++index) {
        pass.attachments[index].loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
        pass.attachments[index].storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
        pass.attachments[index].clearValue.color.float32[0u] = 0.0f;
        pass.attachments[index].clearValue.color.float32[1u] = 0.0f;
        pass.attachments[index].clearValue.color.float32[2u] = 0.0f;
        pass.attachments[index].clearValue.color.float32[3u] = 0.0f;
    }
    pass.renderArea = {0, 0, renderWidth_, renderHeight_};
    pass.subpassCount = 1u;
    subpass.colorAttachmentCount = 2u;
    subpass.colorAttachmentIndices[0u] = 0u;
    subpass.colorAttachmentIndices[1u] = 1u;
}

void RenderNodeGsFloaterV21::BuildOpaqueRenderPass(
    RenderPassDesc& pass, RenderPassSubpassDesc& subpass) const
{
    pass = {};
    subpass = {};
    // 3 attachments: RGBA16F (R32_SFLOAT linear depth), R32UI (owner id), D32_SFLOAT (hardware depth).
    pass.attachmentCount = 3u;
    pass.attachmentHandles[0u] = resources_.opaqueLinearDepth.GetHandle();
    pass.attachmentHandles[1u] = resources_.opaqueOwnerId.GetHandle();
    pass.attachmentHandles[2u] = resources_.opaqueHardwareDepth.GetHandle();
    pass.attachments[0u].loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
    pass.attachments[0u].storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
    pass.attachments[0u].clearValue.color.float32[0u] = 0.0f;
    pass.attachments[1u].loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
    pass.attachments[1u].storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
    pass.attachments[1u].clearValue.color.uint32[0u] = INVALID_GAUSSIAN_ID;
    pass.attachments[2u].loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
    pass.attachments[2u].storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
    pass.attachments[2u].clearValue.depthStencil.depth = 1.0f;
    pass.attachments[2u].clearValue.depthStencil.stencil = 0u;
    pass.renderArea = {0, 0, renderWidth_, renderHeight_};
    pass.subpassCount = 1u;
    subpass.colorAttachmentCount = 2u;
    subpass.colorAttachmentIndices[0u] = 0u;
    subpass.colorAttachmentIndices[1u] = 1u;
    subpass.depthAttachmentIndex = 2u;
    subpass.depthAttachmentCount = 1u;
}

void RenderNodeGsFloaterV21::SetViewportAndScissor(IRenderCommandList& commandList) const
{
    const ViewportDesc viewport{
        0.0f, 0.0f, static_cast<float>(renderWidth_), static_cast<float>(renderHeight_), 0.0f, 1.0f};
    const ScissorDesc scissor{0, 0, renderWidth_, renderHeight_};
    commandList.SetDynamicStateViewport(viewport);
    commandList.SetDynamicStateScissor(scissor);
}

void RenderNodeGsFloaterV21::RecordWeightedPass(IRenderCommandList& commandList, bool applyCumulativeMask)
{
    RenderPassDesc pass;
    RenderPassSubpassDesc subpass;
    BuildWeightedRenderPass(pass, subpass);
    commandList.BeginRenderPass(pass, 0u, subpass);
    SetViewportAndScissor(commandList);
    BindGraphics(GraphicsPipeline::WEIGHTED_DEPTH, commandList);
    const GraphicsPushConstant push{applyCumulativeMask ? 1u : 0u, {0u, 0u, 0u}};
    auto& pipeline = graphicsPipelines_[static_cast<uint32_t>(GraphicsPipeline::WEIGHTED_DEPTH)];
    commandList.PushConstant(pipeline.layout.pushConstant, reinterpret_cast<const uint8_t*>(&push));
    commandList.Draw(4u, gaussianCount_, 0u, 0u);
    commandList.EndRenderPass();
}

void RenderNodeGsFloaterV21::RecordOpaquePass(IRenderCommandList& commandList, bool applyCumulativeMask)
{
    RenderPassDesc pass;
    RenderPassSubpassDesc subpass;
    BuildOpaqueRenderPass(pass, subpass);
    commandList.BeginRenderPass(pass, 0u, subpass);
    SetViewportAndScissor(commandList);
    BindGraphics(GraphicsPipeline::OPAQUE_DISC, commandList);
    const GraphicsPushConstant push{applyCumulativeMask ? 1u : 0u, {0u, 0u, 0u}};
    auto& pipeline = graphicsPipelines_[static_cast<uint32_t>(GraphicsPipeline::OPAQUE_DISC)];
    commandList.PushConstant(pipeline.layout.pushConstant, reinterpret_cast<const uint8_t*>(&push));
    commandList.Draw(4u, gaussianCount_, 0u, 0u);
    commandList.EndRenderPass();
}

void RenderNodeGsFloaterV21::RecordDepthResolve(
    IRenderCommandList& commandList, bool accumulateEvidence)
{
    uint32_t pixelCount = 0u;
    if (!CheckedMultiply(renderWidth_, renderHeight_, pixelCount)) {
        errorState_ = true;
        return;
    }
    const DispatchPushConstant push{
        accumulateEvidence ? 1u : 0u,
        pixelCount,
        currentPeel_,
        0u,
    };
    Dispatch(ComputePipeline::DEPTH_RESOLVE, CeilDivide(pixelCount, LOCAL_SIZE), push, commandList);
}

void RenderNodeGsFloaterV21::RecordPrepareScene(IRenderCommandList& commandList)
{
    if (!resourcesReady_) {
        errorState_ = true;
        return;
    }
    const uint32_t auxiliaryItems = std::max({
        HashCapacity(gaussianCount_),
        SH_DIMENSIONS * SH_HISTOGRAM_BINS,
    });
    DispatchPushConstant push{0u, gaussianCount_, 0u, 0u};
    Dispatch(ComputePipeline::CLEAR, CeilDivide(gaussianCount_, LOCAL_SIZE), push, commandList);
    RecordComputeBarrier(commandList);
    push.count = auxiliaryItems;
    Dispatch(ComputePipeline::CLEAR_AUX, CeilDivide(auxiliaryItems, LOCAL_SIZE), push, commandList);
    RecordComputeBarrier(commandList);

    // Advance to next stage for the next ExecuteFrame call.
    stage_ = Stage::SORT_CAMERA;
}

void RenderNodeGsFloaterV21::RecordRadixSort(IRenderCommandList& commandList)
{
    const uint32_t paddedCount = AlignCount(gaussianCount_);
    const uint32_t groups = CeilDivide(gaussianCount_, LOCAL_SIZE);
    SortPushConstant push;
    push.count = gaussianCount_;
    push.paddedCount = paddedCount;
    push.groupCount = groups;

    for (uint32_t pass = 0u; pass < RADIX_PASSES; ++pass) {
        push.radixShift = pass * 8u;
        push.count = gaussianCount_;
        auto& countPipeline = computePipelines_[static_cast<uint32_t>(ComputePipeline::RADIX_COUNT)];
        BindCompute(ComputePipeline::RADIX_COUNT, commandList);
        commandList.PushConstant(countPipeline.layout.pushConstant, reinterpret_cast<const uint8_t*>(&push));
        commandList.Dispatch(groups, 1u, 1u);
        RecordComputeBarrier(commandList);

        auto& prefixPipeline = computePipelines_[static_cast<uint32_t>(ComputePipeline::RADIX_PREFIX)];
        BindCompute(ComputePipeline::RADIX_PREFIX, commandList);
        commandList.PushConstant(prefixPipeline.layout.pushConstant, reinterpret_cast<const uint8_t*>(&push));
        commandList.Dispatch(1u, 1u, 1u);
        RecordComputeBarrier(commandList);

        auto& scatterPipeline = computePipelines_[static_cast<uint32_t>(ComputePipeline::RADIX_SCATTER)];
        BindCompute(ComputePipeline::RADIX_SCATTER, commandList);
        commandList.PushConstant(scatterPipeline.layout.pushConstant, reinterpret_cast<const uint8_t*>(&push));
        commandList.Dispatch(groups, 1u, 1u);
        RecordComputeBarrier(commandList);
    }
}

void RenderNodeGsFloaterV21::RecordSortCamera(IRenderCommandList& commandList)
{
    const uint32_t gaussianGroups = CeilDivide(gaussianCount_, LOCAL_SIZE);
    DispatchPushConstant push{0u, gaussianCount_, 0u, 0u};
    Dispatch(ComputePipeline::PROJECT_SORT_INIT, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);

    Dispatch(ComputePipeline::SH_RANGE, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    Dispatch(ComputePipeline::SH_HISTOGRAM, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    push.count = SH_DIMENSIONS;
    Dispatch(ComputePipeline::SH_MEDIAN, 1u, push, commandList);
    RecordComputeBarrier(commandList);

    push.mode = 1u;
    push.count = SH_DIMENSIONS * SH_HISTOGRAM_BINS;
    Dispatch(ComputePipeline::CLEAR_AUX, CeilDivide(push.count, LOCAL_SIZE), push, commandList);
    RecordComputeBarrier(commandList);
    push.mode = 0u;
    push.count = gaussianCount_;
    Dispatch(ComputePipeline::SH_MAD_HISTOGRAM, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    push.count = SH_DIMENSIONS;
    Dispatch(ComputePipeline::SH_MAD, 1u, push, commandList);
    RecordComputeBarrier(commandList);

    RecordRadixSort(commandList);
    RecordComputeToGraphicsBarrier(commandList);

    // Advance to PEEL stage; currentPeel_ starts at 0.
    stage_ = Stage::PEEL;
}

void RenderNodeGsFloaterV21::RecordPeel(IRenderCommandList& commandList)
{
    // D19: peel round is set in FillGpuParams via PreExecuteFrame each frame.
    RecordWeightedPass(commandList, true);
    RecordOpaquePass(commandList);
    RecordGraphicsToComputeBarrier(commandList);

    const uint32_t gaussianGroups = CeilDivide(gaussianCount_, LOCAL_SIZE);
    DispatchPushConstant push{2u, gaussianCount_, currentPeel_, 0u};
    Dispatch(ComputePipeline::CLEAR, CeilDivide(gaussianCount_, LOCAL_SIZE), push, commandList);
    RecordComputeBarrier(commandList);
    push.count = HashCapacity(gaussianCount_);
    Dispatch(ComputePipeline::CLEAR_AUX, CeilDivide(push.count, LOCAL_SIZE), push, commandList);
    RecordComputeBarrier(commandList);

    RecordDepthResolve(commandList, true);
    RecordComputeBarrier(commandList);
    push = {0u, gaussianCount_, currentPeel_, 0u};
    Dispatch(ComputePipeline::FOOTPRINT, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    Dispatch(ComputePipeline::PRE_SCORE, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    Dispatch(ComputePipeline::COMPACT_HASH, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    Dispatch(ComputePipeline::KNN_SCORE, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    Dispatch(ComputePipeline::SH_PROPAGATE, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);

    push.mode = 0u;
    Dispatch(ComputePipeline::MASK_UPDATE, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    push.mode = 3u;
    Dispatch(ComputePipeline::MASK_UPDATE, 1u, push, commandList);
    RecordComputeBarrier(commandList);
    push.mode = 1u;
    Dispatch(ComputePipeline::MASK_UPDATE, gaussianGroups, push, commandList);
    RecordComputeBarrier(commandList);
    push.mode = 2u;
    Dispatch(ComputePipeline::MASK_UPDATE, 1u, push, commandList);
    RecordComputeBarrier(commandList);

    ++currentPeel_;
    ++peelsCompleted_;
}

void RenderNodeGsFloaterV21::RecordCompactPass(IRenderCommandList& commandList)
{
    // D16: compact_indices reads cumulativeMask from state buffer and writes
    // compacted gaussian indices into compactIndexBuffer.
    // The compact_indices shader expects: binding 0 = frame params,
    // binding 1 = state, binding 2 = index buffer.
    // We bind manually since this pipeline has its own layout.

    // Clear selectIndex[0] (compacted count) to 0 before dispatch. The buffer
    // is host-visible so we can map and zero the first uint32 directly.
    if (context_ && IsValid(resources_.compactIndexBuffer)) {
        auto& gpu = context_->GetGpuResourceManager();
        if (void* mapped = gpu.MapBuffer(resources_.compactIndexBuffer.GetHandle()); mapped) {
            static constexpr uint32_t zero = 0u;
            BASE_NS::CloneData(mapped, sizeof(uint32_t), &zero, sizeof(uint32_t));
            gpu.UnmapBuffer(resources_.compactIndexBuffer.GetHandle());
        }
    }

    auto& pipeline = computePipelines_[static_cast<uint32_t>(ComputePipeline::COMPACT_INDICES)];
    commandList.BindPipeline(pipeline.pso);
    auto& binder = *pipeline.binder;
    binder.ClearBindings();
    binder.BindBuffer(0u, resources_.frameParams.GetHandle(), 0u);
    binder.BindBuffer(1u, resources_.state.GetHandle(), 0u);
    binder.BindBuffer(2u, resources_.compactIndexBuffer.GetHandle(), 0u);
    commandList.UpdateDescriptorSet(binder.GetDescriptorSetHandle(), binder.GetDescriptorSetLayoutBindingResources());
    commandList.BindDescriptorSet(0u, binder.GetDescriptorSetHandle());
    const DispatchPushConstant push{0u, gaussianCount_, 0u, 0u};
    commandList.PushConstant(pipeline.layout.pushConstant, reinterpret_cast<const uint8_t*>(&push));
    commandList.Dispatch(CeilDivide(gaussianCount_, LOCAL_SIZE), 1u, 1u);
    RecordComputeBarrier(commandList);
}

void RenderNodeGsFloaterV21::PushFloaterRemovalMarkerOnly()
{
    // D17/RC2: push only the FloaterRemoval marker -- no SelectByIndex/RemoveSelected.
    // Pop the user-initiated FloaterRemoval that triggered this pipeline run
    // so it does not linger on the stack and confuse undo logic.
    if (!gsEditStore_) {
        return;
    }
    const auto& stack = gsEditStore_->GetEditActionStack();
    if (!stack.empty() && stack.back().type == GsEditActionType::FloaterRemoval) {
        if (stack.back().params && stack.back().params->IsFloaterRemovalOptions()) {
            (void)gsEditStore_->PopGsEditAction(); // pop user action
        }
    }
    GsEditAction action;
    action.type = GsEditActionType::FloaterRemoval;
    action.cameraViewMatrix = cameraViewMatrix_;
    action.cameraViewProjMatrix = cameraProjMatrix_;
    action.params = BASE_NS::make_shared<GS_EDIT_PARAMS::NoParams>();
    [[maybe_unused]] auto _ = gsEditStore_->PushGsEditAction(action);
}

void RenderNodeGsFloaterV21::PushEditActions([[maybe_unused]] IRenderCommandList& commandList)
{
    if (!gsEditStore_) {
        return;
    }
    // Multi-pass accumulation: when a previous FloaterRemoval triple already
    // sits on the stack below the user action, we restructure to accumulate
    // SelectByIndex entries before a single RemoveSelected. This ensures all
    // floater indices remain in original-splat space and only ONE compaction
    // runs, producing the correct union (F_old | F_new) for SaveToPLY.
    //
    // Stack before (second call): [..., SelectByIndex(F_old), RemoveSelected,
    //   FloaterRemoval(marker_old), FloaterRemoval(user)]
    // Stack after:  [..., SelectByIndex(F_old), SelectByIndex(F_new),
    //   RemoveSelected, FloaterRemoval(marker_new)]
    //
    // SelectByIndex accumulates in the selection mask (does NOT clear it --
    // see gsedit_select_index.comp and RequiresSelectionClear=false).
    {
        const auto& stack = gsEditStore_->GetEditActionStack();
        const size_t sz = stack.size();
        // The user's FloaterRemoval(fx>0) is on top. Below it: marker, RemoveSelected.
        if (sz >= 4u &&
            stack[sz - 1u].type == GsEditActionType::FloaterRemoval &&
            stack[sz - 2u].type == GsEditActionType::FloaterRemoval &&
            stack[sz - 3u].type == GsEditActionType::RemoveSelected) {
            // Pop user action and old marker.
            (void)gsEditStore_->PopGsEditAction(); // user FloaterRemoval
            (void)gsEditStore_->PopGsEditAction(); // old marker
            // Pop old RemoveSelected; keep existing SelectByIndex(s).
            (void)gsEditStore_->PopGsEditAction(); // old RemoveSelected
        }
    }

    // Push SelectByIndex with the compacted index buffer (accumulates with
    // any existing SelectByIndex in the selection mask).
    {
        GsEditAction action;
        action.type = GsEditActionType::SelectByIndex;
        action.cameraViewMatrix = cameraViewMatrix_;
        action.cameraViewProjMatrix = cameraProjMatrix_;
        auto params = BASE_NS::make_shared<GS_EDIT_PARAMS::SelectByIndex>();
        params->buffer = resources_.compactIndexBuffer;
        params->bufSize = gaussianCount_;
        params->indices = nullptr;
        action.params = params;
        [[maybe_unused]] auto _ = gsEditStore_->PushGsEditAction(action);
    }

    // Push RemoveSelected (single compaction removes all accumulated floaters).
    {
        GsEditAction action;
        action.type = GsEditActionType::RemoveSelected;
        action.cameraViewMatrix = cameraViewMatrix_;
        action.cameraViewProjMatrix = cameraProjMatrix_;
        action.params = BASE_NS::make_shared<GS_EDIT_PARAMS::NoParams>();
        [[maybe_unused]] auto _ = gsEditStore_->PushGsEditAction(action);
    }

    // Push FloaterRemoval marker (top of stack for O(1) undo check).
    // Use NoParams as sentinel to distinguish from user-initiated
    // FloaterRemoval (which has FloaterRemovalOptions).
    {
        GsEditAction action;
        action.type = GsEditActionType::FloaterRemoval;
        action.cameraViewMatrix = cameraViewMatrix_;
        action.cameraViewProjMatrix = cameraProjMatrix_;
        action.params = BASE_NS::make_shared<GS_EDIT_PARAMS::NoParams>();
        [[maybe_unused]] auto _ = gsEditStore_->PushGsEditAction(action);
    }

    // Do NOT call MarkStackAsExecuted() here. PushGsEditAction sets
    // actionStackUpdated_ = true, which lets RenderNodeGsRender pick up
    // and execute SelectByIndex + RemoveSelected on the next frame.
    // Calling MarkStackAsExecuted() would suppress that processing.
}

void RenderNodeGsFloaterV21::ExecuteFrame(IRenderCommandList& commandList)
{
    if (!context_ || !hasPendingFloaterRemoval_) {
        return;
    }

    // D17: zero-Gaussian guard -- push marker only, return early.
    if (gaussianCount_ == 0u || gaussianCount_ == INVALID_GAUSSIAN_ID) {
        PushFloaterRemovalMarkerOnly();
        stage_ = Stage::IDLE;
        hasPendingFloaterRemoval_ = false;
        if (gsEditStore_) {
            gsEditStore_->SetFloaterRemovalInProgress(false);
            gsEditStore_->TriggerFloaterRemovalCallback(false);
        }
        return;
    }

    // RC2: if error occurred during PreExecuteFrame, push marker only.
    if (errorState_ || !resourcesReady_ || !pipelinesReady_) {
        PushFloaterRemovalMarkerOnly();
        stage_ = Stage::IDLE;
        hasPendingFloaterRemoval_ = false;
        if (gsEditStore_) {
            gsEditStore_->SetFloaterRemovalInProgress(false);
            gsEditStore_->TriggerFloaterRemovalCallback(false);
        }
        return;
    }

    // Multi-frame state machine: each ExecuteFrame call advances one stage.
    switch (stage_) {
        case Stage::PREPARE_SCENE: {
            RecordPrepareScene(commandList);
            if (errorState_) {
                PushFloaterRemovalMarkerOnly();
                stage_ = Stage::IDLE;
                hasPendingFloaterRemoval_ = false;
            }
            break;
        }
        case Stage::SORT_CAMERA: {
            RecordSortCamera(commandList);
            if (errorState_) {
                PushFloaterRemovalMarkerOnly();
                stage_ = Stage::IDLE;
                hasPendingFloaterRemoval_ = false;
            }
            break;
        }
        case Stage::PEEL: {
            RecordPeel(commandList);
            if (errorState_) {
                PushFloaterRemovalMarkerOnly();
                stage_ = Stage::IDLE;
                hasPendingFloaterRemoval_ = false;
                break;
            }
            // All peels done -- advance to compact stage next frame.
            if (currentPeel_ >= MAX_PEEL_COUNT) {
                stage_ = Stage::COMPACT;
            }
            break;
        }
        case Stage::COMPACT: {
            RecordCompactPass(commandList);
            PushEditActions(commandList);
            ReleaseTaskResources();
            stage_ = Stage::IDLE;
            hasPendingFloaterRemoval_ = false;
            break;
        }
        default: {
            break;
        }
    }

    // Clear the in-progress flag when the pipeline finishes (any exit path
    // that sets stage_ = IDLE and hasPendingFloaterRemoval_ = false).
    if (stage_ == Stage::IDLE && !hasPendingFloaterRemoval_ && gsEditStore_) {
        gsEditStore_->SetFloaterRemovalInProgress(false);
        gsEditStore_->SetFloaterRemovalProgress(1.0f);
        // success = true if COMPACT ran (floaters found and removed), false if
        // the pipeline exited via marker-only path (zero gaussians or error).
        gsEditStore_->TriggerFloaterRemovalCallback(!errorState_);
    }
}

IRenderNode* RenderNodeGsFloaterV21::Create()
{
    return new RenderNodeGsFloaterV21();
}

void RenderNodeGsFloaterV21::Destroy(IRenderNode* instance)
{
    delete static_cast<RenderNodeGsFloaterV21*>(instance);
}

} // namespace GS
