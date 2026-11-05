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

#include <3d/ecs/components/render_mesh_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <3d/ecs/systems/intf_render_system.h>
#include <3d/implementation_uids.h>
#include <3d/intf_graphics_context.h>
#include <3d/intf_plugin.h>
#include <3d/util/intf_scene_util.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/ecs/intf_system.h>
#include <core/intf_engine.h>
#include <core/image/intf_image_loader_manager.h>
#include <core/io/intf_file_manager.h>
#include <core/plugin/intf_plugin.h>
#include <core/plugin/intf_plugin_decl.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/datastore/intf_render_data_store_pod.h>
#include <render/device/intf_shader_manager.h>
#include <render/implementation_uids.h>
#include <render/intf_plugin.h>
#include <render/intf_render_context.h>
#include <meta/interface/intf_object_registry.h>

#include "loader/gs_loader.h"
#include "plugin_gs/ecs/components/gs_component.h"
#include "plugin_gs/ecs/components/gs_configuration_component.h"
#include "plugin_gs/ecs/components/gs_effect_component.h"
#include "plugin_gs/ecs/components/gs_tiling_component.h"
#include "plugin_gs/ecs/systems/gs_system.h"
#include "plugin_gs/ecs/systems/intf_gs_edit_system.h"

#ifdef __OHOS_PLATFORM__
#include "plugin_gs/ecs/systems/intf_gs_ai_edit_system.h"
#include "render/GsAIEdit/render_data_store_gs_ai_edit.h"
#include "render/GsAIEdit/render_node_gs_ai_edit_render.h"
#include "render/render_data_store_gs_floater_v2_1.h"
#include "render/render_node_gs_floater_v2_1.h"
#endif

#include "plugin_gs/ecs/systems/intf_gs_effect_system.h"
#include "plugin_gs/ecs/systems/intf_gs_tiling_system.h"
#include "plugin_gs/implementation_uids.h"
#include "plugin_gs/render/intf_render_data_store_default_gs.h"
#include "plugin_gs/render/intf_render_data_store_gs_edit.h"
#include "plugin_gs/util/intf_gs_picking.h"
#include "render/coloredit/render_effects_coloredit_node.h"
#include "render/comic/render_effects_comic_node.h"
#include "render/obradinn/render_effects_obradinn_node.h"
#include "render/render_data_store_default_gs.h"
#include "render/render_data_store_gs_edit.h"
#include "render/render_node_gs_render.h"
#include "render/retro/render_effects_retro_node.h"
#include "render/watercolor/render_effects_watercolor_node.h"
#include "util/gs_picking.h"

#ifdef __OHOS_PLATFORM__
// TODO(LumeApp): ImageKit WebP loader replaces the libwebp one.
#endif

// Rofs Data.
// The standalone LumeApp build embeds the blob as a plain byte array
// (LumeApp/tools/gs_assets/pack_gs_rofs.py); upstream emits an object whose
// symbol convention differs, so the array form is declared here.
extern "C" const unsigned char GS_BIN[];
extern "C" const uint64_t GS_BIN_SIZE;

CORE_BEGIN_NAMESPACE()
#if defined(CORE_PLUGIN) && (CORE_PLUGIN == 1)
IPluginRegister* gPluginRegistry { nullptr };
__attribute__((visibility("default"))) IPluginRegister& GetPluginRegister()
{
    return *gPluginRegistry;
}
#endif
CORE_END_NAMESPACE()

namespace {
static constexpr RENDER_NS::IShaderManager::ShaderFilePathDesc SHADER_FILE_PATHS {
    "gsshaders://",
    "",
    "gspipelinelayouts://",
    "gsvertexinputdeclarations://",
};

#ifdef __OHOS_PLATFORM__
static constexpr CORE_NS::IImageLoaderManager::ImageLoaderTypeInfo WEBP_LOADER {
    { CORE_NS::IImageLoaderManager::ImageLoaderTypeInfo::UID },
    nullptr,
    BASE_NS::Uid { "4171ec62-57ec-47be-bcd2-0d481e27ce69" },
};
#endif
} // namespace

