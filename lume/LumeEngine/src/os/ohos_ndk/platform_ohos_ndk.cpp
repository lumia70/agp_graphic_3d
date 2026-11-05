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

#include "platform_ohos_ndk.h"

#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/namespace.h>

#include "os/ohos_ndk/ohos_filesystem_ndk.h"
#include "os/platform.h"

CORE_BEGIN_NAMESPACE()

PlatformOhosNdk::PlatformOhosNdk(PlatformCreateInfo const& createInfo)
{
    plat_.appPluginPath = createInfo.appPluginPath;
    plat_.nativeResMgr = createInfo.resMan;
}

PlatformOhosNdk::~PlatformOhosNdk() {}

const PlatformData& PlatformOhosNdk::GetPlatformData() const
{
    return plat_;
}

void PlatformOhosNdk::RegisterPluginLocations(IPluginRegister& registry)
{
    // Skip dynamic plugin path registration ΓÇö we use static plugins only.
    // NOTE: cannot call CORE_LOG_I here, Logger is not yet initialized during CreatePluginRegistry.
}

void PlatformOhosNdk::RegisterRawFilesystem(
    IFileManager& fileManager, BASE_NS::string_view protocol) const
{
    if (!plat_.nativeResMgr) {
        CORE_LOG_E("PlatformOhosNdk::RegisterRawFilesystem: NativeResourceManager is null");
        return;
    }
    fileManager.RegisterFilesystem(
        protocol, IFilesystem::Ptr { new OhosNdkFilesystem(plat_.nativeResMgr) });
    CORE_LOG_I("Registered NDK rawfile filesystem with protocol '%s'",
        BASE_NS::string(protocol).c_str());
}

CORE_NS::IPlatform::Ptr Platform::Create(PlatformCreateInfo const& createInfo)
{
    return CORE_NS::IPlatform::Ptr(new PlatformOhosNdk(createInfo));
}

const CORE_NS::PlatformCreateExtensionInfo* Platform::Extensions(PlatformCreateInfo const& createInfo)
{
    return createInfo.extensions;
}

CORE_END_NAMESPACE()
