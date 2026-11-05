
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

#include "gs_editor.h"

#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <3d/implementation_uids.h>
#include <3d/namespace.h>
#include <3d/render/intf_render_node_scene_util.h>
#include <base/math/mathf.h>
#include <core/intf_engine.h>
#include <core/log.h>
#include <core/plugin/intf_class_register.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_node_context_pso_manager.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/nodecontext/intf_render_node_util.h>
#include <util/gs_serialization.h>
#include <plugin_gs/util/gs_limits.h>

#include "gsedit_shader_action_executor.h"
#include "intf_gsedit_action_executor.h"

namespace GS {

namespace {
#include "app/shaders/common/gs_edit_common.h"
#include "app/shaders/common/gs_struct_common.h"

}  // namespace

using namespace RENDER_NS;
using namespace BASE_NS;
using namespace CORE_NS;
using namespace CORE3D_NS;

namespace {

inline void GetDescriptorCounts(
    const PipelineLayout& pl, const uint32_t setIdx, const uint32_t setCount, DescriptorCounts& descriptorCounts)
{
    const auto& bindings = pl.descriptorSetLayouts[setIdx].bindings;
    descriptorCounts.counts.reserve(descriptorCounts.counts.size() + bindings.size());
    for (const auto& bindingRef : bindings) {
        descriptorCounts.counts.emplace_back(
            DescriptorCounts::TypedCount{bindingRef.descriptorType, bindingRef.descriptorCount * setCount});
    }
}

constexpr GeneralBarrier COMPUTE_WRITE{
    AccessFlagBits::CORE_ACCESS_SHADER_WRITE_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
};
constexpr GeneralBarrier COMPUTE_READ{
    AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT | AccessFlagBits::CORE_ACCESS_INDIRECT_COMMAND_READ_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
        PipelineStageFlagBits::CORE_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
};
constexpr GeneralBarrier COMPUTE_READ_WRITE{
    AccessFlagBits::CORE_ACCESS_SHADER_READ_BIT | AccessFlagBits::CORE_ACCESS_SHADER_WRITE_BIT,
    PipelineStageFlagBits::CORE_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
};
}  // namespace

GsEditor::GsEditor(RENDER_NS::IRenderNodeContextManager* renderNodeContextMgr)
    : renderNodeContextMgr_(renderNodeContextMgr)
{
    IClassRegister* classRegister = renderNodeContextMgr_->GetRenderContext().GetInterface<IClassRegister>();
    if (classRegister) {
        if (auto* renderNodeSceneUtil = CORE_NS::GetInstance<CORE3D_NS::IRenderNodeSceneUtil>(
                *classRegister, CORE3D_NS::UID_RENDER_NODE_SCENE_UTIL);
            renderNodeSceneUtil) {
            const auto& renderNodeGraphData = renderNodeContextMgr_->GetRenderNodeGraphData();
            stores_ = renderNodeSceneUtil->GetSceneRenderDataStores(
                *renderNodeContextMgr_, renderNodeGraphData.renderNodeGraphDataStoreName);
        }
    }

    if (!dataStoreGs_ && renderNodeContextMgr_) {
        dataStoreGs_ = static_cast<RenderDataStoreDefaultGS*>(
            renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(
                stores_.dataStoreNamePrefix + DEFAULT_GS_RENDER_DATA_STORE_NAME_POST_FIX));
    }

    if (!dataStoreGs_) {
        CORE_LOG_E("GsEditor: Failed to acquire RenderDataStoreDefaultGS");
        return;
    }

    if (!dataStoreEdit_ && renderNodeContextMgr_) {
        dataStoreEdit_ =
            static_cast<RenderDataStoreGsEdit*>(renderNodeContextMgr_->GetRenderDataStoreManager().GetRenderDataStore(
                stores_.dataStoreNamePrefix + GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX));
    }

    if (!dataStoreEdit_) {
        CORE_LOG_E("GsEditor: Failed to acquire RenderDataStoreGsEdit");
        return;
    }

    shaderToPsoData_.clear();
}

GsEditor::~GsEditor()
{}

void GsEditor::InitializeExecutors()
{
    actionExecutors_[GsEditActionType::ClearSelection] = make_unique<ClearSelectedExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::ColorSelection] = make_unique<ColorSelectedExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::InvertSelection] = make_unique<InvertSelectionExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::RemoveSelected] = make_unique<RemoveSelectedExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::SelectBox] = make_unique<SelectBoxExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::Select3DBox] = make_unique<Select3DBoxExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::SelectByIndex] = make_unique<SelectByIndexExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::SelectByMask] = make_unique<SelectByMaskExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::Transform] = make_unique<TransformSelectedExecutor>(renderNodeContextMgr_);
    actionExecutors_[GsEditActionType::FloaterRemoval] = make_unique<FloaterRemovalExecutor>(renderNodeContextMgr_);
}

