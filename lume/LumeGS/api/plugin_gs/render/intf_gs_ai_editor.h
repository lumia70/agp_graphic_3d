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

#ifndef INTF_GS_AI_EDITOR_H
#define INTF_GS_AI_EDITOR_H

#include <render/nodecontext/intf_render_command_list.h>
#include <plugin_gs/render/intf_render_data_store_gs_ai_edit.h>

namespace GS {
/** Abstract interface for GS AI Editor. */
class IGsAIEditor {
public:
    virtual ~IGsAIEditor() = default;
    virtual void Update(RENDER_NS::IRenderCommandList& cmdList) = 0;
    virtual void PushOperation(const GsAIEditOp& editOp) = 0;
};
} // namespace GS

#endif // INTF_GS_AI_EDITOR_H
