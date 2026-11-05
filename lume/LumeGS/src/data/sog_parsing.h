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

#ifndef PLUGIN_SOG_PARSING_H
#define PLUGIN_SOG_PARSING_H

#include <atomic>
#include <cmath>
#include <cstdint>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>

#include <base/containers/string.h>
#include <base/containers/vector.h>
#include <base/math/vector.h>
#include <core/image/intf_image_container.h>
#include <core/image/intf_image_loader_manager.h>
#include <core/intf_engine.h>
#include <core/io/intf_file_manager.h>
#include <core/log.h>

namespace GS {
inline float InvLogTransform(float v)
{
    const float a = std::abs(v);
    const float e = std::exp(a) - 1.0f;
    return v < 0.0f ? -e : e;
}

inline float SigmoidInv(float y)
{
    const float e = BASE_NS::Math::clamp(y, 1e-6f, 1.0f - 1e-6f);
    return std::log(e / (1.0f - e));
}

/** Parsed metadata from a .sog archive's meta.json. */
struct SogMeta {
    uint32_t version { 0 };
    uint32_t count { 0 };
    struct {
        BASE_NS::vector<float> mins;
        BASE_NS::vector<float> maxs;
        BASE_NS::vector<BASE_NS::string> files;
    } means;
    struct {
        BASE_NS::vector<float> codebook;
        BASE_NS::vector<BASE_NS::string> files;
    } scales;
    struct {
        BASE_NS::vector<BASE_NS::string> files;
    } quats;
    struct {
        BASE_NS::vector<float> codebook;
        BASE_NS::vector<BASE_NS::string> files;
    } sh0;
    struct {
        uint32_t count { 0 };
        uint32_t bands { 0 };
        BASE_NS::vector<float> codebook;
        BASE_NS::vector<BASE_NS::string> files;
    } shN;
};

/** Cached decoded-packed contents of a single .sog file.
 *  Holds IImageContainer unique_ptrs for each decoded WebP image.
 *  Immutable after state transitions to READY. */
struct CachedSogData {
    enum class State : uint8_t { LOADING, READY, FAILED };
    enum class DataFormat : uint8_t { SOG_PACKED, GLB_EXPANDED };  // add for glb
    std::atomic<State> state { State::LOADING };

    // interleaved data offsets and stride
    static constexpr uint32_t kPosOffset = 0u;
    static constexpr uint32_t kQuatOffset = 6u;
    static constexpr uint32_t kScaleOffset = 10u;
    static constexpr uint32_t kSh0Offset = 13u;
    static constexpr uint32_t kSplatStride = 17u;

    SogMeta meta;
    BASE_NS::vector<uint8_t> packedSplats;
    BASE_NS::vector<uint8_t> packedCentroids;
    BASE_NS::vector<uint16_t> packedLabels;
    uint32_t centroidsWidth { 0 };
    uint32_t shDegree { 0 };

    bool hasPositions { false };
    bool hasQuats { false };
    bool hasScales { false };
    bool hasSh0 { false };

    float expScales[256] { 0.0f };

    DataFormat dataFormat { DataFormat::SOG_PACKED };  // add for glb，default SOG
    std::shared_ptr<void> glbModel { nullptr };
    uint32_t glbAttributeDatasBytes { 0 };
};

/** Parse meta.json bytes into a SogMeta struct. */
bool ParseSogMeta(BASE_NS::array_view<const uint8_t> jsonData, SogMeta& meta);

/** Decode a WebP image from raw bytes using IImageLoaderManager. */
CORE_NS::IImageContainer::Ptr DecodeWebP(
    BASE_NS::array_view<const uint8_t> webpData, CORE_NS::IImageLoaderManager& imgMgr);

/** Decode an entire .sog file (ZIP + WebP images) into a CachedSogData. */
bool DecodeSogFile(const uint8_t* fileData, size_t size, CachedSogData& cache, CORE_NS::IImageLoaderManager& imgMgr,
    const bool decodeAuxiliary = true);

/** Extract a [offset, offset+count) splat range from pre-decoded cache into loadData. */
bool ExtractSplatsFromCache(const CachedSogData& cache, const size_t offset, size_t count, GsObjectLoadData& loadData,
    GsPrimitive& primitive, const std::atomic_bool& cancelled, const bool extractAuxiliary = true,
    const bool skipAabb = false);

/** Extract a splat range directly into a raw byte buffer */
bool ExtractSplatsToBuffer(const CachedSogData& cache, const size_t offset, size_t count, uint8_t* dst,
    const size_t dstBytes, GsPrimitive& primitive, const std::atomic_bool& cancelled, const bool skipAabb = false,
    bool useTilePacked = false);

/** Parse a .sog file from in-memory bytes (convenience wrapper: decode + extract). */
bool ParseGSSog(const uint8_t* fileData, size_t size, GS::GsObjectLoadData& loadData, GS::GsPrimitive& primitive,
    CORE_NS::IImageLoaderManager& imgMgr, size_t offset, size_t count, const std::atomic_bool& cancel);

void PrecomputeCovariancesSog(const BASE_NS::Math::Vec3 scale, const BASE_NS::Math::Vec4 rot,
    BASE_NS::Math::Vec4& covA, BASE_NS::Math::Vec4& covB);

} // namespace GS

#endif // PLUGIN_SOG_PARSING_H
