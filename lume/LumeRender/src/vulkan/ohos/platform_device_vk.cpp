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

#include <vulkan/vulkan.h>

#include "util/log.h"
#include "vulkan/device_vk.h"

RENDER_BEGIN_NAMESPACE()
namespace {
// the extension alone is not enough, export and import are separate driver capabilities
bool HasSemaphoreSyncFdFeature(VkInstance instance, VkPhysicalDevice physicalDevice, VkExternalSemaphoreFeatureFlags f)
{
    auto vkGetPhysicalDeviceExternalSemaphoreProperties =
        (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)vkGetInstanceProcAddr(
            instance, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if (!vkGetPhysicalDeviceExternalSemaphoreProperties) {
        PLUGIN_LOG_E("vkGetInstanceProcAddr failed for vkGetPhysicalDeviceExternalSemaphoreProperties");
        return false;
    }
    constexpr VkPhysicalDeviceExternalSemaphoreInfo externalSemaphoreInfo{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,  // sType
        nullptr,                                                    // pNext
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,              // handleType
    };
    VkExternalSemaphoreProperties externalSemaphoreProperties{
        VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES,  // sType
        nullptr,                                          // pNext
        0,                                                // exportFromImportedHandleTypes
        0,                                                // compatibleHandleTypes
        0,                                                // externalSemaphoreFeatures
    };
    vkGetPhysicalDeviceExternalSemaphoreProperties(
        physicalDevice, &externalSemaphoreInfo, &externalSemaphoreProperties);
    return (externalSemaphoreProperties.externalSemaphoreFeatures & f) != 0;
}
}  // namespace

void DeviceVk::CreatePlatformExtFunctions()
{
    if (platformDeviceExtensions_.externalSemaphoreFd) {
        if (HasSemaphoreSyncFdFeature(
                plat_.instance, plat_.physicalDevice, VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) {
            extFunctions_.vkGetSemaphoreFdKHR =
                (PFN_vkGetSemaphoreFdKHR)vkGetInstanceProcAddr(plat_.instance, "vkGetSemaphoreFdKHR");
            if (!extFunctions_.vkGetSemaphoreFdKHR) {
                PLUGIN_LOG_E("vkGetInstanceProcAddr failed for vkGetSemaphoreFdKHR");
            }
        } else {
            PLUGIN_LOG_W("sync fd semaphore export not supported by the device");
        }
        if (HasSemaphoreSyncFdFeature(
                plat_.instance, plat_.physicalDevice, VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT)) {
            extFunctions_.vkImportSemaphoreFdKHR =
                (PFN_vkImportSemaphoreFdKHR)vkGetInstanceProcAddr(plat_.instance, "vkImportSemaphoreFdKHR");
            if (!extFunctions_.vkImportSemaphoreFdKHR) {
                PLUGIN_LOG_E("vkGetInstanceProcAddr failed for vkImportSemaphoreFdKHR");
            }
        } else {
            PLUGIN_LOG_W("sync fd semaphore import not supported by the device");
        }
        // semaphores are created exportable from this flag, so clear it when export is unusable
        platformDeviceExtensions_.externalSemaphoreFd = (extFunctions_.vkGetSemaphoreFdKHR != nullptr);
    }

    if (platformDeviceExtensions_.externalMemoryHardwareBuffer) {
        platformExtFunctions_.vkGetNativeBufferPropertiesOHOS =
            (PFN_vkGetNativeBufferPropertiesOHOS)vkGetInstanceProcAddr(
                plat_.instance, "vkGetNativeBufferPropertiesOHOS");
        if (!platformExtFunctions_.vkGetNativeBufferPropertiesOHOS) {
            PLUGIN_LOG_E("vkGetInstanceProcAddr failed for vkGetNativeBufferPropertiesOHOS");
        }
        platformExtFunctions_.vkGetMemoryNativeBufferOHOS =
            (PFN_vkGetMemoryNativeBufferOHOS)vkGetInstanceProcAddr(plat_.instance, "vkGetMemoryNativeBufferOHOS");
        if (!platformExtFunctions_.vkGetMemoryNativeBufferOHOS) {
            PLUGIN_LOG_E("vkGetInstanceProcAddr failed for vkGetMemoryNativeBufferOHOS");
        }
    }
}
RENDER_END_NAMESPACE()
