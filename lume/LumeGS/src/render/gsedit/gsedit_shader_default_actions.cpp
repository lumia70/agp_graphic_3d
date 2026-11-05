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

#include "gsedit_shader_default_actions.h"

#include <plugin_gs/render/intf_render_data_store_gs_edit.h>

#include <base/containers/byte_array.h>
#include <base/math/mathf.h>
#include <base/util/hash.h>
#include <core/log.h>
#include <render/device/intf_shader_manager.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_node_context_pso_manager.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node_context_manager.h>

#include "util/gs_utils.h"

using namespace GS;

// executor for box selection
SelectBoxExecutor::SelectBoxExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_select_box.shader",
                                           "gspipelinelayouts://gsedit_generic.shaderpl",
                                           false,  // does not modify splat buffer
                                           true,   // is a select action
                                           false,  // requires selection clear
                                       })
{}

void SelectBoxExecutor::PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData)
{
    const auto* boxParams = static_cast<GS_EDIT_PARAMS::SelectBox*>(action.params.get());
    if (boxParams) {
        pcData.data[0u] = BitCast<uint32_t>(boxParams->min.x);
        pcData.data[1u] = BitCast<uint32_t>(boxParams->min.y);
        pcData.data[2u] = BitCast<uint32_t>(boxParams->max.x);
        pcData.data[3u] = BitCast<uint32_t>(boxParams->max.y);
    } else {
        CORE_LOG_E("SelectBoxExecutor: Failed to get SelectBox params!");
    }
}

// executor for remove selected
RemoveSelectedExecutor::RemoveSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_remove_selection.shader",
                                           "gspipelinelayouts://gsedit_generic.shaderpl",
                                           true,   // modifies splat buffer data
                                           false,  // is not a select action
                                           false,  // does not require selection clear
                                       })
{}

// executor for selection inverse
InvertSelectionExecutor::InvertSelectionExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_select_invert.shader",
                                           "gspipelinelayouts://gsedit_generic.shaderpl",
                                           false,  // does not modify the splat buffer
                                           true,   // is a select action
                                           false   // does not require selection clear
                                       })
{}

// executor for transform selected
TransformSelectedExecutor::TransformSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_transform_selection.shader",
                                           "gspipelinelayouts://gsedit_generic.shaderpl",
                                           true,   // modifies splat buffer data
                                           false,  // is not a select action
                                           false,  // does not require selection clear
                                       })
{}

void TransformSelectedExecutor::PrepareCustomPushConstantData(
    const GsEditAction& action, GsEditPushConstantData& pcData)
{
    const auto* tParams = static_cast<GS_EDIT_PARAMS::Transform*>(action.params.get());
    if (tParams) {
        for (size_t ii = 0; ii < 16u; ii++) {
            pcData.data[ii] = BitCast<uint32_t>(tParams->transform.data[ii]);
        }
    } else {
        CORE_LOG_E("TransformSelectedExecutor: Failed to get Transform params!");
    }
}

// executor for coloring the selected splats
ColorSelectedExecutor::ColorSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_color_selection.shader",
                                           "gspipelinelayouts://gsedit_generic.shaderpl",
                                           true,   // modifies splat buffer data
                                           false,  // is not a select action
                                           false,  // does not require selection clear
                                       })
{}

void ColorSelectedExecutor::PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData)
{
    const auto* cParams = static_cast<GS_EDIT_PARAMS::ColorSelection*>(action.params.get());
    if (cParams) {
        pcData.data[0u] = BitCast<uint32_t>(cParams->color.x);
        pcData.data[1u] = BitCast<uint32_t>(cParams->color.y);
        pcData.data[2u] = BitCast<uint32_t>(cParams->color.z);
        pcData.data[3u] = BitCast<uint32_t>(cParams->color.w);
        pcData.data[4u] = cParams->colorMode;
    } else {
        CORE_LOG_E("ColorSelectedExecutor: Failed to get Transform params!");
    }
}

ClearSelectedExecutor::ClearSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_clear_selection.shader",
                                           "gspipelinelayouts://gsedit_generic.shaderpl",
                                           false,  // does not modify the splat buffer
                                           true,   // is a select action
                                           true    // requires selection clear
                                       })
{}

void ClearSelectedExecutor::Execute(
    RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action, const ActionExecutionContext& context)
{}

Select3DBoxExecutor::Select3DBoxExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_select_3d_box.shader",
                                           "gspipelinelayouts://gsedit_generic.shaderpl",
                                           false,  // does not modify splat buffer
                                           true,   // is a select action
                                           false,  // requires selection clear
                                       })
{}

void Select3DBoxExecutor::PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData)
{
    const auto* cParams = static_cast<GS_EDIT_PARAMS::Select3DBox*>(action.params.get());
    if (cParams) {
        pcData.data[0u] = BitCast<uint32_t>(cParams->min.x);
        pcData.data[1u] = BitCast<uint32_t>(cParams->min.y);
        pcData.data[2u] = BitCast<uint32_t>(cParams->min.z);
        pcData.data[3u] = BitCast<uint32_t>(cParams->max.x);
        pcData.data[4u] = BitCast<uint32_t>(cParams->max.y);
        pcData.data[5u] = BitCast<uint32_t>(cParams->max.z);
    } else {
        CORE_LOG_E("Select3DBoxExecutor: Failed to get Transform params!");
    }
}

SelectByIndexExecutor::SelectByIndexExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_select_index.shader",
                                           "gspipelinelayouts://gsedit_index_selection.shaderpl",
                                           false,  // does not modify the splat buffer
                                           true,   // is a select action
                                           false   // requires selection clear
                                       })
{}

