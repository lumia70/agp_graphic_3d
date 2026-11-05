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

#include "xengine_loader.h"

#include <dlfcn.h>
#include <mutex>

#include <core/log.h>

namespace {
#if defined(__aarch64__) || defined(_M_X64)
constexpr const char* PLUGIN_SO_PATH = "/system/lib64/libxengine_gs.z.so";
#else
constexpr const char* PLUGIN_SO_PATH = "/system/lib/libxengine_gs.z.so";
#endif

// Mangled names of XEngineGsFactory static methods (clang/libc++, aarch64).
// Verified with:
//   nm -D libxengine_gs.z.so | grep XEngineGsFactory
constexpr const char* SYM_CREATE_BACKEND =
    "_ZN2GS16XEngineGsFactory13CreateBackendEv";
constexpr const char* SYM_IS_3DGS_ENABLE =
    "_ZN2GS16XEngineGsFactory12Is3DGSEnableERN6Render14IRenderContextE";
constexpr const char* SYM_IS_UPSCALE_ENABLE =
    "_ZN2GS16XEngineGsFactory22IsSpatialUpscaleEnableERN6Render14IRenderContextE";

using CreateBackendFn = BASE_NS::unique_ptr<GS::IXEngineGsBackend>(*)();
using Is3DgsEnableFn = bool (*)(RENDER_NS::IRenderContext&);
using IsUpscaleEnableFn = bool (*)(RENDER_NS::IRenderContext&);

struct XegState {
    void* handle = nullptr;
    CreateBackendFn createBackend = nullptr;
    Is3DgsEnableFn is3DgsEnable = nullptr;
    IsUpscaleEnableFn isUpscaleEnable = nullptr;

    ~XegState()
    {
        if (handle) {
            dlclose(handle);
        }
    }
};

XegState g_state;
std::once_flag g_initFlag;

bool DoInit()
{
    g_state.handle = dlopen(PLUGIN_SO_PATH, RTLD_NOW | RTLD_LOCAL);
    if (g_state.handle == nullptr) {
        CORE_LOG_E("dlopen failed: %s", dlerror());
        return false;
    }
    g_state.createBackend = reinterpret_cast<CreateBackendFn>(dlsym(g_state.handle, SYM_CREATE_BACKEND));
    g_state.is3DgsEnable = reinterpret_cast<Is3DgsEnableFn>(dlsym(g_state.handle, SYM_IS_3DGS_ENABLE));
    g_state.isUpscaleEnable = reinterpret_cast<IsUpscaleEnableFn>(dlsym(g_state.handle, SYM_IS_UPSCALE_ENABLE));
    if (g_state.createBackend == nullptr || g_state.is3DgsEnable == nullptr ||
        g_state.isUpscaleEnable == nullptr) {
        CORE_LOG_E("dlsym failed: %s", dlerror());
        dlclose(g_state.handle);
        g_state = {};
        return false;
    }
    return true;
}

bool EnsureLoaded()
{
    std::call_once(g_initFlag, [] { DoInit(); });
    return g_state.handle != nullptr;
}
} // anonymous namespace

namespace GS {

bool XEngineGsFactory::IsLoaded()
{
    return EnsureLoaded();
}

BASE_NS::unique_ptr<IXEngineGsBackend> XEngineGsFactory::CreateBackend()
{
    return EnsureLoaded() ? g_state.createBackend() : nullptr;
}

bool XEngineGsFactory::Is3DGSEnable(RENDER_NS::IRenderContext& engine)
{
    return EnsureLoaded() ? g_state.is3DgsEnable(engine) : false;
}

bool XEngineGsFactory::IsSpatialUpscaleEnable(RENDER_NS::IRenderContext& engine)
{
    return EnsureLoaded() ? g_state.isUpscaleEnable(engine) : false;
}

} // namespace GS