namespace GS {
using CORE_NS::GetName;
using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;

const char* GetVersionInfo();

constexpr string_view ROFS = "gsrofs";
constexpr string_view SHADERS = "gsshaders";
constexpr string_view VIDS = "gsvertexinputdeclarations";
constexpr string_view PIDS = "gspipelinelayouts";
constexpr string_view SSTATES = "gsshaderstates";

constexpr string_view SHADER_PATH = "gsrofs://shaders/";
constexpr string_view VID_PATH = "gsrofs://vertexinputdeclarations/";
constexpr string_view PID_PATH = "gsrofs://pipelinelayouts/";
constexpr string_view SHADER_STATE_PATH = "gsrofs://shaderstates/";

constexpr BASE_NS::Uid PLUGIN_DEPENDENCIES[] = {
    RENDER_NS::UID_RENDER_PLUGIN,
    CORE3D_NS::UID_3D_PLUGIN,
};

IComponentManager* IGSComponentManagerInstance(IEcs&);
ISystem* IGsSystemInstance(IEcs&);
ISystem* IGsEditSystemInstance(IEcs&);
ISystem* IGsAIEditSystemInstance(IEcs&);

void IGSComponentManagerDestroy(IComponentManager*);
void IGsSystemDestroy(ISystem*);
void IGsEditSystemDestroy(ISystem*);
void IGsAIEditSystemDestroy(ISystem*);

IComponentManager* IGsConfigurationComponentManagerInstance(IEcs&);
void IGsConfigurationComponentManagerDestroy(IComponentManager*);

IComponentManager* IGsEffectComponentManagerInstance(IEcs&);
ISystem* IGsEffectSystemInstance(IEcs&);

void IGsEffectComponentManagerDestroy(IComponentManager*);
void IGsEffectSystemDestroy(ISystem*);

IComponentManager* IGsTilingComponentManagerInstance(IEcs&);
void IGsTilingComponentManagerDestroy(IComponentManager*);

ISystem* IGsTilingSystemInstance(IEcs&);
void IGsTilingSystemDestroy(ISystem*);

namespace {
constexpr RenderDataStoreTypeInfo RenderDataDefaultGSTypeInfo = {
    { RenderDataStoreTypeInfo::UID },
    RenderDataStoreDefaultGS::UID,
    RenderDataStoreDefaultGS::TYPE_NAME,
    RenderDataStoreDefaultGS::Create,
};

constexpr RenderDataStoreTypeInfo RenderDataDefaultGsEditTypeInfo = {
    { RenderDataStoreTypeInfo::UID },
    RenderDataStoreGsEdit::UID,
    RenderDataStoreGsEdit::TYPE_NAME,
    RenderDataStoreGsEdit::Create,
};

#ifdef __OHOS_PLATFORM__
constexpr RenderDataStoreTypeInfo RenderDataDefaultGsAIEditTypeInfo = {
    { RenderDataStoreTypeInfo::UID },
    RenderDataStoreGsAIEdit::UID,
    RenderDataStoreGsAIEdit::TYPE_NAME,
    RenderDataStoreGsAIEdit::Create,
};

constexpr RenderDataStoreTypeInfo RenderDataStoreGsFloaterV21TypeInfo = {
    { RenderDataStoreTypeInfo::UID },
    RenderDataStoreGsFloaterV21::UID,
    RenderDataStoreGsFloaterV21::TYPE_NAME,
    RenderDataStoreGsFloaterV21::Create,
};
#endif

constexpr RenderDataStoreTypeInfo gRenderDataStores[] = {
    RenderDataDefaultGSTypeInfo,
    RenderDataDefaultGsEditTypeInfo,
#ifdef __OHOS_PLATFORM__
    RenderDataDefaultGsAIEditTypeInfo,
    RenderDataStoreGsFloaterV21TypeInfo,
#endif
};

constexpr RenderNodeTypeInfo RenderNodeGSRenderTypeInfo = {
    { RenderNodeTypeInfo::UID },
    RenderNodeGsRender::UID,
    RenderNodeGsRender::TYPE_NAME,
    RenderNodeGsRender::Create,
    RenderNodeGsRender::Destroy,
    RenderNodeGsRender::BACKEND_FLAGS,
    RenderNodeGsRender::CLASS_TYPE,
    RenderNodeGsRender::AFTER,
    RenderNodeGsRender::BEFORE,
};

constexpr RenderNodeTypeInfo RenderNodeGsAIEditRenderTypeInfo = {
    { RenderNodeTypeInfo::UID },
    RenderNodeGsAIEditRender::UID,
    RenderNodeGsAIEditRender::TYPE_NAME,
    RenderNodeGsAIEditRender::Create,
    RenderNodeGsAIEditRender::Destroy,
    RenderNodeGsAIEditRender::BACKEND_FLAGS,
    RenderNodeGsAIEditRender::CLASS_TYPE,
    RenderNodeGsAIEditRender::AFTER,
    RenderNodeGsAIEditRender::BEFORE,
};

#ifdef __OHOS_PLATFORM__
constexpr RenderNodeTypeInfo RenderNodeGsFloaterV21TypeInfo = {
    { RenderNodeTypeInfo::UID },
    RenderNodeGsFloaterV21::UID,
    RenderNodeGsFloaterV21::TYPE_NAME,
    RenderNodeGsFloaterV21::Create,
    RenderNodeGsFloaterV21::Destroy,
    RenderNodeGsFloaterV21::BACKEND_FLAGS,
    RenderNodeGsFloaterV21::CLASS_TYPE,
    RenderNodeGsFloaterV21::AFTER,
    RenderNodeGsFloaterV21::BEFORE,
};
#endif

constexpr RenderNodeTypeInfo gRenderNodes[] = {
    RenderNodeGSRenderTypeInfo,
    RenderNodeGsAIEditRenderTypeInfo,
#ifdef __OHOS_PLATFORM__
    RenderNodeGsFloaterV21TypeInfo,
#endif
};

constexpr ComponentManagerTypeInfo GSComponentTypeInfo = {
    { ComponentManagerTypeInfo::UID },
    IGsComponentManager::UID,
    GetName<IGsComponentManager>().data(),
    IGSComponentManagerInstance,
    IGSComponentManagerDestroy,
};

constexpr ComponentManagerTypeInfo GsEffectComponentTypeInfo = {
    { ComponentManagerTypeInfo::UID },
    IGsEffectComponentManager::UID,
    GetName<IGsEffectComponentManager>().data(),
    IGsEffectComponentManagerInstance,
    IGsEffectComponentManagerDestroy,
};

constexpr ComponentManagerTypeInfo GsConfigurationComponentTypeInfo = {
    { ComponentManagerTypeInfo::UID },
    IGsConfigurationComponentManager::UID,
    GetName<IGsConfigurationComponentManager>().data(),
    IGsConfigurationComponentManagerInstance,
    IGsConfigurationComponentManagerDestroy,
};

constexpr ComponentManagerTypeInfo GsTilingComponentTypeInfo = {
    { ComponentManagerTypeInfo::UID },
    IGsTilingComponentManager::UID,
    GetName<IGsTilingComponentManager>().data(),
    IGsTilingComponentManagerInstance,
    IGsTilingComponentManagerDestroy,
};

constexpr ComponentManagerTypeInfo gComponentManagers[] = {
    GSComponentTypeInfo,
    GsEffectComponentTypeInfo,
    GsConfigurationComponentTypeInfo,
    GsTilingComponentTypeInfo,
};

constexpr Uid GSSourceSystemRDeps[] = {
    ITransformComponentManager::UID,
    IGsConfigurationComponentManager::UID,
    IGsComponentManager::UID,
    IRenderMeshComponentManager::UID,
};

constexpr Uid GsEditSourceSystemRDeps[] = {
    ITransformComponentManager::UID,
};

constexpr Uid GsEffectSourceSystemRDeps[] = { IGsEffectComponentManager::UID };

constexpr Uid GsTilingSourceSystemRDeps[] = {
    IGsTilingComponentManager::UID,
    IGsComponentManager::UID,
};

#ifdef __OHOS_PLATFORM__
constexpr Uid GsAIEditSourceSystemRDeps[] = {
    ITransformComponentManager::UID,
};
#endif

constexpr SystemTypeInfo gSystems[] = {
    {
        { SystemTypeInfo::UID },
        IGsSystem::UID,
        GetName<IGsSystem>().data(),
        IGsSystemInstance,
        IGsSystemDestroy,
        {},
        GSSourceSystemRDeps,
        {},
        IRenderPreprocessorSystem::UID,
    },
    {
        { SystemTypeInfo::UID },
        IGsEffectSystem::UID,
        GetName<IGsEffectSystem>().data(),
        IGsEffectSystemInstance,
        IGsEffectSystemDestroy,
        {},
        GsEffectSourceSystemRDeps,
        {},
        IRenderPreprocessorSystem::UID,
    },
    {
        { SystemTypeInfo::UID },
        IGsEditSystem::UID,
        GetName<IGsEditSystem>().data(),
        IGsEditSystemInstance,
        IGsEditSystemDestroy,
        {},
        GsEditSourceSystemRDeps,
        {},
        IRenderPreprocessorSystem::UID,
    },
#ifdef __OHOS_PLATFORM__
    {
        { SystemTypeInfo::UID },
        IGsAIEditSystem::UID,
        GetName<IGsAIEditSystem>().data(),
        IGsAIEditSystemInstance,
        IGsAIEditSystemDestroy,
        {},
        GsAIEditSourceSystemRDeps,
        {},
        IRenderPreprocessorSystem::UID,
    },
#endif
    {
        { SystemTypeInfo::UID },
        IGsTilingSystem::UID,
        GetName<IGsTilingSystem>().data(),
        IGsTilingSystemInstance,
        IGsTilingSystemDestroy,
        {},
        GsTilingSourceSystemRDeps,
        {},
        // run before GsSystem so streaming is up-to-date each tick
        IGsSystem::UID,
    },
};

constexpr InterfaceTypeInfo PostprocessEffects[] = {
    {
        nullptr,
        GsEffects::RenderPostProcessRetroNode::UID,
        "",
        [](IClassFactory&, PluginToken token) -> IInterface* {
            auto ret = new GsEffects::RenderPostProcessRetroNode;
            return ret->GetInterface(IInterface::UID);
        },
        nullptr,
    },
    {
        nullptr,
        GsEffects::RenderPostProcessComicNode::UID,
        "",
        [](IClassFactory&, PluginToken token) -> IInterface* {
            auto ret = new GsEffects::RenderPostProcessComicNode;
            return ret->GetInterface(IInterface::UID);
        },
        nullptr,
    },
    {
        nullptr,
        GsEffects::RenderPostProcessWatercolorNode::UID,
        "",
        [](IClassFactory&, PluginToken token) -> IInterface* {
            auto ret = new GsEffects::RenderPostProcessWatercolorNode;
            return ret->GetInterface(IInterface::UID);
        },
        nullptr,
    },
    {
        nullptr,
        GsEffects::RenderPostProcessObraDinnNode::UID,
        "",
        [](IClassFactory&, PluginToken token) -> IInterface* {
            auto ret = new GsEffects::RenderPostProcessObraDinnNode;
            return ret->GetInterface(IInterface::UID);
        },
        nullptr,
    },
    {
        nullptr,
        GsEffects::RenderPostProcessColorEditNode::UID,
        "",
        [](IClassFactory&, PluginToken token) -> IInterface* {
            auto ret = new GsEffects::RenderPostProcessColorEditNode;
            return ret->GetInterface(IInterface::UID);
        },
        nullptr,
    },
};

struct AgpGsPluginState {
    IRenderContext& renderContext;

