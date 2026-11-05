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

#ifndef PLUGIN_API_GS_AI_EDIT_FACTORY_H
#define PLUGIN_API_GS_AI_EDIT_FACTORY_H

#include <vector>

#include <base/math/vector.h>
#include <base/containers/unique_ptr.h>
#include <core/ecs/intf_ecs.h>
#include <core/plugin/intf_plugin_register.h>
#include <plugin_gs/render/intf_gs_ai_editor.h>
#include <plugin_gs/render/intf_render_node_gs_ai_edit.h>

namespace RENDER_NS {
class IRenderNodeContextManager;
} // namespace RENDER_NS

namespace GS {

/** Factory for creating IGsAIEditor instances via dlopen/dlsym. */
class GsAIEditFactory {
public:
    GsAIEditFactory() = delete;
    ~GsAIEditFactory() = delete;

    static bool IsLoaded();
    static BASE_NS::unique_ptr<IGsAIEditor> Create(
        RENDER_NS::IRenderNodeContextManager* ctxMgr,
        IRenderNodeGsAIEdit* renderNode);
    static std::vector<BASE_NS::Math::Vec3> ComputeRecommendedBox(
        CORE_NS::IEcs& ecs, CORE_NS::Entity entity, BASE_NS::Math::Vec3 ori, BASE_NS::Math::Vec3 dir);

    /** Inject the plugin register from the host plugin (libPluginGS).
     *  Must be called once after dlopen and before Create/ComputeRecommendedBox.
     *  This allows libPluginGSAIEdit to resolve CORE_LOG and GetInstance without
     *  a link-time dependency on libPluginGS. */
    static void InitPluginRegister(CORE_NS::IPluginRegister* registry);
};
} // namespace GS

#endif // PLUGIN_API_GS_AI_EDIT_FACTORY_H
