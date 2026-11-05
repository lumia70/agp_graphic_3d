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

#include <algorithm>
#include <plugin_gs/ecs/systems/intf_gs_edit_system.h>
#include <plugin_gs/render/intf_render_data_store_gs_edit.h>

#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/environment_component.h>
#include <3d/ecs/components/light_component.h>
#include <3d/ecs/components/local_matrix_component.h>
#include <3d/ecs/components/material_component.h>
#include <3d/ecs/components/mesh_component.h>
#include <3d/ecs/components/name_component.h>
#include <3d/ecs/components/node_component.h>
#include <3d/ecs/components/post_process_configuration_component.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/render_handle_component.h>
#include <3d/ecs/components/render_mesh_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/components/uri_component.h>
#include <3d/ecs/components/world_matrix_component.h>
#include <3d/ecs/systems/intf_animation_system.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <3d/implementation_uids.h>
#include <3d/intf_graphics_context.h>
#include <3d/loaders/intf_scene_loader.h>
#include <3d/util/intf_mesh_util.h>
#include <3d/util/intf_picking.h>
#include <3d/util/intf_scene_util.h>
#include <base/containers/array_view.h>
#include <base/containers/byte_array.h>
#include <base/containers/fixed_string.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/mathf.h>
#include <base/math/matrix_util.h>
#include <base/math/quaternion.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/ecs/intf_entity_manager.h>
#include <core/engine_info.h>
#include <core/image/intf_image_container.h>
#include <core/image/intf_image_loader_manager.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/json/json.h>
#include <core/log.h>
#include <core/os/intf_platform.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/plugin/intf_class_factory.h>
#include <core/property/intf_property_api.h>
#include <core/property/intf_property_handle.h>
#include <core/property/property_handle_util.h>
#include <core/property/scoped_handle.h>
#include <core/property_tools/property_api_impl.h>
#include <core/property_tools/property_api_impl.inl>
#include <core/property_tools/property_macros.h>
#include <core/util/parallel_sort.h>
#include <render/datastore/intf_render_data_store_default_staging.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/device/gpu_resource_desc.h>
#include <render/device/intf_device.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/implementation_uids.h>
#include <render/intf_render_context.h>
#include <render/intf_renderer.h>
#include <render/nodecontext/intf_render_node_graph_manager.h>
#include <render/property/property_types.h>

#include "plugin_gs/ecs/components/gs_configuration_component.h"
#include "plugin_gs/util/gs_limits.h"
#include "util/gs_edit_command_parsing.h"

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;
using namespace GS_NS;

