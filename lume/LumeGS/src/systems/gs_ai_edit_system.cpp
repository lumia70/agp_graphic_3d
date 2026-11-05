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
#include <plugin_gs/ecs/systems/intf_gs_ai_edit_system.h>
#include <plugin_gs/render/intf_render_data_store_gs_ai_edit.h>

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
#include <plugin_gs/render/gs_ai_edit_factory.h>
#include <plugin_gs/render/gs_ai_edit_error_code.h>

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;
using namespace GS_NS;

PLUGINGS_BEGIN_NAMESPACE()

class GsAIEditSystem final : public IGsAIEditSystem, private IEcs::ComponentListener {
public:
    explicit GsAIEditSystem(CORE_NS::IEcs& ecs);
    ~GsAIEditSystem();

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

    std::future<int> ExtractMainBody(CORE_NS::Entity entity, float X, float Y) override
    {
        auto promise = std::make_shared<std::promise<int>>();
        auto future = promise->get_future();
        if (!GS::GsAIEditFactory::IsLoaded()) {
            promise->set_value(static_cast<int>(GS::GSAIEditErrorCode::ERROR_EXTRACT_FAILED));
            return future;
        }
        if (dataStoreGsAIEdit_ == nullptr) {
            CORE_LOG_E("GsAIEditSystem::ExtractMainBody: dataStoreGsAIEdit_ is nullptr");
            promise->set_value(static_cast<int>(GS::GSAIEditErrorCode::ERROR_DATA_PARSING_FAILED));
            return future;
        }

        dataStoreGsAIEdit_->PushOperation({nullptr, nullptr, promise, entity, X, Y, {}, {}, GsAIEditOpType::Extract});
        return future;
    }

    virtual std::future<std::vector<BASE_NS::Math::UVec4>> GetMainBodyPreviewData(
        CORE_NS::Entity entity, float X, float Y) override
    {
        auto promise = std::make_shared<std::promise<std::vector<BASE_NS::Math::UVec4>>>();
        auto future = promise->get_future();
        if (!GS::GsAIEditFactory::IsLoaded()) {
            promise->set_value({});
            return future;
        }
        if (dataStoreGsAIEdit_ == nullptr) {
            promise->set_value({});
            return future;
        }

        dataStoreGsAIEdit_->PushOperation({promise, nullptr, nullptr, entity, X, Y, {}, {}, GsAIEditOpType::Preview});
        return future;
    }

    virtual void ConfirmExtractMainBody(CORE_NS::Entity entity) override
    {
        if (!GS::GsAIEditFactory::IsLoaded()) {
            return;
        }
        if (dataStoreGsAIEdit_ == nullptr) {
            CORE_LOG_E("GsAIEditSystem::ConfirmExtractMainBody: dataStoreGsAIEdit_ is nullptr");
            return;
        }
       
        dataStoreGsAIEdit_->PushOperation({nullptr, nullptr, nullptr, entity, 0.f, 0.f, {}, {},
            GsAIEditOpType::Confirm});
    }

    virtual void UndoExtractMainBody(CORE_NS::Entity entity) override
    {
        if (!GS::GsAIEditFactory::IsLoaded()) {
            return;
        }
        if (dataStoreGsAIEdit_ == nullptr) {
            CORE_LOG_E("GsAIEditSystem::UndoExtractMainBody: dataStoreGsAIEdit_ is nullptr");
            return;
        }

        dataStoreGsAIEdit_->PushOperation({nullptr, nullptr, nullptr, entity, 0.f, 0.f, {}, {}, GsAIEditOpType::Undo});
    }

    virtual std::vector<BASE_NS::Math::Vec3> GetRecommended3DBox(
        CORE_NS::Entity entity, BASE_NS::Math::Vec3 ori, BASE_NS::Math::Vec3 dir) override
    {
        if (!GS::GsAIEditFactory::IsLoaded()) {
            CORE_LOG_E("GsAIEditSystem::GetRecommended3DBox: GsAIEditFactory is not loaded");
            return {};
        }

        return GS::GsAIEditFactory::ComputeRecommendedBox(ecs_, entity, ori, dir);
    }

