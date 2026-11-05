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

#ifndef RENDER_GS_EDIT_H
#define RENDER_GS_EDIT_H

#include <plugin_gs/render/intf_render_data_store_gs_edit.h>

#include <3d/render/intf_render_node_scene_util.h>
#include <base/containers/byte_array.h>
#include <base/containers/pair.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/render_data_store_default_gs.h>
#include <render/render_data_store_gs_edit.h>
#include <render/render_data_structures.h>

#include "gsedit_shader_default_actions.h"
#include "intf_gsedit_action_executor.h"

namespace GS {

struct GsEditAction;
struct GsRenderData;
class RenderDataStoreDefaultGS;
class RenderDataStoreGsEdit;

/**
 *  GS Editor Implementation.
 *      - Call PushActionToStack for every action to push to stack, in the order they should be invoked in.
 *      - Call PopActionFromStack to remove the last element from the stack ("undo")
 *      - Call ExecuteStack after we have made changes to the action stack.
 *      - Any selection action will save the selection (as indices) to a separate GPU buffer, which
 *        can then be read by any following action. A new select operation will override the selectio buffer.
 *
 *  How to implement a custom action:
 *      - To add a single compute shader -based action:
 *          -> in gsedit_shader_default_actions.h, create the definition for your action.
 *             e.g. class SelectBoxExecutor : public ShaderActionExecutor.
 *          -> in gsedit_shader_default_actions.cpp, implement the action.
 *             e.g. SelectBoxExecutor::SelectBoxExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr).
 *          -> Implement the function PrepareCustomPushConstantData if using push constant data.
 *          -> Add the initialization to GsEditor::InitializeExecutors.
 *     - To add a complex executor with its own state or multiple shader passes, create a new class in
 *       a new file which implements the interface IActionExecutor and call RegisterActionExecutor or
 *       RegisterActionExecutorFactory from somewhere.
 *     - Additionally, add the correct parsing actions in gs_edit_command_parsing.h and
 *       the action definition in intf_render_data_store_gs_edit.h.
 */

class GsEditor {
public:
    explicit GsEditor(RENDER_NS::IRenderNodeContextManager* renderNodeContextMgr);
    ~GsEditor();

    // executes the entire action stack, return true upon full execution, false on failure or pending operations
    bool ExecuteStack(RENDER_NS::IRenderCommandList& cmdList);

    // pushes an action to the stack
    void PushActionToStack(const GsEditAction& action);

    // deletes the last element from the stack and returns the index of the deleted action
    uint32_t PopActionFromStack();

    void SetRenderNodeBuffers(
        const RENDER_NS::RenderHandleReference cameraUbo, const RENDER_NS::RenderHandleReference dynamicDataUbo);

    // these are called from the render node
    void CalculateDescriptorCounts(RENDER_NS::DescriptorCounts& dc);
    void CreateDescriptorSets();

    RENDER_NS::RenderHandleReference GetGsEditedBuffer();
    RENDER_NS::RenderHandleReference GetIndirectDispatchBuffer();

    uint32_t GetEditorNumSplats();
    uint32_t GetNumModifyingPasses() const;
    void ClearActionStack();

    // register a custom executor for an action type
    void RegisterActionExecutor(GsEditActionType actionType, BASE_NS::unique_ptr<IActionExecutor> executor);

    // register a custom action executor using a factory function
    void RegisterActionExecutorFactory(GsEditActionType actionType, ActionExecutorFactory factory);

    // gs export
    void BeginGsModelExport(RENDER_NS::IRenderCommandList& cmdList);
    void EndGsModelExport();

    void ExportFromDirectBuffer();
    void ExportFromStagingBuffer();

private:
    RENDER_NS::IRenderNodeContextManager* renderNodeContextMgr_{nullptr};
    CORE3D_NS::SceneRenderDataStores stores_;
    RenderDataStoreDefaultGS* dataStoreGs_{nullptr};
    RenderDataStoreGsEdit* dataStoreEdit_{nullptr};

