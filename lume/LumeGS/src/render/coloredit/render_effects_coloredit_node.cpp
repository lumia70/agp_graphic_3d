/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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

#include "render_effects_coloredit_node.h"

#include <base/containers/unique_ptr.h>
#include <core/log.h>
#include <core/property/property_handle_util.h>
#include <core/property/property_types.h>
#include <core/property_tools/property_api_impl.inl>
#include <core/property_tools/property_macros.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_node_context_pso_manager.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/nodecontext/intf_render_node_util.h>
#include <render/property/property_types.h>

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;

CORE_BEGIN_NAMESPACE()
using namespace GsEffects;

DATA_TYPE_METADATA(RenderPostProcessColorEditNode::EffectProperties, MEMBER_PROPERTY(enabled, "enabled", 0),
    MEMBER_PROPERTY(exposure, "exposure", 0.0f), MEMBER_PROPERTY(contrast, "contrast", 0.0f),
    MEMBER_PROPERTY(temperature, "temperature", 0.0f), MEMBER_PROPERTY(tint, "tint", 0.0f),
    MEMBER_PROPERTY(saturation, "saturation", 0.0f), MEMBER_PROPERTY(vibrance, "vibrance", 0.0f))

DATA_TYPE_METADATA(RenderPostProcessColorEditNode::NodeInputs, MEMBER_PROPERTY(input, "input", 0))
DATA_TYPE_METADATA(RenderPostProcessColorEditNode::NodeOutputs, MEMBER_PROPERTY(output, "output", 0))

CORE_END_NAMESPACE()

