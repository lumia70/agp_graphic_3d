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

#ifndef GS_SHADER_ACTION_EXECUTOR_H
#define GS_SHADER_ACTION_EXECUTOR_H

#include <base/containers/string_view.h>
#include <base/containers/unordered_map.h>
#include <render/device/pipeline_layout_desc.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/nodecontext/intf_render_node_context_manager.h>

#include "intf_gsedit_action_executor.h"

namespace GS {

// shader-based action executor, executes a singular compute edit action shader
class ShaderActionExecutor : public IActionExecutor {
public:
    struct Config {
        BASE_NS::string_view shaderPath;
        BASE_NS::string_view pipelineLayoutPath;
        bool modifiesSplatBuffer{false};
        bool isSelectionAction{false};
        bool requiresSelectionClear{false};
    };

    ShaderActionExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr, const Config& config);

    ~ShaderActionExecutor() override = default;

    void Execute(RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action,
        const ActionExecutionContext& context) override;

    void CalculateDescriptorCounts(
        RENDER_NS::IRenderNodeContextManager& contextMgr, RENDER_NS::DescriptorCounts& dc) override;

    void CreateDescriptorSets(RENDER_NS::IRenderNodeContextManager& contextMgr) override;

    bool ModifiesSplatBuffer() const override
    {
        return config_.modifiesSplatBuffer;
    }
    bool IsSelectionAction() const override
    {
        return config_.isSelectionAction;
    }
    bool RequiresSelectionClear() const override
    {
        return config_.requiresSelectionClear;
    }

    void PreparePushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) override;

protected:
    virtual void PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) {};

    struct PsoData {
        RENDER_NS::RenderHandle psoHandle;
        RENDER_NS::PushConstant pushConstant;
    };
    PsoData GetPsoData(uint32_t flags);

    RENDER_NS::IRenderNodeContextManager* contextMgr_{nullptr};
    Config config_;

    RENDER_NS::RenderHandle shaderHandle_;
    RENDER_NS::PipelineLayout pipelineLayout_;
    RENDER_NS::IDescriptorSetBinder::Ptr set0Binder_;

    BASE_NS::unordered_map<uint64_t, PsoData> psoCache_;
};

}  // namespace GS

#endif  // GS_SHADER_ACTION_EXECUTOR_H
