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

#include "gs_picking.h"

#include <algorithm>

#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/joint_matrices_component.h>
#include <3d/ecs/components/layer_component.h>
#include <3d/ecs/components/local_matrix_component.h>
#include <3d/ecs/components/mesh_component.h>
#include <3d/ecs/components/render_mesh_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/components/world_matrix_component.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/util/intf_scene_util.h>
#include <base/containers/fixed_string.h>
#include <base/containers/unordered_map.h>
#include <base/math/mathf.h>
#include <base/math/matrix_util.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_ecs.h>
#include <core/implementation_uids.h>
#include <core/log.h>
#include <core/namespace.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/plugin/intf_class_factory.h>
#include <core/plugin/intf_class_register.h>
#include <core/plugin/intf_plugin_register.h>
#include <core/property/intf_property_handle.h>

namespace GS {

namespace CameraMatrixUtil {
BASE_NS::Math::Mat4X4 CalculateProjectionMatrix(const Core3D::CameraComponent& cameraComponent, bool& isCameraNegative)
{
    switch (cameraComponent.projection) {
        case Core3D::CameraComponent::Projection::ORTHOGRAPHIC: {
            auto orthoProj = BASE_NS::Math::OrthoRhZo(cameraComponent.xMag * -0.5f, cameraComponent.xMag * 0.5f,
                cameraComponent.yMag * -0.5f, cameraComponent.yMag * 0.5f, cameraComponent.zNear, cameraComponent.zFar);
            orthoProj[1][1] *= -1.f; // left-hand NDC while Vulkan right-handed -> flip y
            return orthoProj;
        }
        case Core3D::CameraComponent::Projection::PERSPECTIVE: {
            auto aspect = 1.f;
            if (cameraComponent.aspect > 0.f) {
                aspect = cameraComponent.aspect;
            } else if (cameraComponent.renderResolution.y > 0U) {
                aspect = static_cast<float>(cameraComponent.renderResolution.x) /
                         static_cast<float>(cameraComponent.renderResolution.y);
            }
            auto persProj = BASE_NS::Math::PerspectiveRhZo(
                cameraComponent.yFov, aspect, cameraComponent.zNear, cameraComponent.zFar);
            persProj[1][1] *= -1.f; // left-hand NDC while Vulkan right-handed -> flip y
            return persProj;
        }
        case Core3D::CameraComponent::Projection::FRUSTUM: {
            auto aspect = 1.f;
            if (cameraComponent.aspect > 0.f) {
                aspect = cameraComponent.aspect;
            } else if (cameraComponent.renderResolution.y > 0U) {
                aspect = static_cast<float>(cameraComponent.renderResolution.x) /
                         static_cast<float>(cameraComponent.renderResolution.y);
            }
            // with offset 0.5, the camera should offset half the screen
            const float scale = tan(cameraComponent.yFov * 0.5f) * cameraComponent.zNear;
            const float xOffset = cameraComponent.xOffset * scale * aspect * 2.0f;
            const float yOffset = cameraComponent.yOffset * scale * 2.0f;
            float left = -aspect * scale;
            float right = aspect * scale;
            float bottom = -scale;
            float top = scale;
            auto persProj = BASE_NS::Math::PerspectiveRhZo(left + xOffset, right + xOffset, bottom + yOffset,
                top + yOffset, cameraComponent.zNear, cameraComponent.zFar);
            persProj[1][1] *= -1.f; // left-hand NDC while Vulkan right-handed -> flip y
            return persProj;
        }
        case Core3D::CameraComponent::Projection::CUSTOM: {
            isCameraNegative = BASE_NS::Math::Determinant(cameraComponent.customProjectionMatrix) < 0.0f;
            return cameraComponent.customProjectionMatrix;
        }
        default:
            return BASE_NS::Math::IDENTITY_4X4;
    }
}
} // namespace CameraMatrixUtil

BASE_NS::Math::Vec3 ScreenToWorld(const Core3D::CameraComponent& cameraComponent,
    const BASE_NS::Math::Mat4X4& cameraWorldMatrix, BASE_NS::Math::Vec3 screenCoordinate)
{
    screenCoordinate.x = (screenCoordinate.x - 0.5f) * 2.0f;
    screenCoordinate.y = (screenCoordinate.y - 0.5f) * 2.0f;

    bool isCameraNegative = false;
    BASE_NS::Math::Mat4X4 projToView =
        BASE_NS::Math::Inverse(CameraMatrixUtil::CalculateProjectionMatrix(cameraComponent, isCameraNegative));

    auto const& worldFromView = cameraWorldMatrix;
    const auto viewCoordinate =
        (projToView * BASE_NS::Math::Vec4(screenCoordinate.x, screenCoordinate.y, screenCoordinate.z, 1.0f));

    auto worldCoordinate = worldFromView * viewCoordinate;
    worldCoordinate /= worldCoordinate.w;
    return BASE_NS::Math::Vec3 { worldCoordinate.x, worldCoordinate.y, worldCoordinate.z };
}

struct Ray {
    BASE_NS::Math::Vec3 origin;
    BASE_NS::Math::Vec3 direction;
};

Ray RayFromCamera(const Core3D::CameraComponent& cameraComponent,
    const Core3D::WorldMatrixComponent& cameraWorldMatrixComponent, BASE_NS::Math::Vec2 screenCoordinate)
{
    if (cameraComponent.projection == CORE3D_NS::CameraComponent::Projection::ORTHOGRAPHIC) {
        const BASE_NS::Math::Vec3 worldPos = ScreenToWorld(cameraComponent, cameraWorldMatrixComponent.matrix,
            BASE_NS::Math::Vec3(screenCoordinate.x, screenCoordinate.y, 0.0f));

        const auto direction = cameraWorldMatrixComponent.matrix * BASE_NS::Math::Vec4(0.0f, 0.0f, -1.0f, 0.0f);
        return Ray { worldPos, direction };
    }
    // Ray origin is the camera world position.
    const BASE_NS::Math::Vec3& rayOrigin = BASE_NS::Math::Vec3(cameraWorldMatrixComponent.matrix.w);
    const BASE_NS::Math::Vec3 targetPos = ScreenToWorld(cameraComponent, cameraWorldMatrixComponent.matrix,
        BASE_NS::Math::Vec3(screenCoordinate.x, screenCoordinate.y, 1.0f));

    const BASE_NS::Math::Vec3 direction = BASE_NS::Math::Normalize(targetPos - rayOrigin);
    return Ray { rayOrigin, direction };
}

void GsPicking::BuildRaycastCpuAccelerationVolume(
    CORE_NS::IEcs const& ecs, BASE_NS::vector<BASE_NS::vector<GsData>>& gsdata)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "GsPicking::BuildRaycastCpuAccelerationVolume", "", 0xFF00FF);

    const std::lock_guard lg(gsUpdateMtx);

    const uint64_t ecsId = ecs.GetId();

    if (!ecsGsScenes_.contains(ecsId)) {
        ecsGsScenes_.insert({ ecsId, {} });
    }

    EcsGsScene& ecsScene = ecsGsScenes_[ecsId];

    if (gsdata.empty()) {
        ecsScene.aabbStructure.clear();
        ecsScene.worldMatrices.clear();
        ecsScene.sceneNodes.clear();
        return;
    }

    ecsScene.aabbStructure.clear();
    ecsScene.aabbStructure.resize(gsdata.size());

    constexpr size_t targetItemsPerAABB = 1000u;

    // process each GS object separately
    for (size_t objectIndex = 0; objectIndex < gsdata.size(); objectIndex++) {
        const auto& currentObject = gsdata[objectIndex];

        if (currentObject.empty()) {
            continue;
        }

        // find bounding box for this object
        BASE_NS::Math::Vec3 minBounds(
            std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
        BASE_NS::Math::Vec3 maxBounds(std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
            std::numeric_limits<float>::lowest());

        for (const auto& data : currentObject) {
            BASE_NS::Math::Vec3 pos(data.positionOpacity.x, data.positionOpacity.y, data.positionOpacity.z);
            minBounds = BASE_NS::Math::min(minBounds, pos);
            maxBounds = BASE_NS::Math::max(maxBounds, pos);
        }

        constexpr float epsilon = 1e-6f;
        const BASE_NS::Math::Vec3 size =
            BASE_NS::Math::max(maxBounds - minBounds, BASE_NS::Math::Vec3(epsilon, epsilon, epsilon));
        maxBounds = minBounds + size;

        // use a simple grid size calculation
        const size_t totalItems = currentObject.size();
        const size_t gridSize =
            static_cast<size_t>(std::cbrt(static_cast<float>(totalItems) / static_cast<float>(targetItemsPerAABB))) +
            1u;

        const BASE_NS::Math::Vec3 cellSize((maxBounds.x - minBounds.x) / static_cast<float>(gridSize),
            (maxBounds.y - minBounds.y) / static_cast<float>(gridSize),
            (maxBounds.z - minBounds.z) / static_cast<float>(gridSize));

        BASE_NS::unordered_map<size_t, BASE_NS::vector<GsData>> gridCells;

        // assign points to cells
        for (const auto& data : currentObject) {
            if (data.positionOpacity.w <= AABB_ASSIGN_OPACITY_THRESHOLD) {
                continue;
            }

            BASE_NS::Math::Vec3 pos(data.positionOpacity.x, data.positionOpacity.y, data.positionOpacity.z);

            const size_t xx = BASE_NS::Math::min(static_cast<size_t>((pos.x - minBounds.x) / cellSize.x), gridSize - 1);
            const size_t yy = BASE_NS::Math::min(static_cast<size_t>((pos.y - minBounds.y) / cellSize.y), gridSize - 1);
            const size_t zz = BASE_NS::Math::min(static_cast<size_t>((pos.z - minBounds.z) / cellSize.z), gridSize - 1);

            const size_t cellKey = xx + yy * gridSize + zz * gridSize * gridSize;
            gridCells[cellKey].push_back(data);
        }

        // create AABBs directly, splitting large cells as needed
        ecsScene.aabbStructure[objectIndex].reserve(gridCells.size() * size_t(2u));

        for (const auto& cell : gridCells) {
            if (cell.second.empty()) {
                continue;
            }

            if (cell.second.size() <= targetItemsPerAABB * size_t(2u)) {
                // cell is small enough, create single AABB
                AABB aabb;
                aabb.data = cell.second;
                aabb.objectIdx = static_cast<uint32_t>(objectIndex);

                aabb.min = BASE_NS::Math::Vec3(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max());
                aabb.max = BASE_NS::Math::Vec3(std::numeric_limits<float>::lowest(),
                    std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

                for (const GsData& data : cell.second) {
                    aabb.min = BASE_NS::Math::min(aabb.min, data.positionOpacity);
                    aabb.max = BASE_NS::Math::max(aabb.max, data.positionOpacity);
                }

                ecsScene.aabbStructure[objectIndex].push_back(BASE_NS::move(aabb));
            } else {
                // split large cell into multiple AABBs
                const size_t numSubCells = (cell.second.size() + targetItemsPerAABB - 1) / targetItemsPerAABB;
                const size_t itemsPerSubCell = cell.second.size() / numSubCells;

                for (size_t ii = 0; ii < numSubCells; ii++) {
                    const size_t startIdx = ii * itemsPerSubCell;
                    const size_t endIdx = (ii == numSubCells - 1) ? cell.second.size() : (ii + 1) * itemsPerSubCell;

                    AABB aabb;
                    aabb.data.reserve(endIdx - startIdx);
                    for (size_t jj = startIdx; jj < endIdx; jj++) {
                        aabb.data.push_back(cell.second[jj]);
                    }
                    aabb.objectIdx = static_cast<uint32_t>(objectIndex);

                    aabb.min = BASE_NS::Math::Vec3(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max());
                    aabb.max = BASE_NS::Math::Vec3(std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

                    for (const GsData& data : aabb.data) {
                        aabb.min = BASE_NS::Math::min(aabb.min, data.positionOpacity);
                        aabb.max = BASE_NS::Math::max(aabb.max, data.positionOpacity);
                    }

                    ecsScene.aabbStructure[objectIndex].push_back(BASE_NS::move(aabb));
                }
            }
        }
    }

#ifndef NDEBUG
    size_t totalNodes = 0;
    size_t maxNodeSize = 0;
    size_t minNodeSize = std::numeric_limits<size_t>::max();

    for (const auto& objectAABBs : ecsScene.aabbStructure) {
        totalNodes += objectAABBs.size();

        for (const auto& aabb : objectAABBs) {
            maxNodeSize = BASE_NS::Math::max(maxNodeSize, aabb.data.size());
            minNodeSize = BASE_NS::Math::min(minNodeSize, aabb.data.size());
        }
    }
    CORE_LOG_I("GS3D CPU Raycast acceleration structure built with %u objects, %u total nodes (min: %u, max: %u items "
               "per node)",
        static_cast<uint32_t>(ecsScene.aabbStructure.size()), static_cast<uint32_t>(totalNodes),
        static_cast<uint32_t>(minNodeSize), static_cast<uint32_t>(maxNodeSize));
#endif
}

void GsPicking::RemoveGsEcsSystem(CORE_NS::IEcs const& ecs)
{
    const std::lock_guard lg(gsUpdateMtx);

    if (ecsGsScenes_.contains(ecs.GetId())) {
        ecsGsScenes_.erase(ecs.GetId());

#ifndef NDEBUG
        CORE_LOG_I("GsPicking: ECS scene %llu removed", ecs.GetId());
#endif
    }
}

bool IntersectAabb(BASE_NS::Math::Vec3 aabbMin, BASE_NS::Math::Vec3 aabbMax, BASE_NS::Math::Vec3 start,
    BASE_NS::Math::Vec3 invDirection, float& hitDistance)
{
    const float tx1 = (aabbMin.x - start.x) * invDirection.x;
    const float tx2 = (aabbMax.x - start.x) * invDirection.x;

    float tmin = BASE_NS::Math::min(tx1, tx2);
    float tmax = BASE_NS::Math::max(tx1, tx2);

    const float ty1 = (aabbMin.y - start.y) * invDirection.y;
    const float ty2 = (aabbMax.y - start.y) * invDirection.y;

    tmin = BASE_NS::Math::max(tmin, BASE_NS::Math::min(ty1, ty2));
    tmax = BASE_NS::Math::min(tmax, BASE_NS::Math::max(ty1, ty2));

    const float tz1 = (aabbMin.z - start.z) * invDirection.z;
    const float tz2 = (aabbMax.z - start.z) * invDirection.z;

    tmin = BASE_NS::Math::max(tmin, BASE_NS::Math::min(tz1, tz2));
    tmax = BASE_NS::Math::min(tmax, BASE_NS::Math::max(tz1, tz2));

    hitDistance = tmin;
    // If hitDistance < 0, ray origin was inside the AABB.
    if (hitDistance < 0.0f) {
        hitDistance = tmax;
        // No hit, set distance to 0.
        if (hitDistance < 0.0f) {
            hitDistance = 0.0f;
        }
    }

    return tmax >= tmin && tmax > 0.0f;
}

float MaxSplatExtentWorld(const float splatExtent, const BASE_NS::Math::Mat4X4 worldMatrix)
{
    const BASE_NS::Math::Mat3X3 jacobian(worldMatrix.x, worldMatrix.y, worldMatrix.z);

    constexpr size_t POWER_ITERATIONS = 2u;
    BASE_NS::Math::Vec3 vv = BASE_NS::Math::Vec3(1.0f, 0.0f, 0.0f);
    const BASE_NS::Math::Mat3X3 jacobianT = BASE_NS::Math::Transpose(jacobian);
    const BASE_NS::Math::Mat3X3 jtj = jacobianT * jacobian;

    for (size_t ii = 0; ii < POWER_ITERATIONS; ii++) {
        vv = BASE_NS::Math::Normalize(jtj * vv);
    }
    const float maxSingularValueSquared = BASE_NS::Math::Dot(vv, jtj * vv);
    const float maxSingularValue = BASE_NS::Math::sqrt(maxSingularValueSquared);

    return splatExtent * maxSingularValue;
}

bool RayIntersectCircle(const BASE_NS::Math::Vec3 rayOrigin, const BASE_NS::Math::Vec3 rayDir,
    const BASE_NS::Math::Vec3 splatCenter, const float radius, float& rayDistance)
{
    const BASE_NS::Math::Vec3 toCenter = splatCenter - rayOrigin;

    // project ray direction onto the center vector
    const float denominator = BASE_NS::Math::Dot(rayDir, toCenter);

    if (denominator <= 0.0) {
        // ray is parallel
        return false;
    }

    const float tt = BASE_NS::Math::Dot(toCenter, toCenter) / denominator;
    const BASE_NS::Math::Vec3 intersectionPoint = rayOrigin + rayDir * tt;

    // check if point is inside the circle
    const BASE_NS::Math::Vec3 diff = intersectionPoint - splatCenter;
    const float diffDistSq = BASE_NS::Math::Dot(diff, diff);

    if (diffDistSq <= radius * radius) {
        rayDistance = tt;
        return true;
    }

    return false;
}

BASE_NS::vector<CORE3D_NS::RayCastResult> GsPicking::RayCastFromCamera(
    CORE_NS::IEcs const& ecs, CORE_NS::Entity camera, const BASE_NS::Math::Vec2& screenPos) const
{
    return RayCastFromCamera(ecs, camera, screenPos, CORE3D_NS::LayerConstants::ALL_LAYER_MASK);
}

BASE_NS::vector<CORE3D_NS::RayCastResult> GsPicking::RayCastFromCamera(
    CORE_NS::IEcs const& ecs, CORE_NS::Entity camera, const BASE_NS::Math::Vec2& screenPos, uint64_t layerMask) const
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "GsPicking::RayCastFromCamera", "", 0xFF00FF);

    const std::lock_guard lg(gsUpdateMtx);

    const uint64_t ecsId = ecs.GetId();
    const auto& layerComponentManager = CORE_NS::GetManager<CORE3D_NS::ILayerComponentManager>(ecs);

    const auto sceneIt = ecsGsScenes_.find(ecsId);
    if (sceneIt == ecsGsScenes_.end()) {
#ifndef NDEBUG
        CORE_LOG_W("No GS ECS scene found for ECS %llu", ecsId);
#endif
        return {};
    }
    const EcsGsScene& ecsScene = sceneIt->second;

    const auto* worldMatrixManager = CORE_NS::GetManager<CORE3D_NS::IWorldMatrixComponentManager>(ecs);
    const auto* cameraManager = CORE_NS::GetManager<CORE3D_NS::ICameraComponentManager>(ecs);
    if (!worldMatrixManager || !cameraManager) {
        return {};
    }

    const auto wmcId = worldMatrixManager->GetComponentId(camera);
    const auto ccId = cameraManager->GetComponentId(camera);

    if (wmcId != CORE_NS::IComponentManager::INVALID_COMPONENT_ID &&
        ccId != CORE_NS::IComponentManager::INVALID_COMPONENT_ID) {
        const auto cameraComponent = cameraManager->Read(ccId);
        const auto worldMatrixComponent = worldMatrixManager->Get(wmcId);
        const Ray ray = RayFromCamera(*cameraComponent, worldMatrixComponent, screenPos);

#ifndef NDEBUG
        CORE_LOG_I("origin: %f, %f, %f, dir: %f, %f, %f", ray.origin.x, ray.origin.y, ray.origin.z, ray.direction.x,
            ray.direction.y, ray.direction.z);
#endif

        // raycast
        {
            const BASE_NS::Math::Vec3 invDirection = DirectionVectorInverse(ray.direction);

            struct SplatHit {
                float distance;
                float opacity;
                BASE_NS::Math::Vec3 worldPos;
                int objectIndex;

                bool operator<(const SplatHit& other) const
                {
                    return distance < other.distance;
                }
            };

            BASE_NS::vector<SplatHit> splatCandidates;
            splatCandidates.reserve(256u);

            CORE_CPU_PERF_BEGIN(
                gsPickingAabbIntersect, "LUMEGS", "GsPicking::RaycastFromCameraCpu::IntersectAABB", "", 0xFFFF00);

            for (size_t outerIdx = 0; outerIdx < ecsScene.aabbStructure.size(); outerIdx++) {
                if (outerIdx >= ecsScene.worldMatrices.size()) {
                    continue;
                }

                const auto& worldMatrix = ecsScene.worldMatrices[outerIdx];
                const auto& aabbGroup = ecsScene.aabbStructure[outerIdx];

                for (size_t innerIdx = 0; innerIdx < aabbGroup.size(); innerIdx++) {
                    const AABB& aabb = aabbGroup[innerIdx];

                    if (layerComponentManager) {
                        const CORE3D_NS::ISceneNode* objectNode =
                            ecsScene.sceneNodes[static_cast<size_t>(aabb.objectIdx)];
                        if (!(layerComponentManager->Get(objectNode->GetEntity()).layerMask & layerMask)) {
                            continue;
                        }
                    }

                    // transform AABB corners by world matrix
                    const BASE_NS::Math::Vec3 corners[8] = { { aabb.min.x, aabb.min.y, aabb.min.z },
                        { aabb.max.x, aabb.min.y, aabb.min.z }, { aabb.min.x, aabb.max.y, aabb.min.z },
                        { aabb.max.x, aabb.max.y, aabb.min.z }, { aabb.min.x, aabb.min.y, aabb.max.z },
                        { aabb.max.x, aabb.min.y, aabb.max.z }, { aabb.min.x, aabb.max.y, aabb.max.z },
                        { aabb.max.x, aabb.max.y, aabb.max.z } };

                    BASE_NS::Math::Vec3 transformedMin(std::numeric_limits<float>::max(),
                        std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
                    BASE_NS::Math::Vec3 transformedMax(std::numeric_limits<float>::lowest(),
                        std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest());

                    for (size_t ii = 0; ii < 8u; ii++) {
                        BASE_NS::Math::Vec4 corner4d(corners[ii].x, corners[ii].y, corners[ii].z, 1.0f);
                        BASE_NS::Math::Vec4 transformed = worldMatrix * corner4d;
                        BASE_NS::Math::Vec3 transformedCorner(transformed.x, transformed.y, transformed.z);

                        transformedMin.x = BASE_NS::Math::min(transformedMin.x, transformedCorner.x);
                        transformedMin.y = BASE_NS::Math::min(transformedMin.y, transformedCorner.y);
                        transformedMin.z = BASE_NS::Math::min(transformedMin.z, transformedCorner.z);

                        transformedMax.x = BASE_NS::Math::max(transformedMax.x, transformedCorner.x);
                        transformedMax.y = BASE_NS::Math::max(transformedMax.y, transformedCorner.y);
                        transformedMax.z = BASE_NS::Math::max(transformedMax.z, transformedCorner.z);
                    }

                    float hitDistance;
                    if (IntersectAabb(transformedMin, transformedMax, ray.origin, invDirection, hitDistance)) {
                        // collect all splats within AABB intersection
                        const BASE_NS::Math::Mat4X4 wm = ecsScene.worldMatrices[aabb.objectIdx];

                        for (const auto& gsData : aabb.data) {
                            const float maxExtent = MaxSplatExtentWorld(gsData.extent, wm);
                            const BASE_NS::Math::Vec3 splatWorldPos =
                                wm * BASE_NS::Math::Vec4(BASE_NS::Math::Vec3(gsData.positionOpacity), 1.0f);

                            float distance = 0.0f;
                            if (RayIntersectCircle(ray.origin, ray.direction, splatWorldPos, maxExtent, distance)) {
                                splatCandidates.push_back({ distance, gsData.positionOpacity.w, splatWorldPos,
                                    static_cast<int>(aabb.objectIdx) });
                            }
                        }
                    }
                }
            }

            CORE_CPU_PERF_END(gsPickingAabbIntersect);

            // sort by front-to-back distance
            if (!splatCandidates.empty()) {
                CORE_CPU_PERF_BEGIN(
                    gsPickingSortSplats, "LUMEGS", "GsPicking::RaycastFromCameraCpu::SortSplats", "", 0xFFFF00);

                std::sort(splatCandidates.begin(), splatCandidates.end());

                CORE_CPU_PERF_END(gsPickingSortSplats);

                // accumulate until opacity reaches 1.0f
                CORE_CPU_PERF_BEGIN(
                    gsPickingAccumulate, "LUMEGS", "GsPicking::RaycastFromCameraCpu::Accumulate", "", 0xFFFF00);

                float accumulatedOpacity = 0.0f;
                float accumulatedDistance = 0.0f;
                int finalObjectIndex = -1;

#ifndef NDEBUG
                size_t numAccumulated = 0;
#endif

                for (const auto& candidate : splatCandidates) {
                    if (accumulatedOpacity >= OPACITY_THRESHOLD) {
                        break;
                    }

                    const float splatContrib = candidate.opacity * (1.0f - accumulatedOpacity);

                    accumulatedDistance += candidate.distance * splatContrib;

                    if (finalObjectIndex == -1) {
                        finalObjectIndex = candidate.objectIndex;
                    }

                    accumulatedOpacity += splatContrib;

#ifndef NDEBUG
                    numAccumulated++;
#endif
                }

                CORE_CPU_PERF_END(gsPickingAccumulate);

                if (accumulatedOpacity > 0.01f) {
                    CORE3D_NS::RayCastResult result;

                    result.worldPosition = ray.origin + ray.direction * accumulatedDistance;
                    result.distance = accumulatedDistance;
                    result.node =
                        (finalObjectIndex == -1) ? nullptr : ecsScene.sceneNodes[static_cast<size_t>(finalObjectIndex)];
                    result.centerDistance = result.distance;

#ifndef NDEBUG
                    CORE_LOG_I("Ray hit found with accumulated opacity: %f at distance: %f with %d accumulated",
                        accumulatedOpacity, accumulatedDistance, static_cast<int>(numAccumulated));
#endif
                    return BASE_NS::vector<CORE3D_NS::RayCastResult> { result };
                }
            }
        }
    }

    return {};
}

void GsPicking::SetGsObjectWorldMatrices(CORE_NS::IEcs const& ecs,
    BASE_NS::vector<BASE_NS::Math::Mat4X4>& gsWorldMatrices, BASE_NS::vector<Core3D::ISceneNode*> gsSceneNodes)
{
    const std::lock_guard lg(gsUpdateMtx);

    const uint64_t ecsId = ecs.GetId();
    if (!ecsGsScenes_.contains(ecsId)) {
        ecsGsScenes_.insert({ ecsId, {} });
    }

    ecsGsScenes_[ecsId].worldMatrices = gsWorldMatrices;
    ecsGsScenes_[ecsId].sceneNodes = gsSceneNodes;
}

const CORE_NS::IInterface* GsPicking::GetInterface(const BASE_NS::Uid& uid) const
{
    if ((uid == IGsPicking::UID) || (uid == IInterface::UID)) {
        return this;
    }
    return nullptr;
}

CORE_NS::IInterface* GsPicking::GetInterface(const BASE_NS::Uid& uid)
{
    if ((uid == IGsPicking::UID) || (uid == IInterface::UID)) {
        return this;
    }
    return nullptr;
}

void GsPicking::Ref() {}

void GsPicking::Unref() {}

} // namespace GS