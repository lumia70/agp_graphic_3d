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

#include "spatial_codec_loader.h"

#include <dlfcn.h>
#include <mutex>

#include <core/log.h>

namespace {
#if defined(__aarch64__) || defined(_M_X64)
constexpr const char* PLUGIN_SO_PATH = "/system/lib64/libspatialcodec.z.so";
#else
constexpr const char* PLUGIN_SO_PATH = "/system/lib/libspatialcodec.z.so";
#endif

// Mangled names of SpatialCodecFactory static methods (clang/libc++, aarch64).
// OHOS uses std::__h inline namespace (not std::__1), so the mangling differs
// from standard libc++. Verified with:
//   nm -D libspatialcodec.z.so | grep SpatialCodecFactory
constexpr const char* SYM_CREATE_META =
    "_ZN7Spatial19SpatialCodecFactory10CreateMetaERKNSt3__h"
    "12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
constexpr const char* SYM_CREATE_BUFFER =
    "_ZN7Spatial19SpatialCodecFactory12CreateBufferERKNSt3__h"
    "12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
constexpr const char* SYM_CREATE_DEMUXER =
    "_ZN7Spatial19SpatialCodecFactory13CreateDemuxerERKNSt3__h"
    "12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
constexpr const char* SYM_CREATE_DECODER =
    "_ZN7Spatial19SpatialCodecFactory13CreateDecoderERKNSt3__h"
    "12basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";

using CreateMetaFn = std::shared_ptr<Spatial::SpatialMeta>(*)(const std::string&);
using CreateBufferFn = std::shared_ptr<Spatial::SpatialBuffer>(*)(const std::string&);
using CreateDemuxerFn = std::shared_ptr<Spatial::SpatialCodecDemuxer>(*)(const std::string&);
using CreateDecoderFn = std::shared_ptr<Spatial::SpatialModelDecoder>(*)(const std::string&);

struct ScState {
    void* handle = nullptr;
    CreateMetaFn createMeta = nullptr;
    CreateBufferFn createBuffer = nullptr;
    CreateDemuxerFn createDemuxer = nullptr;
    CreateDecoderFn createDecoder = nullptr;

    ~ScState()
    {
        if (handle) {
            dlclose(handle);
        }
    }
};

ScState g_state;
std::once_flag g_initFlag;

bool DoInit()
{
    g_state.handle = dlopen(PLUGIN_SO_PATH, RTLD_NOW | RTLD_LOCAL);
    if (g_state.handle == nullptr) {
        CORE_LOG_E("dlopen failed: %s", dlerror());
        return false;
    }
    g_state.createMeta = reinterpret_cast<CreateMetaFn>(dlsym(g_state.handle, SYM_CREATE_META));
    g_state.createBuffer = reinterpret_cast<CreateBufferFn>(dlsym(g_state.handle, SYM_CREATE_BUFFER));
    g_state.createDemuxer = reinterpret_cast<CreateDemuxerFn>(dlsym(g_state.handle, SYM_CREATE_DEMUXER));
    g_state.createDecoder = reinterpret_cast<CreateDecoderFn>(dlsym(g_state.handle, SYM_CREATE_DECODER));
    if (g_state.createMeta == nullptr || g_state.createBuffer == nullptr ||
        g_state.createDemuxer == nullptr || g_state.createDecoder == nullptr) {
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

namespace Spatial {

bool SpatialCodecFactory::IsLoaded()
{
    return EnsureLoaded();
}

std::shared_ptr<SpatialMeta> SpatialCodecFactory::CreateMeta(const std::string& format)
{
    return EnsureLoaded() ? g_state.createMeta(format) : nullptr;
}

std::shared_ptr<SpatialBuffer> SpatialCodecFactory::CreateBuffer(const std::string& format)
{
    return EnsureLoaded() ? g_state.createBuffer(format) : nullptr;
}

std::shared_ptr<SpatialCodecDemuxer> SpatialCodecFactory::CreateDemuxer(const std::string& format)
{
    return EnsureLoaded() ? g_state.createDemuxer(format) : nullptr;
}

std::shared_ptr<SpatialModelDecoder> SpatialCodecFactory::CreateDecoder(const std::string& format)
{
    return EnsureLoaded() ? g_state.createDecoder(format) : nullptr;
}

} // namespace Spatial
