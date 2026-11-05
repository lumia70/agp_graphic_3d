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

#include "plugin_gs/ecs/systems/gs_system.h"

#define JSON_IMPL

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <numeric>
#include <plugin_gs/implementation_uids.h>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <plugin_gs/util/intf_gs_picking.h>
#include <random>
#include <thread>

#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/environment_component.h>
#include <3d/ecs/components/layer_component.h>
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
#include <base/containers/array_view.h>
#include <base/containers/byte_array.h>
#include <base/containers/fixed_string.h>
#include <base/containers/pair.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/float_packer.h>
#include <base/math/matrix_util.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/json/json.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/plugin/intf_class_factory.h>
#include <core/property/intf_property_api.h>
#include <core/property/intf_property_handle.h>
#include <core/property/property_handle_util.h>
#include <core/property_tools/property_api_impl.h>
#include <core/property_tools/property_api_impl.inl>
#include <core/property_tools/property_macros.h>
#include <core/util/parallel_sort.h>
#include <render/datastore/intf_render_data_store_default_staging.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/datastore/intf_render_data_store_render_post_processes.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/implementation_uids.h>

#include "plugin_gs/ecs/components/gs_component.h"
#include "plugin_gs/ecs/components/gs_configuration_component.h"
#include "render/render_data_store_default_gs.h"
#include "util/gs_loading.h"

#if RENDER_HAS_VULKAN_BACKEND
#include "xengine_loader.h"
#endif
namespace {
#include "app/shaders/common/gs_common.h"
#include "app/shaders/common/gs_struct_common.h"
} // namespace

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;
using namespace GS;

namespace {

const string_view RENDER_DATA_STORE_DEFAULT_STAGING { "RenderDataStoreDefaultStaging" };

class GsSystem final : public IGsSystem, private IEcs::ComponentListener {
public:
    explicit GsSystem(IEcs& aECS);
    ~GsSystem();

    // ISystem
    string_view GetName() const override;
    Uid GetUid() const override;

    IPropertyHandle* GetProperties() override;
    const IPropertyHandle* GetProperties() const override;
    void SetProperties(const IPropertyHandle&) override;

    bool IsActive() const override;
    void SetActive(bool state) override;

    void Initialize() override;
    void Uninitialize() override;
    bool Update(bool frameRenderingQueued, uint64_t time, uint64_t delta) override;

    const IEcs& GetECS() const override;

    // Listener
    void OnComponentEvent(
        EventType type, const IComponentManager& componentManager, array_view<const Entity> entities) override;

private:
    void BuildCpuRaycastAccelerationStructure() const;

    void RemoveGsObject(const Entity entity, const GS::GsLoadContext loadCtx, GsRenderData* renderData,
        const bool recreateBuffers = true);

    void UpdateCustomAnimationShader(const GsConfigurationComponent* gcc, GsRenderData* renderData);
    void UpdateCustomResources(const GsConfigurationComponent* gcc, GsRenderData* renderData);
    void UpdateAnimationState(const GsConfigurationComponent* gcc, GsRenderData* renderData);
    void UpdateRenderFlags(const GsConfigurationComponent* gcc, GsRenderData* renderData);
    void UpdateConfigurationData();

    GS::GsLoadContext GetLoadContext();

    BASE_NS::fixed_string<21u> GetUniqueName() const
    {
        return BASE_NS::to_hex((uintptr_t)this);
    }

    struct PropertyData {
        float speed { 1.f };
    };

    PROPERTY_LIST(PropertyData, ComponentMetadata, MEMBER_PROPERTY(speed, "Simulation speed", 0))

    bool active_ { true };
    IEcs& ecs_;
    RENDER_NS::IRenderContext* renderContext_ { nullptr };
    IWorldMatrixComponentManager& wmcm_;
    ILayerComponentManager& lcm_;

    IGsComponentManager& gsManager_;
    IGsConfigurationComponentManager& gsConfigManager_;
    IRenderHandleComponentManager& rhcManager_;
    IRenderMeshComponentManager& renderMeshManager_;

    CORE_NS::IThreadPool::Ptr threadPool_;

    PropertyData props_ { 1.f };

    PropertyApiImpl<PropertyData> propertyApi_ = PropertyApiImpl<PropertyData>(&props_, ComponentMetadata);
    refcnt_ptr<IRenderDataStoreDefaultGs> dataStoreGs_;

    vector<Entity> addedGsComponents_;
    BASE_NS::vector<GsPrimitive> loadedPrimitives_;
    vector<Entity> removedGsComponents_;

    IRenderDataStoreDefaultStaging* dsStaging_ { nullptr };

    // used to keep track of recreated buffers
    std::shared_ptr<uint32_t> multiGsBufferIndexCounter { nullptr };

    std::thread bgWorker_;
    std::atomic_bool bgWorkerDone_ { true };
    std::atomic_bool bgWorkerExitedSuccesfully_ { false };
    std::atomic_bool shuttingDown_ { false };

    std::mutex entityTrackingMtx_;
    BASE_NS::vector<Entity> entitiesBeingLoaded_;

    CORE_NS::Entity gsConfigEntity_;

    CORE_NS::Entity renderTargetHostEntity_;

    BASE_NS::Math::UVec2 previousResolution_ { 0, 0 };

    BASE_NS::Math::UVec2 renderResolutionFromCC_;

    // Cache for XEG Spatial Upscale platform capability (false if platform doesn't support)
    bool xegPlatformSupported_ { false };