namespace {
constexpr string_view RENDER_DATA_STORE_DEFAULT_STAGING = "RenderDataStoreDefaultStaging";

RenderHandleReference CreateGpuResource(IGpuResourceManager& gpuResourceManager,
    IRenderDataStoreDefaultStaging& staging, uint64_t width, uint64_t height, uint64_t rowSize, Format format,
    uint64_t formatByteSize, array_view<const uint8_t> data)
{
    if (formatByteSize == 0) {
        return {};
    }

    const uint64_t rowTexels = (rowSize > 0) ? (rowSize / formatByteSize) : width;

    GpuImageDesc imageDesc;
    imageDesc.imageType = ImageType::CORE_IMAGE_TYPE_2D;
    imageDesc.imageViewType = ImageViewType::CORE_IMAGE_VIEW_TYPE_2D;
    imageDesc.format = format;
    imageDesc.imageTiling = ImageTiling::CORE_IMAGE_TILING_OPTIMAL;
    imageDesc.usageFlags =
        ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    imageDesc.width = static_cast<uint32_t>(width);
    imageDesc.height = static_cast<uint32_t>(height);
    imageDesc.depth = 1;
    imageDesc.mipCount = 1;
    imageDesc.layerCount = 1;
    imageDesc.sampleCountFlags = SampleCountFlagBits::CORE_SAMPLE_COUNT_1_BIT;

    RenderHandleReference imageHandle = gpuResourceManager.Create(imageDesc);
    if (!imageHandle) {
        return {};
    }

    GpuBufferDesc bufferDesc;
    bufferDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bufferDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    bufferDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE |
                                     EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER |
                                     EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DEFERRED_DESTROY;
    bufferDesc.byteSize = static_cast<uint32_t>(data.size());

    RenderHandleReference stagingBuffer = gpuResourceManager.Create(bufferDesc);
    if (!stagingBuffer) {
        return {};
    }

    if (auto* ptr = static_cast<uint8_t*>(gpuResourceManager.MapBufferMemory(stagingBuffer))) {
        CloneData(ptr, data.size(), data.data(), data.size());
    }
    gpuResourceManager.UnmapBuffer(stagingBuffer);

    BufferImageCopy bic {};
    bic.bufferOffset = 0;
    bic.bufferRowLength = static_cast<uint32_t>(rowTexels);
    bic.bufferImageHeight = static_cast<uint32_t>(height);
    bic.imageSubresource = { CORE_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    bic.imageOffset = { 0, 0, 0 };
    bic.imageExtent = { static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1 };

    staging.CopyBufferToImage(stagingBuffer, imageHandle, bic);
    return imageHandle;
}

RenderHandleReference CreateGpuBuffer(IGpuResourceManager& gpuResourceManager, IRenderDataStoreDefaultStaging& staging,
    const vector<array_view<const uint8_t>>& dataSegments)
{
    if (dataSegments.empty()) {
        return {};
    }
    size_t totalSize {};
    for (const auto& seg : dataSegments) {
        totalSize += seg.size_bytes();
    }
    if (totalSize == 0) {
        return {};
    }
    if (totalSize > GS::MAX_GPU_BUFFER_BYTE_SIZE) {
        CORE_LOG_E("CreateGpuBuffer: totalSize exceeds MAX_GPU_BUFFER_BYTE_SIZE limit");
        return {};
    }

    GpuBufferDesc gpuBufferDesc;
    gpuBufferDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                               BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
    gpuBufferDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    gpuBufferDesc.byteSize = static_cast<uint32_t>(totalSize);
    RenderHandleReference gpuBuffer = gpuResourceManager.Create(gpuBufferDesc);
    if (!gpuBuffer) {
        return {};
    }

    GpuBufferDesc stagingDesc;
    stagingDesc.usageFlags = BufferUsageFlagBits::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingDesc.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    stagingDesc.engineCreationFlags = EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE |
                                      EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER |
                                      EngineBufferCreationFlagBits::CORE_ENGINE_BUFFER_CREATION_DEFERRED_DESTROY;
    stagingDesc.byteSize = static_cast<uint32_t>(totalSize);
    RenderHandleReference stagingBuffer = gpuResourceManager.Create(stagingDesc);
    if (!stagingBuffer) {
        return {};
    }

    if (auto* base = static_cast<uint8_t*>(gpuResourceManager.MapBufferMemory(stagingBuffer)); base) {
        size_t offset = 0;
        for (const auto& seg : dataSegments) {
            if (!CloneData(base + offset, seg.size_bytes(), seg.data(), seg.size_bytes())) {
                CORE_LOG_E("CreateGpuBuffer: CloneData failed during GPU buffer creation");
                break;
            }
            offset += seg.size_bytes();
        }
    }
    gpuResourceManager.UnmapBuffer(stagingBuffer);

    BufferCopy copy {};
    copy.srcOffset = 0;
    copy.dstOffset = 0;
    copy.size = static_cast<uint32_t>(totalSize);
    staging.CopyBufferToBuffer(stagingBuffer, gpuBuffer, copy);
    return gpuBuffer;
}
} // anonymous namespace

PLUGINGS_BEGIN_NAMESPACE()

class GsEditSystem final : public IGsEditSystem, private IEcs::ComponentListener {
public:
    explicit GsEditSystem(CORE_NS::IEcs& ecs);
    ~GsEditSystem();

    // ISystem
    BASE_NS::string_view GetName() const override;
    BASE_NS::Uid GetUid() const override;

    CORE_NS::IPropertyHandle* GetProperties() override;
    const CORE_NS::IPropertyHandle* GetProperties() const override;
    void SetProperties(const CORE_NS::IPropertyHandle&) override;

    bool IsActive() const override;
    void SetActive(bool state) override;

    void Initialize() override;
    void Uninitialize() override;
    bool Update(bool frameRenderingQueued, uint64_t time, uint64_t delta) override;

    const CORE_NS::IEcs& GetECS() const override;

