/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
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

#ifndef GS_LIMITS_H
#define GS_LIMITS_H

#include <cstdint>
#include <cstring>
#include <limits>

namespace GS {
 
// gs_decoder.cpp limits
constexpr size_t MAX_UWAGS_PATCH_NUM = 100000000u;   // max patches per UWA-GS attribute meta
constexpr size_t MAX_UWAGS_BYTE_LENGTH = 104857600u; // max total byte length (100MB)
 
// sog_parsing.cpp limits
constexpr size_t MAX_SOG_SPLAT_COUNT = 50000000u;    // max splat count from SOG meta.json
 
// gs_editor.cpp limits
// Large scenes require buffers approaching 4GB; lower values cause export/select to fail.
constexpr size_t MAX_GS_EXPORT_BUFFER_BYTE_SIZE = UINT32_MAX; // ~4GB
constexpr size_t MAX_GS_SELECT_BUFFER_BYTE_SIZE = UINT32_MAX;  // ~4GB
 
// tilescene.cpp limits
constexpr size_t MAX_SCENE_CONFIG_FILE_SIZE = 256ULL * 1024 * 1024; // max scene config / LOD meta JSON (256MB)
constexpr size_t MAX_LOD_TREE_NODE_CHILDREN = UINT16_MAX;           // max children per LOD tree node
 
// image_loader_webp.cpp limits
// Large scenes may have WebP textures exceeding 256MB; 2GB balances OOM protection with scalability.
constexpr size_t MAX_WEBP_FILE_SIZE = 2ULL * 1024 * 1024 * 1024; // 2GB
constexpr size_t MAX_IMAGE_DIM = 16384;
 
// gs_serialization.cpp limits
constexpr size_t MAX_SPLATS_LIMIT = 8000000u;
 
// gs_tiling_system.cpp limits
// Large SOG archives may exceed 256MB; 2GB balances OOM protection with scalability.
constexpr size_t MAX_SOG_FILE_SIZE = 2ULL * 1024 * 1024 * 1024; // 2GB
 
// gs_loading.cpp limits
constexpr size_t MAX_GAUSSIAN_COUNT = 8000000u; // max gaussian count per primitive (8M)
 
// GPU buffer byte size limit: UINT32_MAX is the max representable in desc.byteSize (uint32_t).
// Lower values cause allocation failures for large scenes; exceeding UINT32_MAX truncates via getvalidSize.
constexpr size_t MAX_GPU_BUFFER_BYTE_SIZE = UINT32_MAX;

enum class LsgsRenderPreference : uint32_t {
    SMOOTH = 0u,
    BALANCED = 1u,
    DETAILED = 2u
};

inline const char* ToStringLsgsRenderPreference(LsgsRenderPreference preference)
{
    switch (preference) {
        case LsgsRenderPreference::SMOOTH:
            return "smooth";
        case LsgsRenderPreference::DETAILED:
            return "detailed";
        case LsgsRenderPreference::BALANCED:
        default:
            return "balanced";
    }
}

inline bool MatchLsgsRenderPreference(const char* value, LsgsRenderPreference candidate,
    LsgsRenderPreference& out)
{
    if (strcmp(value, ToStringLsgsRenderPreference(candidate)) != 0) {
        return false;
    }
    out = candidate;
    return true;
}

inline bool MatchLsgsRenderPreferenceAny(const char* value, LsgsRenderPreference& out)
{
    return MatchLsgsRenderPreference(value, LsgsRenderPreference::SMOOTH, out) ||
        MatchLsgsRenderPreference(value, LsgsRenderPreference::BALANCED, out) ||
        MatchLsgsRenderPreference(value, LsgsRenderPreference::DETAILED, out);
}

inline bool TryParseLsgsRenderPreference(const char* value, LsgsRenderPreference& out)
{
    if (value != nullptr && MatchLsgsRenderPreferenceAny(value, out)) {
        return true;
    }
    out = LsgsRenderPreference::BALANCED;
    return false;
}

struct LsgsLevelPreset {
    uint32_t maxLodRenderCount;   // Max gaussians rendered within the view
    uint32_t minLodRenderCount;   // Lower bound when degrading under budget
    uint64_t sogCacheBytes;       // SOG cache memory budget
    uint64_t gpuPoolBytes;        // GPU memory pool budget
};

inline LsgsLevelPreset GetLsgsLevelPreset(uint32_t level)
{
    switch (level) {
        case static_cast<uint32_t>(LsgsRenderPreference::SMOOTH):
            return { 600000u, 60000u, 256u * 1024u * 1024u, 704u * 1024u * 1024u };
        case static_cast<uint32_t>(LsgsRenderPreference::DETAILED):
            return { 2000000u, 200000u, 256u * 1024u * 1024u, 704u * 1024u * 1024u };
        default: // BALANCED level / fallback for invalid values
            return { 1000000u, 100000u, 256u * 1024u * 1024u, 704u * 1024u * 1024u };
    }
}
 
} // namespace GS

#endif // GS_LIMITS_H
