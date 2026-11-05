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

#ifndef API_PLUGINGSEFFECT_ECS_SYSTEM_H
#define API_PLUGINGSEFFECT_ECS_SYSTEM_H

#include <plugin_gs/namespace.h>

#include <base/util/uid.h>
#include <core/ecs/intf_system.h>

PLUGINGS_BEGIN_NAMESPACE()
class IGsEffectSystem : public CORE_NS::ISystem {
public:
    static constexpr BASE_NS::Uid UID { "9d7e2c81-7f04-465c-b9a2-0d3c51a45d69" };

protected:
    IGsEffectSystem() = default;
    virtual ~IGsEffectSystem() = default;
    IGsEffectSystem(const IGsEffectSystem&) = delete;
    IGsEffectSystem(const IGsEffectSystem&&) = delete;
    IGsEffectSystem& operator=(const IGsEffectSystem&) = delete;
};

inline constexpr const char* GetName(const IGsEffectSystem*)
{
    return "GsEffectSystem";
}
PLUGINGS_END_NAMESPACE()

#endif // API_PLUGINGSEFFECT_ECS_SYSTEM_H
