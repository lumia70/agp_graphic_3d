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

#ifndef API_GS_UTIL_IGSPICKING_H
#define API_GS_UTIL_IGSPICKING_H

#include <limits>

#include <3d/namespace.h>
#include <3d/util/intf_picking.h>
#include <base/containers/array_view.h>
#include <base/containers/refcnt_ptr.h>
#include <base/containers/vector.h>
#include <base/math/matrix.h>
#include <base/namespace.h>
#include <base/util/uid.h>
#include <core/ecs/intf_system.h>
#include <core/intf_engine.h>
#include <core/log.h>
#include <core/namespace.h>
#include <core/plugin/intf_interface.h>

CORE3D_BEGIN_NAMESPACE()
class ISceneNode;
CORE3D_END_NAMESPACE()

namespace GS {

class IGsPicking : public CORE3D_NS::IPicking {
public:
    static constexpr auto UID = BASE_NS::Uid { "e028a28b-d984-4f70-81b5-c53997981e8b" };

    using Ptr = BASE_NS::refcnt_ptr<IGsPicking>;

    struct GsData {
        BASE_NS::Math::Vec4 positionOpacity;
        float extent { 0.0f };
    };

    // should only be called from GsSystem
    virtual void BuildRaycastCpuAccelerationVolume(
        CORE_NS::IEcs const& ecs, BASE_NS::vector<BASE_NS::vector<GsData>>& gsdata) = 0;

    // should only be called from GsSystem, uninititalizes ECS scene
    virtual void RemoveGsEcsSystem(CORE_NS::IEcs const& ecs) = 0;

    // should only be called from GsSystem
    virtual void SetGsObjectWorldMatrices(CORE_NS::IEcs const& ecs,
        BASE_NS::vector<BASE_NS::Math::Mat4X4>& gsWorldMatrices, BASE_NS::vector<Core3D::ISceneNode*> gsSceneNodes) = 0;

    virtual BASE_NS::vector<CORE3D_NS::RayCastResult> RayCastFromCamera(
        CORE_NS::IEcs const& ecs, CORE_NS::Entity camera, const BASE_NS::Math::Vec2& screenPos) const = 0;
};

} // namespace GS

#endif // API_GS_UTIL_IGSPICKING_H