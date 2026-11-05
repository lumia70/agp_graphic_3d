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

#ifndef API_PLUGINGS_GS_AI_EDIT_ECS_SYSTEM_H
#define API_PLUGINGS_GS_AI_EDIT_ECS_SYSTEM_H

#include <plugin_gs/namespace.h>

#include <base/containers/string.h>
#include <base/util/uid.h>
#include <core/ecs/intf_system.h>
#include <core/ecs/entity.h>
#include <future>
#include <base/math/vector.h>
PLUGINGS_BEGIN_NAMESPACE()
class IGsAIEditSystem : public CORE_NS::ISystem {
public:
    static constexpr BASE_NS::Uid UID { "c4cb87e1-5182-4e1a-b8e7-b1646b226d06" };

    virtual std::future<int> ExtractMainBody(CORE_NS::Entity entity, float minX, float minY) = 0;
    virtual std::future<std::vector<BASE_NS::Math::UVec4>> GetMainBodyPreviewData(CORE_NS::Entity entity,
                                                                                  float X, float Y) = 0;
    virtual void ConfirmExtractMainBody(CORE_NS::Entity entity) = 0;
    virtual void UndoExtractMainBody(CORE_NS::Entity entity) = 0;
    virtual std::vector<BASE_NS::Math::Vec3> GetRecommended3DBox(CORE_NS::Entity entity,
                                                                 BASE_NS::Math::Vec3 ori,
                                                                 BASE_NS::Math::Vec3 dir) = 0;

protected:
    IGsAIEditSystem() = default;
    virtual ~IGsAIEditSystem() = default;
    IGsAIEditSystem(const IGsAIEditSystem&) = delete;
    IGsAIEditSystem(const IGsAIEditSystem&&) = delete;
    IGsAIEditSystem& operator=(const IGsAIEditSystem&) = delete;
};

inline constexpr const char* GetName(const IGsAIEditSystem*)
{
    return "GsAIEditSystem";
}
PLUGINGS_END_NAMESPACE()

#endif // API_PLUGINGS_GS_AI_EDIT_ECS_SYSTEM_H
