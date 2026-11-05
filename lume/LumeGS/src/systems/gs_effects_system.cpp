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
#include <plugin_gs/ecs/components/gs_effect_component.h>
#include <plugin_gs/ecs/systems/intf_gs_effect_system.h>

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
#include <render/datastore/intf_render_data_store_render_post_processes.h>
#include <render/device/gpu_resource_desc.h>
#include <render/device/intf_device.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/implementation_uids.h>
#include <render/intf_render_context.h>
#include <render/intf_renderer.h>
#include <render/nodecontext/intf_render_node_graph_manager.h>
#include <render/property/property_types.h>

#include "render/coloredit/render_effects_coloredit_node.h"
#include "render/comic/render_effects_comic_node.h"
#include "render/obradinn/render_effects_obradinn_node.h"
#include "render/retro/render_effects_retro_node.h"
#include "render/watercolor/render_effects_watercolor_node.h"

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;
using namespace GS_NS;

PLUGINGS_BEGIN_NAMESPACE()

constexpr string_view GS_RDS_POST_PROCESSES { "RenderDataStoreGsRenderEffects" };
constexpr string_view GS_POST_PROCESSES_NAME { "GsRenderEffectsName" };

class GsEffectSystem final : public IGsEffectSystem, private IEcs::ComponentListener {
public:
    explicit GsEffectSystem(CORE_NS::IEcs& ecs);
    ~GsEffectSystem();

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

    void OnComponentEvent(
        EventType type, const IComponentManager& componentManager, array_view<const Entity> entities) override;

private:
    bool active_ { true };
    CORE_NS::IEcs& ecs_;
    RENDER_NS::IRenderContext* renderContext_ { nullptr };
    CORE_NS::IClassFactory* renderClassFactory_ { nullptr };

    IGsEffectComponentManager& gsEffectComponentManager_;

    BASE_NS::vector<Entity> addedGsComponents_;
    BASE_NS::vector<Entity> activeGsEffectComponents_;
    BASE_NS::vector<Entity> removedGsComponents_;

    refcnt_ptr<IRenderDataStoreRenderPostProcesses> rdsPostProcesses_;

    IRenderPostProcess::Ptr rppRetro8_;
    IRenderPostProcess::Ptr rppComic_;
    IRenderPostProcess::Ptr rppWatercolor_;
    IRenderPostProcess::Ptr rppObraDinn_;
    IRenderPostProcess::Ptr rppColorEdit_;

    bool isInjected_ { false };

    enum EnabledEffect { None, Retro, Comic, Watercolor, Obradinn, Coloredit };
    EnabledEffect enabledEffect_ { EnabledEffect::None };
};

GsEffectSystem::GsEffectSystem(IEcs& ecs)
    : ecs_(ecs), gsEffectComponentManager_(*GetManager<IGsEffectComponentManager>(ecs))
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

void GsEffectSystem::Initialize()
{
    ecs_.AddListener(gsEffectComponentManager_, *this);
}

void GsEffectSystem::Uninitialize()
{
    ecs_.RemoveListener(gsEffectComponentManager_, *this);
}

GsEffectSystem::~GsEffectSystem()
{
    Uninitialize();
}

void GsEffectSystem::SetActive(bool state)
{
    active_ = state;
}

bool GsEffectSystem::IsActive() const
{
    return active_;
}

string_view GsEffectSystem::GetName() const
{
    return GS_NS::GetName(this);
}

Uid GsEffectSystem::GetUid() const
{
    return UID;
}

IPropertyHandle* GsEffectSystem::GetProperties()
{
    return nullptr;
}

const IPropertyHandle* GsEffectSystem::GetProperties() const
{
    return nullptr;
}

void GsEffectSystem::SetProperties(const IPropertyHandle& dataHandle) {}

const IEcs& GsEffectSystem::GetECS() const
{
    return ecs_;
}

refcnt_ptr<IRenderDataStoreRenderPostProcesses> CreateRenderDataStore(
    IRenderDataStoreManager& rdsMgr, IEcs& ecs, IRenderContext* renderContext)
{
    refcnt_ptr<IRenderDataStoreRenderPostProcesses> renderDataStore;

    if (auto render = GetSystem<IRenderPreprocessorSystem>(ecs)) {
        if (auto propertyHandle = ScopedHandle<const IRenderPreprocessorSystem::Properties>(render->GetProperties())) {
            renderDataStore = refcnt_ptr<IRenderDataStoreRenderPostProcesses>(
                renderContext->GetRenderDataStoreManager().Create(IRenderDataStoreRenderPostProcesses::UID,
                    (propertyHandle->dataStorePrefix + GS_RDS_POST_PROCESSES).data()));
        }
    }
    if (!renderDataStore) {
        CORE_LOG_E("Could not get or create GS Effect Data Store!");
    }

    CORE_ASSERT(renderDataStore);
    return renderDataStore;
}

