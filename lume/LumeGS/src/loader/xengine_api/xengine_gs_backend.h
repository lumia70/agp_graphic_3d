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

#ifndef XENGINE_GS_BACKEND_H
#define XENGINE_GS_BACKEND_H

#include <base/math/matrix.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/render_data_structures.h>
#include <render/resource_handle.h>

#include "plugin_gs/render/intf_render_data_store_default_gs.h"

namespace RENDER_NS {
class IRenderContext;
} // namespace RENDER_NS

namespace GS {

enum class XegFrameGenPhase {
    START = 0,
    PREFRAME_OVER = 1,
    FRAME_RENDERED_OVER = 2,
    PAUSE = 3,
    CLOSED = 4,
};

class IXEngineGsBackend {
public:
    virtual ~IXEngineGsBackend() = default;

    virtual void InitCapabilities(RENDER_NS::IRenderNodeContextManager& ctx) = 0;

    virtual bool Is3DGSEnabled() const = 0;
    virtual bool IsUpscaleEnabled() const = 0;
    virtual XegFrameGenPhase GetFrameGenPhase() const = 0;
    virtual bool IsFrameGenClosed() const = 0;

    virtual bool ShouldBypassGpuSort(bool enableFrameGeneration, bool singleFrameGenEnable) const = 0;
    virtual bool ShouldSkipPrecompute(bool enableFrameGeneration, bool singleFrameGenEnable) const = 0;
    virtual bool ShouldSkipSphericalHarmonics(bool enableFrameGeneration, bool singleFrameGenEnable) const = 0;

    virtual void OnTiledModeActive() = 0;

    virtual bool OnGaussianRenderPass(RENDER_NS::IRenderCommandList& cmdList,
        const RENDER_NS::RenderPass& activeRenderPass, GS::IRenderDataStoreDefaultGs& dataStore,
        uint32_t numGs, bool gsEditorActive) = 0;

    virtual void TryEnterUpscaleOnlyMode(RENDER_NS::IRenderCommandList& cmdList,
        const RENDER_NS::RenderPass& activeRenderPass, GS::IRenderDataStoreDefaultGs& dataStore) = 0;

    virtual void ExecuteBackendFrame(const RENDER_NS::ILowLevelDevice& device,
        const RENDER_NS::RenderBackendRecordingState& recordingState,
        GS::IRenderDataStoreDefaultGs& dataStore, uint32_t mergedTotalActiveSplats) = 0;

    virtual void SetCameraMatrices(
        const BASE_NS::Math::Mat4X4& view, const BASE_NS::Math::Mat4X4& proj) = 0;
    virtual void SetViewport(const RENDER_NS::ViewportDesc& viewport) = 0;
    virtual void SetRenderParams(bool singleFrameGenEnable, float covarianceStddev, float upscaleSharpness) = 0;
};

} // namespace GS

#endif // XENGINE_GS_BACKEND_H