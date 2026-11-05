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

#include <gtest/gtest.h>
#include <dlfcn.h>

#include <algorithm>
#include <thread>

#include <base/math/matrix_util.h>
#include <base/math/quaternion_util.h>
#include <base/containers/fixed_string.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/util/uid_util.h>
#include <core/os/platform_create_info.h>
#include <core/ecs/intf_system_graph_loader.h>
#include <core/engine_info.h>
#include <core/intf_engine.h>
#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/os/intf_platform.h>
#include <core/plugin/intf_class_factory.h>
#include <core/plugin/intf_class_register.h>
#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/name_component.h>
#include <3d/ecs/components/node_component.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/post_process_effect_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/implementation_uids.h>
#include <3d/intf_graphics_context.h>
#include <3d/util/intf_scene_util.h>
#include <render/datastore/intf_render_data_store_default_gpu_resource_data_copy.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/datastore/intf_render_data_store_render_post_processes.h>
#include <render/device/intf_device.h>
#include <render/implementation_uids.h>
#include <render/intf_render_context.h>
#include <render/intf_renderer.h>
#include <render/nodecontext/intf_render_node_graph_manager.h>
#include <render/util/intf_render_frame_util.h>
#include <render/util/intf_render_util.h>

#if RENDER_HAS_GLES_BACKEND || RENDER_HAS_GL_BACKEND
#include <render/gles/intf_device_gles.h>
#endif
#if RENDER_HAS_VULKAN_BACKEND
#include <vulkan/vulkan.h>

#include <render/vulkan/intf_device_vk.h>
#endif

#include <scene/base/namespace.h>

#include <plugin_gs/ecs/components/gs_component.h>
#include <plugin_gs/ecs/components/gs_effect_component.h>
#include <plugin_gs/ecs/systems/gs_system.h>
#include <plugin_gs/implementation_uids.h>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <plugin_gs/util/intf_gs_loader.h>
#include <plugin_gs/util/intf_gs_picking.h>



#include <plugin_gs/ecs/components/gs_configuration_component.h>
#include <plugin_gs/ecs/systems/intf_gs_ai_edit_system.h>
#include <plugin_gs/util/intf_gs_picking.h>
#include <plugin_gs/util/intf_gs_picking.h>

// #include <Eigen/Dense>
// #include <Eigen/Core>

// #include "GsAIEdit/gs_image_extractor.h" // moved to LumeGSAIEdit module

// #include "spatial_codec_factory.h"
// #include "spatial_buffer.h"
// #include "spatial_meta.h"
// #include "spatial_tag.h"


using namespace testing;
using namespace testing::ext;
using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
//using namespace Spatial;


CORE_BEGIN_NAMESPACE()

/** Get plugin register */
IPluginRegister &(*GetPluginRegister)() = nullptr;
/** Setup the plugin register */
void (*CreatePluginRegistry)(const struct PlatformCreateInfo &platformCreateInfo) = nullptr;
/** Get whether engine is build in debug mode */
bool (*IsDebugBuild)() = nullptr;
/** Get version */
BASE_NS::string_view (*GetVersion)() = nullptr;

template <typename T>
bool LoadFunc(T &fn, const char *fName, void *handle)
{
    fn = reinterpret_cast<T>(dlsym(handle, fName));
    return true;
}

CORE_END_NAMESPACE()

namespace OHOS::Render3D {
void registerTestFileProtocol(CORE_NS::IFileManager& aFileManager)
{
    constexpr const BASE_NS::string_view applicationTestAssetsDirectory = "file:///data/local/3dgs_test/assets";

    // Create test:// protocol that points to application test data directory.
    aFileManager.RegisterAssetPath(applicationTestAssetsDirectory);
    aFileManager.RegisterPath("assets", applicationTestAssetsDirectory, false);
}

class PluginGSTest : public testing::Test {
public:
    CORE_NS::IEngine::Ptr engine;
    RENDER_NS::IRenderContext::Ptr renderContext;
    CORE3D_NS::IGraphicsContext::Ptr graphicsContext;

    RENDER_NS::RenderHandleReference imageHandle_;
    RENDER_NS::RenderHandleReference bufferHandle_;
    BASE_NS::unique_ptr<ByteArray> byteArray_;
    bool validTargets { false };

    Core::IEcs::Ptr ecs;

    void* libHandle_;

    void Init()
    {
        if (ecs) {
            return;
        }

        graphicsContext->Init();
        engine->Init();
        ecs = engine->CreateEcs();
        ecs->Initialize();
    }

    void CreateGSNode(CORE3D_NS::INodeSystem* nodeSystem, CORE3D_NS::ISceneNode* rootNode, CORE3D_NS::ISceneNode* splatsNode, const BASE_NS::string& path)
    {
        // load GS data
        {
            splatsNode = nodeSystem->CreateNode();
            ASSERT_TRUE(splatsNode);
            splatsNode->SetParent(*rootNode);
            auto entity = splatsNode->GetEntity();
            {
                CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
                CORE_ASSERT(gsComponentManager);
                gsComponentManager->Create(entity);
                ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(entity));
                data->gsPath = path;
            }
        }
    }