bool GsEffectSystem::Update(bool frameRenderingQueued, uint64_t /* time */, uint64_t delta)
{
    if (!active_) {
        return false;
    }
    if (!renderContext_ || !renderClassFactory_) {
        return false;
    }

    if (!rdsPostProcesses_) {
        rdsPostProcesses_ = CreateRenderDataStore(renderContext_->GetRenderDataStoreManager(), ecs_, renderContext_);
    }

    // remove components
    for (auto& entity : removedGsComponents_) {
        const auto toErase = std::find(activeGsEffectComponents_.begin(), activeGsEffectComponents_.end(), entity);

        if (toErase != activeGsEffectComponents_.end()) {
            activeGsEffectComponents_.erase(toErase);
        }
    }
    removedGsComponents_.clear();

    // add new components, create interfaces if not exist, etc. Can only be created once.
    for (auto& entity : addedGsComponents_) {
        if (!rppRetro8_) {
            rppRetro8_ =
                CreateInstance<IRenderPostProcess>(*renderClassFactory_, GsEffects::RenderPostProcessRetroNode::UID);
        }
        if (!rppComic_) {
            rppComic_ =
                CreateInstance<IRenderPostProcess>(*renderClassFactory_, GsEffects::RenderPostProcessComicNode::UID);
        }
        if (!rppWatercolor_) {
            rppWatercolor_ = CreateInstance<IRenderPostProcess>(
                *renderClassFactory_, GsEffects::RenderPostProcessWatercolorNode::UID);
        }
        if (!rppObraDinn_) {
            rppObraDinn_ =
                CreateInstance<IRenderPostProcess>(*renderClassFactory_, GsEffects::RenderPostProcessObraDinnNode::UID);
        }
        if (!rppColorEdit_) {
            rppColorEdit_ = CreateInstance<IRenderPostProcess>(
                *renderClassFactory_, GsEffects::RenderPostProcessColorEditNode::UID);
        }

        // inject
        if (!isInjected_) {
            isInjected_ = true;

            const BASE_NS::string dsName = BASE_NS::string(rdsPostProcesses_->GetName());
            const BASE_NS::string dsTypeName = BASE_NS::string(rdsPostProcesses_->GetTypeName());

            // construct the post-processing effect RNG
            auto& renderNodeGraphMgr = renderContext_->GetRenderNodeGraphManager();

            json::standalone_value jsonVal(json::standalone_value::object {});
            jsonVal["typeName"] = "RenderNodeDefaultCameraPostProcessInterfaceController";
            jsonVal["nodeName"] = "RenderNodeGsRenderPostProcesses";

            json::standalone_value renderDataStore(json::standalone_value::object {});
            renderDataStore["dataStoreName"] = dsName;
            renderDataStore["configurationName"] = "GsRenderEffectsName";
            renderDataStore["typeName"] = dsTypeName;

            jsonVal["renderDataStore"] = renderDataStore;

            const auto insertDesc = RenderNodeDesc { "RenderNodeDefaultCameraPostProcessInterfaceController",
                "RN_CAMERA_POST_PROCESS_INTERFACE_CTRL", {}, to_string(jsonVal) };
            RenderNodeDependency dependencies[] = { RenderNodeDependency {
                "RenderNodeDefaultCameraPostProcessController", RenderNodeDependency ::Position::AFTER_LAST } };
            renderNodeGraphMgr.AddRenderNodeInsertion(insertDesc, dependencies);
        }

        activeGsEffectComponents_.push_back(entity);
    }
    addedGsComponents_.clear();

    // update parameters
    for (const auto& entity : activeGsEffectComponents_) {
        ScopedHandle<const GsEffectComponent> dfc = gsEffectComponentManager_.Read(entity);

        // Check if any effect is enabled. Priority order.
        if (dfc->retroEffectEnabled) {
            enabledEffect_ = EnabledEffect::Retro;
        } else if (dfc->comicEffectEnabled) {
            enabledEffect_ = EnabledEffect::Comic;
        } else if (dfc->watercolorEffectEnabled) {
            enabledEffect_ = EnabledEffect::Watercolor;
        } else if (dfc->obradinnEffectEnabled) {
            enabledEffect_ = EnabledEffect::Obradinn;
        } else if (dfc->colorEditingEnabled) {
            enabledEffect_ = EnabledEffect::Coloredit;
        } else {
            enabledEffect_ = EnabledEffect::None;
        }

        BASE_NS::vector<IRenderDataStoreRenderPostProcesses::PostProcessData> ppd;
        auto retroProps = rppRetro8_->GetProperties();
        auto comicProps = rppComic_->GetProperties();
        auto watercolorProps = rppWatercolor_->GetProperties();
        auto obradinnProps = rppObraDinn_->GetProperties();
        auto coloreditProps = rppColorEdit_->GetProperties();

        CORE_NS::SetPropertyValue(retroProps, "enabled", enabledEffect_ == EnabledEffect::Retro);
        CORE_NS::SetPropertyValue(comicProps, "enabled", enabledEffect_ == EnabledEffect::Comic);
        CORE_NS::SetPropertyValue(watercolorProps, "enabled", enabledEffect_ == EnabledEffect::Watercolor);
        CORE_NS::SetPropertyValue(obradinnProps, "enabled", enabledEffect_ == EnabledEffect::Obradinn);
        CORE_NS::SetPropertyValue(coloreditProps, "enabled", enabledEffect_ == EnabledEffect::Coloredit);

        switch (enabledEffect_) {
            case EnabledEffect::Retro:
                CORE_NS::SetPropertyValue(retroProps, "colorNum", dfc->retroEffectColorNum);
                CORE_NS::SetPropertyValue(retroProps, "pixelSize", dfc->retroEffectPixelSize);
                CORE_NS::SetPropertyValue(retroProps, "blendEnabled", dfc->retroEffectBlending);
                CORE_NS::SetPropertyValue(retroProps, "curve", dfc->retroEffectCurve);

                ppd.push_back({ 0u, rppRetro8_ });
                break;
            case EnabledEffect::Comic:
                CORE_NS::SetPropertyValue(comicProps, "lineThreshold", dfc->comicEffectLineThreshold);
                CORE_NS::SetPropertyValue(comicProps, "lineColor", dfc->comicEffectLineColor);

                ppd.push_back({ 1u, rppComic_ });
                break;
            case EnabledEffect::Watercolor:
                CORE_NS::SetPropertyValue(watercolorProps, "radius", dfc->watercolorEffectRadius);
                CORE_NS::SetPropertyValue(watercolorProps, "threshold", dfc->watercolorEffectThreshold);

                ppd.push_back({ 2u, rppWatercolor_ });
                break;
            case EnabledEffect::Obradinn:
                CORE_NS::SetPropertyValue(obradinnProps, "noiseStrength", dfc->obradinnEffectNoiseStrength);
                CORE_NS::SetPropertyValue(obradinnProps, "threshold", dfc->obradinnEffectThreshold);
                CORE_NS::SetPropertyValue(obradinnProps, "foregroundColor", dfc->obradinnEffectForegroundColor);
                CORE_NS::SetPropertyValue(obradinnProps, "backgroundColor", dfc->obradinnEffectBackgroundColor);

                ppd.push_back({ 3u, rppObraDinn_ });
                break;
            case EnabledEffect::Coloredit:
                CORE_NS::SetPropertyValue(coloreditProps, "exposure", dfc->colorEditingExposure);
                CORE_NS::SetPropertyValue(coloreditProps, "contrast", dfc->colorEditingContrast);
                CORE_NS::SetPropertyValue(coloreditProps, "temperature", dfc->colorEditingTemperature);
                CORE_NS::SetPropertyValue(coloreditProps, "tint", dfc->colorEditingTint);
                CORE_NS::SetPropertyValue(coloreditProps, "saturation", dfc->colorEditingSaturation);
                CORE_NS::SetPropertyValue(coloreditProps, "vibrance", dfc->colorEditingVibrance);

                ppd.push_back({ 4u, rppColorEdit_ });
                break;
            default:
                break;
        }

        rdsPostProcesses_->AddData(GS_POST_PROCESSES_NAME, ppd);

        // do not loop more than once
        break;
    }

    return false;
}

void GsEffectSystem::OnComponentEvent(
    EventType type, const IComponentManager& componentManager, array_view<const Entity> entities)
{
    switch (type) {
        case EventType::CREATED:
            if (activeGsEffectComponents_.empty()) {
                addedGsComponents_.append(entities.cbegin(), entities.cend());
            } else {
                CORE_LOG_W("Trying to add a new GsEffectComponent even though one already exists, aborting!");
            }
            break;

        case EventType::MODIFIED:
            break;

        case EventType::DESTROYED:
            removedGsComponents_.append(entities.cbegin(), entities.cend());
            break;

        case EventType::MOVED:
            break;
    }
}

PLUGINGS_END_NAMESPACE()

namespace GS {
CORE_NS::ISystem* IGsEffectSystemInstance(IEcs& ecs)
{
    return new GsEffectSystem(ecs);
}
void IGsEffectSystemDestroy(CORE_NS::ISystem* instance)
{
    delete static_cast<GsEffectSystem*>(instance);
}
} // namespace GS