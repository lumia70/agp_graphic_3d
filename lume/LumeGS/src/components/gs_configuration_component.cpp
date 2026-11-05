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

#include "plugin_gs/ecs/components/gs_configuration_component.h"

#include "ComponentTools/base_manager.h"
#include "ComponentTools/base_manager.inl"

#define IMPLEMENT_MANAGER
#include <core/property_tools/property_macros.h>

namespace GS {
class GsConfigurationComponentManager final
    : public CORE_NS::BaseManager<GsConfigurationComponent, IGsConfigurationComponentManager> {
    BEGIN_PROPERTY(GsConfigurationComponent, ComponentMetadata)
#include "plugin_gs/ecs/components/gs_configuration_component.h"
    END_PROPERTY();
    const BASE_NS::array_view<const CORE_NS::Property> componentMetaData_ { ComponentMetadata,
        BASE_NS::countof(ComponentMetadata) };

public:
    explicit GsConfigurationComponentManager(CORE_NS::IEcs& ecs)
        : BaseManager<GsConfigurationComponent, IGsConfigurationComponentManager>(
              ecs, CORE_NS::GetName<GsConfigurationComponent>())
    {}

    ~GsConfigurationComponentManager() = default;

    BASE_NS::Uid GetUid() const override
    {
        return IGsConfigurationComponentManager::UID;
    }

    size_t PropertyCount() const override
    {
        return componentMetaData_.size();
    }

    const CORE_NS::Property* MetaData(size_t index) const override
    {
        if (index < componentMetaData_.size()) {
            return &componentMetaData_[index];
        }
        return nullptr;
    }

    BASE_NS::array_view<const CORE_NS::Property> MetaData() const override
    {
        return componentMetaData_;
    }
};

CORE_NS::IComponentManager* IGsConfigurationComponentManagerInstance(CORE_NS::IEcs& ecs)
{
    return new GsConfigurationComponentManager(ecs);
}

void IGsConfigurationComponentManagerDestroy(CORE_NS::IComponentManager* instance)
{
    delete static_cast<GsConfigurationComponentManager*>(instance);
}
} // namespace GS