void GsEditor::RegisterActionExecutor(GsEditActionType actionType, unique_ptr<IActionExecutor> executor)
{
    actionExecutors_[actionType] = move(executor);
}

void GsEditor::RegisterActionExecutorFactory(GsEditActionType actionType, ActionExecutorFactory factory)
{
    pendingFactories_[actionType] = factory;
}

RENDER_NS::RenderHandleReference GsEditor::CreateGpuBuffer(const BASE_NS::string_view& name,
    const RENDER_NS::GpuBufferDesc desc, const BASE_NS::array_view<const uint8_t>& data)
{
    if (name.empty()) {
        CORE_LOG_E("GsEditor::CreateGpuBuffer: Tried to create buffer with empty name!");
        return {};
    }

    RENDER_NS::RenderHandleReference rhr = renderNodeContextMgr_->GetGpuResourceManager().Create(name, desc, data);
    return rhr;
}

RENDER_NS::RenderHandleReference GsEditor::CreateGpuBuffer(
    const RENDER_NS::GpuBufferDesc desc, const BASE_NS::string name)
{
    RENDER_NS::RenderHandleReference rhr = name.empty()
                                               ? renderNodeContextMgr_->GetGpuResourceManager().Create(desc)
                                               : renderNodeContextMgr_->GetGpuResourceManager().Create(name, desc);
    return rhr;
}

bool GsEditor::ExecuteStack(RENDER_NS::IRenderCommandList& cmdList)
{
    if (!dataStoreGs_ != !dataStoreEdit_) {
        return false;
    }

    if (editActionStack_.empty()) {
        perActionDataBufferUpdated_ = true;
        numModifyingPasses_ = 0;
        return false;
    }

    // upload the per-action buffer
    if (!perActionDataBufferUpdated_) {
        UpdateActionCameraMatrixBufferData();
    }

    stackBufferPingPong_ = true;
    numSplatsEdited_ = 0;

    GsRenderData* renderData = dataStoreGs_->GetGsRenderData();

    if (!gsEditSplatData0) {
        CreateBuffers(cmdList, renderData);
        return false;
    }

    numModifyingPasses_ = 0;

    ClearSelection(cmdList);

    for (size_t actionIdx = 0; actionIdx < editActionStack_.size(); actionIdx++) {
        const GsEditActionType actionType = editActionStack_[actionIdx].type;

        auto it = actionExecutors_.find(actionType);
        if (it == actionExecutors_.end()) {
            CORE_LOG_E("GsEditor::ExecuteStack: No executor registered for action type %u!",
                static_cast<uint32_t>(actionType));
            return false;
        }

        IActionExecutor* executor = it->second.get();

        const bool isSelectionAction = executor->IsSelectionAction();
        const bool clearSelectionBuffer = executor->RequiresSelectionClear();
        const bool modifiesBuffer = executor->ModifiesSplatBuffer();

        if (clearSelectionBuffer) {
            ClearSelection(cmdList);
        }

        ExecuteAction(cmdList,
            executor,
            editActionStack_[actionIdx],
            renderData,
            static_cast<uint32_t>(actionIdx),
            numModifyingPasses_,
            isSelectionAction);

        // Barrier between actions so next action sees counter/buffer writes
        if (actionIdx + 1u < editActionStack_.size()) {
            cmdList.CustomMemoryBarrier(COMPUTE_WRITE, COMPUTE_READ_WRITE);
            cmdList.AddCustomBarrierPoint();
        }

        if (modifiesBuffer) {
            stackBufferPingPong_ = !stackBufferPingPong_;
            numModifyingPasses_++;
        }
    }

    // Final barrier so render passes see counter and splat buffer writes
    if (numModifyingPasses_ > 0u) {
        cmdList.CustomMemoryBarrier(COMPUTE_WRITE, COMPUTE_READ);
        cmdList.AddCustomBarrierPoint();
    }

    return true;
}