    void AwaitAsyncGsLoad(const CORE_NS::Entity entity, const uint64_t timeoutSeconds = 60u)
    {
        GS::IGsComponentManager* gsComponentManager = CORE_NS::GetManager<GS::IGsComponentManager>(*ecs);
        ASSERT_TRUE(gsComponentManager);

        const auto timeout = std::chrono::seconds(timeoutSeconds);
        const auto start = std::chrono::steady_clock::now();

        ASSERT_TRUE(CORE_NS::EntityUtil::IsValid(entity));
        ASSERT_TRUE(gsComponentManager->HasComponent(entity));

        while (std::chrono::steady_clock::now() - start < timeout) {
            if (gsComponentManager->Read(entity)->isLoaded) {
                break;
            }
            
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // tick once more to flip the render data storage
        auto* ecsRawPtr = ecs.get();
        engine->TickFrame(BASE_NS::array_view(&ecsRawPtr, 1));
    }

    void RenderFrame()
    {
        RENDER_NS::IRenderer& renderer = renderContext->GetRenderer();
        const auto ecsRngs = graphicsContext->GetRenderNodeGraphs(*ecs);
        BASE_NS::vector<RENDER_NS::RenderHandleReference> rngs;
        rngs.append(ecsRngs.begin(), ecsRngs.end());
        renderer.RenderFrame(rngs);
    }

    void CreateTargets()
    {
        if (validTargets) {
            return;
        }

        GpuImageDesc imageDesc;
        imageDesc.width = 512;
        imageDesc.height = 512;
        imageDesc.depth = 1;
        imageDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        imageDesc.format = BASE_FORMAT_R8G8B8A8_UNORM;
        imageDesc.imageTiling = CORE_IMAGE_TILING_OPTIMAL;
        imageDesc.imageType = CORE_IMAGE_TYPE_2D;
        imageDesc.imageViewType = CORE_IMAGE_VIEW_TYPE_2D;
        imageDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        imageDesc.usageFlags = CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | CORE_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imageHandle_ = renderContext->GetDevice().GetGpuResourceManager().Create("TestOutputGSImage", imageDesc);

        GpuBufferDesc bufferDesc;
        bufferDesc.byteSize = 512 * 512 * 4;
        bufferDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        bufferDesc.usageFlags = CORE_BUFFER_USAGE_TRANSFER_DST_BIT;
        bufferDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT | CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        bufferHandle_ = renderContext->GetDevice().GetGpuResourceManager().Create("TestOutputGSBuffer", bufferDesc);

        {
            Render::IRenderFrameUtil::BackBufferConfiguration bbc;
            bbc.backBufferType = IRenderFrameUtil::BackBufferConfiguration::BackBufferType::GPU_IMAGE_BUFFER_COPY;
            bbc.present = false;
            bbc.backBufferHandle = imageHandle_;
            bbc.gpuBufferHandle = bufferHandle_;
            renderContext->GetRenderUtil().GetRenderFrameUtil().SetBackBufferConfiguration(bbc);
        }
        byteArray_ = make_unique<ByteArray>(bufferDesc.byteSize);

        ASSERT_TRUE(byteArray_);
        validTargets = true;
    }

    void SetUp() override
    {
        // Load Engine DLL
        libHandle_ = dlopen(LIB_ENGINE_CORE, RTLD_LAZY);
        ASSERT_TRUE(LoadFunc(
            CORE_NS::CreatePluginRegistry, "_ZN4Core20CreatePluginRegistryERKNS_18PlatformCreateInfoE", libHandle_));
        ASSERT_TRUE(LoadFunc(CORE_NS::GetPluginRegister, "_ZN4Core17GetPluginRegisterEv", libHandle_));
        ASSERT_TRUE(LoadFunc(CORE_NS::IsDebugBuild, "_ZN4Core12IsDebugBuildEv", libHandle_));
        ASSERT_TRUE(LoadFunc(CORE_NS::GetVersion, "_ZN4Core13GetVersionRevEv", libHandle_));

        // Setup
#define TO_STRING(name) #name
#define PLATFORM_PATH_NAME(name) TO_STRING(name)
        CORE_NS::PlatformCreateInfo platformCreateInfo {
            PLATFORM_PATH_NAME(PLATFORM_CORE_ROOT_PATH),
            PLATFORM_PATH_NAME(PLATFORM_CORE_PLUGIN_PATH),
            PLATFORM_PATH_NAME(PLATFORM_APP_ROOT_PATH),
            PLATFORM_PATH_NAME(PLATFORM_APP_PLUGIN_PATH)
        };
#undef TO_STRING
#undef PLATFORM_PATH_NAME

        CreatePluginRegistry(platformCreateInfo);

        auto factory = CORE_NS::GetInstance<CORE_NS::IEngineFactory>(CORE_NS::UID_ENGINE_FACTORY);
        engine = factory->Create({ platformCreateInfo, { "GsApiTests", 0, 1, 0 }, {} });
        engine->Init();

        registerTestFileProtocol(engine->GetFileManager());

        constexpr BASE_NS::Uid plugins[] = {
            RENDER_NS::UID_RENDER_PLUGIN,
            CORE3D_NS::UID_3D_PLUGIN,
            SCENE_NS::UID_SCENE_PLUGIN, // GsNode
            GS::UID_GS_PLUGIN,
        };
        ASSERT_TRUE(CORE_NS::GetPluginRegister().LoadPlugins(plugins));

        renderContext = BASE_NS::refcnt_ptr<RENDER_NS::IRenderContext>(
            engine->GetInterface<CORE_NS::IClassFactory>()->CreateInstance(RENDER_NS::UID_RENDER_CONTEXT));

        RENDER_NS::DeviceCreateInfo deviceCreateInfo;
#if RENDER_HAS_VULKAN_BACKEND
        RENDER_NS::BackendExtraVk vkExtra;
        deviceCreateInfo.backendType = RENDER_NS::DeviceBackendType::VULKAN;
        deviceCreateInfo.backendConfiguration = &vkExtra;
#elif RENDER_HAS_GLES_BACKEND
        RENDER_NS::BackendExtraGLES glesExtra;
        glesExtra.applicationContext = EGL_NO_CONTEXT;
        glesExtra.sharedContext = EGL_NO_CONTEXT;
        glesExtra.MSAASamples = 0;
        glesExtra.depthBits = 24; // 24: bits of depth
        deviceCreateInfo.backendType = RENDER_NS::DeviceBackendType::OPENGLES;
        deviceCreateInfo.backendConfiguration = &glesExtra;
#elif RENDER_HAS_GL_BACKEND
        RENDER_NS::BackendExtraGL glExtra;
        glExtra.MSAASamples = 0;
        glExtra.depthBits = 24; // 24: bits of depth
        glExtra.alphaBits = 8; // 8: bits of alpha
        glExtra.stencilBits = 0;
        deviceCreateInfo.backendType = RENDER_NS::DeviceBackendType::OPENGL;
        deviceCreateInfo.backendConfiguration = &glExtra;
#endif
        auto rrc = renderContext->Init({ { "GsTUnitTests", 1, 0, 0 }, deviceCreateInfo });
        ASSERT_EQ(rrc, RenderResultCode::RENDER_SUCCESS);

        graphicsContext = CORE_NS::CreateInstance<CORE3D_NS::IGraphicsContext>(
            *renderContext->GetInterface<CORE_NS::IClassFactory>(), CORE3D_NS::UID_GRAPHICS_CONTEXT);
    }

    void TearDown() override
    {
        renderContext->GetDevice().DestroySwapchain();

        imageHandle_ = {};
        bufferHandle_ = {};

        renderContext->GetRenderer().RenderFrame({});

        graphicsContext = nullptr;
        renderContext = nullptr;
        ecs = nullptr;
        engine = nullptr;
        validTargets = false;

        // dlclose(libHandle_);
        // if (libHandle_) {
        //     dlclose(libHandle_);
        //     libHandle_ = nullptr;
        // }
    }
};

/**
 * @tc.name: EnvTest
 * @tc.desc: Verify EnvTest
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, EnvTest, TestSize.Level1)
{
    ASSERT_TRUE(graphicsContext);
}

/**
 * @tc.name: GsBasicTest01
 * @tc.desc: Verify GsBasicTest01
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsBasicTest01, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();
        
        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;

            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    Entity gsEntity;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://banana.ply";

            data->useFadeAnimation = false;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = true;
            data->computeLods = false;
            data->gsUseCropBox = true;
            data->cropBoxAABBMax = { 2.0, 2.0, 2.0 };
            data->cropBoxAABBMin = { -2.0, -2.0, -2.0 };
        }
    }
    // load GS data
    CORE3D_NS::ISceneNode* splatsNode2;
    Entity gsEntity2;
    {
        splatsNode2 = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode2);
        splatsNode2->SetParent(*rootNode);
        gsEntity2 = splatsNode2->GetEntity();
        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity2);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity2));

            data->gsPath = "assets://panda.ply";
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);
    AwaitAsyncGsLoad(gsEntity2);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 2);

    ASSERT_TRUE(primitives[0].isValid);
    ASSERT_TRUE(primitives[1].isValid);
    ASSERT_EQ(primitives[0].shDegree, 3);
    ASSERT_EQ(primitives[1].shDegree, 0);

    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_FALSE(renderData->gaussianIndices.empty());
    ASSERT_FALSE(renderData->gaussianPositions.empty());

    ASSERT_FALSE(renderData->areResourcesLoaded);

    CreateTargets();

    RenderFrame();

    Entity cameraEntity2;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity2 = sceneUtil.CreateCamera(*ecs, Math::Vec3(5, 5, 5), Math::Quat(1, 0, 0, 0), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity2);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle2 = cameraManager->Write(cameraEntity2); cameraHandle2) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle2;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;

            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity2, { 512, 512 }, true, yfov, 1.0f);
    }
    const bool needRender2 = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender2);
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    nodeSystem->DestroyNode(*splatsNode);
    nodeSystem->DestroyNode(*splatsNode2);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsLodTest01
 * @tc.desc: Verify GsLodTest01
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsLodTest01, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    Entity gsEntity;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://doll.glb";

            data->useFadeAnimation = true;
            data->gsUseCropBox = true;

            data->useAutomaticCulling = true;

            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;

            data->maxShDegreeOverride = 0;

            data->gsEnableDepthPass = true;
            data->computeLods = true;
        }
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);

    ASSERT_TRUE(primitives[0].isValid);
    ASSERT_TRUE(primitives[0].computeLods);

    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();

    // GPU resources are loaded during render
    ASSERT_FALSE(renderData->areResourcesLoaded);

    ASSERT_FALSE(primitives[0].gsLodLookup.empty());
    ASSERT_LE(primitives[0].numGs1, primitives[0].numGs);

    ASSERT_TRUE(needRender);

    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsBasicTest02
 * @tc.desc: Verify GsBasicTest02
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsBasicTest02, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;

            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    Entity gsEntity;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://no_ply_data.ply";

            data->useFadeAnimation = false;
            data->gsUseCropBox = false;

            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;

            data->maxShDegreeOverride = 3;

            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 0);

    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_TRUE(renderData->gaussianIndices.empty());
    ASSERT_TRUE(renderData->gaussianPositions.empty());

    CreateTargets();

    // render a frame
    RenderFrame();
}

/**
 * @tc.name: GsLodTest02
 * @tc.desc: Verify GsLodTest02
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsLodTest02, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    Entity gsEntity;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://no_glb_data.glb";

            data->useFadeAnimation = true;
            data->gsUseCropBox = true;

            data->useAutomaticCulling = true;

            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;

            data->maxShDegreeOverride = 0;

            data->gsEnableDepthPass = true;
            data->computeLods = true;
        }
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 0);

    ASSERT_TRUE(needRender);
}

/**
 * @tc.name: GsBasicTest03
 * @tc.desc: Verify GsBasicTest03
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsBasicTest03, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;

            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    Entity gsEntity;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);
        splatsNode->SetParent(*rootNode);
        gsEntity = splatsNode->GetEntity();
        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);
            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));
            data->gsPath = "assets://flower.glb";
            data->useFadeAnimation = false;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }
    CORE3D_NS::ISceneNode* splatsNode2;
    Entity gsEntity2;
    {
        splatsNode2 = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode2);
        splatsNode2->SetParent(*rootNode);
        gsEntity2 = splatsNode2->GetEntity();
        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);
            gsComponentManager->Create(gsEntity2);
            ScopedHandle<GS::GsComponent> data2(gsComponentManager->GetData(gsEntity2));
            data2->gsPath = "assets://panda.glb";
            data2->useFadeAnimation = false;
            data2->gsUseCropBox = false;
            data2->useAutomaticCulling = true;
            data2->gsScaleFactor = 2;
            data2->use16bitSurfaceTarget = false;
            data2->maxShDegreeOverride = 3;
            data2->gsEnableDepthPass = false;
            data2->computeLods = false;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);
    AwaitAsyncGsLoad(gsEntity2);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 2);

    ASSERT_TRUE(primitives[0].isValid);
    ASSERT_TRUE(primitives[1].isValid);
    ASSERT_EQ(primitives[0].shDegree, 3);
    ASSERT_EQ(primitives[1].shDegree, 0);

    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_FALSE(renderData->gaussianIndices.empty());
    ASSERT_FALSE(renderData->gaussianPositions.empty());

    ASSERT_FALSE(renderData->areResourcesLoaded);

    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    nodeSystem->DestroyNode(*splatsNode);
    nodeSystem->DestroyNode(*splatsNode2);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsLodTest03
 * @tc.desc: Verify GsLodTest03
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsLodTest03, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    Entity gsEntity;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);
        splatsNode->SetParent(*rootNode);
        gsEntity = splatsNode->GetEntity();
        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);
            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));
            data->gsPath = "assets://flower.glb";
            data->useFadeAnimation = true;
            data->gsUseCropBox = true;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 0;
            data->gsEnableDepthPass = true;
            data->computeLods = true;
        }
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);

    ASSERT_TRUE(primitives[0].isValid);
    ASSERT_TRUE(primitives[0].computeLods);

    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();

    // GPU resources are loaded during render
    ASSERT_FALSE(renderData->areResourcesLoaded);

    ASSERT_FALSE(primitives[0].gsLodLookup.empty());
    ASSERT_LE(primitives[0].numGs1, primitives[0].numGs);

    ASSERT_TRUE(needRender);

    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsBasicTest04
 * @tc.desc: Verify GsBasicTest04
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsBasicTest04, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;

            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }
    // load GS data
    CORE3D_NS::ISceneNode* splatsNode = nullptr;
    CORE3D_NS::ISceneNode* splatsNode1 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode2 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode3 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode4 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode5 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode6 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode7 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode8 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode9 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode10 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode11 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode12 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode13 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode14 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode15 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode16 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode17 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode18 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode19 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode20 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode21 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode22 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode23 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode24 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode25 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode26 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode27 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode28 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode29 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode30 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode31 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode32 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode33 = nullptr;
    CORE3D_NS::ISceneNode* splatsNode34 = nullptr;
    CreateGSNode(nodeSystem, rootNode, splatsNode, "assets://attack.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode1, "assets://attack1.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode2, "assets://attack2.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode3, "assets://attack3.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode4, "assets://attack4.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode5, "assets://attack5.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode6, "assets://attack6.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode7, "assets://attack7.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode8, "assets://attack8.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode9, "assets://attack9.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode10, "assets://attack10.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode11, "assets://attack11.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode12, "assets://attack12.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode13, "assets://attack13.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode14, "assets://attack14.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode15, "assets://attack15.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode16, "assets://attack16.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode17, "assets://attack17.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode18, "assets://attack18.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode19, "assets://attack19.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode20, "assets://attack20.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode21, "assets://attack21.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode22, "assets://attack22.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode23, "assets://attack.ply");
    CreateGSNode(nodeSystem, rootNode, splatsNode24, "assets://attack_d0.ply");
    CreateGSNode(nodeSystem, rootNode, splatsNode25, "assets://attack_d1.ply");
    CreateGSNode(nodeSystem, rootNode, splatsNode26, "assets://attack_d2.ply");
    CreateGSNode(nodeSystem, rootNode, splatsNode27, "assets://attack_d3.ply");
    CreateGSNode(nodeSystem, rootNode, splatsNode28, "assets://attack_d4.ply");
    CreateGSNode(nodeSystem, rootNode, splatsNode29, "assets://attack23.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode30, "assets://attack24.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode31, "assets://attack25.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode32, "assets://attack26.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode33, "assets://attack27.glb");
    CreateGSNode(nodeSystem, rootNode, splatsNode34, "assets://attack28.glb");

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    BASE_NS::vector<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 0);
}

/**
 * @tc.name: GsBasicTest05
 * @tc.desc: Verify GsBasicTest05
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsBasicTest05, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    Entity gsEntity;
    {
        // load mp4
        IClassRegister* classRegister = renderContext->GetInterface<IClassRegister>();
        ASSERT_TRUE(classRegister);
        auto loader = GetInstance<CORE3D_NS::ISceneLoader>(*classRegister, GS::UID_GS_LOADER);
        ASSERT_TRUE(loader);
        auto loadResult = loader->GetInterface<GS::IGsLoader>()->Load("assets://2408944_381654.mp4", 2408944u);
        ASSERT_FALSE(loadResult.error);
        auto importer = loader->CreateSceneImporter(*ecs);
        ASSERT_TRUE(importer);
        importer->ImportResources(
            loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
        importer->Execute(0u);
        importer->Cancel();
        importer->IsCompleted();
        const auto& importResult = importer->GetResult();
        ASSERT_FALSE(importResult.error);
        gsEntity = importer->ImportScene(0, rootNode->GetEntity());
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);
    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);
    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_TRUE(needRender);
    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsBasicTest06
 * @tc.desc: Verify GsBasicTest06
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsBasicTest06, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    {   
        // load mp4
        auto classRegister = renderContext->GetInterface<Core::IClassRegister>();
        auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
        auto scenceLoader = loader->GetInterface<CORE3D_NS::ISceneLoader>();
        auto gsloader = loader->GetInterface<GS::IGsLoader>();
        auto loadResult = gsloader->Load("assets://xxxxxxxx.mp4", 2408944u);
        auto importer = scenceLoader->CreateSceneImporter(*(ecs.get()));
        importer->ImportResources(loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);

        Entity entity;
        entity = importer->ImportScene(0, entity);
        splatsNode = nodeSystem->GetNode(entity);
        ASSERT_EQ(splatsNode, nullptr);
    }
    CORE3D_NS::ISceneNode* splatsNode2;
    {   
        // load mp4
        auto classRegister = renderContext->GetInterface<Core::IClassRegister>();
        auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
        auto scenceLoader = loader->GetInterface<CORE3D_NS::ISceneLoader>();
        auto gsloader = loader->GetInterface<GS::IGsLoader>();
        auto loadResult = gsloader->Load("assets://xxxxxxxx.mp4", 2408944u);
        auto importer = scenceLoader->CreateSceneImporter(*(ecs.get()));
        importer->ImportResources(loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);

        Entity entity = importer->ImportScene(0);
        splatsNode2 = nodeSystem->GetNode(entity);
        ASSERT_EQ(splatsNode2, nullptr);
    }
    CORE3D_NS::ISceneNode* splatsNode3;
    {   
        // load mp4
        auto classRegister = renderContext->GetInterface<Core::IClassRegister>();
        auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
        auto scenceLoader = loader->GetInterface<CORE3D_NS::ISceneLoader>();
        auto gsloader = loader->GetInterface<GS::IGsLoader>();
        auto loadResult = gsloader->Load("assets://xxxxxxxx.mp4", 2408944u);
        auto importer = scenceLoader->CreateSceneImporter(*(ecs.get()));
        importer->ImportResources(loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);

        Entity entity = importer->ImportScene(0, CORE3D_NS::SceneImportFlagBits::CORE_IMPORT_COMPONENT_FLAG_BITS_ALL);
        splatsNode3 = nodeSystem->GetNode(entity);
        ASSERT_EQ(splatsNode3, nullptr);
    }

    auto* ecsRawPtr = ecs.get();
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsPickerTest01
 * @tc.desc: Verify GsPickerTest01
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsPickerTest01, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);
    Entity gsEntity;
    // load GS data
    {   
        // load mp4
        auto classRegister = renderContext->GetInterface<Core::IClassRegister>();
        ASSERT_TRUE(classRegister);
        auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
        ASSERT_TRUE(loader);
        auto scenceLoader = loader->GetInterface<CORE3D_NS::ISceneLoader>();
        ASSERT_TRUE(scenceLoader);
        auto gsloader = loader->GetInterface<GS::IGsLoader>();
        ASSERT_TRUE(gsloader);
        auto loadResult = gsloader->Load("assets://banana.ply", 0u);
        ASSERT_FALSE(loadResult.error);
        auto importer = scenceLoader->CreateSceneImporter(*(ecs.get()));
        ASSERT_TRUE(importer);
        importer->ImportResources(loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
        importer->Execute(0u);
        importer->Cancel();
        importer->IsCompleted();
        gsEntity = importer->ImportScene(0, nodeSystem->GetRootNode().GetEntity());
        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);
            // gsComponentManager->Create(entity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));
            data->useFadeAnimation = true;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 1, 7), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);
    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);
    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_TRUE(needRender);
    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    // nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));

    // render another frame
    RenderFrame();

    auto fac = renderContext->GetInterface<CORE_NS::IClassRegister>();
    ASSERT_TRUE(fac);
    auto picking = static_cast<CORE3D_NS::IPicking*>(fac->GetInstance(GS::UID_GS_PICKING));
    ASSERT_TRUE(picking);
    auto picker = CORE3D_NS::IPicking::Ptr(picking);
    ASSERT_TRUE(picker);
    BASE_NS::vector<CORE3D_NS::RayCastResult> result = picker->RayCastFromCamera(*ecs, cameraEntity, {0, 0});
    ASSERT_TRUE(result.empty());
}

/**
 * @tc.name: GsPickerTest02
 * @tc.desc: Verify GsPickerTest02
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsPickerTest02, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);
    Entity gsEntity;
    // load GS data
    {   
        // load mp4
        auto classRegister = renderContext->GetInterface<Core::IClassRegister>();
        ASSERT_TRUE(classRegister);
        auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
        ASSERT_TRUE(loader);
        auto scenceLoader = loader->GetInterface<CORE3D_NS::ISceneLoader>();
        ASSERT_TRUE(scenceLoader);
        auto gsloader = loader->GetInterface<GS::IGsLoader>();
        ASSERT_TRUE(gsloader);
        auto loadResult = gsloader->Load("assets://2408944_381654.mp4", 2408944u);
        ASSERT_FALSE(loadResult.error);
        auto importer = scenceLoader->CreateSceneImporter(*(ecs.get()));
        ASSERT_TRUE(importer);
        importer->ImportResources(loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
        importer->Execute(0u);
        importer->Cancel();
        importer->IsCompleted();
        gsEntity = importer->ImportScene(0, nodeSystem->GetRootNode().GetEntity());
        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);
            // gsComponentManager->Create(entity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));
            data->useFadeAnimation = true;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }

        CORE3D_NS::ITransformComponentManager* transformManager = CORE_NS::GetManager<CORE3D_NS::ITransformComponentManager>(ecsRef);

        if (auto tcomp = transformManager->Write(gsEntity)) {
            tcomp->position = BASE_NS::Math::Vec3(-2.5, 3.5, 1);
            tcomp->rotation = BASE_NS::Math::Euler(BASE_NS::Math::Vec3(-180, 0, 0));
        }
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, BASE_NS::Math::Vec3(0.0f, 2.75f, 3.5f),
                BASE_NS::Math::AngleAxis((Math::DEG2RAD * -25.0f), BASE_NS::Math::Vec3(1.0f, 0.0f, 0.0f)), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);
    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);
    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_TRUE(needRender);
    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    // nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
    auto fac = renderContext->GetInterface<CORE_NS::IClassRegister>();
    ASSERT_TRUE(fac);
    auto picking = static_cast<CORE3D_NS::IPicking*>(fac->GetInstance(GS::UID_GS_PICKING));
    ASSERT_TRUE(picking);
    auto picker = CORE3D_NS::IPicking::Ptr(picking);
    ASSERT_TRUE(picker);
    BASE_NS::vector<CORE3D_NS::RayCastResult> result = picker->RayCastFromCamera(*ecs, cameraEntity, {0.5, 0.5});
    ASSERT_EQ(result.size(), 1);
    ASSERT_EQ(result[0].node, nodeSystem->GetNode(gsEntity));

    // camera - no aspect
    {
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;
            cameraComponent.aspect = 0;
        }
    }

    result = picker->RayCastFromCamera(*ecs, cameraEntity, {0.5, 0.5});
    ASSERT_EQ(result.size(), 1);
    ASSERT_EQ(result[0].node, nodeSystem->GetNode(gsEntity));
}

/**
 * @tc.name: GsPickerTest03
 * @tc.desc: Verify GsPickerTest03
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsPickerTest03, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);
    Entity gsEntity;
    // load GS data
    {   
        // load mp4
        auto classRegister = renderContext->GetInterface<Core::IClassRegister>();
        ASSERT_TRUE(classRegister);
        auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
        ASSERT_TRUE(loader);
        auto scenceLoader = loader->GetInterface<CORE3D_NS::ISceneLoader>();
        ASSERT_TRUE(scenceLoader);
        auto gsloader = loader->GetInterface<GS::IGsLoader>();
        ASSERT_TRUE(gsloader);
        auto loadResult = gsloader->Load("assets://2408944_381654.mp4", 2408944u);
        ASSERT_FALSE(loadResult.error);
        auto importer = scenceLoader->CreateSceneImporter(*(ecs.get()));
        ASSERT_TRUE(importer);
        importer->ImportResources(loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
        importer->Execute(0u);
        importer->Cancel();
        importer->IsCompleted();
        gsEntity = importer->ImportScene(0, nodeSystem->GetRootNode().GetEntity());

        CORE3D_NS::ITransformComponentManager* transformManager = CORE_NS::GetManager<CORE3D_NS::ITransformComponentManager>(ecsRef);
        if (auto tcomp = transformManager->Write(gsEntity)) {
            tcomp->position = BASE_NS::Math::Vec3(-2.5, 3.5, 1);
            tcomp->rotation = BASE_NS::Math::Euler(BASE_NS::Math::Vec3(-180, 0, 0));
        }
    }

    auto CustomizeProjectiveMat = [](const CORE3D_NS::CameraComponent& cameraComponent) -> Math::Mat4X4 {
        float aspect = 1.0f;

        if (cameraComponent.aspect > 0.0f) {
            aspect = cameraComponent.aspect;
        } else if (cameraComponent.renderResolution.y > 0U) {
            aspect = static_cast<float>(cameraComponent.renderResolution.x) /
                static_cast<float>(cameraComponent.renderResolution.y);
        }

        BASE_NS::Math::Mat4X4 persProj = BASE_NS::Math::PerspectiveRhZo(cameraComponent.yFov, aspect, cameraComponent.zNear, cameraComponent.zFar);
        persProj[1][1] *= -1.0f; // Flip Y for Vulkan's right-handed NDC

        return persProj;
    };

    // create a camera - custom proj
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, BASE_NS::Math::Vec3(0.0f, 2.75f, 3.5f),
                BASE_NS::Math::AngleAxis((Math::DEG2RAD * -25.0f), BASE_NS::Math::Vec3(1.0f, 0.0f, 0.0f)), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;
            cameraComponent.projection = CORE3D_NS::CameraComponent::Projection::CUSTOM;
            cameraComponent.customProjectionMatrix = CustomizeProjectiveMat(cameraComponent);
            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);
    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);
    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_TRUE(needRender);
    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    // nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));

    const GS::IGsPicking *picker = GetInstance<GS::IGsPicking>(*renderContext->GetInterface<IClassRegister>(), GS::UID_GS_PICKING);
    ASSERT_TRUE(picker);
    BASE_NS::vector<CORE3D_NS::RayCastResult> result = picker->RayCastFromCamera(*ecs, cameraEntity, {0.5, 0.5});
    ASSERT_EQ(result.size(), 1);
    ASSERT_EQ(result[0].node, nodeSystem->GetNode(gsEntity));
}

/**
 * @tc.name: GsPickerTest04
 * @tc.desc: Verify GsPickerTest04
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsPickerTest04, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);
    Entity gsEntity;
    // load GS data
    {   
        // load mp4
        auto classRegister = renderContext->GetInterface<Core::IClassRegister>();
        ASSERT_TRUE(classRegister);
        auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
        ASSERT_TRUE(loader);
        auto scenceLoader = loader->GetInterface<CORE3D_NS::ISceneLoader>();
        ASSERT_TRUE(scenceLoader);
        auto gsloader = loader->GetInterface<GS::IGsLoader>();
        ASSERT_TRUE(gsloader);
        auto loadResult = gsloader->Load("assets://banana.ply", 0u);
        ASSERT_FALSE(loadResult.error);
        auto importer = scenceLoader->CreateSceneImporter(*(ecs.get()));
        ASSERT_TRUE(importer);
        importer->ImportResources(loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
        importer->Execute(0u);
        importer->Cancel();
        importer->IsCompleted();
        gsEntity = importer->ImportScene(0, nodeSystem->GetRootNode().GetEntity());

        CORE3D_NS::ITransformComponentManager* transformManager = CORE_NS::GetManager<CORE3D_NS::ITransformComponentManager>(ecsRef);
        if (auto tcomp = transformManager->Write(gsEntity)) {
            tcomp->position = BASE_NS::Math::Vec3(-2.5, 3.5, 1);
            tcomp->rotation = BASE_NS::Math::Euler(BASE_NS::Math::Vec3(-180, 0, 0));
        }
    }

    // create a camera - ortho
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, BASE_NS::Math::Vec3(0.0f, 2.75f, 3.5f),
                BASE_NS::Math::AngleAxis((Math::DEG2RAD * -25.0f), BASE_NS::Math::Vec3(1.0f, 0.0f, 0.0f)), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;
            cameraComponent.projection = CORE3D_NS::CameraComponent::Projection::ORTHOGRAPHIC;
            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);
    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);
    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_TRUE(needRender);
    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    // nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));

    auto fac = renderContext->GetInterface<CORE_NS::IClassRegister>();
    ASSERT_TRUE(fac);
    auto picking = static_cast<CORE3D_NS::IPicking*>(fac->GetInstance(GS::UID_GS_PICKING));
    ASSERT_TRUE(picking);
    auto picker = CORE3D_NS::IPicking::Ptr(picking);
    ASSERT_TRUE(picker);
    BASE_NS::vector<CORE3D_NS::RayCastResult> result = picker->RayCastFromCamera(*ecs, cameraEntity, {0, 0});
    ASSERT_TRUE(result.empty());

    // camera - no aspect
    {
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;
            cameraComponent.aspect = 0;
        }
    }

    result = picker->RayCastFromCamera(*ecs, cameraEntity, {0, 0});
    ASSERT_TRUE(result.empty());

    // camera - frustrum
    {
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;
            cameraComponent.projection = CORE3D_NS::CameraComponent::Projection::FRUSTUM;
            cameraComponent.xOffset = 512.0;
            cameraComponent.yOffset = 512.0;
        }
    }

    result = picker->RayCastFromCamera(*ecs, cameraEntity, {0, 0});
    ASSERT_TRUE(result.empty());
}

/**
 * @tc.name: GsRetroEffectTest
 * @tc.desc: Verify GsRetroEffectTest
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsRetroEffectTest, TestSize.Level1)
{
    Init();
    Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    Entity gsEntity;
    CORE3D_NS::ISceneNode* splatsNode;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://banana.ply";

            data->useFadeAnimation = false;
            data->gsUseCropBox = false;

            data->useAutomaticCulling = false;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }

    // effect - retro
    {
        CORE_NS::IComponentManager* effectComponentMgr = ecs->GetComponentManager(GS::IGsEffectComponentManager::UID);
        CORE_ASSERT(effectComponentMgr);

            effectComponentMgr->Create(gsEntity);
            ScopedHandle<GS::GsEffectComponent> data(effectComponentMgr->GetData(gsEntity));

            data->retroEffectEnabled = true;
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<RENDER_NS::IRenderDataStoreRenderPostProcesses*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreGsRenderEffects").get());
    ASSERT_TRUE(dataStore);

    auto postProcessPipelines = dataStore->GetData("GsRenderEffectsName").postProcesses;
    ASSERT_EQ(postProcessPipelines.size(), 1);
    ASSERT_EQ(postProcessPipelines[0].id, 0u);

    // create a camera
    Entity cameraEntity;
    {
        Entity postEffectEntity = ecsRef.GetEntityManager().Create();
        CORE3D_NS::IPostProcessEffectComponentManager* postEffectComponentManager = CORE_NS::GetManager<CORE3D_NS::IPostProcessEffectComponentManager>(ecsRef);
        ASSERT_TRUE(postEffectComponentManager);
        postEffectComponentManager->Create(postEffectEntity);
        ScopedHandle<CORE3D_NS::PostProcessEffectComponent> data(postEffectComponentManager->GetData(postEffectEntity));
        data->effects.push_back(postProcessPipelines[0].postProcess);

        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 1.0f };
            cameraComponent.postProcess = postEffectEntity;
            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }
    engine->TickFrame(array_view(&ecsRawPtr, 1));
    CreateTargets();

    RenderFrame();

    nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsComicEffectTest
 * @tc.desc: Verify GsComicEffectTest
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsComicEffectTest, TestSize.Level1)
{
    Init();
    Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    Entity gsEntity;
    CORE3D_NS::ISceneNode* splatsNode;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://banana.ply";

            data->useFadeAnimation = false;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }

    // effect - comic
    {
        CORE_NS::IComponentManager*effectComponentMgr = ecs->GetComponentManager(GS::IGsEffectComponentManager::UID);
        CORE_ASSERT(effectComponentMgr);

            effectComponentMgr->Create(gsEntity);
            ScopedHandle<GS::GsEffectComponent> data(effectComponentMgr->GetData(gsEntity));

            data->comicEffectEnabled = true;
    }
    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<RENDER_NS::IRenderDataStoreRenderPostProcesses*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreGsRenderEffects").get());
    ASSERT_TRUE(dataStore);

    auto postProcessPipelines = dataStore->GetData("GsRenderEffectsName").postProcesses;
    ASSERT_EQ(postProcessPipelines.size(), 1);
    ASSERT_EQ(postProcessPipelines[0].id, 1u);

    // create a camera
    Entity cameraEntity;
    {
        Entity postEffectEntity = ecsRef.GetEntityManager().Create();
        CORE3D_NS::IPostProcessEffectComponentManager* postEffectComponentManager = CORE_NS::GetManager<CORE3D_NS::IPostProcessEffectComponentManager>(ecsRef);
        ASSERT_TRUE(postEffectComponentManager);
        postEffectComponentManager->Create(postEffectEntity);
        ScopedHandle<CORE3D_NS::PostProcessEffectComponent> data(postEffectComponentManager->GetData(postEffectEntity));
        data->effects.push_back(postProcessPipelines[0].postProcess);

        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 1.0f };
            cameraComponent.postProcess = postEffectEntity;
            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }
    engine->TickFrame(array_view(&ecsRawPtr, 1));
    CreateTargets();

    RenderFrame();

    nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsWaterColorEffectTest
 * @tc.desc: Verify GsWaterColorEffectTest
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsWaterColorEffectTest, TestSize.Level1)
{
    Init();
    Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    Entity gsEntity;
    CORE3D_NS::ISceneNode* splatsNode;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://banana.ply";

            data->useFadeAnimation = false;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }

    // effect - watercolor
    {
        CORE_NS::IComponentManager*effectComponentMgr = ecs->GetComponentManager(GS::IGsEffectComponentManager::UID);
        CORE_ASSERT(effectComponentMgr);

            effectComponentMgr->Create(gsEntity);
            ScopedHandle<GS::GsEffectComponent> data(effectComponentMgr->GetData(gsEntity));

            data->watercolorEffectEnabled = true;
    }
    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<RENDER_NS::IRenderDataStoreRenderPostProcesses*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreGsRenderEffects").get());
    ASSERT_TRUE(dataStore);

    auto postProcessPipelines = dataStore->GetData("GsRenderEffectsName").postProcesses;
    ASSERT_EQ(postProcessPipelines.size(), 1);
    ASSERT_EQ(postProcessPipelines[0].id, 2u);

    // create a camera
    Entity cameraEntity;
    {
        Entity postEffectEntity = ecsRef.GetEntityManager().Create();
        CORE3D_NS::IPostProcessEffectComponentManager* postEffectComponentManager = CORE_NS::GetManager<CORE3D_NS::IPostProcessEffectComponentManager>(ecsRef);
        ASSERT_TRUE(postEffectComponentManager);
        postEffectComponentManager->Create(postEffectEntity);
        ScopedHandle<CORE3D_NS::PostProcessEffectComponent> data(postEffectComponentManager->GetData(postEffectEntity));
        data->effects.push_back(postProcessPipelines[0].postProcess);

        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 1.0f };
            cameraComponent.postProcess = postEffectEntity;
            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }
    engine->TickFrame(array_view(&ecsRawPtr, 1));
    CreateTargets();

    RenderFrame();

    nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsObraDinnEffectTest
 * @tc.desc: Verify GsObraDinnEffectTest
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsObraDinnEffectTest, TestSize.Level1)
{
    Init();
    Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    Entity gsEntity;
    CORE3D_NS::ISceneNode* splatsNode;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://banana.ply";
            data->useFadeAnimation = false;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }

    // effect - obra-dinn
    {
        CORE_NS::IComponentManager*effectComponentMgr = ecs->GetComponentManager(GS::IGsEffectComponentManager::UID);
        CORE_ASSERT(effectComponentMgr);

            effectComponentMgr->Create(gsEntity);
            ScopedHandle<GS::GsEffectComponent> data(effectComponentMgr->GetData(gsEntity));

            data->obradinnEffectEnabled = true;
    }
    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<RENDER_NS::IRenderDataStoreRenderPostProcesses*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreGsRenderEffects").get());
    ASSERT_TRUE(dataStore);

    auto postProcessPipelines = dataStore->GetData("GsRenderEffectsName").postProcesses;
    ASSERT_EQ(postProcessPipelines.size(), 1);
    ASSERT_EQ(postProcessPipelines[0].id, 3u);

    // create a camera
    Entity cameraEntity;
    {
        Entity postEffectEntity = ecsRef.GetEntityManager().Create();
        CORE3D_NS::IPostProcessEffectComponentManager* postEffectComponentManager = CORE_NS::GetManager<CORE3D_NS::IPostProcessEffectComponentManager>(ecsRef);
        ASSERT_TRUE(postEffectComponentManager);
        postEffectComponentManager->Create(postEffectEntity);
        ScopedHandle<CORE3D_NS::PostProcessEffectComponent> data(postEffectComponentManager->GetData(postEffectEntity));
        data->effects.push_back(postProcessPipelines[0].postProcess);

        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 1.0f };
            cameraComponent.postProcess = postEffectEntity;
            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }
    engine->TickFrame(array_view(&ecsRawPtr, 1));
    CreateTargets();

    RenderFrame();

    nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

/**
 * @tc.name: GsColorEditEffectTest
 * @tc.desc: Verify GsColorEditEffectTest
 * @tc.type: FUNC
 */
HWTEST_F(PluginGSTest, GsColorEditEffectTest, TestSize.Level1)
{
    Init();
    Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    Entity gsEntity;
    CORE3D_NS::ISceneNode* splatsNode;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://banana.ply";
            data->useFadeAnimation = false;
            data->gsUseCropBox = false;
            data->useAutomaticCulling = true;
            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;
            data->maxShDegreeOverride = 3;
            data->gsEnableDepthPass = false;
            data->computeLods = false;
        }
    }

    // effect - color edit
    {
        CORE_NS::IComponentManager*effectComponentMgr = ecs->GetComponentManager(GS::IGsEffectComponentManager::UID);
        CORE_ASSERT(effectComponentMgr);

            effectComponentMgr->Create(gsEntity);
            ScopedHandle<GS::GsEffectComponent> data(effectComponentMgr->GetData(gsEntity));

            data->colorEditingEnabled = true;
    }
    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    ASSERT_TRUE(needRender);

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<RENDER_NS::IRenderDataStoreRenderPostProcesses*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreGsRenderEffects").get());
    ASSERT_TRUE(dataStore);

    auto postProcessPipelines = dataStore->GetData("GsRenderEffectsName").postProcesses;
    ASSERT_EQ(postProcessPipelines.size(), 1);
    ASSERT_EQ(postProcessPipelines[0].id, 4u);

    // create a camera
    Entity cameraEntity;
    {
        Entity postEffectEntity = ecsRef.GetEntityManager().Create();
        CORE3D_NS::IPostProcessEffectComponentManager* postEffectComponentManager = CORE_NS::GetManager<CORE3D_NS::IPostProcessEffectComponentManager>(ecsRef);
        ASSERT_TRUE(postEffectComponentManager);
        postEffectComponentManager->Create(postEffectEntity);
        ScopedHandle<CORE3D_NS::PostProcessEffectComponent> data(postEffectComponentManager->GetData(postEffectEntity));
        data->effects.push_back(postProcessPipelines[0].postProcess);

        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        float yfov = 0.0f;
        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 1.0f };
            cameraComponent.postProcess = postEffectEntity;
            yfov = cameraComponent.yFov;
        }

        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, true, yfov, 1.0f);
    }
    engine->TickFrame(array_view(&ecsRawPtr, 1));
    CreateTargets();

    RenderFrame();

    engine->TickFrame(array_view(&ecsRawPtr, 1));
    RenderFrame();

    nodeSystem->DestroyNode(*splatsNode);
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}