namespace GsEffects {
namespace {
constexpr DynamicStateEnum DYNAMIC_STATES[] = { CORE_DYNAMIC_STATE_ENUM_VIEWPORT, CORE_DYNAMIC_STATE_ENUM_SCISSOR };

constexpr string_view SHADER_NAME { "gsshaders://shader/coloredit/coloredit.shader" };

constexpr int32_t SHADER_PARAM_COUNT { 3 };
constexpr int32_t BUFFER_SIZE_IN_BYTES = SHADER_PARAM_COUNT * 4 * sizeof(float);

constexpr GpuBufferDesc UBO_DESC { CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    (CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    CORE_ENGINE_BUFFER_CREATION_DYNAMIC_RING_BUFFER, 0U };

RenderHandleReference CreateGpuBuffers(
    IRenderNodeGpuResourceManager& gpuResourceMgr, const RenderHandleReference& handle)
{
    // Create buffer for the shader data.
    GpuBufferDesc desc = UBO_DESC;
    desc.byteSize = BUFFER_SIZE_IN_BYTES;
    return gpuResourceMgr.Create(handle, desc);
}

void UpdateBuffer(IRenderNodeGpuResourceManager& gpuResourceMgr, const RenderHandle& handle,
    const RenderPostProcessColorEditNode::ShaderParameters& shaderParameters)
{
    if (void* data = gpuResourceMgr.MapBuffer(handle); data) {
        CloneData(data, sizeof(RenderPostProcessColorEditNode::ShaderParameters), &shaderParameters,
            sizeof(RenderPostProcessColorEditNode::ShaderParameters));
        gpuResourceMgr.UnmapBuffer(handle);
    }
}

RenderPassDesc::RenderArea GetImageRenderArea(
    const IRenderNodeGpuResourceManager& gpuResourceMgr, const RenderHandle handle)
{
    const GpuImageDesc desc = gpuResourceMgr.GetImageDescriptor(handle);
    return { 0U, 0U, desc.width, desc.height };
}
} // namespace

RenderPostProcessColorEditNode::RenderPostProcessColorEditNode()
    : properties_(&propertiesData_, PropertyType::DataType<EffectProperties>::MetaDataFromType()),
      inputProperties_(&nodeInputsData_, PropertyType::DataType<NodeInputs>::MetaDataFromType()),
      outputProperties_(&nodeOutputsData_, PropertyType::DataType<NodeOutputs>::MetaDataFromType())
{}

RenderPostProcessColorEditNode::~RenderPostProcessColorEditNode() = default;

IPropertyHandle* RenderPostProcessColorEditNode::GetRenderInputProperties()
{
    return inputProperties_.GetData();
}

IPropertyHandle* RenderPostProcessColorEditNode::GetRenderOutputProperties()
{
    return outputProperties_.GetData();
}

DescriptorCounts RenderPostProcessColorEditNode::GetRenderDescriptorCounts() const
{
    return descriptorCounts_;
}

void RenderPostProcessColorEditNode::SetRenderAreaRequest(const RenderAreaRequest& renderAreaRequest)
{
    useRequestedRenderArea_ = true;
    renderAreaRequest_ = renderAreaRequest;
}

void RenderPostProcessColorEditNode::InitNode(IRenderNodeContextManager& renderNodeContextMgr)
{
    // clear
    pipelineData_ = {};

    renderNodeContextMgr_ = &renderNodeContextMgr;

    // default inputs
    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    defaultInput_.handle = gpuResourceMgr.GetImageHandle("CORE_DEFAULT_GPU_IMAGE");
    defaultInput_.samplerHandle = gpuResourceMgr.GetSamplerHandle("CORE_DEFAULT_SAMPLER_LINEAR_CLAMP");
    gpuBuffer_ = CreateGpuBuffers(gpuResourceMgr, gpuBuffer_);

    // load shaders
    const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr_->GetShaderManager();
    pipelineData_.gsd = shaderMgr.GetGraphicsShaderDataByShaderHandle(shaderMgr.GetShaderHandle(SHADER_NAME));

    // create psos
    INodeContextPsoManager& psoMgr = renderNodeContextMgr_->GetPsoManager();
    pipelineData_.pso = psoMgr.GetGraphicsPsoHandle(pipelineData_.gsd, {}, DYNAMIC_STATES);

    descriptorCounts_.counts.clear();
    pipelineData_.descriptorSet = {};

    // get needed descriptor set counts
    const IRenderNodeUtil& renderNodeUtil = renderNodeContextMgr_->GetRenderNodeUtil();
    descriptorCounts_ = renderNodeUtil.GetDescriptorCounts(pipelineData_.gsd.pipelineLayoutData);

    valid_ = true;
}

void RenderPostProcessColorEditNode::PreExecuteFrame()
{
    if (valid_) {
        if (const auto* props = GetProperties(); props) {
            enabled_ = CORE_NS::GetPropertyValue<bool>(props, "enabled");
            shaderParameters_.exposure = CORE_NS::GetPropertyValue<float>(props, "exposure");
            shaderParameters_.contrast = CORE_NS::GetPropertyValue<float>(props, "contrast");
            shaderParameters_.temperature = CORE_NS::GetPropertyValue<float>(props, "temperature");
            shaderParameters_.tint = CORE_NS::GetPropertyValue<float>(props, "tint");
            shaderParameters_.saturation = CORE_NS::GetPropertyValue<float>(props, "saturation");
            shaderParameters_.vibrance = CORE_NS::GetPropertyValue<float>(props, "vibrance");
        }
    } else {
        enabled_ = false;
    }

    if (enabled_) {
        // check input and output
        EvaluateOutputImageCreation();
    }
}

IRenderNode::ExecuteFlags RenderPostProcessColorEditNode::GetExecuteFlags() const
{
    if (enabled_ && valid_) {
        return 0;
    } else {
        return IRenderNode::ExecuteFlagBits::EXECUTE_FLAG_BITS_DO_NOT_EXECUTE;
    }
}

void RenderPostProcessColorEditNode::ExecuteFrame(IRenderCommandList& cmdList)
{
    CORE_ASSERT(enabled_);

    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "GsAppPostProcessColorEdit", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    if (!pipelineData_.descriptorSet) {
        // create descriptor set and descriptor set binder
        pipelineData_.descriptorSet = renderNodeContextMgr_->GetDescriptorSetManager().CreateDescriptorSetBinder(
            pipelineData_.gsd.pipelineLayoutData.descriptorSetLayouts[0U].bindings);
    }
    if (!pipelineData_.descriptorSet) {
        return;
    }
    currInput_ = RenderHandleUtil::IsValid(nodeInputsData_.input.handle) ? nodeInputsData_.input : defaultInput_;
    if (!RenderHandleUtil::IsValid(currInput_.samplerHandle)) {
        currInput_.samplerHandle = defaultInput_.samplerHandle;
    }
    currOutput_.handle = RenderHandleUtil::IsValid(nodeOutputsData_.output.handle)
                             ? nodeOutputsData_.output.handle
                             : ownOutputImageData_.handle.GetHandle();
    if (!RenderHandleUtil::IsValid(currOutput_.handle)) {
        return;
    }

    const IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    const RenderPassDesc::RenderArea renderArea =
        useRequestedRenderArea_ ? renderAreaRequest_.area : GetImageRenderArea(gpuResourceMgr, currOutput_.handle);
    if ((renderArea.extentWidth == 0) || (renderArea.extentHeight == 0)) {
        return;
    }

    // update data
    shaderParameters_.resX = static_cast<float>(renderAreaRequest_.area.extentWidth);
    shaderParameters_.resY = static_cast<float>(renderAreaRequest_.area.extentHeight);
    shaderParameters_.time =
        renderNodeContextMgr_->GetRenderNodeGraphData().renderingConfiguration.renderTimings.z * 1.0f;

    UpdateBuffer(renderNodeContextMgr_->GetGpuResourceManager(), gpuBuffer_.GetHandle(), shaderParameters_);

    // render
    RenderPass renderPass;
    renderPass.renderPassDesc.attachmentCount = 1;
    renderPass.renderPassDesc.renderArea = renderAreaRequest_.area;
    renderPass.renderPassDesc.subpassCount = 1;
    renderPass.renderPassDesc.attachments[0].loadOp = CORE_ATTACHMENT_LOAD_OP_CLEAR;
    renderPass.renderPassDesc.attachments[0].storeOp = CORE_ATTACHMENT_STORE_OP_STORE;
    renderPass.renderPassDesc.attachmentHandles[0] = { currOutput_.handle };
    renderPass.subpassStartIndex = 0;
    auto& subpass = renderPass.subpassDesc;
    subpass.colorAttachmentCount = 1;
    subpass.colorAttachmentIndices[0] = 0;

    cmdList.BeginRenderPass(renderPass.renderPassDesc, 0U, renderPass.subpassDesc);

    // dynamic state
    const auto& renderNodeUtil = renderNodeContextMgr_->GetRenderNodeUtil();
    cmdList.SetDynamicStateViewport(renderNodeUtil.CreateDefaultViewport(renderPass));
    cmdList.SetDynamicStateScissor(renderNodeUtil.CreateDefaultScissor(renderPass));

    // bind pso
    cmdList.BindPipeline(pipelineData_.pso);

    // update descriptor sets
    {
        auto& binder = *pipelineData_.descriptorSet;
        uint32_t binding = 0U;
        binder.BindBuffer(binding++, gpuBuffer_.GetHandle(), 0U);
        binder.BindImage(binding++, currInput_);

        cmdList.UpdateDescriptorSet(binder.GetDescriptorSetHandle(), binder.GetDescriptorSetLayoutBindingResources());

        cmdList.BindDescriptorSet(0U, binder.GetDescriptorSetHandle());
    }

    // draw
    cmdList.Draw(3U, 1U, 0U, 0U);

    cmdList.EndRenderPass();
}

void RenderPostProcessColorEditNode::EvaluateOutputImageCreation()
{
    if (RenderHandleUtil::IsValid(nodeOutputsData_.output.handle)) {
        currOutput_ = nodeOutputsData_.output;
    } else {
        IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
        Math::UVec2 size { 1U, 1U };
        if (useRequestedRenderArea_) {
            const RenderPassDesc::RenderArea& area = renderAreaRequest_.area;
            size = { area.offsetX + area.extentWidth, area.offsetY + area.extentHeight };
        }
        if ((size.x != ownOutputImageData_.width) || (size.y != ownOutputImageData_.height)) {
            GpuImageDesc desc = gpuResourceMgr.GetImageDescriptor(currInput_.handle);
            if (desc.format == BASE_FORMAT_UNDEFINED) {
                desc.format = BASE_FORMAT_R8G8B8A8_SRGB;
            }
            desc.width = size.x;
            desc.height = size.y;
            desc.usageFlags |= ImageUsageFlagBits::CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                               ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT;
            desc.engineCreationFlags |=
                EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_RESET_STATE_ON_FRAME_BORDERS |
                EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS;
            desc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

            ownOutputImageData_.handle = gpuResourceMgr.Create(desc);
            ownOutputImageData_.width = size.x;
            ownOutputImageData_.height = size.y;
        }
    }
}

} // namespace GsEffects