    void OnComponentEvent(
        EventType type, const IComponentManager& componentManager, array_view<const Entity> entities) override;

private:
    bool active_ { true };
    CORE_NS::IEcs& ecs_;
    IGsConfigurationComponentManager& gsConfigManager_;
    RENDER_NS::IRenderContext* renderContext_ { nullptr };
    CORE_NS::IClassFactory* renderClassFactory_ { nullptr };

    refcnt_ptr<IRenderDataStoreGsAIEdit> dataStoreGsAIEdit_;

    Entity gsConfigEntity_;
};

GsAIEditSystem::GsAIEditSystem(IEcs& ecs) : ecs_(ecs),
                                            gsConfigManager_(*GetManager<IGsConfigurationComponentManager>(ecs))
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

void GsAIEditSystem::Initialize()
{
    ecs_.AddListener(gsConfigManager_, *this);
}

void GsAIEditSystem::Uninitialize()
{
    ecs_.RemoveListener(gsConfigManager_, *this);
}

GsAIEditSystem::~GsAIEditSystem()
{
    Uninitialize();
}

void GsAIEditSystem::SetActive(bool state)
{
    active_ = state;
}

bool GsAIEditSystem::IsActive() const
{
    return active_;
}

string_view GsAIEditSystem::GetName() const
{
    return GS_NS::GetName(this);
}

Uid GsAIEditSystem::GetUid() const
{
    return UID;
}

IPropertyHandle* GsAIEditSystem::GetProperties()
{
    return nullptr;
}

const IPropertyHandle* GsAIEditSystem::GetProperties() const
{
    return nullptr;
}

void GsAIEditSystem::SetProperties(const IPropertyHandle& dataHandle) {}

const IEcs& GsAIEditSystem::GetECS() const
{
    return ecs_;
}


bool GsAIEditSystem::Update(bool frameRenderingQueued, uint64_t /* time */, uint64_t delta)
{
    if (!active_) {
        return false;
    }
    if (!renderContext_ || !renderClassFactory_) {
        CORE_LOG_E("GsAIEditSystem::Update: Empty RenderContext");
        return false;
    }

    // create the edit data store
    auto render = GetSystem<IRenderPreprocessorSystem>(ecs_);
    auto propertyHandle = ScopedHandle<const IRenderPreprocessorSystem::Properties>(render->GetProperties());
    if (!dataStoreGsAIEdit_ && render && propertyHandle) {
        dataStoreGsAIEdit_ = refcnt_ptr<IRenderDataStoreGsAIEdit>(
            renderContext_->GetRenderDataStoreManager().Create(IRenderDataStoreGsAIEdit::UID,
                (propertyHandle->dataStorePrefix + GS_AI_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).data()));
        CORE_LOG_V("create Ai edit data store: %s", (propertyHandle->dataStorePrefix +
            GS_AI_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).data());

        if (!dataStoreGsAIEdit_) {
            CORE_LOG_E("Could not get or create DataStoreGsAIEdit!");
            return false;
        }
        dataStoreGsAIEdit_->setECS(ecs_);
    }

    return frameRenderingQueued;
}

void GsAIEditSystem::OnComponentEvent(
    EventType type, const IComponentManager& componentManager, array_view<const Entity> entities)
{
    switch (type) {
        case EventType::CREATED:
            if (EntityUtil::IsValid(gsConfigEntity_)) {
                CORE_LOG_E("GsAIEditSystem::OnComponentEvent: "
                           "GsConfigurationComponent already set! Only one can exist!");
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
CORE_NS::ISystem* IGsAIEditSystemInstance(IEcs& ecs)
{
    return new GsAIEditSystem(ecs);
}
void IGsAIEditSystemDestroy(CORE_NS::ISystem* instance)
{
    delete static_cast<GsAIEditSystem*>(instance);
}
} // namespace GS