HWTEST_F(PluginGSTest, GsCameraMotion2dTest, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();
    renderConfigMgr->Create(rootEntity);

    Entity gsEntity;
    {
        // load mp4
        IClassRegister* classRegister = renderContext->GetInterface<IClassRegister>();
        if (!classRegister) {
            std::cout << "ERROR: classRegister is null" << std::endl;
        }
        ASSERT_TRUE(classRegister);
        auto loader = GetInstance<CORE3D_NS::ISceneLoader>(*classRegister, GS::UID_GS_LOADER);
        if (!loader) {
            std::cout << "ERROR: loader is null" << std::endl;
        }
        ASSERT_TRUE(loader);
        auto loadResult = loader->GetInterface<GS::IGsLoader>()->Load("assets://2408944_381654.mp4", 2408944u);
        std::cout << "loadResult.error: " << loadResult.error << std::endl;
        ASSERT_FALSE(loadResult.error);
        auto importer = loader->CreateSceneImporter(*ecs);
        if (!importer) {
            std::cout << "ERROR: importer is null" << std::endl;
        }
        ASSERT_TRUE(importer);
        importer->ImportResources(
            loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
        importer->Execute(0u);
        importer->Cancel();
        importer->IsCompleted();
        const auto& importResult = importer->GetResult();
        std::cout << "importResult.error: " << importResult.error << std::endl;
        ASSERT_FALSE(importResult.error);
        gsEntity = importer->ImportScene(0, rootNode->GetEntity());
        std::cout << "gsEntity created, entity valid: " << CORE_NS::EntityUtil::IsValid(gsEntity) << std::endl;
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;
            
            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));
    std::cout << "needRender: " << needRender << std::endl;
    ASSERT_TRUE(needRender);
    std::cout << "Before AwaitAsyncGsLoad, gsEntity: "  << std::endl;
    AwaitAsyncGsLoad(gsEntity);

    CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
    ASSERT_TRUE(gsComponentManager);
    ASSERT_TRUE(gsComponentManager->HasComponent(gsEntity));


    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);
    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);

    ASSERT_TRUE(needRender);
    CreateTargets();
    RenderFrame();



    engine->TickFrame(array_view(&ecsRawPtr, 1));
}