void GsEditor::PushActionToStack(const GsEditAction& action)
{
    editActionStack_.push_back(action);
    RecreateActionCameraMatrixBuffer();
}

uint32_t GsEditor::PopActionFromStack()
{
    editActionStack_.pop_back();
    RecreateActionCameraMatrixBuffer();
    return static_cast<uint32_t>(editActionStack_.size());
}

void GsEditor::SetRenderNodeBuffers(
    const RENDER_NS::RenderHandleReference cameraUbo, const RENDER_NS::RenderHandleReference dynamicDataUbo)
{
    cameraUniformBuffer_ = cameraUbo;
    gsDynamicDataBuffer_ = dynamicDataUbo;
}

GsEditor::PsoData GsEditor::GetPsoData(
    const RENDER_NS::PipelineLayout& pl, const RENDER_NS::RenderHandle& shaderHandle, const uint32_t flags)
{
    uint64_t seed = shaderHandle.id;
    HashCombine(seed, static_cast<uint64_t>(flags));
    if (const auto iter = shaderToPsoData_.find(seed); iter != shaderToPsoData_.cend()) {
        return iter->second;
    } else {
        const auto& shaderMgr = renderNodeContextMgr_->GetShaderManager();
        constexpr size_t maxSpecializationFlagCount{4u};
        uint32_t specializationFlags[maxSpecializationFlagCount] = {};
        const ShaderSpecializationConstantView& sscv = shaderMgr.GetReflectionSpecialization(shaderHandle);
        const size_t maxSpecializations = Math::min(maxSpecializationFlagCount, sscv.constants.size());
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

        const ShaderSpecializationConstantDataView spec{
            {sscv.constants.data(), maxSpecializations}, {specializationFlags, maxSpecializations}};
        const RenderHandle psoHandle =
            renderNodeContextMgr_->GetPsoManager().GetComputePsoHandle(shaderHandle, pl, spec);
        const PsoData psoData = {psoHandle, pl.pushConstant};
        shaderToPsoData_[seed] = psoData;
        return psoData;
    }
}

void GsEditor::CreateBuffers(RENDER_NS::IRenderCommandList& cmdList, GS::GsRenderData* renderData)
{
    if (!renderData || !renderNodeContextMgr_) {
        return;
    }

    GpuBufferDesc splatBufferDesc =
        renderNodeContextMgr_->GetGpuResourceManager().GetBufferDescriptor(renderData->gsSplatData.GetHandle());

    splatBufferDesc.usageFlags |= BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
    splatBufferDesc.memoryPropertyFlags |=
        MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    gsEditSplatData0 = CreateGpuBuffer(splatBufferDesc);
    gsEditSplatData1 = CreateGpuBuffer(splatBufferDesc);
    if (!gsEditSplatData0 || !gsEditSplatData1) {
        CORE_LOG_E("GsEditor: Failed to create GPU splat data buffer");
        return;
    }

    const uint32_t numSplats = splatBufferDesc.byteSize / sizeof(GsSplatData);

    if (numSplats > (MAX_GS_SELECT_BUFFER_BYTE_SIZE - 1) / sizeof(uint32_t)) {
        CORE_LOG_E("GsEditor: numSplats overflow in select buffer allocation");
        return;
    }

    // selection buffer
    {
        GpuBufferDesc selectBufferDesc;
        selectBufferDesc.byteSize = (numSplats + 1) * sizeof(uint32_t);
        selectBufferDesc.engineCreationFlags =
            EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        selectBufferDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        selectBufferDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        gsSelectionIndexBuffer = CreateGpuBuffer(selectBufferDesc, "GsEditSelectIndexBuffer");
    }

    // indirect counter buffer
    {
        GpuBufferDesc icBufferDesc;
        icBufferDesc.byteSize = sizeof(IndirectEditDispatchCounterBuffer) * 4u;
        icBufferDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        icBufferDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                           MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        icBufferDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                  BufferUsageFlagBits::CORE_BUFFER_USAGE_INDIRECT_BUFFER_BIT;

        indirectDispatchBuffer_ = CreateGpuBuffer(icBufferDesc, "GsEditIndirectCounterBuffer");
    }
}

