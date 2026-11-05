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

#ifndef PLUGIN_GS_UTILS_H
#define PLUGIN_GS_UTILS_H

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <numeric>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <thread>
#include <utility>

#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/environment_component.h>
#include <3d/ecs/components/light_component.h>
#include <3d/ecs/components/local_matrix_component.h>
#include <3d/ecs/components/material_component.h>
#include <3d/ecs/components/mesh_component.h>
#include <3d/ecs/components/name_component.h>
#include <3d/ecs/components/node_component.h>
#include <3d/ecs/components/post_process_configuration_component.h>
#include <3d/ecs/components/render_configuration_component.h>
#include <3d/ecs/components/render_handle_component.h>
#include <3d/ecs/components/render_mesh_component.h>
#include <3d/ecs/components/transform_component.h>
#include <3d/ecs/components/uri_component.h>
#include <3d/ecs/components/world_matrix_component.h>
#include <3d/ecs/systems/intf_animation_system.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <3d/ecs/systems/intf_render_preprocessor_system.h>
#include <base/containers/array_view.h>
#include <base/containers/byte_array.h>
#include <base/containers/fixed_string.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/float_packer.h>
#include <base/math/matrix_util.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector_util.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/json/json.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/plugin/intf_class_factory.h>
#include <core/property/intf_property_api.h>
#include <core/property/intf_property_handle.h>
#include <core/property/property_handle_util.h>
#include <core/util/parallel_sort.h>
#include <render/device/intf_gpu_resource_manager.h>

