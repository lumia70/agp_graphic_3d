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

#ifndef PLUGIN_GS_MULTIVIEW_TYPES_H
#define PLUGIN_GS_MULTIVIEW_TYPES_H

#include <array>
#include <base/math/matrix_util.h>
#include <render/resource_handle.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>

constexpr int MAX_GS_VIEWPORT_COUNT = 4;

struct GSCamData {
    BASE_NS::Math::Mat4X4 proj;
    BASE_NS::Math::Mat4X4 view;
};

using CameraMatrixArray = std::array<GSCamData, MAX_GS_VIEWPORT_COUNT>;

struct MultiviewRenderDataPack {
    GSCamData camMat;
    RENDER_NS::RenderHandleReference colorAttachment;
    RENDER_NS::RenderHandleReference depthAttachment;
    RENDER_NS::RenderHandleReference depthStoreTexture;
    RENDER_NS::RenderHandleReference camBuffer;
    RENDER_NS::RenderHandleReference indexBuffer;
    RENDER_NS::IDescriptorSetBinder::Ptr camSet;
    RENDER_NS::IDescriptorSetBinder::Ptr preComputeProjectionSet;
    RENDER_NS::IDescriptorSetBinder::Ptr preSortSet;
    RENDER_NS::IDescriptorSetBinder::Ptr depthRenderSet;
};

using MultiviewRenderDataPackArray = std::array<MultiviewRenderDataPack, MAX_GS_VIEWPORT_COUNT>;

#endif // PLUGIN_GS_MULTIVIEW_TYPES_H
