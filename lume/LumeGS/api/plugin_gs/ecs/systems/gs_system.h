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

#ifndef PLUGIN_API_GS_SYSTEM_H
#define PLUGIN_API_GS_SYSTEM_H

#include <core/ecs/intf_system.h>
#include <core/property/property_types.h>

namespace GS {
class IGsSystem : public CORE_NS::ISystem {
public:
    static constexpr BASE_NS::Uid UID { "3a2f88e4-29c7-4a84-a3c5-eb50d3f55e3d" };

    struct RayCastResult {
        bool isHit { false };
        BASE_NS::Math::Vec3 hitPoint { 0.0f, 0.0f, 0.0f };
        bool raycastDone { false };
    };

protected:
    IGsSystem() = default;
    virtual ~IGsSystem() = default;
    IGsSystem(const IGsSystem&) = delete;
    IGsSystem(const IGsSystem&&) = delete;
    IGsSystem& operator=(const IGsSystem&) = delete;
};

inline constexpr const char* GetName(const IGsSystem*)
{
    return "GsSystem";
}
} // namespace GS
#endif