    GsPicking picking_;
    GsSceneLoader loader_;
    InterfaceTypeInfo interfaces[2] = {
        InterfaceTypeInfo { this, GS::UID_GS_PICKING, CORE_NS::GetName<IGsPicking>().data(), nullptr,
            [](IClassRegister& registry, PluginToken token) -> IInterface* {
                if (token) {
                    AgpGsPluginState* state = static_cast<AgpGsPluginState*>(token);
                    return &state->picking_;
                }
                return nullptr;
            } },
        InterfaceTypeInfo { this, GS::UID_GS_LOADER, "GsLoader", nullptr,
            [](IClassRegister& registry, PluginToken token) -> IInterface* {
                if (token) {
                    AgpGsPluginState* state = static_cast<AgpGsPluginState*>(token);
                    return static_cast<CORE3D_NS::ISceneLoader*>(&state->loader_);
                }
                return nullptr;
            } },
    };
};

PluginToken Initialize(IRenderContext& context)
{
    AgpGsPluginState* token = new AgpGsPluginState { context, {}, context };

    auto& fm = context.GetEngine().GetFileManager();
    // Create gsrofs:// filesystem that points to embedded asset files.

    auto rofs = fm.CreateROFilesystem(GS_BIN, GS_BIN_SIZE);

    fm.RegisterFilesystem(ROFS, move(rofs));
    // And register it under the needed proxy protocols.
    fm.RegisterPath(SHADERS, SHADER_PATH, false);
    fm.RegisterPath(VIDS, VID_PATH, false);
    fm.RegisterPath(PIDS, PID_PATH, false);
    fm.RegisterPath(SSTATES, SHADER_STATE_PATH, false);

    auto& renderDataStoreManager = context.GetRenderDataStoreManager();
    {
        auto rsp =
            refcnt_ptr<IRenderDataStorePod>(renderDataStoreManager.Create(IRenderDataStorePod::UID, "GSPodStore"));
        if (rsp) {
            struct {
                Math::Vec4 texSizeInvTexSize { 2.f, 2.f, 1.f / 2.f, 1.f / 2.f };
            } config;
            rsp->CreatePod("TexSizeConfig", "TexSizeShaderConfig", arrayviewU8(config));
        }
    }

    if (auto* classRegister = context.GetInterface<CORE_NS::IClassRegister>()) {
        for (const auto& info : PostprocessEffects) {
            classRegister->RegisterInterfaceType(info);
        }

        for (const auto& info : token->interfaces) {
            classRegister->RegisterInterfaceType(info);
        }
    }

    context.GetDevice().GetShaderManager().LoadShaderFiles(SHADER_FILE_PATHS);
    return token;
}

void Uninitialize(PluginToken token)
{
    AgpGsPluginState* gsPluginState = static_cast<AgpGsPluginState*>(token);
    auto context = &gsPluginState->renderContext;

    context->GetDevice().GetShaderManager().UnloadShaderFiles(SHADER_FILE_PATHS);

    if (auto* classRegister = context->GetInterface<CORE_NS::IClassRegister>()) {
        for (const auto& info : PostprocessEffects) {
            classRegister->UnregisterInterfaceType(info);
        }

        for (const auto& info : gsPluginState->interfaces) {
            classRegister->UnregisterInterfaceType(info);
        }
    }

    auto& fm = context->GetEngine().GetFileManager();
    fm.UnregisterPath(SHADERS, SHADER_PATH);
    fm.UnregisterPath(VIDS, VID_PATH);
    fm.UnregisterPath(PIDS, PID_PATH);
    fm.UnregisterFilesystem(ROFS);

    delete gsPluginState;
}

PluginToken Initialize3dPlugin(IGraphicsContext& context)
{
    auto classRegister = context.GetRenderContext().GetInterface<IClassRegister>();
    if (!classRegister) {
        return nullptr;
    }

    auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
    if (!loader) {
        return nullptr;
    }

    auto sceneLoader = loader->GetInterface<ISceneLoader>();
    if (!sceneLoader) {
        return nullptr;
    }

    context.GetSceneUtil().RegisterSceneLoader(ISceneLoader::Ptr { sceneLoader });
    return &context;
}

void Uninitialize3dPlugin(PluginToken token)
{
    auto context = static_cast<IGraphicsContext*>(token);
    if (!context) {
        return;
    }
    auto classRegister = context->GetRenderContext().GetInterface<IClassRegister>();
    if (!classRegister) {
        return;
    }
    auto loader = classRegister->GetInstance(GS::UID_GS_LOADER);
    if (!loader) {
        return;
    }
    auto sceneLoader = loader->GetInterface<ISceneLoader>();
    if (!sceneLoader) {
        return;
    }
    context->GetSceneUtil().UnregisterSceneLoader(ISceneLoader::Ptr { sceneLoader });
}

static constexpr IRenderPlugin RENDER_PLUGIN(Initialize, Uninitialize);

static constexpr I3DPlugin CORE3D_PLUGIN(Initialize3dPlugin, Uninitialize3dPlugin);
} // namespace

PluginToken RegisterInterfaces(IPluginRegister& pluginRegistry)
{
#if defined(CORE_PLUGIN) && (CORE_PLUGIN == 1)
    gPluginRegistry = &pluginRegistry;
#endif
    for (const auto& info : gRenderDataStores) {
        pluginRegistry.RegisterTypeInfo(info);
    }
    for (const auto& info : gRenderNodes) {
        pluginRegistry.RegisterTypeInfo(info);
    }
    for (const auto& info : gComponentManagers) {
        pluginRegistry.RegisterTypeInfo(info);
    }
    for (const auto& info : gSystems) {
        pluginRegistry.RegisterTypeInfo(info);
    }
    pluginRegistry.RegisterTypeInfo(RENDER_PLUGIN);
    pluginRegistry.RegisterTypeInfo(CORE3D_PLUGIN);
#ifdef __OHOS_PLATFORM__
    pluginRegistry.RegisterTypeInfo(WEBP_LOADER);
#endif
    return &pluginRegistry;
}

void UnregisterInterfaces(PluginToken token)
{
    auto& pluginRegistry = *static_cast<IPluginRegister*>(token);
#ifdef __OHOS_PLATFORM__
    pluginRegistry.UnregisterTypeInfo(WEBP_LOADER);
#endif
    pluginRegistry.UnregisterTypeInfo(CORE3D_PLUGIN);
    pluginRegistry.UnregisterTypeInfo(RENDER_PLUGIN);
    for (const auto& info : gSystems) {
        pluginRegistry.UnregisterTypeInfo(info);
    }
    for (const auto& info : gComponentManagers) {
        pluginRegistry.UnregisterTypeInfo(info);
    }
    for (const auto& info : gRenderNodes) {
        pluginRegistry.UnregisterTypeInfo(info);
    }
    for (const auto& info : gRenderDataStores) {
        pluginRegistry.UnregisterTypeInfo(info);
    }
}
} // namespace GS

extern "C" {
PLUGIN_DATA(GSPlugin) {
    { CORE_NS::IPlugin::UID },
    "GSPlugin",
    { GS::UID_GS_PLUGIN, GS::GetVersionInfo },
    GS::RegisterInterfaces,
    GS::UnregisterInterfaces,
    { GS::PLUGIN_DEPENDENCIES },
};
}