void GsEditor::RecreateActionCameraMatrixBuffer()
{
    GpuBufferDesc perActionBuffer;
    perActionBuffer.byteSize = sizeof(ActionCameraDataStruct) * static_cast<uint32_t>(editActionStack_.size());
    perActionBuffer.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS |
                                          EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE;
    perActionBuffer.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    perActionBuffer.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    perActionDataBuffer_ = CreateGpuBuffer(
        perActionBuffer, BASE_NS::string("GsEditPerActionCameraBuffer_" + BASE_NS::to_string(generationCounter_)));
    generationCounter_++;

    perActionDataBufferUpdated_ = false;
}

void GsEditor::UpdateActionCameraMatrixBufferData()
{
    BASE_NS::vector<ActionCameraDataStruct> perActionData;

    for (const auto& action : editActionStack_) {
        ActionCameraDataStruct acd;
        acd.viewMatrix = action.cameraViewMatrix;
        acd.viewProjMatrix = action.cameraViewProjMatrix;

        perActionData.push_back(acd);
    }

    const uint32_t byteSize = static_cast<uint32_t>(editActionStack_.size() * sizeof(ActionCameraDataStruct));

    void* mappedData = renderNodeContextMgr_->GetGpuResourceManager().MapBufferMemory(perActionDataBuffer_.GetHandle());
    if (mappedData) {
        if (BASE_NS::CloneData(mappedData, byteSize, &perActionData[0], byteSize)) {
            perActionDataBufferUpdated_ = true;
        }
        renderNodeContextMgr_->GetGpuResourceManager().UnmapBuffer(perActionDataBuffer_.GetHandle());
    } else {
        CORE_LOG_E("GsEditor::UpdateActionCameraMatrixBufferData: Failed to map perActionDataBuffer_!");
    }
}

