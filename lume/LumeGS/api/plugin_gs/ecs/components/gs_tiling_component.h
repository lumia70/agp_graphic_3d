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

#if !defined(PLUGIN_API_GS_TILING_COMPONENT_H) || defined(IMPLEMENT_MANAGER)
#define PLUGIN_API_GS_TILING_COMPONENT_H

#ifndef IMPLEMENT_MANAGER
#include <cstdint>

#include <base/math/vector.h>
#include <core/ecs/component_struct_macros.h>
#include <core/ecs/intf_component_manager.h>
#include <core/namespace.h>
#include <core/property/property_types.h>

namespace GS {
#endif

BEGIN_COMPONENT(IGsTilingComponentManager, GsTilingComponent)

    /** URI of the tile file (.sog, .ply, etc.) — set by app before entity is live. */
    DEFINE_PROPERTY(BASE_NS::string, uri, "Tile file URI", 0, VALUE(""))
    DEFINE_PROPERTY(uint32_t, offset, "Byte offset", 0, VALUE(0))
    DEFINE_PROPERTY(uint32_t, count, "Number of splats", 0, VALUE(0))

    /** World-space AABB min, set by app from BVH metadata, used for culling. */
    DEFINE_PROPERTY(BASE_NS::Math::Vec3, aabbMin, "AABB min", 0, VALUE(BASE_NS::Math::Vec3(0.f, 0.f, 0.f)))

    /** World-space AABB max, set by app from BVH metadata, used for culling. */
    DEFINE_PROPERTY(BASE_NS::Math::Vec3, aabbMax, "AABB max", 0, VALUE(BASE_NS::Math::Vec3(0.f, 0.f, 0.f)))

    /** Visible - when false skip rendering this tile without evicting it from memory. */
    DEFINE_PROPERTY(bool, visible, "Is visible", 0, VALUE(true))

    /** Read-only: set to true by GsTilingSystem once splat data is loaded. */
    DEFINE_PROPERTY(bool, isResident, "Is resident in GPU", CORE_NS::PropertyFlags::IS_READONLY, VALUE(false))

    /** Read-only: number of splats currently resident for this tile. */
    DEFINE_PROPERTY(uint32_t, numResidentSplats, "Num resident splats", CORE_NS::PropertyFlags::IS_READONLY, VALUE(0))

    DEFINE_PROPERTY(bool, preload, "Is preload", 0, VALUE(false))

END_COMPONENT(IGsTilingComponentManager, GsTilingComponent, "c3d4e5f6-a7b8-4901-bcde-f01234567890")

#ifndef IMPLEMENT_MANAGER
}
#endif
#endif // PLUGIN_API_GS_TILING_COMPONENT_H