void SelectByIndexExecutor::Execute(
    RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action, const ActionExecutionContext& context)
{
    if (!set0Binder_) {
        CORE_LOG_E("SelectByIndexExecutor::Execute: Descriptor set binder not created!");
        return;
    }

    const PsoData psoData = GetPsoData(0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    // push constant data
    GsEditPushConstantData pcData{};
    pcData.editCommon.bufferFlipIndex = context.bufferPingPong ? 1u : 0u;
    pcData.editCommon.editPassIndex = context.modifyingPassIdx;
    pcData.editCommon.actionIndex = context.actionIndex;

    PrepareCustomPushConstantData(action, pcData);

    set0Binder_->BindBuffer(0u, context.gsSelectionIndexBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(1u, context.indirectDispatchBuffer.GetHandle(), 0);

    const auto cParams = static_pointer_cast<GS_EDIT_PARAMS::SelectByIndex>(action.params);
    if (!cParams) {
        CORE_LOG_E("SelectByIndexExecutor::Execute: parms is null!");
        return;
    }
    auto hnd = cParams->buffer.GetHandle();
    set0Binder_->BindBuffer(2u, hnd, 0);

    const auto& set0Handle = set0Binder_->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, set0Binder_->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, set0Handle);

    BASE_NS::ByteArray pushConstantData(GS_EDIT_PUSH_CONSTANT_MAX_SIZE);
    BASE_NS::CloneData(
        &pushConstantData.GetData()[0], GS_EDIT_PUSH_CONSTANT_MAX_SIZE, &pcData, GS_EDIT_PUSH_CONSTANT_MAX_SIZE);

    cmdList.PushConstant(psoData.pushConstant, &pushConstantData.GetData()[0]);

    cmdList.Dispatch(
        (static_cast<uint32_t>(cParams->bufSize) + GS_EDIT_WORKGROUP_SIZE - 1) / GS_EDIT_WORKGROUP_SIZE, 1, 1);
}

SelectByMaskExecutor::SelectByMaskExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : ShaderActionExecutor(contextMgr, {
                                           "gsshaders://computeshader/gsedit/gsedit_select_mask.shader",
                                           "gspipelinelayouts://gsedit_mask_selection.shaderpl",
                                           false,  // does not modify the splat buffer
                                           true,   // is a select action
                                           false   // requires selection clear
                                       })
{}

void SelectByMaskExecutor::Execute(
    RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action, const ActionExecutionContext& context)
{
    if (!set0Binder_) {
        CORE_LOG_E("SelectByMaskExecutor::Execute: Descriptor set binder not created!");
        return;
    }

    const PsoData psoData = GetPsoData(0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    // push constant data
    GsEditPushConstantData pcData{};
    pcData.editCommon.bufferFlipIndex = context.bufferPingPong ? 1u : 0u;
    pcData.editCommon.editPassIndex = context.modifyingPassIdx;
    pcData.editCommon.actionIndex = context.actionIndex;

    PrepareCustomPushConstantData(action, pcData);

    set0Binder_->BindBuffer(0u, context.immutableSplatData.GetHandle(), 0);
    set0Binder_->BindBuffer(1u, context.gsEditSplatData0.GetHandle(), 0);
    set0Binder_->BindBuffer(2u, context.gsEditSplatData1.GetHandle(), 0);
    set0Binder_->BindBuffer(3u, context.gsSelectionIndexBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(4u, context.indirectDispatchBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(5u, context.perActionDataBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(6u, context.cameraUniformBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(7u, context.gsDynamicDataBuffer.GetHandle(), 0);

    auto& gpuResourceMgr = contextMgr_->GetGpuResourceManager();

    const auto cParams = static_pointer_cast<GS_EDIT_PARAMS::SelectByMask>(action.params);
    if (!cParams) {
        CORE_LOG_E("SelectByMaskExecutor::Execute: parms is null!");
        return;
    }

    auto sampler = gpuResourceMgr.GetSamplerHandle("CORE_DEFAULT_SAMPLER_LINEAR_CLAMP");

    set0Binder_->BindImage(8u, cParams->image.GetHandle(), sampler);

    const auto& set0Handle = set0Binder_->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, set0Binder_->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, set0Handle);

    BASE_NS::ByteArray pushConstantData(GS_EDIT_PUSH_CONSTANT_MAX_SIZE);
    BASE_NS::CloneData(
        &pushConstantData.GetData()[0], GS_EDIT_PUSH_CONSTANT_MAX_SIZE, &pcData, GS_EDIT_PUSH_CONSTANT_MAX_SIZE);

    cmdList.PushConstant(psoData.pushConstant, &pushConstantData.GetData()[0]);

    cmdList.DispatchIndirect(context.indirectDispatchBuffer.GetHandle(), 0);
}

// FloaterRemoval no-op executor: actual removal is handled by RenderNodeGsFloaterV21.
FloaterRemovalExecutor::FloaterRemovalExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr)
    : IActionExecutor()
{
    (void)contextMgr;
}

void FloaterRemovalExecutor::Execute(
    RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action, const ActionExecutionContext& context)
{
    (void)cmdList;
    (void)action;
    (void)context;
}

void FloaterRemovalExecutor::CalculateDescriptorCounts(
    RENDER_NS::IRenderNodeContextManager& contextMgr, RENDER_NS::DescriptorCounts& dc)
{
    (void)contextMgr;
    (void)dc;
}

void FloaterRemovalExecutor::CreateDescriptorSets(RENDER_NS::IRenderNodeContextManager& contextMgr)
{
    (void)contextMgr;
}

void FloaterRemovalExecutor::PreparePushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData)
{
    (void)action;
    (void)pcData;
}
