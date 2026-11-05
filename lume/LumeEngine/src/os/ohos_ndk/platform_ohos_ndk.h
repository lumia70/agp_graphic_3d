/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#ifndef CORE_OS_OHOS_NDK_PLATFORM_OHOS_NDK_H
#define CORE_OS_OHOS_NDK_PLATFORM_OHOS_NDK_H

#include <rawfile/raw_file_manager.h>

#include <platform/ohos_ndk/core/os/intf_platform.h>

#include <base/containers/string.h>
#include <core/namespace.h>

CORE_BEGIN_NAMESPACE()

struct PlatformData {
    /** Application plugin path */
    BASE_NS::string appPluginPath;
    /** NativeResourceManager handle (non-owning). */
    NativeResourceManager* nativeResMgr { nullptr };
};

/** IPlatform implementation for the OHOS NDK variant. */
class PlatformOhosNdk final : public IPlatform {
public:
    explicit PlatformOhosNdk(PlatformCreateInfo const& createInfo);
    ~PlatformOhosNdk() override;

    const PlatformData& GetPlatformData() const override;
    void RegisterPluginLocations(IPluginRegister& registry) override;
    void RegisterRawFilesystem(IFileManager& fileManager, BASE_NS::string_view protocol) const override;

private:
    PlatformData plat_;
};

CORE_END_NAMESPACE()

#endif // CORE_OS_OHOS_NDK_PLATFORM_OHOS_NDK_H
