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

#ifndef GS_UTIL_GSPICKING_H
#define GS_UTIL_GSPICKING_H

#include <limits>
#include <mutex>
#include <plugin_gs/util/intf_gs_picking.h>

#include <base/containers/string_view.h>
#include <base/containers/unordered_map.h>
#include <core/namespace.h>

namespace GS {

// a helper macro, which defines a function as unsupported
#define DECLARE_UNSUPPORTED_PICKING_METHOD(returnType, methodName, ...)     \
    returnType methodName(__VA_ARGS__) const override                       \
    {                                                                       \
        CORE_LOG_E("Method " #methodName " not supported for IGsPicking!"); \
        return {};                                                          \
    }

class GsPicking : public IGsPicking {
public:
    GsPicking() = default;
    ~GsPicking() override = default;

    void BuildRaycastCpuAccelerationVolume(
        CORE_NS::IEcs const& ecs, BASE_NS::vector<BASE_NS::vector<GsData>>& gsdata) override;

    void RemoveGsEcsSystem(CORE_NS::IEcs const& ecs) override;

    BASE_NS::vector<CORE3D_NS::RayCastResult> RayCastFromCamera(
        CORE_NS::IEcs const& ecs, CORE_NS::Entity camera, const BASE_NS::Math::Vec2& screenPos) const override;

    BASE_NS::vector<CORE3D_NS::RayCastResult> RayCastFromCamera(CORE_NS::IEcs const& ecs, CORE_NS::Entity camera,
        const BASE_NS::Math::Vec2& screenPos, uint64_t layerMask) const override;

    void SetGsObjectWorldMatrices(CORE_NS::IEcs const& ecs, BASE_NS::vector<BASE_NS::Math::Mat4X4>& gsWorldMatrices,
        BASE_NS::vector<Core3D::ISceneNode*> gsSceneNodes) override;

    // IInterface
    const IInterface* GetInterface(const BASE_NS::Uid& uid) const override;
    IInterface* GetInterface(const BASE_NS::Uid& uid) override;

    void Ref() override;
    void Unref() override;

    // unsupported functions for GsPicking

    DECLARE_UNSUPPORTED_PICKING_METHOD(BASE_NS::Math::Vec3, ScreenToWorld, CORE_NS::IEcs const& ecs,
        CORE_NS::Entity cameraEntity, BASE_NS::Math::Vec3 screenCoordinate)

    DECLARE_UNSUPPORTED_PICKING_METHOD(BASE_NS::Math::Vec3, WorldToScreen, CORE_NS::IEcs const& ecs,
        CORE_NS::Entity cameraEntity, BASE_NS::Math::Vec3 worldCoordinate)

    DECLARE_UNSUPPORTED_PICKING_METHOD(BASE_NS::vector<CORE3D_NS::RayCastResult>, RayCast, CORE_NS::IEcs const& ecs,
        const BASE_NS::Math::Vec3& start, const BASE_NS::Math::Vec3& direction)

    DECLARE_UNSUPPORTED_PICKING_METHOD(BASE_NS::vector<CORE3D_NS::RayCastResult>, RayCast, CORE_NS::IEcs const& ecs,
        const BASE_NS::Math::Vec3& start, const BASE_NS::Math::Vec3& direction, uint64_t layerMask)

    DECLARE_UNSUPPORTED_PICKING_METHOD(BASE_NS::vector<CORE3D_NS::RayTriangleCastResult>, RayCast,
        const BASE_NS::Math::Vec3& start, const BASE_NS::Math::Vec3& direction,
        BASE_NS::array_view<const BASE_NS::Math::Vec3> triangles)

    DECLARE_UNSUPPORTED_PICKING_METHOD(BASE_NS::vector<CORE3D_NS::RayTriangleCastResult>, RayCastFromCamera,
        CORE_NS::IEcs const& ecs, CORE_NS::Entity camera, const BASE_NS::Math::Vec2& screenPos,
        BASE_NS::array_view<const BASE_NS::Math::Vec3> triangles)

    DECLARE_UNSUPPORTED_PICKING_METHOD(CORE3D_NS::MinAndMax, GetWorldAABB, const BASE_NS::Math::Mat4X4& world,
        const BASE_NS::Math::Vec3& aabbMin, const BASE_NS::Math::Vec3& aabbMax)

    DECLARE_UNSUPPORTED_PICKING_METHOD(
        CORE3D_NS::MinAndMax, GetWorldMatrixComponentAABB, CORE_NS::Entity entity, bool isRecursive, CORE_NS::IEcs& ecs)

    DECLARE_UNSUPPORTED_PICKING_METHOD(
        CORE3D_NS::MinAndMax, GetTransformComponentAABB, CORE_NS::Entity entity, bool isRecursive, CORE_NS::IEcs& ecs)

protected:
    inline BASE_NS::Math::Vec3 DirectionVectorInverse(const BASE_NS::Math::Vec3& direction) const
    {
        BASE_NS::Math::Vec3 invDir { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
            std::numeric_limits<float>::max() };

        if (direction.x != 0.f) {
            invDir.x = 1.f / direction.x;
        }
        if (direction.y != 0.f) {
            invDir.y = 1.f / direction.y;
        }
        if (direction.z != 0.f) {
            invDir.z = 1.f / direction.z;
        }

        return invDir;
    }

    const float AABB_ASSIGN_OPACITY_THRESHOLD { 0.2f };
    const float OPACITY_THRESHOLD { 0.99f };

    struct AABB {
        BASE_NS::Math::Vec3 min;
        BASE_NS::Math::Vec3 max;
        BASE_NS::vector<GsData> data;

        uint32_t objectIdx { 0u };

        AABB()
            : min(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                  std::numeric_limits<float>::max()),
              max(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                  std::numeric_limits<float>::lowest())
        {}
    };

    struct EcsGsScene {
        BASE_NS::vector<BASE_NS::vector<AABB>> aabbStructure;
        BASE_NS::vector<BASE_NS::Math::Mat4X4> worldMatrices;
        BASE_NS::vector<Core3D::ISceneNode*> sceneNodes;
    };

    // storage support for multi-ECS scenes
    BASE_NS::unordered_map<uint64_t, EcsGsScene> ecsGsScenes_;

    mutable std::mutex gsUpdateMtx;
};

inline constexpr BASE_NS::string_view GetName(const IGsPicking*)
{
    return "IGsPicking";
}

} // namespace GS

#endif // GS_UTIL_GSPICKING_H