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

#ifndef GS_DECODER_H
#define GS_DECODER_H

#include <cstdint>
#include <string>
#include <vector>

#include <base/containers/array_view.h>
#include <base/containers/fixed_string.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/matrix_util.h>
#include <base/math/quaternion_util.h>
#include <base/math/vector_util.h>
#include <base/namespace.h>

namespace GS_DECODER {

struct UwaGsPatchMeta {
    size_t patchIndex { 0 };
    size_t patchSize { 0 };
    uint8_t patchQuantBits { 0 };
    BASE_NS::vector<float> patchQuantMinValue;
    BASE_NS::vector<float> patchQuantMaxValue;
};

struct UwaGsAttributeMeta {
    size_t attributeType { 0 };
    uint8_t componentsCount { 0 };
    uint8_t uncompressedDataType { 0 };
    uint8_t attributeQuantizationFlag { 0 };
    uint8_t attributeEncoderScheme { 0 };
    uint8_t quantizationBits { 0 };
    BASE_NS::vector<float> quantMinValue;
    BASE_NS::vector<float> quantMaxValue;
    uint8_t attributeIndexType { 0 };
    size_t attributeCodebookLength { 0 };
    size_t byteOffset { 0 };
    size_t byteLength { 0 };
    size_t uncompressedByteLength { 0 };
    size_t patchNum { 0 };
    uint8_t patchGlobalEnableIndexFlag { 0 };
    uint8_t patchGlobalEnableSizeFlag { 0 };
    uint8_t patchGlobalEnableQuantBitFlag { 0 };
    uint8_t patchGlobalEnableQuantMinMaxFlag { 0 };
    uint8_t patchGlobalEnable2DmapingFlag { 0 };
    size_t lastPatchSize { 0 };
    // 2D
    uint8_t attribute2DmimeType { 0 };
    uint16_t attribute2DSingleWidth { 0 };
    uint16_t attribute2DSingleHeight { 0 };
    uint8_t attribute2DSingleAlign { 0 };
    uint8_t attribute2DConcat { 0 };
    uint8_t attribute2DConcatMaxInWidth { 0 };
    uint8_t attribute2DConcatMaxInHeight { 0 };
    uint8_t attribute2DTexNum { 0 };
    BASE_NS::vector<size_t> attribute2DTexSizes;

    BASE_NS::vector<UwaGsPatchMeta> patchMetas;
};

struct UwaGsHeader {
    BASE_NS::vector<uint8_t> version;
    size_t totalByteLength { 0 };
    uint8_t superCompressionScheme { 0 };
    size_t numPoints { 0 };
    uint8_t numAttribute { 0 };

    BASE_NS::vector<UwaGsAttributeMeta> attributeMetas;
};

struct GsSplatData {
    float position[3u] = {};
    float scaling[3u] = {};
    float rotation[4u] = {};
    float opacity { 0 };
    float shs[48u] = {};
};

struct CompressedGsData {
    int shDegree { 0 };
    UwaGsHeader uwaHeader;
    // compressed data stream
    BASE_NS::vector<uint8_t> attributeDatas;
};

struct GsBasicInfo {
    float minPosition[3];
    float maxPosition[3];
    size_t numGS;
    size_t shDegree;
};

int DecompressZlib(const unsigned char* compressedData, size_t compressedSize, unsigned char* decompressedData,
    size_t decompressedSize);

float ParseFloat(const uint8_t* dataStream, size_t& pointer);

// Parse a single patch
bool ParseUwaGsPatch(const uint8_t* dataStream, size_t size, size_t& pointer, const UwaGsAttributeMeta& attributeInfo,
    UwaGsPatchMeta& patchMeta);

// Parse an attribute meta
bool ParseUWAGSAttributeMeta(const uint8_t* dataStream, size_t size, size_t& pointer, UwaGsAttributeMeta& attrMeta);

// Helper function to map data type sizes
size_t GetDataTypeSize(uint8_t dataType);

bool DecodeAttribute(const BASE_NS::vector<uint8_t>& attributeStream, const UwaGsAttributeMeta& attributeInfo,
    GsSplatData& gsVertex, size_t idx, size_t numGS);

bool ParseGsCompressedUwa(const uint8_t* dataStream, size_t size, CompressedGsData& gsData);

// get the basic information without decoding the whole model
// the information is saved in GsBasicInfo struct.
bool ParseBasicInfo(const uint8_t* fileData, size_t size, GsBasicInfo& info, UwaGsHeader& header);

// decoding the whole model and save the data to gsData object!
bool ParseGsCompressed(const uint8_t* fileData, size_t size, CompressedGsData& gsData);

} // namespace GS_DECODER
#endif