HWTEST_F(PluginGSTest, GsCameraTest01, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    Entity gsEntity;
    {
        // load mp4
        IClassRegister* classRegister = renderContext->GetInterface<IClassRegister>();
        if (!classRegister) {
            std::cout << "ERROR: classRegister is null" << std::endl;
        }
        ASSERT_TRUE(classRegister);
        auto loader = GetInstance<CORE3D_NS::ISceneLoader>(*classRegister, GS::UID_GS_LOADER);
        if (!loader) {
            std::cout << "ERROR: loader is null" << std::endl;
        }
        ASSERT_TRUE(loader);
        auto loadResult = loader->GetInterface<GS::IGsLoader>()->Load("assets://2408944_381654.mp4", 2408944u);
        std::cout << "loadResult.error: " << loadResult.error << std::endl;
        ASSERT_FALSE(loadResult.error);
        auto importer = loader->CreateSceneImporter(*ecs);
        if (!importer) {
            std::cout << "ERROR: importer is null" << std::endl;
        }
        ASSERT_TRUE(importer);
        importer->ImportResources(
            loadResult.data, CORE3D_NS::ResourceImportFlagBits::CORE_IMPORT_RESOURCE_FLAG_BITS_ALL);
        importer->Execute(0u);
        importer->Cancel();
        importer->IsCompleted();
        const auto& importResult = importer->GetResult();
        std::cout << "importResult.error: " << importResult.error << std::endl;
        ASSERT_FALSE(importResult.error);
        gsEntity = importer->ImportScene(0, rootNode->GetEntity());
        std::cout << "gsEntity created, entity valid: " << CORE_NS::EntityUtil::IsValid(gsEntity) << std::endl;
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);

        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 512, 512 }, 1.0f, 60.0f, 1.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);
    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);
    const GS::GsRenderData* renderData = dataStore->GetGsRenderData();
    ASSERT_TRUE(needRender);
    CreateTargets();

    // render a frame
    RenderFrame();

    ASSERT_TRUE(renderData->areResourcesLoaded);

    engine->TickFrame(array_view(&ecsRawPtr, 1));
}


