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

#ifndef GS_IACTION_EXECUTOR_H
#define GS_IACTION_EXECUTOR_H

#include <base/containers/unique_ptr.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/namespace.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/resource_handle.h>

namespace GS {
struct GsEditAction;
struct GsRenderData;
}  // namespace GS

namespace GS {

#include "app/shaders/common/gs_edit_common.h"

// action executor context containing all necessary buffer references and states
struct ActionExecutionContext {
    RENDER_NS::RenderHandleReference immutableSplatData;  // immutable input splat data
    RENDER_NS::RenderHandleReference gsEditSplatData0;    // ping-pong buffer 0
    RENDER_NS::RenderHandleReference gsEditSplatData1;    // ping-pong buffer 1

    RENDER_NS::RenderHandleReference gsSelectionIndexBuffer;
    RENDER_NS::RenderHandleReference indirectDispatchBuffer;

    RENDER_NS::RenderHandleReference perActionDataBuffer;
    RENDER_NS::RenderHandleReference cameraUniformBuffer;
    RENDER_NS::RenderHandleReference gsDynamicDataBuffer;

    GsRenderData* renderData{nullptr};

    uint32_t actionIndex{0};
    uint32_t modifyingPassIdx{0};
    bool bufferPingPong{true};  // if true, read from gsEditSplatData0, write to gsEditSplatData1
};

// generic action executor interface
class IActionExecutor {
public:
    virtual ~IActionExecutor() = default;

    // execute the action
    virtual void Execute(
        RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action, const ActionExecutionContext& context) = 0;

    // calculate descriptor counts needed by all shaders of this executor
    virtual void CalculateDescriptorCounts(
        RENDER_NS::IRenderNodeContextManager& contextMgr, RENDER_NS::DescriptorCounts& dc) = 0;

    // create descriptors for this executor
    virtual void CreateDescriptorSets(RENDER_NS::IRenderNodeContextManager& contextMgr) = 0;

    // returns true if the action modified the splat data buffer
    virtual bool ModifiesSplatBuffer() const = 0;

    // returns true if this is a selection action
    virtual bool IsSelectionAction() const = 0;

    // returntrue if the selection buffer should be cleared before execution
    virtual bool RequiresSelectionClear() const = 0;

    // returns true if ExecuteUpdateIndirectDispatch should run before this action.
    // No-op marker executors return false to avoid resetting the indirect counter.
    virtual bool NeedsIndirectDispatchSetup() const
    {
        return true;
    }

    // per-action push constant data
    virtual void PreparePushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) = 0;
};

// factory type for creating action executors
using ActionExecutorFactory = BASE_NS::unique_ptr<IActionExecutor> (*)(
    RENDER_NS::IRenderNodeContextManager* contextMgr);

}  // namespace GS

#endif  // GS_IACTION_EXECUTOR_H