namespace GS {

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;

inline void GetLargestOffDiagonalElement(const Math::Mat3X3& S, float& pivot, size_t& pivot_i, size_t& pivot_j) noexcept
{
    for (size_t ii = 0; ii < 3; ii++) {
        for (size_t jj = ii + 1; jj < 3; jj++) {
            if (Math::abs(S[ii][jj]) > pivot) {
                pivot = Math::abs(S[ii][jj]);
                pivot_i = ii;
                pivot_j = jj;
            }
        }
    }
}

inline uint32_t PackHalf2x16(const float x, const float y)
{
    const uint16_t x_half = BASE_NS::Math::F32ToF16(x);
    const uint16_t y_half = BASE_NS::Math::F32ToF16(y);
    return (static_cast<uint32_t>(y_half) << 16u) | x_half;
}

inline uint32_t PackUint2x16(uint32_t x, uint32_t y)
{
    x &= 0xFFFFu;
    y &= 0xFFFFu;

    return (y << 16u) | x;
}

inline BASE_NS::Math::UVec2 UnpackUint2x16(uint32_t pack)
{
    uint32_t x = pack & 0xFFFF;
    uint32_t y = (pack >> 16) & 0xFFFF;

    return BASE_NS::Math::UVec2(x, y);
}

inline BASE_NS::Math::Vec2 UnpackHalf2x16(const uint32_t pack)
{
    uint16_t x = static_cast<uint16_t>(pack & 0xFFFF);
    uint16_t y = static_cast<uint16_t>((pack >> 16) & 0xFFFF);
    return BASE_NS::Math::Vec2(BASE_NS::Math::F16ToF32(x), BASE_NS::Math::F16ToF32(y));
}

inline uint32_t PackGSColor(const Math::Vec3& color, const float opacity)
{
    // pack the color into 8 bits per channel

    uint32_t packed = 0;

    const uint8_t r = static_cast<uint8_t>(Math::clamp(color.x * 255.0f, 0.0f, 255.0f));
    const uint8_t g = static_cast<uint8_t>(Math::clamp(color.y * 255.0f, 0.0f, 255.0f));
    const uint8_t b = static_cast<uint8_t>(Math::clamp(color.z * 255.0f, 0.0f, 255.0f));
    const uint8_t a = static_cast<uint8_t>(Math::clamp(opacity * 255.0f, 0.0f, 255.0f));

    packed |= (r << 24u);
    packed |= (g << 16u);
    packed |= (b << 8u);
    packed |= a;

    return packed;
}

inline uint32_t PackGSDcWithMinMax(const Math::Vec3& dc, float dcMin, float dcMax)
{
    // pack the normalized dc into 8 bits per channel

    uint32_t packed = 0;
    if (dcMin > dcMax) { std::swap(dcMin, dcMax); }
    Math::Vec3 normalizedDc = (dc - dcMin) / (dcMax - dcMin + 1e-6f);

    const uint8_t r = static_cast<uint8_t>(Math::clamp(normalizedDc.x * 255.0f, 0.0f, 255.0f));
    const uint8_t g = static_cast<uint8_t>(Math::clamp(normalizedDc.y * 255.0f, 0.0f, 255.0f));
    const uint8_t b = static_cast<uint8_t>(Math::clamp(normalizedDc.z * 255.0f, 0.0f, 255.0f));

    packed |= (r << 24u);
    packed |= (g << 16u);
    packed |= (b << 8u);

    return packed;
}

inline float SqrtWithSign(float x) {
    float sign = (x < 0.0f) ? -1.0f : 1.0f;
    float result = std::sqrt(std::abs(x));
    return sign * result;
}

inline float SqWithSign(float x) {
    float sign = (x < 0.0f) ? -1.0f : 1.0f;
    float result = sign * x * x;
    return result;
}

/**
 * Type-punning via CloneData to avoid strict aliasing violations.
 * Replaces reinterpret_cast for converting between trivially-copyable types (e.g. uint32_t <-> float).
 */
template<typename To, typename From>
To BitCast(const From& from)
{
    static_assert(sizeof(To) == sizeof(From), "BitCast requires types of the same size");
    static_assert(std::is_trivially_copyable_v<To>, "Target type must be trivially copyable");
    static_assert(std::is_trivially_copyable_v<From>, "Source type must be trivially copyable");

    To res;
    BASE_NS::CloneData(&res, sizeof(To), &from, sizeof(From));
    return res;
}

inline void UnpackGSColor(const float packedFloat, Math::Vec3& outColor)
{
    uint32_t packed = BitCast<uint32_t>(packedFloat);

    uint8_t r = (packed >> 24) & 0xFF;
    uint8_t g = (packed >> 16) & 0xFF;
    uint8_t b = (packed >> 8) & 0xFF;
    // uint8_t a = packed & 0xFF;

    outColor.x = static_cast<float>(r) / 255.0f;
    outColor.y = static_cast<float>(g) / 255.0f;
    outColor.z = static_cast<float>(b) / 255.0f;
}

/**
 * @brief Extract camera world position from a view matrix.
 *
 * The view matrix is assumed to be in row-major order:
 * | r00 r01 r02  0  |
 * | r10 r11 r12  0  |
 * | r20 r21 r22  0  |
 * | tx  ty  tz   1  |
 *
 * For a camera at position P, the view matrix is V = R * T(-P),
 * where R is the rotation matrix and T is the translation.
 * The translation component t = -R * P, so P = -R * t.
 *
 * Note: Multiply R's rows by t (not columns), since R is stored row-major.
 *
 * @param viewMatrix The view matrix in row-major order.
 * @return Camera world position.
 */
inline Math::Vec3 ExtractCameraPositionFromViewMatrix(const Math::Mat4X4& viewMatrix)
{
    // Rotation matrix elements (row-major)
    Math::Vec3 r0 = { viewMatrix[0u][0u], viewMatrix[0u][1u], viewMatrix[0u][2u] };
    Math::Vec3 r1 = { viewMatrix[1u][0u], viewMatrix[1u][1u], viewMatrix[1u][2u] };
    Math::Vec3 r2 = { viewMatrix[2u][0u], viewMatrix[2u][1u], viewMatrix[2u][2u] };
    // Translation component
    Math::Vec3 t = { viewMatrix[3u][0u], viewMatrix[3u][1u], viewMatrix[3u][2u] };

    // Camera position P = -R * t (multiply rows of R by t)
    return Math::Vec3(
        -(Math::Dot(r0, t)),
        -(Math::Dot(r1, t)),
        -(Math::Dot(r2, t))
    );
}

} // namespace GS

#endif