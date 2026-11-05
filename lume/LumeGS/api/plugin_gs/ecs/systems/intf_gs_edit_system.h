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

#ifndef API_PLUGINGSEDIT_ECS_SYSTEM_H
#define API_PLUGINGSEDIT_ECS_SYSTEM_H

#include <functional>
#include <plugin_gs/namespace.h>
#include <plugin_gs/render/intf_render_data_store_gs_edit.h>

#include <base/containers/pair.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/math/vector.h>
#include <base/util/uid.h>
#include <core/ecs/entity.h>
#include <core/ecs/intf_system.h>

PLUGINGS_BEGIN_NAMESPACE()
class IGsEditSystem : public CORE_NS::ISystem {
public:
    static constexpr BASE_NS::Uid UID { "c4cb87e1-5182-4e1a-b8e7-b1646b226d05" };

    using ExportCb = std::function<void(bool success, BASE_NS::string_view msg)>;
    using Export3DBoxCb = std::function<void(bool finished, BASE_NS::string_view msg,
        const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& result)>;

    virtual uint32_t PushEditAction(const BASE_NS::string& actionStr) = 0;
    virtual GS::IRenderDataStoreGsEdit* GetEditDataStore() = 0;
    virtual void ClearEditActionStack() = 0;

    inline void ExportEditedModelAsync(const BASE_NS::string& path)
    {
        ExportEditedModelAsync(path, nullptr);
    }
    virtual void ExportEditedModelAsync(const BASE_NS::string& path, ExportCb cb) = 0;
    virtual bool IsGsExportDone() const = 0;
    virtual CORE_NS::Entity GetGsConfigEntity() const = 0;
    virtual void InitDataStore() = 0;
    virtual void GetRecommended3DBox(
        BASE_NS::Math::Vec3& rayOrigin, BASE_NS::Math::Vec3& rayDir, Export3DBoxCb cb) const = 0;


protected:
    IGsEditSystem() = default;
    virtual ~IGsEditSystem() = default;
    IGsEditSystem(const IGsEditSystem&) = delete;
    IGsEditSystem(const IGsEditSystem&&) = delete;
    IGsEditSystem& operator=(const IGsEditSystem&) = delete;
};

inline constexpr const char* GetName(const IGsEditSystem*)
{
    return "GsEditSystem";
}
PLUGINGS_END_NAMESPACE()

#endif // API_PLUGINGSEDIT_ECS_SYSTEM_H