    // Track GPU sort flag transitions to trigger a reload of loaded GS assets.
    // GPU sort buffers are only allocated at load time (gs_loading.cpp); toggling
    // useGpuSort/use24BitGpuSort at runtime without a reload leaves the buffers
    // empty, which makes RenderNodeGsRender::IsValid() fail every frame and
    // freezes the editor preview.
    bool prevUseGpuSort_ { false };
    bool prevUse24BitGpuSort_ { false };
};

GsSystem::GsSystem(IEcs& ecs)
    : ecs_(ecs), wmcm_(*GetManager<IWorldMatrixComponentManager>(ecs)), lcm_(*GetManager<ILayerComponentManager>(ecs)),
      gsManager_(*GetManager<IGsComponentManager>(ecs)),
      gsConfigManager_(*GetManager<IGsConfigurationComponentManager>(ecs)),
      rhcManager_(*GetManager<IRenderHandleComponentManager>(ecs)),
      renderMeshManager_(*GetManager<IRenderMeshComponentManager>(ecs))
{
    IClassRegister* classRegister = ecs.GetClassFactory().GetInterface<IClassRegister>();
    if (classRegister) {
        renderContext_ = GetInstance<IRenderContext>(*classRegister, UID_RENDER_CONTEXT);
    }
    if (renderContext_) {
        const IRenderDataStoreManager& dsManager = renderContext_->GetRenderDataStoreManager();
        dsStaging_ = static_cast<IRenderDataStoreDefaultStaging*>(
            dsManager.GetRenderDataStore(RENDER_DATA_STORE_DEFAULT_STAGING).get());
    }

    CORE_ASSERT(dsStaging_);

    const auto factory = GetInstance<ITaskQueueFactory>(UID_TASK_QUEUE_FACTORY);
    if (factory) {
        threadPool_ =
            factory->CreateThreadPool(Math::max(static_cast<uint32_t>(std::thread::hardware_concurrency()) - 1, 1u));
    }
}

GsSystem ::~GsSystem() = default;

void GsSystem::SetActive(bool state)
{
    active_ = state;
}

bool GsSystem::IsActive() const
{
    return active_;
}

string_view GsSystem::GetName() const
{
    return GS::GetName(this);
}

Uid GsSystem::GetUid() const
{
    return UID;
}

IPropertyHandle* GsSystem::GetProperties()
{
    return propertyApi_.GetData();
}

const IPropertyHandle* GsSystem::GetProperties() const
{
    return propertyApi_.GetData();
}

void GsSystem::SetProperties(const IPropertyHandle& dataHandle)
{
    if (dataHandle.Owner() != &propertyApi_) {
        return;
    }
    if (auto data = ScopedHandle<const PropertyData>(&dataHandle); data) {
        props_ = *data;
    }
}

const IEcs& GsSystem::GetECS() const
{
    return ecs_;
}

void GsSystem::Initialize()
{
    ecs_.AddListener(gsManager_, *this);
    ecs_.AddListener(gsConfigManager_, *this);

    bgWorkerDone_ = true;
    shuttingDown_ = false;

    addedGsComponents_.clear();
    removedGsComponents_.clear();
    entitiesBeingLoaded_.clear();

    multiGsBufferIndexCounter = std::make_shared<uint32_t>(0u);

    // Detect XEG Spatial Upscale platform capability at initialization
    if (renderContext_ == nullptr) {
        xegPlatformSupported_ = false;
        CORE_LOG_E("XEG spatial upscale is disabled: renderContext is null");
    } else {
        xegPlatformSupported_ = XEngineGsFactory::IsSpatialUpscaleEnable(*renderContext_);
        if (!xegPlatformSupported_) {
            CORE_LOG_E("XEG spatial upscale is disabled: platform does not support XEG Spatial Upscale API");
        }
    }
}

void GsSystem::Uninitialize()
{
    // wait for async load thread to finish
    shuttingDown_ = true;
    if (bgWorker_.joinable()) {
        bgWorker_.join();
    }

    if (!bgWorkerDone_.load()) {
        CORE_LOG_E("GsSystem::Uninitialize: Failed to properly shutdown bg worker thread!");
    }

    // clear tracking data
    {
        std::lock_guard<std::mutex> lock(entityTrackingMtx_);
        entitiesBeingLoaded_.clear();
    }

    // clean up any loaded but not-yet-added primitives
    loadedPrimitives_.clear();

    GS::GsLoadContext ctx = GetLoadContext();

    // remove all GS objects
    if (dataStoreGs_) {
        GsRenderData* renderData = dataStoreGs_->GetGsRenderData();

        BASE_NS::vector<Entity> ownedEntities;
        ownedEntities.reserve(dataStoreGs_->GetGsPrimitives().size());
        for (const auto& prim : dataStoreGs_->GetGsPrimitives()) {
            ownedEntities.push_back(prim.splatEntity);
        }

        for (const auto& entity : ownedEntities) {
            if (!renderData) {
                continue;
            }
            RemoveGsObject(entity, ctx, renderData, false);
        }

        if (EntityUtil::IsValid(renderTargetHostEntity_) && renderData) {
            const bool hostWasOwned = std::find(ownedEntities.cbegin(), ownedEntities.cend(),
                                          renderTargetHostEntity_) != ownedEntities.cend();
            if (!hostWasOwned) {
                GS::RemoveGsRenderTargets(renderData, renderTargetHostEntity_, ctx);
                renderData->gaussianRenderTarget0 = {};
                renderData->gaussianRenderTarget1 = {};
                renderData->gaussianRenderTarget2 = {};
                renderData->gaussianRenderTarget3 = {};
                renderData->gaussianRenderTargetBB = {};
                renderData->depthStoreCacheHandle = {};
                renderData->gaussianRenderTargetDepth1 = {};
                renderData->sampler = {};
                renderData->gaussianRenderTargetStatusUbo = {};
            }

            renderTargetHostEntity_ = {};
        }

        removedGsComponents_.clear();
        addedGsComponents_.clear();

        if (renderData && dataStoreGs_->GetGsPrimitives().empty()) {
            renderData->gsMaterial = {};
        }
    }

    if (dataStoreGs_) {
        RenderDataStoreDefaultGS* dsgs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());

        // clear both render data buffers, since one may contain loaded-but-not-added data
        if (dsgs) {
            dsgs->ClearRenderData();
        }
    }

    IClassRegister* classRegister = ecs_.GetClassFactory().GetInterface<IClassRegister>();
    if (classRegister) {
        RENDER_NS::IRenderContext* renderContext = GetInstance<IRenderContext>(*classRegister, UID_RENDER_CONTEXT);

        if (renderContext) {
            IClassRegister* rcontextClassRegister = renderContext->GetInterface<IClassRegister>();

            if (rcontextClassRegister) {
                GS::IGsPicking* gsPicking = static_cast<GS::IGsPicking*>(
                    GetInstance<GS::IGsPicking>(*rcontextClassRegister, GS::UID_GS_PICKING));

                if (gsPicking) {
                    gsPicking->RemoveGsEcsSystem(ecs_);
                }
            }
        }
    }

    ecs_.RemoveListener(gsManager_, *this);
    ecs_.RemoveListener(gsConfigManager_, *this);

    dataStoreGs_ = nullptr;
}

