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

#include "gsedit_shader_action_executor.h"

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

namespace GS {

namespace {

inline void GetDescriptorCountsFromLayout(const RENDER_NS::PipelineLayout& pl, const uint32_t setIdx,
    const uint32_t setCount, RENDER_NS::DescriptorCounts& descriptorCounts)
{
    if (setIdx >= RENDER_NS::PipelineLayoutConstants::MAX_DESCRIPTOR_SET_COUNT) {
        return;
    }
    const auto& bindings = pl.descriptorSetLayouts[setIdx].bindings;
    descriptorCounts.counts.reserve(descriptorCounts.counts.size() + bindings.size());
    for (const auto& bindingRef : bindings) {
        descriptorCounts.counts.emplace_back(
            RENDER_NS::DescriptorCounts::TypedCount{bindingRef.descriptorType, bindingRef.descriptorCount * setCount});
    }
}
}  // namespace

ShaderActionExecutor::ShaderActionExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr, const Config& config)
    : contextMgr_(contextMgr), config_(config)
{}

void ShaderActionExecutor::Execute(
    RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action, const ActionExecutionContext& context)
{
    if (!set0Binder_) {
        CORE_LOG_E("ShaderActionExecutor::Execute: Descriptor set binder not created!");
        return;
    }

    const PsoData psoData = GetPsoData(0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    // bind all buffers
    set0Binder_->BindBuffer(0u, context.immutableSplatData.GetHandle(), 0);
    set0Binder_->BindBuffer(1u, context.gsEditSplatData0.GetHandle(), 0);
    set0Binder_->BindBuffer(2u, context.gsEditSplatData1.GetHandle(), 0);
    set0Binder_->BindBuffer(3u, context.gsSelectionIndexBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(4u, context.indirectDispatchBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(5u, context.perActionDataBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(6u, context.cameraUniformBuffer.GetHandle(), 0);
    set0Binder_->BindBuffer(7u, context.gsDynamicDataBuffer.GetHandle(), 0);

    const auto& set0Handle = set0Binder_->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, set0Binder_->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, set0Handle);

    // push constant data
    GsEditPushConstantData pcData{};
    pcData.editCommon.bufferFlipIndex = context.bufferPingPong ? 1u : 0u;
    pcData.editCommon.editPassIndex = context.modifyingPassIdx;
    pcData.editCommon.actionIndex = context.actionIndex;

    PrepareCustomPushConstantData(action, pcData);

    BASE_NS::ByteArray pushConstantData(GS_EDIT_PUSH_CONSTANT_MAX_SIZE);
    if (!BASE_NS::CloneData(
            &pushConstantData.GetData()[0], GS_EDIT_PUSH_CONSTANT_MAX_SIZE, &pcData, GS_EDIT_PUSH_CONSTANT_MAX_SIZE)) {
        CORE_LOG_E("ShaderActionExecutor::Execute: CloneData failed");
        return;
    }

    cmdList.PushConstant(psoData.pushConstant, &pushConstantData.GetData()[0]);

    cmdList.DispatchIndirect(context.indirectDispatchBuffer.GetHandle(), 0);
}

void ShaderActionExecutor::CalculateDescriptorCounts(
    RENDER_NS::IRenderNodeContextManager& contextMgr, RENDER_NS::DescriptorCounts& dc)
{
    const auto& shaderMgr = contextMgr.GetShaderManager();

    shaderHandle_ = shaderMgr.GetShaderHandle(config_.shaderPath);
    pipelineLayout_ = shaderMgr.GetPipelineLayout(shaderMgr.GetPipelineLayoutHandle(config_.pipelineLayoutPath));

    GetDescriptorCountsFromLayout(pipelineLayout_, 0u, 1u, dc);
}

void ShaderActionExecutor::CreateDescriptorSets(RENDER_NS::IRenderNodeContextManager& contextMgr)
{
    auto& descriptorSetMgr = contextMgr.GetDescriptorSetManager();

    const RENDER_NS::RenderHandle setDescHandle = descriptorSetMgr.CreateDescriptorSet(0u, pipelineLayout_);
    set0Binder_ =
        descriptorSetMgr.CreateDescriptorSetBinder(setDescHandle, pipelineLayout_.descriptorSetLayouts[0u].bindings);
}

void ShaderActionExecutor::PreparePushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData)
{
    PrepareCustomPushConstantData(action, pcData);
}

ShaderActionExecutor::PsoData ShaderActionExecutor::GetPsoData(uint32_t flags)
{
    uint64_t seed = shaderHandle_.id;
    BASE_NS::HashCombine(seed, static_cast<uint64_t>(flags));

    if (const auto iter = psoCache_.find(seed); iter != psoCache_.cend()) {
        return iter->second;
    }

    const auto& shaderMgr = contextMgr_->GetShaderManager();
    constexpr size_t maxSpecializationFlagCount{4u};
    uint32_t specializationFlags[maxSpecializationFlagCount] = {};

    const auto& sscv = shaderMgr.GetReflectionSpecialization(shaderHandle_);
    const size_t maxSpecializations = BASE_NS::Math::min(maxSpecializationFlagCount, sscv.constants.size());

    for (size_t idx = 0; idx < maxSpecializations; ++idx) {
        const auto& ref = sscv.constants[idx];
        const uint32_t constantId = ref.offset / sizeof(uint32_t);

        switch (ref.id) {
            case 0u:
                if (constantId < maxSpecializationFlagCount) {
                    specializationFlags[constantId] = flags;
                }
                break;
            default:
                break;
        }
    }

    const RENDER_NS::ShaderSpecializationConstantDataView spec{
        {sscv.constants.data(), maxSpecializations}, {specializationFlags, maxSpecializations}};

    const RENDER_NS::RenderHandle psoHandle =
        contextMgr_->GetPsoManager().GetComputePsoHandle(shaderHandle_, pipelineLayout_, spec);

    const PsoData psoData = {psoHandle, pipelineLayout_.pushConstant};
    psoCache_[seed] = psoData;
    return psoData;
}

}  // namespace GS
