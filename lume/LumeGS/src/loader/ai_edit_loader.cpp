/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
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

#include <plugin_gs/render/gs_ai_edit_factory.h>

#include <dlfcn.h>

#include <base/containers/unique_ptr.h>
#include <core/log.h>
#include <core/plugin/intf_plugin_register.h>
#include <render/nodecontext/intf_render_node_context_manager.h>

#include "GsAIEdit/render_node_gs_ai_edit_render.h"

namespace {
#if defined(__aarch64__) || defined(_M_X64)
constexpr const char* PLUGIN_SO_PATH = "/system/lib64/graphics3d/libPluginGSAIEdit.z.so";
#else
constexpr const char* PLUGIN_SO_PATH = "/system/lib/graphics3d/libPluginGSAIEdit.z.so";
#endif

// Mangled name of GsAIEditFactory::Create (clang/libc++, aarch64).
// Verified with:
//   nm -D libPluginGSAIEdit.z.so | grep GsAIEditFactory
constexpr const char* SYM_CREATE =
    "_ZN2GS15GsAIEditFactory6CreateEPN6Render25IRenderNodeContextManagerEPNS_19IRenderNodeGsAIEditE";

// Mangled name of GsAIEditFactory::ComputeRecommendedBox (clang/libc++, aarch64).
constexpr const char* SYM_COMPUTE_RECOMMENDED_BOX =
    "_ZN2GS15GsAIEditFactory21ComputeRecommendedBoxERN4Core4IEcsENS1_6EntityEN4Base4Math4Vec3ES7_";

// Mangled name of GsAIEditFactory::InitPluginRegister (clang/libc++, aarch64).
constexpr const char* SYM_INIT_PLUGIN_REGISTER =
    "_ZN2GS15GsAIEditFactory18InitPluginRegisterEPN4Core15IPluginRegisterE";

using CreateFn = BASE_NS::unique_ptr<GS::IGsAIEditor>(*)(
    RENDER_NS::IRenderNodeContextManager*, GS::IRenderNodeGsAIEdit*);
using ComputeRecommendedBoxFn = std::vector<BASE_NS::Math::Vec3>(*)(
    CORE_NS::IEcs&, CORE_NS::Entity, BASE_NS::Math::Vec3, BASE_NS::Math::Vec3);
using InitPluginRegisterFn = void(*)(CORE_NS::IPluginRegister*);

struct AiEditState {
    void* handle = nullptr;
    CreateFn create = nullptr;
    ComputeRecommendedBoxFn computeRecommendedBox = nullptr;

    ~AiEditState()
    {
        if (handle) {
            dlclose(handle);
        }
    }
};

AiEditState& GetState()
{
    static AiEditState state = [] {
        AiEditState s;
        s.handle = dlopen(PLUGIN_SO_PATH, RTLD_NOW | RTLD_LOCAL);
        if (s.handle == nullptr) {
            CORE_LOG_E("AiEdit dlopen failed: %s", dlerror());
            return s;
        }
        // Inject our plugin register so libPluginGSAIEdit can resolve
        // CORE_LOG and GetInstance without a link-time dependency on us.
        auto initFn = reinterpret_cast<InitPluginRegisterFn>(dlsym(s.handle, SYM_INIT_PLUGIN_REGISTER));
        if (initFn != nullptr) {
            initFn(&CORE_NS::GetPluginRegister());
        } else {
            CORE_LOG_W("AiEdit dlsym %s failed: %s", SYM_INIT_PLUGIN_REGISTER, dlerror());
        }
        s.create = reinterpret_cast<CreateFn>(dlsym(s.handle, SYM_CREATE));
        if (s.create == nullptr) {
            CORE_LOG_E("AiEdit dlsym failed: %s", dlerror());
            dlclose(s.handle);
            s = {};
            return s;
        }
        s.computeRecommendedBox =
            reinterpret_cast<ComputeRecommendedBoxFn>(dlsym(s.handle, SYM_COMPUTE_RECOMMENDED_BOX));
        if (s.computeRecommendedBox == nullptr) {
            CORE_LOG_W("AiEdit dlsym %s failed: %s", SYM_COMPUTE_RECOMMENDED_BOX, dlerror());
        }
        return s;
    }();
    return state;
}

bool EnsureLoaded()
{
    return GetState().handle != nullptr;
}
} // anonymous namespace

namespace GS {

bool GsAIEditFactory::IsLoaded()
{
    return EnsureLoaded();
}

BASE_NS::unique_ptr<IGsAIEditor> GsAIEditFactory::Create(
    RENDER_NS::IRenderNodeContextManager* ctxMgr,
    IRenderNodeGsAIEdit* renderNode)
{
    return EnsureLoaded() ? GetState().create(ctxMgr, renderNode) : nullptr;
}

std::vector<BASE_NS::Math::Vec3> GsAIEditFactory::ComputeRecommendedBox(
    CORE_NS::IEcs& ecs, CORE_NS::Entity entity, BASE_NS::Math::Vec3 ori, BASE_NS::Math::Vec3 dir)
{
    if (EnsureLoaded() && GetState().computeRecommendedBox != nullptr) {
        return GetState().computeRecommendedBox(ecs, entity, ori, dir);
    }
    return {};
}

} // namespace GS