bool GsSystem::Update(bool frameRenderingQueued, uint64_t time, uint64_t delta)
{
    frameRenderingQueued = false;
    if (!active_) {
        return frameRenderingQueued;
    }

    if (!renderContext_ || !renderContext_->GetInterface<IClassRegister>()) {
        CORE_LOG_E("Empty RenderContext or ClassRegister");
        return frameRenderingQueued;
    }
    if (!dataStoreGs_) {
        if (auto render = GetSystem<IRenderPreprocessorSystem>(ecs_)) {
            if (auto propertyHandle =
                    ScopedHandle<const IRenderPreprocessorSystem::Properties>(render->GetProperties())) {
                dataStoreGs_ = refcnt_ptr<IRenderDataStoreDefaultGs>(
                    renderContext_->GetRenderDataStoreManager().Create(IRenderDataStoreDefaultGs::UID,
                        (propertyHandle->dataStorePrefix + DEFAULT_GS_RENDER_DATA_STORE_NAME_POST_FIX).data()));
            }
        }
        if (!dataStoreGs_) {
            CORE_LOG_E("Could not get or create DataStoreGs!");
            return frameRenderingQueued;
        }
    }

#ifdef WITH_EDITOR
    renderResolutionFromCC_ = dataStoreGs_->GetRenderResolution();
#else
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecs_);
    if (!cameraManager) {
        return frameRenderingQueued;
    }
    for (unsigned int i = 0; i < cameraManager->GetComponentCount(); i++) {
        auto cameraEntity = cameraManager->GetEntity(i);
        if (auto cameraHandle = cameraManager->Read(cameraEntity); cameraHandle) {
            if ((cameraHandle->sceneFlags & CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT) != 0) {
                renderResolutionFromCC_ = cameraHandle->renderResolution;
                break;
            }
        }
    }
#endif

    UpdateConfigurationData();

    // GPU sort buffers are allocated at load time (gs_loading.cpp). When useGpuSort
    // or use24BitGpuSort is toggled at runtime the existing buffers don't match the
    // requested mode (either missing entirely or sized for the wrong key width),
    // which makes RenderNodeGsRender::IsValid() fail and freezes the preview. Force
    // a full reload of every currently-loaded GS primitive so the buffers get
    // re-allocated with the new settings.
    if (dataStoreGs_) {
        GsRenderData* renderData = dataStoreGs_->GetGsRenderData();
        auto* gsDataStore = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
        if (renderData && gsDataStore) {
            const bool gpuSortChanged = renderData->useGpuSort != prevUseGpuSort_;
            const bool gpuSortBitsChanged =
                renderData->useGpuSort && (gsDataStore->use24BitGpuSort != prevUse24BitGpuSort_);
            if (gpuSortChanged || gpuSortBitsChanged) {
                prevUseGpuSort_ = renderData->useGpuSort;
                prevUse24BitGpuSort_ = gsDataStore->use24BitGpuSort;
                for (const auto& prim : dataStoreGs_->GetGsPrimitives()) {
                    const Entity splatEntity = prim.splatEntity;
                    if (std::find(removedGsComponents_.begin(), removedGsComponents_.end(), splatEntity) ==
                        removedGsComponents_.end()) {
                        removedGsComponents_.push_back(splatEntity);
                    }
                    if (std::find(addedGsComponents_.begin(), addedGsComponents_.end(), splatEntity) ==
                        addedGsComponents_.end()) {
                        addedGsComponents_.push_back(splatEntity);
                    }
                }
                frameRenderingQueued = true;
            }
        }
    }

    // When isAnimationRunning changed, mark frame dirty
    if (dataStoreGs_ && dataStoreGs_->GetGsRenderData() && dataStoreGs_->GetGsRenderData()->isAnimationRunningChanged) {
        frameRenderingQueued = true;
    }

    GS::GsLoadContext loadCtx = GetLoadContext();

    // handle completed background loads
    if (bgWorkerExitedSuccesfully_) {
        if (bgWorker_.joinable()) {
            bgWorker_.join();
        }

        dataStoreGs_->DisableSortFlag();

        // add all loaded primitives
        for (auto& prim : loadedPrimitives_) {
            std::lock_guard<std::mutex> lock(entityTrackingMtx_);
            if (auto it = std::find(entitiesBeingLoaded_.begin(), entitiesBeingLoaded_.end(), prim.splatEntity);
                it != entitiesBeingLoaded_.end()) {
                entitiesBeingLoaded_.erase(it);
            }

            dataStoreGs_->AddGsPrimitive(BASE_NS::move(prim));
        }
        loadedPrimitives_.clear();

        static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())->FlipValidGsRenderData();

        dataStoreGs_->EnableSortFlag();

        if (!addedGsComponents_.empty()) {
            addedGsComponents_.erase(addedGsComponents_.begin());
        }

        GS::GsRenderData* activeRenderData = dataStoreGs_->GetGsRenderData();

        if (!EntityUtil::IsValid(renderTargetHostEntity_)) {
            if (!dataStoreGs_->GetGsPrimitives().empty()) {
                renderTargetHostEntity_ = dataStoreGs_->GetGsPrimitives().front().splatEntity;
                loadCtx.tempRenderData = activeRenderData;
                CreateGsRenderTargets(loadCtx.tempRenderData, renderTargetHostEntity_,
                    loadCtx, renderResolutionFromCC_);
                const float totalScale = dataStoreGs_->GetGsScaleFactor() * dataStoreGs_->GetXegUpscaleFactor();
                previousResolution_ = Math::UVec2(static_cast<uint32_t>(renderResolutionFromCC_.x / totalScale),
                                                  static_cast<uint32_t>(renderResolutionFromCC_.y / totalScale));
            }
        }

        if (activeRenderData && activeRenderData->enableGsPicking) {
            // add initial picking world matrices
            {
                GS::IGsPicking* gsPicking =
                    GetInstance<GS::IGsPicking>(*renderContext_->GetInterface<IClassRegister>(), GS::UID_GS_PICKING);

                BASE_NS::vector<BASE_NS::Math::Mat4X4> worldMatrices;
                BASE_NS::vector<Core3D::ISceneNode*> sceneNodes;

                for (const auto& prim : dataStoreGs_->GetGsPrimitives()) {
                    worldMatrices.push_back(prim.worldMatrix);
                    auto* nodeSystem = GetSystem<INodeSystem>(ecs_);
                    if (!nodeSystem) {
                        continue;
                    }
                    sceneNodes.push_back(nodeSystem->GetNode(prim.splatEntity));
                }
                if (gsPicking) {
                    gsPicking->SetGsObjectWorldMatrices(ecs_, worldMatrices, sceneNodes);
                }
            }

            BuildCpuRaycastAccelerationStructure();
        }

        bgWorkerExitedSuccesfully_.store(false);

#ifndef NDEBUG
        CORE_LOG_D("GsSystem: All buffers valid after load");