    struct CompBinders {
        RENDER_NS::IDescriptorSetBinder::Ptr set0;
        RENDER_NS::IDescriptorSetBinder::Ptr set1;
        RENDER_NS::PipelineLayout pl;
    };

    struct PsoData {
        RENDER_NS::RenderHandle psoHandle;
        RENDER_NS::PushConstant pushConstant;
    };

    // execute a single action
    void ExecuteAction(RENDER_NS::IRenderCommandList& cmdList, IActionExecutor* executor, const GsEditAction& action,
        GsRenderData* renderData, const uint32_t actionIndex, const uint32_t modifyingPassIdx,
        const bool isSelectionAction);

    // updates the indirect dispatch sizes and zeroes the count
    void ExecuteUpdateIndirectDispatch(RENDER_NS::IRenderCommandList& cmdList, const uint32_t numInputSplats,
        const uint32_t selectCountClearValue = 0xFFFFffff);

    PsoData GetPsoData(
        const RENDER_NS::PipelineLayout& pl, const RENDER_NS::RenderHandle& shaderHandle, uint32_t flags);

    RENDER_NS::RenderHandleReference CreateGpuBuffer(const BASE_NS::string_view& name,
        const RENDER_NS::GpuBufferDesc desc, const BASE_NS::array_view<const uint8_t>& data);
    RENDER_NS::RenderHandleReference CreateGpuBuffer(
        const RENDER_NS::GpuBufferDesc desc, const BASE_NS::string name = "");

    void CreateBuffers(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData);

    void RecreateActionCameraMatrixBuffer();
    void UpdateActionCameraMatrixBufferData();

    void ClearSelection(RENDER_NS::IRenderCommandList& cmdList);

    // initialize default action executors
    void InitializeExecutors();

    BASE_NS::unordered_map<uint64_t, PsoData> shaderToPsoData_;

    RENDER_NS::RenderHandleReference indirectDispatchBuffer_;
    CompBinders updateIndirectDispatchBinders_;
    RENDER_NS::RenderHandle updateIndirectDispatchHandle_;

    CompBinders clearSelectionBinders_;
    RENDER_NS::RenderHandle clearSelectionHandle_;

    // action executors lookup
    BASE_NS::unordered_map<GsEditActionType, BASE_NS::unique_ptr<IActionExecutor>> actionExecutors_;

    // pending executor factories, registered before calling CalculateDescriptorCounts
    BASE_NS::unordered_map<GsEditActionType, ActionExecutorFactory> pendingFactories_;

    // If true, the final action in the stack wrote to gsEditSplatData1. Else, gsEditSplatData0.
    bool stackBufferPingPong_{true};

    RENDER_NS::RenderHandleReference gsEditSplatData0;  // write ping-pong buffer 0, alternate reading
    RENDER_NS::RenderHandleReference gsEditSplatData1;  // write ping-pong buffer 1, alternate reading

    // NOTE: currently no support for spherical harmonics or ASTC handling!

    RENDER_NS::RenderHandleReference gsSelectionIndexBuffer;  // edit index buffer

    RENDER_NS::RenderHandleReference cameraUniformBuffer_;  // camera uniform buffer from RenderNodeGsRender
    RENDER_NS::RenderHandleReference gsDynamicDataBuffer_;  // GS dynamic data uniform buffer from RenderNodeGsRender

    RENDER_NS::RenderHandleReference perActionDataBuffer_;  // camera view matrices at the time of the action
    bool perActionDataBufferUpdated_{false};

    BASE_NS::vector<GsEditAction> editActionStack_;

    uint32_t numSplatsEdited_{0};
    uint32_t numModifyingPasses_{0};

    uint32_t generationCounter_{0};

    RENDER_NS::RenderHandleReference gsEditorExportBuffer_;
    CompBinders exportCopyBinder_;
    RENDER_NS::RenderHandle exportCopyHandle_;
};

}  // namespace GS

#endif  // RENDER_GS_EDIT_H