HWTEST_F(PluginGSTest, GsLodTest02Camera, TestSize.Level1)
{
    Init();
    const Core::IEcs& ecsRef = *(ecs.get());

    CORE3D_NS::INodeSystem* nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecsRef);
    ASSERT_TRUE(nodeSystem);

    auto renderConfigMgr = CORE_NS::GetManager<CORE3D_NS::IRenderConfigurationComponentManager>(ecsRef);
    auto cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecsRef);

    CORE3D_NS::ISceneNode* rootNode = nodeSystem->CreateNode();
    ASSERT_TRUE(rootNode);

    CORE_NS::Entity rootEntity = rootNode->GetEntity();

    renderConfigMgr->Create(rootEntity);

    // load GS data
    CORE3D_NS::ISceneNode* splatsNode;
    Entity gsEntity;
    {
        splatsNode = nodeSystem->CreateNode();
        ASSERT_TRUE(splatsNode);

        splatsNode->SetParent(*rootNode);

        gsEntity = splatsNode->GetEntity();

        {
            CORE_NS::IComponentManager* gsComponentManager = ecs->GetComponentManager(GS::IGsComponentManager::UID);
            CORE_ASSERT(gsComponentManager);

            gsComponentManager->Create(gsEntity);
            ScopedHandle<GS::GsComponent> data(gsComponentManager->GetData(gsEntity));

            data->gsPath = "assets://panda.glb";

            data->useFadeAnimation = true;
            data->gsUseCropBox = true;

            data->useAutomaticCulling = true;

            data->gsScaleFactor = 2;
            data->use16bitSurfaceTarget = false;

            data->maxShDegreeOverride = 0;

            data->gsEnableDepthPass = true;
            data->computeLods = true;
        }
        

        auto tmGS = GetManager<CORE3D_NS::ITransformComponentManager>(ecsRef);
        ASSERT_TRUE(tmGS);
        if (auto tcomp = tmGS->Write(gsEntity)) {
            tcomp->position = BASE_NS::Math::Vec3(-2.5, 3.5, 1);
            tcomp->rotation = BASE_NS::Math::Euler(BASE_NS::Math::Vec3(-180, 0, 0));
        }
    }

    // create a camera
    Entity cameraEntity;
    {
        const auto& sceneUtil = graphicsContext->GetSceneUtil();

        CORE3D_NS::INameComponentManager* nameManager = CORE_NS::GetManager<CORE3D_NS::INameComponentManager>(ecsRef);
        ASSERT_TRUE(nameManager);
        
        cameraEntity = sceneUtil.CreateCamera(*ecs, Math::Vec3(0, 0, 0), Math::Quat(0, 0, 0, 1), 0.1f, 100.0f, 60.0f);
        nameManager->Set(cameraEntity, { "MainCamera" });
        
        sceneUtil.UpdateCameraViewport(*ecs, cameraEntity, { 1080, 1920 }, true, 1.04716f, 3.0f);

        auto node = nodeSystem->GetNode(cameraEntity);
        node->SetParent(*rootNode);

        if (auto cameraHandle = cameraManager->Write(cameraEntity); cameraHandle) {
            CORE3D_NS::CameraComponent& cameraComponent = *cameraHandle;

            cameraComponent.sceneFlags |= CORE3D_NS::CameraComponent::SceneFlagBits::MAIN_CAMERA_BIT;
            cameraComponent.pipelineFlags |= CORE3D_NS::CameraComponent::PipelineFlagBits::CLEAR_COLOR_BIT |
                                             CORE3D_NS::CameraComponent::PipelineFlagBits::DEPTH_OUTPUT_BIT;
            cameraComponent.renderingPipeline = CORE3D_NS::CameraComponent::RenderingPipeline::LIGHT_FORWARD;
            cameraComponent.clearColorValue = { 0.0f, 0.0f, 0.0f, 0.0f };
            cameraComponent.layerMask = CORE3D_NS::LayerConstants::ALL_LAYER_MASK;
        }
    }

    auto* ecsRawPtr = ecs.get();
    const bool needRender = engine->TickFrame(array_view(&ecsRawPtr, 1));

    AwaitAsyncGsLoad(gsEntity);

    for(int i = 0; i < 3; i++){
        engine->TickFrame(array_view(&ecsRawPtr, 1));
    }

    auto dataStore = static_cast<GS::IRenderDataStoreDefaultGs*>(
        renderContext->GetRenderDataStoreManager().GetRenderDataStore("RenderDataStoreDefaultGs").get());
    ASSERT_TRUE(dataStore);

    const array_view<GS::GsPrimitive> primitives = dataStore->GetGsPrimitives();
    ASSERT_EQ(primitives.size(), 1);
    ASSERT_TRUE(primitives[0].isValid);

    ASSERT_TRUE(needRender);
 
    {
        // GSImageExtractor moved to LumeGSAIEdit module (internal implementation detail)
        // auto* ecsRawPtr = ecs.get();
        // bool extractCalled = false;
        // auto startTime = std::chrono::steady_clock::now();
        // const auto testDuration = std::chrono::seconds(60);
        //
        // while (std::chrono::duration_cast<std::chrono::seconds>(
        //         std::chrono::steady_clock::now() - startTime) < testDuration) {
        //     imageExtractor.preFrame(renderContext);
        //
        //     if (engine->TickFrame(BASE_NS::array_view(&ecsRawPtr, 1))) {
        //         RenderFrame();
        //     }
        //
        //     auto tm = GetManager<CORE3D_NS::ITransformComponentManager>(ecsRef);
        //     tm->Write(cameraEntity);
        //     auto* ges = CORE_NS::GetSystem<GS::IGsAIEditSystem>(*ecs);
        //     if (ges) {
        //         ges->ExtractMainBody(gsEntity, 0.5f, 0.5f);
        //         extractCalled = true;
        //     }
        //
        //     std::this_thread::sleep_for(std::chrono::milliseconds(16));
        // }
        // engine->TickFrame(BASE_NS::array_view(&ecsRawPtr, 1));
        // RenderFrame();
    }
    engine->TickFrame(array_view(&ecsRawPtr, 1));
}

}  // namespace OHOS::Render3D