#endif
    }

    // reload if path changed
    BASE_NS::vector<Entity> reloadGsComponents;
    const auto& prims = dataStoreGs_->GetGsPrimitives();
    const auto gsComponents = gsManager_.GetComponentCount();
    // should perhaps rely on component modified events.
    for (IComponentManager::ComponentId id = 0U; id < gsComponents; ++id) {
        ScopedHandle<const GsComponent> dfc = gsManager_.Read(id);
        if (!dfc) {
            continue;
        }

        const auto splatEntity = gsManager_.GetEntity(id);
        auto pos = std::find_if(prims.cbegin(), prims.cend(),
            [splatEntity](const GsPrimitive& prim) { return prim.splatEntity == splatEntity; });

        if ((pos == prims.cend()) && (!dfc->gsPath.empty())) {
            // component doesn't have a primitive and has a path -> should try loading.
            if ((std::find(addedGsComponents_.cbegin(), addedGsComponents_.cend(), splatEntity) !=
                    addedGsComponents_.cend())) {
                // already in the list of added components so no need to addd again.
                continue;
            }
            std::lock_guard<std::mutex> lock(entityTrackingMtx_);
            if (std::find(entitiesBeingLoaded_.cbegin(), entitiesBeingLoaded_.cend(), splatEntity) !=
                entitiesBeingLoaded_.cend()) {
                // already loading so no need to add again.
                continue;
            }
            frameRenderingQueued = true;
            addedGsComponents_.push_back(splatEntity);

        } else if ((pos != prims.cend()) && (dfc->gsPath != pos->gsPath)) {
            // component has a primitive and the path has changed -> should try reloading.
            if (std::find(removedGsComponents_.begin(), removedGsComponents_.end(), splatEntity) ==
                removedGsComponents_.end()) {
                removedGsComponents_.push_back(splatEntity);
            }
            frameRenderingQueued = true;
            if ((!dfc->gsPath.empty())) {
                reloadGsComponents.push_back(splatEntity);
            }
        }
    }

    // handle removals
    if (!removedGsComponents_.empty()) {
        if (!bgWorkerDone_) {
            shuttingDown_.store(true);

            if (bgWorker_.joinable()) {
                bgWorker_.join();
            }

            // clear the tracking vector
            {
                std::lock_guard<std::mutex> lock(entityTrackingMtx_);
                entitiesBeingLoaded_.clear();
            }

            // clear loaded primitives that weren't added yet
            loadedPrimitives_.clear();

            shuttingDown_.store(false);
            bgWorkerDone_ = true;

            // if the background load completed successfully, flip it
            if (bgWorkerExitedSuccesfully_) {
                dataStoreGs_->DisableSortFlag();

                for (auto& prim : loadedPrimitives_) {
                    dataStoreGs_->AddGsPrimitive(BASE_NS::move(prim));
                }
                loadedPrimitives_.clear();

                static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())->FlipValidGsRenderData();

                dataStoreGs_->EnableSortFlag();

                GS::GsRenderData* activeRenderData = dataStoreGs_->GetGsRenderData();

                if (!EntityUtil::IsValid(renderTargetHostEntity_)) {
                    if (!dataStoreGs_->GetGsPrimitives().empty()) {
                        renderTargetHostEntity_ = dataStoreGs_->GetGsPrimitives().front().splatEntity;
                        loadCtx.tempRenderData = activeRenderData;
                        CreateGsRenderTargets(loadCtx.tempRenderData, renderTargetHostEntity_,
                            loadCtx, renderResolutionFromCC_);
                        const float totalScale = dataStoreGs_->GetGsScaleFactor() * dataStoreGs_->GetXegUpscaleFactor();
                        previousResolution_ =
                            Math::UVec2(static_cast<uint32_t>(renderResolutionFromCC_.x / totalScale),
                                        static_cast<uint32_t>(renderResolutionFromCC_.y / totalScale));
                    }
                }

                bgWorkerExitedSuccesfully_.store(false);
            }
        }

        GS::GsRenderData* activeRenderData = dataStoreGs_->GetGsRenderData();
        loadCtx.tempRenderData = activeRenderData;

        // remove objects
        dataStoreGs_->DisableSortFlag();

        for (const auto& entity : removedGsComponents_) {
            RemoveGsObject(entity, loadCtx, activeRenderData);
        }

        dataStoreGs_->EnableSortFlag();

        removedGsComponents_.clear();

        if (!reloadGsComponents.empty()) {
            for (const auto& entity : reloadGsComponents) {
                if (std::find(addedGsComponents_.begin(), addedGsComponents_.end(), entity) ==
                    addedGsComponents_.end()) {
                    addedGsComponents_.push_back(entity);
                }
            }
        }

        if (activeRenderData && activeRenderData->enableGsPicking) {
            BuildCpuRaycastAccelerationStructure();
        }

#ifndef NDEBUG
        CORE_LOG_D("GsSystem: All buffers valid after remove object");