void GsEditor::ClearSelection(RENDER_NS::IRenderCommandList& cmdList)
{
    if (!renderNodeContextMgr_) {
        CORE_LOG_E("GsEditor::ClearSelection: render node context manager is nullptr!");
        return;
    }
    auto& binders = clearSelectionBinders_;

    if (!binders.set0) {
        CORE_LOG_E("GsEditor::ClearSelection: set0 binder is nullptr!");
        return;
    }
    const PsoData psoData = GetPsoData(binders.pl, clearSelectionHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    binders.set0->BindBuffer(0u, gsSelectionIndexBuffer.GetHandle(), 0);

    const auto& set0Handle = binders.set0->GetDescriptorSetHandle();
    cmdList.UpdateDescriptorSet(set0Handle, binders.set0->GetDescriptorSetLayoutBindingResources());
    cmdList.BindDescriptorSet(0, set0Handle);

    const uint32_t numSplats =
        renderNodeContextMgr_->GetGpuResourceManager().GetBufferDescriptor(gsEditSplatData0.GetHandle()).byteSize /
        sizeof(GsSplatData);

    cmdList.Dispatch((numSplats + GS_EDIT_WORKGROUP_SIZE - 1) / GS_EDIT_WORKGROUP_SIZE, 1, 1);
}

void GsEditor::ExecuteAction(RENDER_NS::IRenderCommandList& cmdList, IActionExecutor* executor,
    const GsEditAction& action, GsRenderData* renderData, const uint32_t actionIndex, const uint32_t modifyingPassIdx,
    const bool isSelectionAction)
{
    if (!renderData) {
        CORE_LOG_E("GsEditor::ExecuteAction: renderData is nullptr!");
        return;
    }
    const uint32_t numInputSplats =
        modifyingPassIdx == 0u ? static_cast<uint32_t>(renderData->gaussianPositions.size()) : 0u;
    const uint32_t selectClear = isSelectionAction ? 0 : 0xFFFFffff;
    if (executor->NeedsIndirectDispatchSetup()) {
        ExecuteUpdateIndirectDispatch(cmdList, numInputSplats, selectClear);
        // Ensure indirect dispatch params are visible before DispatchIndirect reads them
        cmdList.CustomMemoryBarrier(COMPUTE_WRITE, COMPUTE_READ);
        cmdList.AddCustomBarrierPoint();
    }

    if (!executor) {
        CORE_LOG_E("GsEditor::ExecuteAction: executor was null!");
        return;
    }

    // execution context
    ActionExecutionContext context;
    context.immutableSplatData = renderData->gsSplatData;
    context.gsEditSplatData0 = gsEditSplatData0;
    context.gsEditSplatData1 = gsEditSplatData1;
    context.gsSelectionIndexBuffer = gsSelectionIndexBuffer;
    context.indirectDispatchBuffer = indirectDispatchBuffer_;
    context.perActionDataBuffer = perActionDataBuffer_;
    context.cameraUniformBuffer = cameraUniformBuffer_;
    context.gsDynamicDataBuffer = gsDynamicDataBuffer_;
    context.renderData = renderData;
    context.actionIndex = actionIndex;
    context.modifyingPassIdx = modifyingPassIdx;
    context.bufferPingPong = stackBufferPingPong_;

    executor->Execute(cmdList, action, context);
}

void GsEditor::ExecuteUpdateIndirectDispatch(
    RENDER_NS::IRenderCommandList& cmdList, const uint32_t numInputSplats, const uint32_t selectCountClearValue)
{
    auto& binders = updateIndirectDispatchBinders_;

    const PsoData psoData = GetPsoData(binders.pl, updateIndirectDispatchHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    {
        binders.set0->BindBuffer(0u, indirectDispatchBuffer_.GetHandle(), 0);
        binders.set0->BindBuffer(1u, gsSelectionIndexBuffer.GetHandle(), 0);

        const auto& set0Handle = binders.set0->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, binders.set0->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);
    }

    GsEditIndirectPushConstantData uPc;
    uPc.numInitialSplats = numInputSplats;
    uPc.resetCounterValue = 0;
    uPc.indexClearValue = selectCountClearValue;

    cmdList.PushConstant(psoData.pushConstant, reinterpret_cast<const uint8_t*>(&uPc));

    cmdList.Dispatch(1, 1, 1);
}

void GsEditor::CalculateDescriptorCounts(RENDER_NS::DescriptorCounts& dc)
{
    const auto& shaderMgr = renderNodeContextMgr_->GetShaderManager();

    // indirect args
    {
        updateIndirectDispatchHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gsedit/gsedit_update_indirect_args.shader");

        updateIndirectDispatchBinders_.pl = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gsedit_update_indirect_args.shaderpl"));
        GetDescriptorCounts(updateIndirectDispatchBinders_.pl, 0u, 1u, dc);
    }

    // clear selection
    {
        clearSelectionHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gsedit/gsedit_clear_selection.shader");

        clearSelectionBinders_.pl = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gsedit_clear_selection.shaderpl"));
        GetDescriptorCounts(clearSelectionBinders_.pl, 0u, 1u, dc);
    }

    // gs export copy
    {
        exportCopyHandle_ =
            shaderMgr.GetShaderHandle("gsshaders://computeshader/gsedit/gsedit_buffer_copy_indirect.shader");

        exportCopyBinder_.pl = shaderMgr.GetPipelineLayout(
            shaderMgr.GetPipelineLayoutHandle("gspipelinelayouts://gsedit_buffer_copy_indirect.shaderpl"));
        GetDescriptorCounts(exportCopyBinder_.pl, 0u, 1u, dc);
    }

    // Initialize default executors
    InitializeExecutors();

    // create executors from factories
    for (auto& atypeFactory : pendingFactories_) {
        actionExecutors_[atypeFactory.first] = atypeFactory.second(renderNodeContextMgr_);
    }
    pendingFactories_.clear();

    // descriptor counts for all executors
    for (auto& atypeExecutor : actionExecutors_) {
        atypeExecutor.second->CalculateDescriptorCounts(*renderNodeContextMgr_, dc);
    }
}

void GsEditor::CreateDescriptorSets()
{
    auto createDescriptorSet = [](INodeContextDescriptorSetManager& descriptorSetMgr,
                                   PipelineLayout& pl,
                                   const uint32_t bufferSetIndex,
                                   IDescriptorSetBinder::Ptr& setBinder) {
        const RenderHandle setDescHandle = descriptorSetMgr.CreateDescriptorSet(bufferSetIndex, pl);
        setBinder =
            descriptorSetMgr.CreateDescriptorSetBinder(setDescHandle, pl.descriptorSetLayouts[bufferSetIndex].bindings);
    };

    auto& descriptorSetMgr = renderNodeContextMgr_->GetDescriptorSetManager();

    createDescriptorSet(descriptorSetMgr, updateIndirectDispatchBinders_.pl, 0u, updateIndirectDispatchBinders_.set0);
    createDescriptorSet(descriptorSetMgr, clearSelectionBinders_.pl, 0u, clearSelectionBinders_.set0);
    createDescriptorSet(descriptorSetMgr, exportCopyBinder_.pl, 0u, exportCopyBinder_.set0);

    for (auto& [actionType, executor] : actionExecutors_) {
        executor->CreateDescriptorSets(*renderNodeContextMgr_);
    }
}

RENDER_NS::RenderHandleReference GsEditor::GetGsEditedBuffer()
{
    if (numModifyingPasses_ == 0 && dataStoreGs_) {
        auto renderData = dataStoreGs_->GetGsRenderData();
        if (renderData) {
            return renderData->gsSplatData;
        }
    }

    if (!stackBufferPingPong_) {
        return gsEditSplatData1;
    } else {
        return gsEditSplatData0;
    }
}

RENDER_NS::RenderHandleReference GsEditor::GetIndirectDispatchBuffer()
{
    return indirectDispatchBuffer_;
}

uint32_t GsEditor::GetEditorNumSplats()
{
    uint8_t* const mappedData = reinterpret_cast<uint8_t*>(
        renderNodeContextMgr_->GetGpuResourceManager().MapBufferMemory(indirectDispatchBuffer_.GetHandle()));

    if (!mappedData) {
        CORE_LOG_E("GsEditor::GetEditorNumSplats: Failed to map indirect count buffer!");
        return 0;
    }

    IndirectEditDispatchCounterBuffer counterData;
    CloneData(
        &counterData, sizeof(IndirectEditDispatchCounterBuffer), mappedData, sizeof(IndirectEditDispatchCounterBuffer));
    renderNodeContextMgr_->GetGpuResourceManager().UnmapBuffer(indirectDispatchBuffer_.GetHandle());

    numSplatsEdited_ = counterData.counter;

    return numSplatsEdited_;
}

uint32_t GsEditor::GetNumModifyingPasses() const
{
    return numModifyingPasses_;
}

void GsEditor::ClearActionStack()
{
    editActionStack_.clear();
}

void GsEditor::BeginGsModelExport(RENDER_NS::IRenderCommandList& cmdList)
{
    if (numModifyingPasses_ == 0) {
        return;
    }

    if (!renderNodeContextMgr_ || !dataStoreGs_ || !dataStoreGs_->GetGsRenderData() || !exportCopyBinder_.set0) {
        return;
    }
    GpuBufferDesc gsSplatDataDesc = renderNodeContextMgr_->GetGpuResourceManager().GetBufferDescriptor(
        dataStoreGs_->GetGsRenderData()->gsSplatData.GetHandle());
    const auto byteSize = gsSplatDataDesc.byteSize + sizeof(GsObjectLoadData::GsSplatData);
    if (byteSize > GS::MAX_GS_EXPORT_BUFFER_BYTE_SIZE) {
        CORE_LOG_E("GsEditor::BeginGsModelExport: byteSize overflow");
        return;
    }
    if (!gsEditorExportBuffer_ ||
        renderNodeContextMgr_->GetGpuResourceManager().GetBufferDescriptor(gsEditorExportBuffer_.GetHandle()).byteSize <
            byteSize) {
        gsSplatDataDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT |
                                     BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        gsSplatDataDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        gsSplatDataDesc.engineCreationFlags =
            EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE |
            EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER;
        gsSplatDataDesc.byteSize = static_cast<uint32_t>(byteSize);
        gsEditorExportBuffer_ =
            renderNodeContextMgr_->GetGpuResourceManager().Create("GsSplatDataStagingBuffer", gsSplatDataDesc);
    }
    GsObjectLoadData::GsSplatData* mappedData = reinterpret_cast<GsObjectLoadData::GsSplatData*>(
        renderNodeContextMgr_->GetGpuResourceManager().MapBufferMemory(gsEditorExportBuffer_.GetHandle()));
    if (mappedData) {
        (*mappedData).astcOffsetColor.x = 0;
        renderNodeContextMgr_->GetGpuResourceManager().UnmapBuffer(gsEditorExportBuffer_.GetHandle());
    }
    auto& binders = exportCopyBinder_;
    const PsoData psoData = GetPsoData(binders.pl, exportCopyHandle_, 0xFFFFffff);
    cmdList.BindPipeline(psoData.psoHandle);

    {
        binders.set0->BindBuffer(0u, gsEditorExportBuffer_.GetHandle(), 0);
        binders.set0->BindBuffer(1u, GetGsEditedBuffer().GetHandle(), 0);
        binders.set0->BindBuffer(2u, indirectDispatchBuffer_.GetHandle(), 0);

        const auto& set0Handle = binders.set0->GetDescriptorSetHandle();
        cmdList.UpdateDescriptorSet(set0Handle, binders.set0->GetDescriptorSetLayoutBindingResources());
        cmdList.BindDescriptorSet(0, set0Handle);
    }

    cmdList.DispatchIndirect(indirectDispatchBuffer_.GetHandle(), 0);
}

void GsEditor::EndGsModelExport()
{
    if (numModifyingPasses_ == 0) {
        ExportFromDirectBuffer();
        return;
    }

    ExportFromStagingBuffer();
}

void GsEditor::ExportFromDirectBuffer()
{
    renderNodeContextMgr_->GetRenderContext().GetDevice().WaitForIdle();

    auto* renderData = dataStoreGs_->GetGsRenderData();
    if (!renderData) {
        dataStoreEdit_->TriggerExportModelCallback(false, "renderData null");
        dataStoreEdit_->SetExportModelFlag(false);
        dataStoreEdit_->UpdateExportModelStatus();
        return;
    }

    auto srcHandle = GetGsEditedBuffer().GetHandle();
    GpuBufferDesc srcDesc = renderNodeContextMgr_->GetGpuResourceManager()
                                .GetBufferDescriptor(srcHandle);

    const uint32_t numSplatsFromGpu = srcDesc.byteSize / sizeof(GsObjectLoadData::GsSplatData);
    const uint32_t numSplatsFromCpu = renderData->gaussianPositions.size();

    if (numSplatsFromGpu != numSplatsFromCpu) {
        dataStoreEdit_->TriggerExportModelCallback(false, "numSplats mismatch");
        dataStoreEdit_->SetExportModelFlag(false);
        dataStoreEdit_->UpdateExportModelStatus();
        return;
    }

    const uint32_t numSplats = numSplatsFromGpu;

    GsObjectLoadData::GsSplatData* mappedSrc = reinterpret_cast<GsObjectLoadData::GsSplatData*>(
        renderNodeContextMgr_->GetGpuResourceManager().MapBufferMemory(srcHandle));

    if (!mappedSrc) {
        dataStoreEdit_->TriggerExportModelCallback(false, "map src failed");
        dataStoreEdit_->SetExportModelFlag(false);
        dataStoreEdit_->UpdateExportModelStatus();
        return;
    }

    dataStoreEdit_->UpdateExportModelStatus();

    auto errc = WriteGaussianData(
        renderNodeContextMgr_->GetRenderContext().GetEngine().GetFileManager(),
        dataStoreEdit_->GetExportModelPath(), mappedSrc, numSplats);
    if (errc == GS_SERIALIZATION_ERRC::INVALID_PATH) {
        dataStoreEdit_->TriggerExportModelCallback(false, "invalid path");
    } else {
        dataStoreEdit_->TriggerExportModelCallback(true, "");
    }

    renderNodeContextMgr_->GetGpuResourceManager().UnmapBuffer(srcHandle);
    dataStoreEdit_->SetExportModelFlag(false);
    dataStoreEdit_->UpdateExportModelStatus();
}

void GsEditor::ExportFromStagingBuffer()
{
    GsObjectLoadData::GsSplatData* mappedData = reinterpret_cast<GsObjectLoadData::GsSplatData*>(
        renderNodeContextMgr_->GetGpuResourceManager().MapBufferMemory(gsEditorExportBuffer_.GetHandle()));
    if (mappedData) {
        dataStoreEdit_->UpdateExportModelStatus();
        auto numSplats = (*mappedData).astcOffsetColor.x;

        auto errc = WriteGaussianData(renderNodeContextMgr_->GetRenderContext().GetEngine().GetFileManager(),
            dataStoreEdit_->GetExportModelPath(),
            mappedData + 1,
            numSplats);
        if (errc == GS_SERIALIZATION_ERRC::INVALID_PATH) {
            dataStoreEdit_->TriggerExportModelCallback(false, "invalid path");
        } else {
            dataStoreEdit_->TriggerExportModelCallback(true, "");
        }
        dataStoreEdit_->SetExportModelFlag(false);
        dataStoreEdit_->UpdateExportModelStatus();
    }

    if (mappedData) {
        renderNodeContextMgr_->GetGpuResourceManager().UnmapBuffer(gsEditorExportBuffer_.GetHandle());
    }
    gsEditorExportBuffer_ = {};
}

}  // namespace GS
