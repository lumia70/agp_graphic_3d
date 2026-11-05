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

#ifndef PLUGIN_GLB_PARSING_H
#define PLUGIN_GLB_PARSING_H

#include <cmath>
#include <cstdint>

#include "sog_parsing.h"

namespace GS {

struct TileExtractBuffer {
    size_t offset { 0 };
    size_t count { 0 };
    uint8_t* dst { nullptr };
    size_t dstBytes { 0 };
    bool skipAabb { false };
    bool useTilePacked { false };
};

/** Decode an entire .glb file into a CachedSogData (meta only, lazy decode). */
bool DecodeGlbFile(const uint8_t* fileData, size_t size, CachedSogData& cache, const bool decodeAuxiliary = false);

/** Extract a splat range from GLB cache by lazy decoding required blocks. */
bool ExtractGlbSplatsToBuffer(const CachedSogData& cache, const std::atomic_bool& cancelled,
    const TileExtractBuffer& tileBuffer, GsPrimitive& primitive);

}
#endif