#endif
    }

    const auto gsAddWorker = [&](BASE_NS::vector<BASE_NS::pair<GsComponent, Entity>> handles, GS::GsLoadContext ctx,
                                 BASE_NS::vector<GsPrimitive>* outPrimitives) {
        if (handles.empty()) {
            bgWorkerDone_ = true;
            return;
        }

        bool loadCompletedSuccessfully = true;

        for (auto& dfc : handles) {
            if (ctx.shuttingDown_->load()) {
                loadCompletedSuccessfully = false;
                break;
            }

            {
                CORE_NS::ScopedHandle<GsComponent> writeHandle = gsManager_.Write(dfc.second);
                if (writeHandle) {
                    writeHandle->isLoaded = false;
                    writeHandle->isPrimitiveValid = false;
                }
            }

            GsPrimitive prim = LoadGsPrimitive(dfc.first, dfc.second, ctx);

            if (ctx.shuttingDown_->load()) {
                loadCompletedSuccessfully = false;
                break;
            }

            if (prim.isValid) {
                prim.gsPath = dfc.first.gsPath;
                // defer the data store add
                outPrimitives->push_back(BASE_NS::move(prim));
                CORE_NS::ScopedHandle<GsComponent> writeHandle = gsManager_.Write(dfc.second);
                if (writeHandle) {
                    writeHandle->isPrimitiveValid = true;
                }
            } else {
                CORE_NS::ScopedHandle<GsComponent> writeHandle = gsManager_.Write(dfc.second);
                if (writeHandle) {
                    writeHandle->isPrimitiveValid = false;
                }
            }

            if (auto gsHandle = gsManager_.Write(dfc.second)) {
                gsHandle->gsPosData = dfc.first.gsPosData;
                gsHandle->cameraMotion2d = dfc.first.cameraMotion2d;
            }
        }

        // update tracking and loaded status
        {
            std::lock_guard<std::mutex> lock(entityTrackingMtx_);
            for (const auto& dfc : handles) {
                if (auto it = std::find(entitiesBeingLoaded_.begin(), entitiesBeingLoaded_.end(), dfc.second);
                    it != entitiesBeingLoaded_.end()) {
                    entitiesBeingLoaded_.erase(it);

                    CORE_NS::ScopedHandle<GsComponent> writeHandle = gsManager_.Write(dfc.second);
                    if (writeHandle) {
                        writeHandle->isLoaded = true;
                    }
                }

                if (dfc.first.useFadeAnimation && ctx.tempRenderData) {
                    ctx.tempRenderData->useAnimations = true;
                }
            }
        }

        bgWorkerDone_ = true;

        if (loadCompletedSuccessfully) {
            bgWorkerExitedSuccesfully_ = true;
        }
    };

    // start new loads
    if (!addedGsComponents_.empty() && bgWorkerDone_) {
        BASE_NS::vector<BASE_NS::pair<GsComponent, Entity>> workHandles;

        bgWorkerDone_ = false;

        {
            std::lock_guard<std::mutex> lock(entityTrackingMtx_);

            loadCtx.tempRenderData =
                static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())->GetSafeLoadingGsRenderData();

            // only process one entity at a time to guarantee correct data offsets
            const Entity entity = addedGsComponents_.front();

            if (dataStoreGs_->GetGsPrimitives().size() >= GS_MAX_NUM_GS_OBJECTS) {
                CORE_LOG_W("Cannot create more than %d GS3D models!", GS_MAX_NUM_GS_OBJECTS);
                addedGsComponents_.erase(addedGsComponents_.begin());
                bgWorkerDone_ = true;
            } else {
                const bool isPendingRemoval = std::find(removedGsComponents_.begin(), removedGsComponents_.end(),
                                                  entity) != removedGsComponents_.end();

                if (isPendingRemoval) {
                    addedGsComponents_.erase(addedGsComponents_.begin());
                    bgWorkerDone_ = true;
                } else {
                    const bool isAlreadyLoading =
                        std::find_if(entitiesBeingLoaded_.begin(), entitiesBeingLoaded_.end(),
                            [entity](Entity loading) { return loading.id == entity.id; }) != entitiesBeingLoaded_.end();

                    if (isAlreadyLoading) {
                        // skip, will retry next frame
                        bgWorkerDone_ = true;
                    } else {
                        auto componentHandle = gsManager_.Read(entity);
                        if (!componentHandle) {
                            CORE_LOG_W("GsSystem: Component for entity was destroyed before loading could start");
                            addedGsComponents_.erase(addedGsComponents_.begin());
                            bgWorkerDone_ = true;
                        } else {
                            workHandles.push_back(BASE_NS::pair(*componentHandle, entity));
                            entitiesBeingLoaded_.push_back(entity);
                        }
                    }
                }
            }
        }

        if (bgWorker_.joinable()) {
            bgWorker_.join();
        }
        shuttingDown_.store(false);

        if (!workHandles.empty()) {
            loadedPrimitives_.clear();
            bgWorker_ = std::thread(gsAddWorker, workHandles, loadCtx, &loadedPrimitives_);
        } else {
            bgWorkerDone_ = true;
        }
    }

    GS::GsRenderData* renderData = dataStoreGs_->GetGsRenderData();
    // Check if render targets need rebuild based on effective render size
    // (considers output resolution, scale factor, and upscale factor)
    const float upscaleRatio = dataStoreGs_->GetGsScaleFactor() * dataStoreGs_->GetXegUpscaleFactor();
    const Math::UVec2 effectiveRenderSize = Math::UVec2(
        static_cast<uint32_t>(renderResolutionFromCC_.x / upscaleRatio),
        static_cast<uint32_t>(renderResolutionFromCC_.y / upscaleRatio));
    if (effectiveRenderSize != previousResolution_ && dataStoreGs_->GetGsScaleFactor() != 0xFFFFffffu) {
        RemoveGsRenderTargets(renderData, renderTargetHostEntity_, loadCtx);
        // Pass original output resolution to avoid precision loss in target1 creation
        GS::CreateGsRenderTargets(renderData, renderTargetHostEntity_, loadCtx, renderResolutionFromCC_, true);
        previousResolution_ = effectiveRenderSize;
    }

    // If render targets were never created (e.g. only GsTilingSystem primitives exist, no GsComponent),
    // set them up now once the scale factor is known.
    if (!EntityUtil::IsValid(renderTargetHostEntity_) && !dataStoreGs_->GetGsPrimitives().empty()) {
        renderTargetHostEntity_ = dataStoreGs_->GetGsPrimitives().front().splatEntity;
        loadCtx.tempRenderData = renderData;
        if (previousResolution_.x == 0U) {
            previousResolution_.x = 1U;
        }
        if (previousResolution_.y == 0U) {
            previousResolution_.y = 1U;
        }
        dataStoreGs_->SetGsScaleFactor(2U);
        GS::CreateGsRenderTargets(renderData, renderTargetHostEntity_, loadCtx, previousResolution_);
    }

    // update existing primitive parameters
    for (auto& prim : dataStoreGs_->GetGsPrimitives()) {
        if (auto worldMatrixHandle = wmcm_.Read(prim.splatEntity)) {
            prim.worldMatrix = worldMatrixHandle->matrix;

            if (prim.isDirty || prim.animationFadeProperties.useFadeAnimation) {
                frameRenderingQueued = true;
            }
        }

        if (auto layerHandle = lcm_.Read(prim.splatEntity)) {
            prim.layerMask = layerHandle->layerMask;
        }

        ScopedHandle<const GsComponent> dfc = gsManager_.Read(prim.splatEntity);

        if (dfc) {
            prim.enabled = dfc->enabled;
            if (prim.enabled != prim.prevEnabled) {
                frameRenderingQueued = true;
            }

            if (dfc->cropBoxAABBMin != prim.cropBoxProperties.aabbMin ||
                dfc->cropBoxAABBMax != prim.cropBoxProperties.aabbMax ||
                dfc->gsUseCropBox != prim.cropBoxProperties.useCropBox) {
                prim.cropBoxProperties.aabbMin = dfc->cropBoxAABBMin;
                prim.cropBoxProperties.aabbMax = dfc->cropBoxAABBMax;
                prim.cropBoxProperties.useCropBox = dfc->gsUseCropBox;

                prim.cropBoxProperties.cropBoxChanged = true;
            } else {
                prim.cropBoxProperties.cropBoxChanged = false;
            }

            if (renderData && renderData->enableCustomAnimation) {
                if (dfc->useFadeAnimation != renderData->useAnimations) {
                    bool anyAnim = renderData->useAnimations || dfc->useFadeAnimation;
                    for (const auto& primIter : dataStoreGs_->GetGsPrimitives()) {
                        ScopedHandle<const GsComponent> primIterDfc = gsManager_.Read(primIter.splatEntity);
                        if (primIterDfc) {
                            anyAnim |= primIterDfc->useFadeAnimation;
                        }
                    }

                    renderData->useAnimations = anyAnim;
                }
            }
        }
    }

    // update CPU raycast world matrices
    if (!renderContext_ || !renderContext_->GetInterface<IClassRegister>()) {
        CORE_LOG_E("Empty RenderContext or ClassRegister");
        return frameRenderingQueued;
    }

    GS::IGsPicking* gsPicking =
        GetInstance<GS::IGsPicking>(*renderContext_->GetInterface<IClassRegister>(), GS::UID_GS_PICKING);

    BASE_NS::vector<BASE_NS::Math::Mat4X4> worldMatrices;
    BASE_NS::vector<Core3D::ISceneNode*> sceneNodes;

    for (const auto& prim : dataStoreGs_->GetGsPrimitives()) {
        worldMatrices.push_back(prim.worldMatrix);
        auto* nodeSystem = GetSystem<INodeSystem>(ecs_);
        if (!nodeSystem) {
            continue;
        }
        sceneNodes.push_back(nodeSystem->GetNode(prim.splatEntity));
    }
    if (gsPicking) {
        gsPicking->SetGsObjectWorldMatrices(ecs_, worldMatrices, sceneNodes);
    }

    return frameRenderingQueued;
}

