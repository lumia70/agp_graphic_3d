/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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

#ifndef VULKAN_OHOS_PLATFORM_VK_H
#define VULKAN_OHOS_PLATFORM_VK_H

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_ohos.h>

#include <base/containers/string_view.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <render/namespace.h>

RENDER_BEGIN_NAMESPACE()
struct PlatformDeviceExtensions {
    // hardware buffers supported
    bool externalMemoryHardwareBuffer{false};
    // semaphores can be exported as a POSIX sync fd (native fence)
    bool externalSemaphoreFd{false};
};

struct PlatformExtFunctions {
    // VK_OpenHarmony_external_memory_OHOS_native_buffer
    PFN_vkGetNativeBufferPropertiesOHOS vkGetNativeBufferPropertiesOHOS{nullptr};
    PFN_vkGetMemoryNativeBufferOHOS vkGetMemoryNativeBufferOHOS{nullptr};
};

// pNext for a semaphore exportable as a sync fd, or nullptr when unsupported. Points to static storage.
inline const void* GetExportableSemaphoreCreateInfoPNext(const PlatformDeviceExtensions& platformDeviceExtensions)
{
    static constexpr VkExportSemaphoreCreateInfo EXPORT_SEMAPHORE_CREATE_INFO{
        VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,  // sType
        nullptr,                                         // pNext
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,   // handleTypes
    };
    return platformDeviceExtensions.externalSemaphoreFd ? &EXPORT_SEMAPHORE_CREATE_INFO : nullptr;
}

void GetPlatformDeviceExtensions(BASE_NS::vector<BASE_NS::string_view>& extensions);

PlatformDeviceExtensions GetEnabledPlatformDeviceExtensions(
    const BASE_NS::unordered_map<BASE_NS::string, uint32_t>& enabledDeviceExtensions);

inline const char* GetPlatformSurfaceName()
{
    return VK_OHOS_SURFACE_EXTENSION_NAME;
}

inline bool CanDevicePresent(VkInstance instance, VkPhysicalDevice device, uint32_t queuefamily)
{
    return true;
}
RENDER_END_NAMESPACE()
#endif  // VULKAN_OHOS_PLATFORM_VK_H
