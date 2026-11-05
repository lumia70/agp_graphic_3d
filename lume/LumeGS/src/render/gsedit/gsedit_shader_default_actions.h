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

#ifndef GS_SHADER_DEFAULT_ACTIONS_H
#define GS_SHADER_DEFAULT_ACTIONS_H

#include "gsedit_shader_action_executor.h"

namespace GS {

// executors for the default shader action

class SelectBoxExecutor : public ShaderActionExecutor {
public:
    explicit SelectBoxExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);

protected:
    void PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) override;
};

class RemoveSelectedExecutor : public ShaderActionExecutor {
public:
    explicit RemoveSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);
};

class InvertSelectionExecutor : public ShaderActionExecutor {
public:
    explicit InvertSelectionExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);
};

class TransformSelectedExecutor : public ShaderActionExecutor {
public:
    explicit TransformSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);

protected:
    void PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) override;
};

class ColorSelectedExecutor : public ShaderActionExecutor {
public:
    explicit ColorSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);

protected:
    void PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) override;
};

class ClearSelectedExecutor : public ShaderActionExecutor {
public:
    explicit ClearSelectedExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);

protected:
    void Execute(RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action,
        const ActionExecutionContext& context) override;
};

class Select3DBoxExecutor : public ShaderActionExecutor {
public:
    explicit Select3DBoxExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);

protected:
    void PrepareCustomPushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) override;
};

class SelectByIndexExecutor : public ShaderActionExecutor {
public:
    explicit SelectByIndexExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);

protected:
    void Execute(RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action,
        const ActionExecutionContext& context) override;
};

class SelectByMaskExecutor : public ShaderActionExecutor {
public:
    explicit SelectByMaskExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);

protected:
    void Execute(RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action,
        const ActionExecutionContext& context) override;
};

// No-op executor for FloaterRemoval marker action. The actual floater removal
// is handled by RenderNodeGsFloaterV21; this executor only satisfies the
// ExecuteStack lookup so MarkStackAsExecuted is reached.
class FloaterRemovalExecutor : public IActionExecutor {
public:
    explicit FloaterRemovalExecutor(RENDER_NS::IRenderNodeContextManager* contextMgr);
    ~FloaterRemovalExecutor() override = default;

    void Execute(RENDER_NS::IRenderCommandList& cmdList, const GsEditAction& action,
        const ActionExecutionContext& context) override;

    void CalculateDescriptorCounts(
        RENDER_NS::IRenderNodeContextManager& contextMgr, RENDER_NS::DescriptorCounts& dc) override;

    void CreateDescriptorSets(RENDER_NS::IRenderNodeContextManager& contextMgr) override;

    bool ModifiesSplatBuffer() const override
    {
        return false;
    }
    bool IsSelectionAction() const override
    {
        return false;
    }
    bool RequiresSelectionClear() const override
    {
        return false;
    }
    bool NeedsIndirectDispatchSetup() const override
    {
        return false;
    }

    void PreparePushConstantData(const GsEditAction& action, GsEditPushConstantData& pcData) override;
};

}  // namespace GS

#endif  // GS_SHADER_DEFAULT_ACTIONS_H