void GsSystem::BuildCpuRaycastAccelerationStructure() const
{
    if (!renderContext_ || !renderContext_->GetInterface<IClassRegister>()) {
        return;
    }
    GS::IGsPicking* gsPicking =
        GetInstance<GS::IGsPicking>(*renderContext_->GetInterface<IClassRegister>(), GS::UID_GS_PICKING);
    if (!gsPicking) {
        CORE_LOG_E("Could not get gsPicking instance, aborting!");
        return;
    }

    if (!dataStoreGs_) {
        return;
    }

    const auto* renderData = dataStoreGs_->GetGsRenderData();
    if (!renderData) {
        return;
    }

    BASE_NS::vector<BASE_NS::vector<GS::IGsPicking::GsData>> gsData;

    dataStoreGs_->DisableSortFlag();

    for (const GsPrimitive& prim : dataStoreGs_->GetGsPrimitives()) {
        BASE_NS::vector<GS::IGsPicking::GsData> objectData;
        objectData.reserve(prim.numGs);

        if (prim.startOffset + prim.numGs - 1 >= renderData->gaussianPositions.size()) {
            CORE_LOG_E(
                "BuildCpuRaycastAccelerationStructure: startOffset + numGs - 1 >= gaussianPositions.size(), aborting!");
            dataStoreGs_->EnableSortFlag();
            return;
        }

        for (size_t ii = 0; ii < static_cast<size_t>(prim.numGs); ii++) {
            const size_t globalIdx = prim.startOffset + ii;
            const auto& positionOpacity = renderData->gaussianPositions[globalIdx];
            const float extent = renderData->splatExtents[globalIdx];

            objectData.emplace_back(IGsPicking::GsData { positionOpacity, extent });
        }

        gsData.push_back(BASE_NS::move(objectData));
    }
    dataStoreGs_->EnableSortFlag();

    gsPicking->BuildRaycastCpuAccelerationVolume(ecs_, gsData);
}

