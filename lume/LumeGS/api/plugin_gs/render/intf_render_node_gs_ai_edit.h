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

#ifndef PLUGIN_API_RENDER_NODE_GS_AI_EDIT_H
#define PLUGIN_API_RENDER_NODE_GS_AI_EDIT_H

#include <cstddef>

#include <3d/render/intf_render_node_scene_util.h>
#include <base/math/matrix.h>
#include <plugin_gs/render/gs_multiview_types.h>
#include <plugin_gs/render/intf_render_data_store_gs_ai_edit.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_command_list.h>

namespace GS {

// Abstract interface exposing the methods on RenderNodeGsAIEditRender that
// the AIEdit plugin (libPluginGSAIEdit.z.so) needs.  The concrete
// implementation is provided by RenderNodeGsAIEditRender in LumeGS.
class IRenderNodeGsAIEdit {
public:
    virtual ~IRenderNodeGsAIEdit() = default;

    virtual IRenderDataStoreGsAIEdit* GetDataStoreAIEdit() const = 0;
    virtual const CORE3D_NS::SceneRenderDataStores& GetStores() const = 0;
    virtual BASE_NS::Math::Mat4X4 GetCamProjMatrix() const = 0;
    virtual BASE_NS::Math::Mat4X4 GetCamViewMatrix() const = 0;
    virtual bool UpdateDynamicDataUBO(RENDER_NS::IRenderCommandList& cmdList, bool forceUpdate = false) = 0;
    virtual void ExecuteFrameWithViewport(RENDER_NS::IRenderCommandList& cmdList, int index,
                                          MultiviewRenderDataPack* optionalMVHandle = nullptr) = 0;
    virtual RENDER_NS::IDescriptorSetBinder::Ptr CreateCameraDescriptorSet() = 0;
    virtual RENDER_NS::IDescriptorSetBinder::Ptr CreatePresortDescriptorSet() = 0;
    virtual RENDER_NS::IDescriptorSetBinder::Ptr CreatePreComputeDescriptorSet() = 0;
    virtual RENDER_NS::IDescriptorSetBinder::Ptr CreateDepthRenderDescriptorSet() = 0;
    virtual size_t GetCameraUboSize() const = 0;
};

} // namespace GS

#endif // PLUGIN_API_RENDER_NODE_GS_AI_EDIT_H
