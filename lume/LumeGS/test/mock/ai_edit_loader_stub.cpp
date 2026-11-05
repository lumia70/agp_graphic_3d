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

#include <plugin_gs/render/gs_ai_edit_factory.h>

namespace GS {
// Test stub: ai_edit_loader.cpp is not linked into LumeGS test binaries (it
// lives in the separate 3d_gs_ai_edit_loader source_set). These stubs provide
// the symbols so that 3d_gs_src compiles and links. The tests do not exercise
// the dlopen/dlsym loading path or the AIEdit plugin.
bool GsAIEditFactory::IsLoaded()
{
    return true;
}

BASE_NS::unique_ptr<IGsAIEditor> GsAIEditFactory::Create(
    RENDER_NS::IRenderNodeContextManager* ctxMgr,
    IRenderNodeGsAIEdit* renderNode)
{
    return nullptr;
}

std::vector<BASE_NS::Math::Vec3> GsAIEditFactory::ComputeRecommendedBox(
    CORE_NS::IEcs& ecs, CORE_NS::Entity entity, BASE_NS::Math::Vec3 ori, BASE_NS::Math::Vec3 dir)
{
    return {};
}
} // namespace GS