void GsSystem::RemoveGsObject(
    const Entity entity, const GS::GsLoadContext loadCtx, GsRenderData* renderData, const bool recreateBuffers)
{
    if (!renderData) {
        return;
    }
    const int toRemovePrimIndex = dataStoreGs_->GetGsPrimitiveIndex(entity);
    if (toRemovePrimIndex < 0) {
        CORE_LOG_E("Could not find Gs primitive to remove with entity!");
        return;
    }

    dataStoreGs_->DisableSortFlag();

    auto& primitives = dataStoreGs_->GetGsPrimitives();
    const auto& primToRemove = primitives[static_cast<size_t>(toRemovePrimIndex)];
    const CORE_NS::Entity toRemoveEntity = primToRemove.splatEntity;

    if (toRemoveEntity == renderTargetHostEntity_) {
        RemoveGsRenderTargets(renderData, renderTargetHostEntity_, loadCtx);
    }

    if (primToRemove.attribute2DTexNum != 0 && primToRemove.platformsSupportsBlockFormat && recreateBuffers) {
        // cleanup ASTC resources

        RENDER_NS::IGpuResourceManager& gpuResourceMgr = renderContext_->GetDevice().GetGpuResourceManager();

        GpuImageDesc descImg;
        descImg.usageFlags =
            ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT | ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT;
        descImg.imageTiling = ImageTiling::CORE_IMAGE_TILING_OPTIMAL;
        descImg.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        descImg.engineCreationFlags = 0;
        descImg.width = 4u;
        descImg.height = 4u;
        descImg.format = Format::BASE_FORMAT_R8G8B8A8_UNORM;
        BASE_NS::vector<uint8_t> shASTCData;
        shASTCData.resize(16u);

        const size_t remTo = static_cast<size_t>(primToRemove.textIdOffset + primToRemove.attribute2DTexNum);

        if (renderData->gsShASTCDataRefs.size() >= remTo) {
            for (size_t ii = primToRemove.textIdOffset; ii < remTo; ii++) {
                renderData->gsShASTCDataRefs[ii] = gpuResourceMgr.Create(descImg,
                    BASE_NS::array_view<uint8_t>((uint8_t*)(shASTCData.data()), shASTCData.size() * sizeof(uint8_t)));
                if (!renderData->gsShASTCDataRefs[ii]) {
                    CORE_LOG_E("Failed to create ASTC texture");
                    continue;
                }

                renderData->astcSizes[ii] = Math::UVec2(4u, 4u);
            }
        }
    }

    const size_t dataStartOffset = primToRemove.startOffset;
    const size_t dataStartShOffset = primToRemove.startShOffset;
    const size_t dataNumElements = primToRemove.gsBufferSize;

    auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
    const bool removed = internalDs ? internalDs->RemoveGsPrimitiveIfPresent(entity) : false;
    if (!removed) {
        dataStoreGs_->EnableSortFlag();
        return;
    }

    renderMeshManager_.Destroy(entity);

    primitives = dataStoreGs_->GetGsPrimitives();

    if (toRemoveEntity == renderTargetHostEntity_ && recreateBuffers && primitives.size() >= 1u) {
        renderTargetHostEntity_ = primitives.front().splatEntity;
        GS::CreateGsRenderTargets(renderData, renderTargetHostEntity_, loadCtx, renderResolutionFromCC_);
        const float totalScale = dataStoreGs_->GetGsScaleFactor() * dataStoreGs_->GetXegUpscaleFactor();
        previousResolution_ = Math::UVec2(static_cast<uint32_t>(renderResolutionFromCC_.x / totalScale),
                                          static_cast<uint32_t>(renderResolutionFromCC_.y / totalScale));
    } else if (primitives.size() == 0u) {
        CORE3D_NS::IMaterialComponentManager* materialManager = GetManager<CORE3D_NS::IMaterialComponentManager>(ecs_);
        materialManager->Destroy(renderData->gsMaterial);
        renderData->gsMaterial = {};

        // invalidate render targets
        renderData->gaussianRenderTarget0 = {};
        renderData->gaussianRenderTarget1 = {};
        renderData->gaussianRenderTarget2 = {};
        renderData->gaussianRenderTarget3 = {};
        renderData->gaussianRenderTargetBB = {};
        renderData->depthStoreCacheHandle = {};
        renderData->gaussianRenderTargetDepth1 = {};
        renderData->sampler = {};
        renderData->gaussianRenderTargetStatusUbo = {};

        renderTargetHostEntity_ = {};
    }

    if (recreateBuffers) {
        uint32_t totalNumberSplats = 0;
        uint32_t totalNumberIndexable = 0;
        uint32_t totalSplatsNeedSh = 0;

        for (const auto& prim : primitives) {
            totalNumberSplats += prim.numGs + prim.numGs1;
            totalNumberIndexable += prim.numGs;
            if (prim.RequireSH()) {
                totalSplatsNeedSh += prim.gsBufferSize;
            }
        }

        if (totalNumberSplats > 0) {
            GS::CreateGsRenderDataBuffers(
                renderData, totalNumberSplats, totalNumberIndexable, totalSplatsNeedSh, loadCtx);
        }
    }

    if (primitives.empty()) {
        // invalidate GS buffers
        renderData->gaussianPositions = {};
        renderData->gaussianIndices = {};
        renderData->splatExtents = {};
        renderData->gaussianMaxScales = {};

        renderData->dataStorageCacheHandle = {};
        renderData->viewZStoreCacheHandle = {};
        renderData->gsIndicesRef = {};
        renderData->gsIndicesRef2 = {};
        renderData->gsSplatData = {};
        renderData->gsSplatDataRender = {};
        renderData->gsHarmonicsDataRef = {};

        renderData->gsDataCopyRefByteSize = 0;
        renderData->gsHarmonicsDataRefByteSize = 0;

        renderData->gsShASTCDataRefs.clear();
        renderData->astcSizes.clear();

        renderData->gpuSortBuffers.sortKeyIndexBuffer0 = {};
        renderData->gpuSortBuffers.sortKeyIndexBuffer1 = {};
        renderData->gpuSortBuffers.sortCountBuffer = {};

        renderData->isTiled = false;
        renderData->tilePool.reset();

        renderData->areResourcesLoaded = false;

        renderTargetHostEntity_ = {};
    }

    // update start offsets
    for (size_t ii = 0; ii < primitives.size(); ii++) {
        if (primitives[ii].startOffset > dataStartOffset) {
            primitives[ii].startOffset -= static_cast<uint32_t>(dataNumElements);
        }
        if (primitives[ii].startShOffset > dataStartShOffset) {
            primitives[ii].startShOffset -= static_cast<uint32_t>(dataNumElements);
        }
    }

#ifndef NDEBUG
    CORE_LOG_D("A GS3D primitive removed with start offset of %du and size of %du. Currently %du GS models are "
               "loaded.",
        (uint32_t)dataStartOffset, (uint32_t)dataNumElements, (uint32_t)primitives.size());
#endif

    dataStoreGs_->EnableSortFlag();
}

void GsSystem::UpdateCustomAnimationShader(
    const GsConfigurationComponent* gcc, GsRenderData* renderData)
{
    if (!EntityUtil::IsValid(gcc->customAnimationShader)) {
        return;
    }
    CORE_NS::ScopedHandle<const RenderHandleComponent> rhc = rhcManager_.Read(gcc->customAnimationShader);
    if (!rhc) {
        CORE_LOG_E("GsSystem: customAnimationShader RenderHandle is invalid, is the Entity alive?");
        return;
    }
    if (!RenderHandleUtil::IsValid(rhc->reference.GetHandle())) {
        CORE_LOG_E("GsSystem: customAnimationShader handle is invalid!");
        return;
    }
    renderData->customAnimationShader = rhc->reference;
}

void GsSystem::UpdateCustomResources(
    const GsConfigurationComponent* gcc, GsRenderData* renderData)
{
    BASE_NS::vector<RENDER_NS::RenderHandleReference> customResourceHandles;
    for (size_t ii = 0; ii < gcc->customResources.size(); ii++) {
        const auto customResourceEntity = gcc->customResources[ii];
        if (!EntityUtil::IsValid(customResourceEntity)) {
            continue;
        }
        CORE_NS::ScopedHandle<const RenderHandleComponent> rhc = rhcManager_.Read(customResourceEntity);
        if (!rhc) {
            CORE_LOG_E("GsSystem: customResources RenderHandle at index %u is invalid, is the Entity alive?",
                static_cast<uint32_t>(ii));
            continue;
        }
        if (!RenderHandleUtil::IsValid(rhc->reference.GetHandle())) {
            CORE_LOG_E("GsSystem: customResources handle at index %u is invalid!", static_cast<uint32_t>(ii));
            continue;
        }
        customResourceHandles.push_back(rhc->reference);
    }
    if (!customResourceHandles.empty()) {
        renderData->customAnimationResources = customResourceHandles;
    }
}