    void ClearEditActionStack() override;
    uint32_t PushEditAction(const BASE_NS::string& actionStr) override;
    GS::IRenderDataStoreGsEdit* GetEditDataStore() override;
    void ExportEditedModelAsync(const BASE_NS::string& path, ExportCb cb) override;
    bool IsGsExportDone() const override;
    void GetRecommended3DBox(
        BASE_NS::Math::Vec3& rayOrigin, BASE_NS::Math::Vec3& rayDir, Export3DBoxCb cb) const override;

    CORE_NS::Entity GetGsConfigEntity() const override;
    void InitDataStore() override;

    void OnComponentEvent(
        EventType type, const IComponentManager& componentManager, array_view<const Entity> entities) override;

private:
    bool active_ { true };
    CORE_NS::IEcs& ecs_;
    IGsConfigurationComponentManager& gsConfigManager_;
    RENDER_NS::IRenderContext* renderContext_ { nullptr };
    CORE_NS::IClassFactory* renderClassFactory_ { nullptr };

    refcnt_ptr<IRenderDataStoreGsEdit> dataStoreGsEdit_;

    CORE_NS::Entity gsConfigEntity_;
};

GsEditSystem::GsEditSystem(IEcs& ecs) : ecs_(ecs), gsConfigManager_(*GetManager<IGsConfigurationComponentManager>(ecs))
{
    Initialize();
    IClassRegister* classRegister = ecs.GetClassFactory().GetInterface<IClassRegister>();
    if (classRegister) {
        renderContext_ = GetInstance<IRenderContext>(*classRegister, UID_RENDER_CONTEXT);
    }
    if (renderContext_) {
        renderClassFactory_ = renderContext_->GetInterface<IClassFactory>();
    }
}

void GsEditSystem::Initialize()
{
    ecs_.AddListener(gsConfigManager_, *this);
}

void GsEditSystem::Uninitialize()
{
    ecs_.RemoveListener(gsConfigManager_, *this);
}

GsEditSystem::~GsEditSystem()
{
    Uninitialize();
}

void GsEditSystem::SetActive(bool state)
{
    active_ = state;
}

bool GsEditSystem::IsActive() const
{
    return active_;
}

string_view GsEditSystem::GetName() const
{
    return GS_NS::GetName(this);
}

Uid GsEditSystem::GetUid() const
{
    return UID;
}

IPropertyHandle* GsEditSystem::GetProperties()
{
    return nullptr;
}

const IPropertyHandle* GsEditSystem::GetProperties() const
{
    return nullptr;
}

void GsEditSystem::SetProperties(const IPropertyHandle& dataHandle) {}

const IEcs& GsEditSystem::GetECS() const
{
    return ecs_;
}

void GsEditSystem::ClearEditActionStack()
{
    if (!dataStoreGsEdit_) {
        return;
    }

    dataStoreGsEdit_->ClearEditActionStack();
}

GS::IRenderDataStoreGsEdit* GsEditSystem::GetEditDataStore()
{
    return dataStoreGsEdit_.get();
}

void GsEditSystem::ExportEditedModelAsync(const BASE_NS::string& path, ExportCb cb)
{
    if (!dataStoreGsEdit_) {
        CORE_LOG_E("GsEditSystem::ExportEditedModelAsync: Tried to export model without edit being enabled!");
        return;
    }
    dataStoreGsEdit_->SetExportModelCallback(cb);
    dataStoreGsEdit_->SetExportModelPath(path);
    dataStoreGsEdit_->SetExportModelFlag(true);
}

bool GsEditSystem::IsGsExportDone() const
{
    if (!dataStoreGsEdit_) {
        return false;
    }

    return dataStoreGsEdit_->IsGsExportDone();
}

void GsEditSystem::GetRecommended3DBox(
    BASE_NS::Math::Vec3& rayOrigin, BASE_NS::Math::Vec3& rayDir, Export3DBoxCb cb) const
{
    if (!dataStoreGsEdit_) {
        if (cb) {
            cb(false, "Failed to get recommended 3D box: Edit system is destroyed!", {});
        }
        return;
    }
    dataStoreGsEdit_->SetRecommended3DBoxFlag(true);
    dataStoreGsEdit_->SetRecommended3DBoxRay(rayOrigin, rayDir, std::move(cb));
}

uint32_t GsEditSystem::PushEditAction(const BASE_NS::string& actionStr)
{

    if (!dataStoreGsEdit_) {
        return 0;
    }
    if (!renderContext_) {
        return 0;
    }

    const GS_EDIT::GsEditActionParseResult editAction = GS_EDIT::ParseGsEditActionString(actionStr);
    if (editAction.editAction.has_value()) {
        auto action = editAction.editAction.value();
        if (editAction.editAction.value().type == GsEditActionType::SelectByMask) {
            auto staging =
                static_cast<IRenderDataStoreDefaultStaging*>(renderContext_->GetRenderDataStoreManager()
                                                                 .GetRenderDataStore(RENDER_DATA_STORE_DEFAULT_STAGING)
                                                                 .get());
            if (!staging) {
                CORE_LOG_E("GsEditSystem::PushEditAction: Failed to get the pointer of dsStaging");
                return 0;
            }
            auto ptr = static_pointer_cast<GS_EDIT_PARAMS::SelectByMask>(action.params);
            if (ptr->height == 0 || ptr->rowSize == 0 || ptr->height > UINT64_MAX / ptr->rowSize) {
                CORE_LOG_E("CreateGpuResource: image dimensions overflow");
                return 0;
            }
            ptr->image = CreateGpuResource(renderContext_->GetDevice().GetGpuResourceManager(), *staging, ptr->width,
                ptr->height, ptr->rowSize, ptr->format, ptr->formatByteSize,
                { reinterpret_cast<uint8_t*>(ptr->data), ptr->height * ptr->rowSize });
            if (!ptr->image) {
                CORE_LOG_E("GPU resource create failed");
                return 0;
            }
        } else if (editAction.editAction.value().type == GsEditActionType::SelectByIndex) {
            auto staging =
                static_cast<IRenderDataStoreDefaultStaging*>(renderContext_->GetRenderDataStoreManager()
                                                                 .GetRenderDataStore(RENDER_DATA_STORE_DEFAULT_STAGING)
                                                                 .get());
            auto ptr = static_pointer_cast<GS_EDIT_PARAMS::SelectByIndex>(action.params);
            if (!staging || !ptr) {
                CORE_LOG_E("GsEditSystem::PushEditAction: Failed to get the pointer of dsStaging");
                return 0;
            }
            if (ptr->bufSize == 0 || ptr->bufSize > SIZE_MAX / sizeof(uint32_t)) {
                CORE_LOG_E("CreateGpuBuffer: bufSize overflow");
                return 0;
            }
            uint32_t bufSize = static_cast<uint32_t>(ptr->bufSize);
            ptr->buffer = CreateGpuBuffer(renderContext_->GetDevice().GetGpuResourceManager(), *staging,
                { { reinterpret_cast<uint8_t*>(&bufSize), sizeof(uint32_t) },
                    { reinterpret_cast<uint8_t*>(ptr->indices), ptr->bufSize * sizeof(uint32_t) } });
            if (!ptr->buffer) {
                CORE_LOG_E("GPU buffer create failed");
                return 0;
            }
        }
        const uint32_t result = dataStoreGsEdit_->PushGsEditAction(std::move(action));
        return result;
    } else if (editAction.isPopAction) {
        // Undo semantics: FloaterRemoval marker on top means the pipeline
        // completed. The stack below the marker is [SelectByIndex x N,
        // RemoveSelected] where N >= 1 (multi-pass accumulation pushes one
        // SelectByIndex per call before a single RemoveSelected). The
        // zero-floater case has only the marker (no RemoveSelected below).
        // Undo must pop the marker, RemoveSelected, and ALL consecutive
        // SelectByIndex entries to fully revert the floater removal.
        const auto& editStack = dataStoreGsEdit_->GetEditActionStack();
        if (!editStack.empty()) {
            const auto& topAction = editStack.back();
            if (topAction.type == GsEditActionType::FloaterRemoval) {
                const size_t stackSize = editStack.size();
                if (stackSize >= 2u &&
                    editStack[stackSize - 2u].type == GsEditActionType::RemoveSelected) {
                    (void)dataStoreGsEdit_->PopGsEditAction(); // marker
                    (void)dataStoreGsEdit_->PopGsEditAction(); // RemoveSelected
                    // Pop all consecutive SelectByIndex entries.
                    while (!dataStoreGsEdit_->GetEditActionStack().empty()) {
                        const auto& s = dataStoreGsEdit_->GetEditActionStack();
                        if (!s.empty() && s.back().type == GsEditActionType::SelectByIndex) {
                            (void)dataStoreGsEdit_->PopGsEditAction();
                        } else {
                            break;
                        }
                    }
                    return static_cast<uint32_t>(dataStoreGsEdit_->GetEditActionStack().size());
                }
                return dataStoreGsEdit_->PopGsEditAction();
            }
        }
        return dataStoreGsEdit_->PopGsEditAction();
    } else {
        CORE_LOG_E("GsEditSystem::PushEditAction: Invalid command: %s", actionStr.c_str());
        return 0xFFFFffff;
    }
}

bool GsEditSystem::Update(bool frameRenderingQueued, uint64_t /* time */, uint64_t delta)
{
    if (!active_) {
        CORE_LOG_E("GsEditSystem::Update: inActive");
        return false;
    }
    if (!renderContext_ || !renderClassFactory_) {
        CORE_LOG_E("GsEditSystem::Update: Empty RenderContext");
        return false;
    }

    const ScopedHandle<const GsConfigurationComponent> gcc = gsConfigManager_.Read(gsConfigEntity_);
    if (!gcc) {
        return false;
    }

    if (gcc->enableGsEditor) {
        // create the edit data store
        if (!dataStoreGsEdit_) {
            if (auto render = GetSystem<IRenderPreprocessorSystem>(ecs_)) {
                if (auto propertyHandle =
                        ScopedHandle<const IRenderPreprocessorSystem::Properties>(render->GetProperties())) {
                    dataStoreGsEdit_ = refcnt_ptr<IRenderDataStoreGsEdit>(
                        renderContext_->GetRenderDataStoreManager().Create(IRenderDataStoreGsEdit::UID,
                            (propertyHandle->dataStorePrefix + GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).data()));
                }
            }
            if (!dataStoreGsEdit_) {
                CORE_LOG_E("Could not get or create DataStoreGsEdit!");
                return false;
            }
        }

        dataStoreGsEdit_->SetEditState(true);
    } else {
        if (dataStoreGsEdit_) {
            dataStoreGsEdit_->SetEditState(false);
        }
    }

    return frameRenderingQueued;
}

CORE_NS::Entity GsEditSystem::GetGsConfigEntity() const
{
    return gsConfigEntity_;
}

void GsEditSystem::InitDataStore()
{
    if (!dataStoreGsEdit_) {
        if (auto render = GetSystem<IRenderPreprocessorSystem>(ecs_)) {
            if (auto propertyHandle =
                    ScopedHandle<const IRenderPreprocessorSystem::Properties>(render->GetProperties())) {
                dataStoreGsEdit_ = refcnt_ptr<IRenderDataStoreGsEdit>(
                    renderContext_->GetRenderDataStoreManager().Create(IRenderDataStoreGsEdit::UID,
                        (propertyHandle->dataStorePrefix + GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).data()));
            }
        }
        if (!dataStoreGsEdit_) {
            CORE_LOG_E("Could not get or create DataStoreGsEdit!");
            return;
        }
    }
    return;
}

void GsEditSystem::OnComponentEvent(
    EventType type, const IComponentManager& componentManager, array_view<const Entity> entities)
{
    switch (type) {
        case EventType::CREATED:
            if (EntityUtil::IsValid(gsConfigEntity_)) {
                CORE_LOG_E("GsEditSystem::OnComponentEvent: GsConfigurationComponent already set! "
                           "Only one can exist!");
            } else {
                gsConfigEntity_ = entities.at(0);
            }
            break;
        case EventType::MODIFIED:
            break;
        case EventType::DESTROYED:
            gsConfigEntity_ = {};
            break;
        case EventType::MOVED:
            break;
    }
}

PLUGINGS_END_NAMESPACE()

namespace GS {
CORE_NS::ISystem* IGsEditSystemInstance(IEcs& ecs)
{
    return new GsEditSystem(ecs);
}
void IGsEditSystemDestroy(CORE_NS::ISystem* instance)
{
    delete static_cast<GsEditSystem*>(instance);
}
} // namespace GS
