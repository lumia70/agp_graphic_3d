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

#include "render_effects_watercolor_node.h"

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

DATA_TYPE_METADATA(RenderPostProcessWatercolorNode::EffectProperties, MEMBER_PROPERTY(enabled, "enabled", 0),
    MEMBER_PROPERTY(radius, "radius", 0), MEMBER_PROPERTY(threshold, "threshold", 0))

DATA_TYPE_METADATA(RenderPostProcessWatercolorNode::NodeInputs, MEMBER_PROPERTY(input, "input", 0))
DATA_TYPE_METADATA(RenderPostProcessWatercolorNode::NodeOutputs, MEMBER_PROPERTY(output, "output", 0))

CORE_END_NAMESPACE()

namespace GsEffects {
namespace {
constexpr DynamicStateEnum DYNAMIC_STATES[] = { CORE_DYNAMIC_STATE_ENUM_VIEWPORT, CORE_DYNAMIC_STATE_ENUM_SCISSOR };

constexpr string_view SHADER_NAME_TENSOR { "gsshaders://shader/watercolor/tensor.shader" };
constexpr string_view SHADER_NAME_KUWAHARA { "gsshaders://shader/watercolor/kuwahara.shader" };
constexpr string_view SHADER_NAME_FINAL { "gsshaders://shader/watercolor/final.shader" };

constexpr int32_t SHADER_PARAM_COUNT { 2 };
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
    const RenderPostProcessWatercolorNode::ShaderParameters& shaderParameters)
{
    if (void* data = gpuResourceMgr.MapBuffer(handle); data) {
        CloneData(data, sizeof(RenderPostProcessWatercolorNode::ShaderParameters), &shaderParameters,
            sizeof(RenderPostProcessWatercolorNode::ShaderParameters));
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

RenderPostProcessWatercolorNode::RenderPostProcessWatercolorNode()
    : properties_(&propertiesData_, PropertyType::DataType<EffectProperties>::MetaDataFromType()),
      inputProperties_(&nodeInputsData_, PropertyType::DataType<NodeInputs>::MetaDataFromType()),
      outputProperties_(&nodeOutputsData_, PropertyType::DataType<NodeOutputs>::MetaDataFromType())
{}

RenderPostProcessWatercolorNode::~RenderPostProcessWatercolorNode() = default;

IPropertyHandle* RenderPostProcessWatercolorNode::GetRenderInputProperties()
{
    return inputProperties_.GetData();
}

IPropertyHandle* RenderPostProcessWatercolorNode::GetRenderOutputProperties()
{
    return outputProperties_.GetData();
}

DescriptorCounts RenderPostProcessWatercolorNode::GetRenderDescriptorCounts() const
{
    return descriptorCounts_;
}

void RenderPostProcessWatercolorNode::SetRenderAreaRequest(const RenderAreaRequest& renderAreaRequest)
{
    useRequestedRenderArea_ = true;
    renderAreaRequest_ = renderAreaRequest;
}

void RenderPostProcessWatercolorNode::InitNode(IRenderNodeContextManager& renderNodeContextMgr)
{
    renderNodeContextMgr_ = &renderNodeContextMgr;

    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();
    defaultInput_.handle = gpuResourceMgr.GetImageHandle("CORE_DEFAULT_GPU_IMAGE");
    defaultInput_.samplerHandle = gpuResourceMgr.GetSamplerHandle("CORE_DEFAULT_SAMPLER_LINEAR_CLAMP");
    gpuBuffer_ = CreateGpuBuffers(gpuResourceMgr, gpuBuffer_);

    InitShaderPass(tensorPass_, SHADER_NAME_TENSOR);
    InitShaderPass(kuwaharaPass_, SHADER_NAME_KUWAHARA);
    InitShaderPass(finalPass_, SHADER_NAME_FINAL);

    tensorPass_.shaderName = 0u;
    kuwaharaPass_.stageIdx = 1u;
    finalPass_.stageIdx = 2u;

    const IRenderNodeUtil& renderNodeUtil = renderNodeContextMgr_->GetRenderNodeUtil();

    DescriptorCounts tensorCounts = renderNodeUtil.GetDescriptorCounts(tensorPass_.gsd.pipelineLayoutData);
    DescriptorCounts kuwaharaCounts = renderNodeUtil.GetDescriptorCounts(kuwaharaPass_.gsd.pipelineLayoutData);
    DescriptorCounts finalCounts = renderNodeUtil.GetDescriptorCounts(finalPass_.gsd.pipelineLayoutData);

    descriptorCounts_.counts.clear();

    for (const auto& count : tensorCounts.counts) {
        descriptorCounts_.counts.push_back(count);
    }

    for (const auto& count : kuwaharaCounts.counts) {
        descriptorCounts_.counts.push_back(count);
    }

    for (const auto& count : finalCounts.counts) {
        descriptorCounts_.counts.push_back(count);
    }

    valid_ = true;
}

void RenderPostProcessWatercolorNode::PreExecuteFrame()
{
    if (valid_) {
        if (const auto* props = GetProperties(); props) {
            enabled_ = CORE_NS::GetPropertyValue<bool>(props, "enabled");

            shaderParameters_.radius = CORE_NS::GetPropertyValue<float>(props, "radius");
            shaderParameters_.threshold = CORE_NS::GetPropertyValue<float>(props, "threshold");
        }
    } else {
        enabled_ = false;
    }

    if (enabled_) {
        EvaluateOutputImageCreation();
    }
}

IRenderNode::ExecuteFlags RenderPostProcessWatercolorNode::GetExecuteFlags() const
{
    if (enabled_ && valid_) {
        return 0;
    } else {
        return IRenderNode::ExecuteFlagBits::EXECUTE_FLAG_BITS_DO_NOT_EXECUTE;
    }
}

void RenderPostProcessWatercolorNode::ExecuteFrame(IRenderCommandList& cmdList)
{
    CORE_ASSERT(enabled_);

    RENDER_DEBUG_MARKER_COL_SCOPE(cmdList, "GsAppPostProcessWatercolor", Math::Vec4(1.0f, 0.0f, 1.0f, 1.0f));

    frameRendered_ = RenderHandleUtil::IsValid(nodeInputsData_.input.handle) ? nodeInputsData_.input : defaultInput_;
    if (!RenderHandleUtil::IsValid(frameRendered_.samplerHandle)) {
        frameRendered_.samplerHandle = defaultInput_.samplerHandle;
    }
    currOutput_.handle = RenderHandleUtil::IsValid(nodeOutputsData_.output.handle)
                             ? nodeOutputsData_.output.handle
                             : ownOutputImageData_.handle.GetHandle();
    if (!RenderHandleUtil::IsValid(currOutput_.handle)) {
        return;
    }

    const IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();

    // a workaround for color conversion with GS rendering
    const auto framebufferDesc = gpuResourceMgr.GetImageDescriptor(currOutput_.handle);
    const bool isTargetSrgb = framebufferDesc.format == BASE_NS::BASE_FORMAT_R8G8B8A8_SRGB;

    RenderPassDesc::RenderArea renderArea;
    if (useRequestedRenderArea_) {
        renderArea = renderAreaRequest_.area;
    } else {
        renderArea = GetImageRenderArea(gpuResourceMgr, currOutput_.handle);
    }

    if ((renderArea.extentWidth == 0) || (renderArea.extentHeight == 0)) {
        return;
    }

    renderAreaRequest_.area = renderArea;

    shaderParameters_.resX = static_cast<float>(renderArea.extentWidth);
    shaderParameters_.resY = static_cast<float>(renderArea.extentHeight);
    shaderParameters_.time =
        renderNodeContextMgr_->GetRenderNodeGraphData().renderingConfiguration.renderTimings.z * 1.0f;
    shaderParameters_.isTargetSrgb = isTargetSrgb ? 1.0f : 0.0f;

    UpdateBuffer(renderNodeContextMgr_->GetGpuResourceManager(), gpuBuffer_.GetHandle(), shaderParameters_);

    CreateTemporaryTargets();

    if (!init_) {
        init_ = true;
        return;
    }

    ExecuteTensorPass(cmdList);
    ExecuteKuwaharaPass(cmdList);
    ExecuteFinalPass(cmdList);
}

void RenderPostProcessWatercolorNode::ExecuteTensorPass(RENDER_NS::IRenderCommandList& cmdList)
{
    RENDER_NS::BindableImage tensorOutput;
    tensorOutput.handle = tensorPass_.targetImage.GetHandle();
    tensorOutput.samplerHandle = defaultInput_.samplerHandle;

    BASE_NS::vector<RENDER_NS::BindableImage> inputs = { frameRendered_ };

    ExecuteShaderPass(cmdList, tensorPass_, inputs, tensorOutput);
}

void RenderPostProcessWatercolorNode::ExecuteKuwaharaPass(RENDER_NS::IRenderCommandList& cmdList)
{
    RENDER_NS::BindableImage tensorOutput;
    tensorOutput.handle = tensorPass_.targetImage.GetHandle();
    tensorOutput.samplerHandle = defaultInput_.samplerHandle;

    RENDER_NS::BindableImage kuwaharaOutput;
    kuwaharaOutput.handle = kuwaharaPass_.targetImage.GetHandle();
    kuwaharaOutput.samplerHandle = defaultInput_.samplerHandle;

    BASE_NS::vector<RENDER_NS::BindableImage> inputs = { frameRendered_, tensorOutput };

    ExecuteShaderPass(cmdList, kuwaharaPass_, inputs, kuwaharaOutput);
}

void RenderPostProcessWatercolorNode::ExecuteFinalPass(RENDER_NS::IRenderCommandList& cmdList)
{
    RENDER_NS::BindableImage kuwaharaOutput;
    kuwaharaOutput.handle = kuwaharaPass_.targetImage.GetHandle();
    kuwaharaOutput.samplerHandle = defaultInput_.samplerHandle;

    BASE_NS::vector<RENDER_NS::BindableImage> inputs = { kuwaharaOutput };

    ExecuteShaderPass(cmdList, finalPass_, inputs, currOutput_);
}

void RenderPostProcessWatercolorNode::InitShaderPass(ShaderPass& pass, BASE_NS::string_view shaderName)
{
    const IRenderNodeShaderManager& shaderMgr = renderNodeContextMgr_->GetShaderManager();
    pass.shaderName = shaderName;
    pass.gsd = shaderMgr.GetGraphicsShaderDataByShaderHandle(shaderMgr.GetShaderHandle(shaderName));
    pass.descriptorSet = {};

    INodeContextPsoManager& psoMgr = renderNodeContextMgr_->GetPsoManager();
    pass.pso = psoMgr.GetGraphicsPsoHandle(pass.gsd, {}, DYNAMIC_STATES);
}

void RenderPostProcessWatercolorNode::ExecuteShaderPass(RENDER_NS::IRenderCommandList& cmdList, ShaderPass& pass,
    const BASE_NS::array_view<RENDER_NS::BindableImage>& inputs, const RENDER_NS::BindableImage& output)
{
    if (!RenderHandleUtil::IsValid(output.handle)) {
        return;
    }

    if (!pass.descriptorSet) {
        pass.descriptorSet = renderNodeContextMgr_->GetDescriptorSetManager().CreateDescriptorSetBinder(
            pass.gsd.pipelineLayoutData.descriptorSetLayouts[0u].bindings);
    }
    if (!pass.descriptorSet) {
        return;
    }

    if (!RenderHandleUtil::IsValid(gpuBuffer_.GetHandle())) {
        return;
    }

    for (const auto& input : inputs) {
        if (!RenderHandleUtil::IsValid(input.handle) || !RenderHandleUtil::IsValid(input.samplerHandle)) {
            return;
        }
    }

    RenderPass renderPass;
    renderPass.renderPassDesc.attachmentCount = 1;
    renderPass.renderPassDesc.renderArea = renderAreaRequest_.area;
    renderPass.renderPassDesc.subpassCount = 1;
    renderPass.renderPassDesc.attachments[0].loadOp = CORE_ATTACHMENT_LOAD_OP_CLEAR;
    renderPass.renderPassDesc.attachments[0].storeOp = CORE_ATTACHMENT_STORE_OP_STORE;
    renderPass.renderPassDesc.attachmentHandles[0] = { output.handle };
    renderPass.subpassStartIndex = 0;
    auto& subpass = renderPass.subpassDesc;
    subpass.colorAttachmentCount = 1;
    subpass.colorAttachmentIndices[0] = 0;

    cmdList.BeginRenderPass(renderPass.renderPassDesc, 0U, renderPass.subpassDesc);

    const auto& renderNodeUtil = renderNodeContextMgr_->GetRenderNodeUtil();
    cmdList.SetDynamicStateViewport(renderNodeUtil.CreateDefaultViewport(renderPass));
    cmdList.SetDynamicStateScissor(renderNodeUtil.CreateDefaultScissor(renderPass));

    cmdList.BindPipeline(pass.pso);

    {
        auto& binder = *pass.descriptorSet;
        uint32_t binding = 0U;

        binder.BindBuffer(binding++, gpuBuffer_.GetHandle(), 0U);

        for (const auto& input : inputs) {
            binder.BindImage(binding++, input);
        }

        cmdList.UpdateDescriptorSet(binder.GetDescriptorSetHandle(), binder.GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0U, binder.GetDescriptorSetHandle());
    }

    cmdList.Draw(3U, 1U, 0U, 0U);
    cmdList.EndRenderPass();
}

void RenderPostProcessWatercolorNode::CreateTemporaryTargets()
{
    IRenderNodeGpuResourceManager& gpuResourceMgr = renderNodeContextMgr_->GetGpuResourceManager();

    uint32_t width, height;
    if (useRequestedRenderArea_) {
        const RenderPassDesc::RenderArea& area = renderAreaRequest_.area;
        width = area.offsetX + area.extentWidth;
        height = area.offsetY + area.extentHeight;
    } else if (RenderHandleUtil::IsValid(frameRendered_.handle)) {
        const GpuImageDesc desc = gpuResourceMgr.GetImageDescriptor(frameRendered_.handle);
        width = desc.width;
        height = desc.height;
    } else {
        width = 1;
        height = 1;
    }

    if (ownOutputImageData_.width == width && ownOutputImageData_.height == height &&
        RenderHandleUtil::IsValid(tensorPass_.targetImage.GetHandle()) &&
        RenderHandleUtil::IsValid(kuwaharaPass_.targetImage.GetHandle())) {
        return;
    }

    GpuImageDesc desc;
    if (RenderHandleUtil::IsValid(frameRendered_.handle)) {
        desc = gpuResourceMgr.GetImageDescriptor(frameRendered_.handle);
    } else {
        desc.format = BASE_FORMAT_R8G8B8A8_SRGB;
    }

    if (desc.format == BASE_FORMAT_UNDEFINED) {
        desc.format = BASE_FORMAT_R8G8B8A8_SRGB;
    }
    desc.width = width;
    desc.height = height;
    desc.usageFlags |= ImageUsageFlagBits::CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                       ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT |
                       ImageUsageFlagBits::CORE_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    desc.engineCreationFlags |= EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_RESET_STATE_ON_FRAME_BORDERS |
                                EngineImageCreationFlagBits::CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS;
    desc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

    tensorPass_.targetImage = gpuResourceMgr.Create(desc);
    kuwaharaPass_.targetImage = gpuResourceMgr.Create(desc);

    ownOutputImageData_.width = width;
    ownOutputImageData_.height = height;
    init_ = false;
}

void RenderPostProcessWatercolorNode::EvaluateOutputImageCreation()
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
            GpuImageDesc desc = gpuResourceMgr.GetImageDescriptor(frameRendered_.handle);
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