void GsSystem::UpdateAnimationState(
    const GsConfigurationComponent* gcc, GsRenderData* renderData)
{
    if (renderData->isAnimationRunningPrevious != gcc->isAnimationRunning) {
        renderData->isAnimationRunningChanged = true;
        renderData->isAnimationRunningPrevious = gcc->isAnimationRunning;
    } else {
        renderData->isAnimationRunningChanged = false;
    }

    renderData->useAnimations = gcc->isAnimationRunning && gcc->enableCustomAnimation;
    renderData->useGpuSort = gcc->useGpuSort;
    renderData->isAnimationPaused = gcc->isAnimationPaused;
    // Note: RLock/WLock are no-ops in release builds; ensure atomic read-check-reset
    if (gcc->animationSeekTime != -1.0f) {
        renderData->animationSeekTimestamp = gcc->animationSeekTime;
        auto wcc = gsConfigManager_.Write(gsConfigEntity_);
        if (wcc) {
            wcc->animationSeekTime = -1.0f;
        }
    }
}

void GsSystem::UpdateRenderFlags(
    const GsConfigurationComponent* gcc, GsRenderData* renderData)
{
    renderData->numFramesToComputeSh = gcc->frameToComputeSphericalHarmonics;
    renderData->frustumScale = gcc->frustumScale;
    renderData->adhocCullingScale = gcc->adhocCullingScale;
    renderData->enableGsPicking = gcc->enableGsPicking;
    renderData->enableCustomAnimation = gcc->enableCustomAnimation;
    renderData->enablePackedData = gcc->enablePackedData;
    renderData->enableTilePackedData = gcc->enableTilePackedData;
    renderData->enableCpuScheduling = gcc->enableCpuScheduling;
    renderData->enableScoreCulling = gcc->enableScoreCulling;
    renderData->enableDynamicResolution = gcc->enableDynamicResolution;
    renderData->dynamicResolutionScale = gcc->dynamicResolutionScale;
    renderData->enableFrameGeneration = gcc->enableFrameGeneration;
    renderData->enableGsOpt = gcc->enableGsOpt;

    static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())->use24BitGpuSort = gcc->use24BitGpuSort;
    static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())->SetRenderCovarianceStddev(gcc->renderCovarianceStddev);
    static_cast<RenderDataStoreDefaultGS*>(
        dataStoreGs_.get())->SetRenderSharpeningCoefficient(gcc->renderSharpeningCoefficient);

    // XEG Upscale enabled flag from configuration
    renderData->enableXegUpscale = gcc->enableXegUpscale;
    renderData->xegUpscaleFactor = gcc->xegUpscaleFactor;
    // Check if platform supports XEG Spatial Upscale (detected at initialization)
    // If not supported, disable XEG upscale and reset factor to 1.0 to ensure correct render target sizes
    if (renderData->enableXegUpscale && !xegPlatformSupported_) {
        renderData->enableXegUpscale = false;
        renderData->xegUpscaleFactor = 1.0;
    }
}

void GsSystem::UpdateConfigurationData()
{
    if (!dataStoreGs_) {
        return;
    }
    GsRenderData* renderData = dataStoreGs_->GetGsRenderData();
    if (!renderData) {
        return;
    }

    if (!EntityUtil::IsValid(gsConfigEntity_)) {
        return;
    }

    const ScopedHandle<const GsConfigurationComponent> gcc = gsConfigManager_.Read(gsConfigEntity_);
    if (!gcc) {
        return;
    }

    UpdateCustomAnimationShader(&(*gcc), renderData);
    UpdateCustomResources(&(*gcc), renderData);
    UpdateAnimationState(&(*gcc), renderData);
    UpdateRenderFlags(&(*gcc), renderData);

    auto wcc = gsConfigManager_.Write(gsConfigEntity_);
    if (wcc) {
        wcc->currentAnimationTime =
            static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())->GetCurrentAnimationTime();
    }
}

GS::GsLoadContext GsSystem::GetLoadContext()
{
    GS::GsLoadContext loadCtx;
    loadCtx.dataStoreGs_ = dataStoreGs_;
    loadCtx.dsStaging_ = dsStaging_;
    loadCtx.ecs_ = &ecs_;
    loadCtx.multiGsBufferIndexCounter = multiGsBufferIndexCounter;
    loadCtx.renderContext_ = renderContext_;
    loadCtx.renderMeshManager_ = &renderMeshManager_;
    loadCtx.threadPool_ = threadPool_;
    loadCtx.uniqueName = GetUniqueName();
    loadCtx.wmcm_ = &wmcm_;
    loadCtx.shuttingDown_ = &shuttingDown_;

    if (EntityUtil::IsValid(gsConfigEntity_)) {
        GsConfigurationComponent gcc = *gsConfigManager_.Read(gsConfigEntity_);
        loadCtx.gsConfigComponent_ = BASE_NS::make_shared<GsConfigurationComponent>(gcc);
    }

    return loadCtx;
}

void GsSystem::OnComponentEvent(
    EventType type, const IComponentManager& componentManager, array_view<const Entity> entities)
{
    switch (type) {
        case EventType::CREATED:
            if (componentManager.GetName() == gsManager_.GetName()) {
                for (const auto& entity : entities) {
                    if (std::find(addedGsComponents_.begin(), addedGsComponents_.end(), entity) ==
                        addedGsComponents_.end()) {
                        addedGsComponents_.push_back(entity);
                    }
                }
            } else if (componentManager.GetName() == gsConfigManager_.GetName()) {
                if (EntityUtil::IsValid(gsConfigEntity_)) {
                    CORE_LOG_E("GsSystem::OnComponentEvent: GsConfigurationComponent already set! Only one can exist!");
                } else {
                    gsConfigEntity_ = entities.at(0);
                }
            }
            break;
        case EventType::MODIFIED:
            break;
        case EventType::DESTROYED:
            if (componentManager.GetName() == gsManager_.GetName()) {
                for (const auto& entity : entities) {
                    auto addedIt = std::find(addedGsComponents_.begin(), addedGsComponents_.end(), entity);
                    if (addedIt != addedGsComponents_.end()) {
                        addedGsComponents_.erase(addedIt);
                    }

                    if (std::find(removedGsComponents_.begin(), removedGsComponents_.end(), entity) ==
                        removedGsComponents_.end()) {
                        removedGsComponents_.push_back(entity);
                    }
                }
            } else if (componentManager.GetName() == gsConfigManager_.GetName()) {
                gsConfigEntity_ = {};
            }
            break;
        case EventType::MOVED:
            break;
    }
}

} // namespace

namespace GS {
CORE_NS::ISystem* IGsSystemInstance(IEcs& ecs)
{
    return new GsSystem(ecs);
}
void IGsSystemDestroy(CORE_NS::ISystem* instance)
{
    delete static_cast<GsSystem*>(instance);
}
} // namespace GS