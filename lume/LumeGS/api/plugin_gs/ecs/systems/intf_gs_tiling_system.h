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

#ifndef PLUGIN_API_GS_TILING_SYSTEM_H
#define PLUGIN_API_GS_TILING_SYSTEM_H

#include <plugin_gs/ecs/components/gs_tiling_component.h>

#include <core/ecs/entity.h>
#include <core/ecs/intf_system.h>

namespace GS {
class IGsTilingSystem : public CORE_NS::ISystem {
public:
    static constexpr BASE_NS::Uid UID { "d4e5f6a7-b8c9-4012-abcd-ef1234567890" };

    virtual void CreateTile(const CORE_NS::Entity entity, const GsTilingComponent& data) = 0;
    virtual void SetTileVisible(const CORE_NS::Entity entity, const bool visible) = 0;
    virtual void DestroyTile(const CORE_NS::Entity entity) = 0;
    virtual bool IsTileResident(const CORE_NS::Entity entity) const = 0;

protected:
    IGsTilingSystem() = default;
    virtual ~IGsTilingSystem() = default;
    IGsTilingSystem(const IGsTilingSystem&) = delete;
    IGsTilingSystem(const IGsTilingSystem&&) = delete;
    IGsTilingSystem& operator=(const IGsTilingSystem&) = delete;
};

inline constexpr const char* GetName(const IGsTilingSystem*)
{
    return "GsTilingSystem";
}
} // namespace GS
#endif // PLUGIN_API_GS_TILING_SYSTEM_H