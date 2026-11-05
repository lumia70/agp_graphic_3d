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

#include "gs_decoder.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <zlib.h>

#include "core/log.h"
#include <plugin_gs/util/gs_limits.h>

int GS_DECODER::DecompressZlib(const unsigned char* compressedData, size_t compressedSize,
    unsigned char* decompressedData, size_t decompressedSize)
{
    z_stream stream;
    int ret;

    // initialize zlib stream structure
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    stream.next_in = const_cast<Bytef*>(compressedData);
    stream.avail_in = static_cast<uInt>(compressedSize);
    stream.next_out = decompressedData;
    stream.avail_out = static_cast<uInt>(decompressedSize);

    // initialize decompression
    ret = inflateInit(&stream);
    if (ret != Z_OK) {
        return ret;
    }

    // execute decompression
    ret = inflate(&stream, Z_FINISH);
    if (ret != Z_STREAM_END) {
        inflateEnd(&stream);
        return (ret == Z_OK) ? Z_BUF_ERROR : ret;
    }

    // verify whether the decompressed data fully fills the buffer.
    if (stream.total_out != decompressedSize) {
        inflateEnd(&stream);
        return Z_BUF_ERROR;
    }

    inflateEnd(&stream);
    return Z_OK;
}

float GS_DECODER::ParseFloat(const uint8_t* dataStream, size_t& pointer)
{
    float value;
    BASE_NS::CloneData(&value, sizeof(float), &dataStream[pointer], sizeof(float));
    pointer += sizeof(float);
    return value;
}

// Parse a single patch
bool GS_DECODER::ParseUwaGsPatch(const uint8_t* dataStream, size_t size, size_t& pointer,
    const GS_DECODER::UwaGsAttributeMeta& attributeInfo, GS_DECODER::UwaGsPatchMeta& patchMeta)
{
    size_t pointerBefore = pointer;
    if (attributeInfo.patchGlobalEnableIndexFlag == 1) {
        if ((pointer + sizeof(uint32_t)) > size) {
            return false;
        }
        BASE_NS::CloneData(&patchMeta.patchIndex, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
        pointer += sizeof(uint32_t);
    }

    if (attributeInfo.patchGlobalEnableSizeFlag == 1) {
        if ((pointer + sizeof(uint32_t)) > size) {
            return false;
        }
        BASE_NS::CloneData(&patchMeta.patchSize, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
        pointer += sizeof(uint32_t);
    }

    if (attributeInfo.attributeQuantizationFlag == 1) {
        if (attributeInfo.patchGlobalEnableQuantBitFlag == 1) {
            if ((pointer + sizeof(uint8_t)) > size) {
                return false;
            }
            patchMeta.patchQuantBits = dataStream[pointer];
            pointer += sizeof(uint8_t);
        }

        if (attributeInfo.patchGlobalEnableQuantMinMaxFlag == 1) {
            if ((pointer + 2u * sizeof(float) * attributeInfo.componentsCount) > size) {
                return false;
            }
            patchMeta.patchQuantMinValue.resize(attributeInfo.componentsCount);
            for (size_t ii = 0; ii < attributeInfo.componentsCount; ii++) {
                patchMeta.patchQuantMinValue[ii] = GS_DECODER::ParseFloat(dataStream, pointer);
            }

            patchMeta.patchQuantMaxValue.resize(attributeInfo.componentsCount);
            for (size_t ii = 0; ii < attributeInfo.componentsCount; ii++) {
                patchMeta.patchQuantMaxValue[ii] = GS_DECODER::ParseFloat(dataStream, pointer);
            }
        }
    }

    return pointer > pointerBefore ? true : false;
}

// Parse an attribute meta
bool GS_DECODER::ParseUWAGSAttributeMeta(
    const uint8_t* dataStream, size_t size, size_t& pointer, GS_DECODER::UwaGsAttributeMeta& attrMeta)
{
    // GS_DECODER::UwaGsAttributeMeta attrMeta;

    uint8_t tmp;
    if ((pointer + sizeof(uint32_t) + 3u) > size) {
        return false;
    }
    BASE_NS::CloneData(&attrMeta.attributeType, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
    pointer += sizeof(uint32_t);

    attrMeta.componentsCount = dataStream[pointer++];
    attrMeta.uncompressedDataType = dataStream[pointer++];

    tmp = dataStream[pointer++];
    attrMeta.attributeQuantizationFlag = static_cast<uint8_t>((tmp >> 4u) & 0xF);
    attrMeta.attributeEncoderScheme = static_cast<uint8_t>(tmp & 0xF);

    if (attrMeta.attributeQuantizationFlag == 1) {
        if ((pointer + sizeof(float) * attrMeta.componentsCount + 1u) > size) {
            return false;
        }
        attrMeta.quantizationBits = dataStream[pointer++];
        if (attrMeta.quantizationBits == 0u || attrMeta.quantizationBits > 16u) {
            CORE_LOG_E("Invalid quantizationBits %u", attrMeta.quantizationBits);
            return false;
        }

        attrMeta.quantMinValue.resize(attrMeta.componentsCount);
        for (size_t ii = 0; ii < attrMeta.componentsCount; ii++) {
            attrMeta.quantMinValue[ii] = GS_DECODER::ParseFloat(dataStream, pointer);
        }

        attrMeta.quantMaxValue.resize(attrMeta.componentsCount);
        for (size_t ii = 0; ii < attrMeta.componentsCount; ii++) {
            attrMeta.quantMaxValue[ii] = GS_DECODER::ParseFloat(dataStream, pointer);
        }
    }

    if (attrMeta.attributeEncoderScheme == 1) {
        if ((pointer + sizeof(uint32_t) + 1u) > size) {
            return false;
        }
        attrMeta.attributeIndexType = dataStream[pointer++];
        BASE_NS::CloneData(&attrMeta.attributeCodebookLength, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
        pointer += sizeof(uint32_t);
    }

    if (attrMeta.attributeEncoderScheme == 2u) {
        if ((pointer + 10u) > size) {
            return false;
        }
        // parse the first 10 bytes of the field
        attrMeta.attribute2DmimeType = dataStream[pointer];
        pointer += 1u;

        attrMeta.attribute2DSingleHeight = static_cast<uint16_t>(
            static_cast<uint16_t>(dataStream[pointer]) | static_cast<uint16_t>(dataStream[pointer + 1]) << 8u);
        pointer += 2u;

        attrMeta.attribute2DSingleWidth = static_cast<uint16_t>(
            static_cast<uint16_t>(dataStream[pointer]) | static_cast<uint16_t>(dataStream[pointer + 1]) << 8u);
        pointer += 2u;

        attrMeta.attribute2DSingleAlign = dataStream[pointer++];
        attrMeta.attribute2DConcat = dataStream[pointer++];
        attrMeta.attribute2DConcatMaxInWidth = dataStream[pointer++];
        attrMeta.attribute2DConcatMaxInHeight = dataStream[pointer++];
        attrMeta.attribute2DTexNum = dataStream[pointer++];

        if ((pointer + 4u * attrMeta.attribute2DTexNum) > size) {
            return false;
        }

        // parsing dynamic-length texture size arrays
        attrMeta.attribute2DTexSizes.resize(attrMeta.attribute2DTexNum);
        for (uint8_t ii = 0; ii < attrMeta.attribute2DTexNum; ii++) {
            uint32_t texSize = static_cast<uint32_t>(dataStream[pointer]) |
                               (static_cast<uint32_t>(dataStream[pointer + 1u]) << 8u) |
                               (static_cast<uint32_t>(dataStream[pointer + 2u]) << 16u) |
                               (static_cast<uint32_t>(dataStream[pointer + 3u]) << 24u);

            attrMeta.attribute2DTexSizes[ii] = static_cast<size_t>(texSize);
            pointer += 4u;
        }
    }
    if ((pointer + 4u * sizeof(uint32_t)) > size) {
        return false;
    }

    BASE_NS::CloneData(&attrMeta.byteOffset, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
    pointer += sizeof(uint32_t);

    BASE_NS::CloneData(&attrMeta.byteLength, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
    pointer += sizeof(uint32_t);

    BASE_NS::CloneData(&attrMeta.uncompressedByteLength, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
    pointer += sizeof(uint32_t);

    if (attrMeta.byteLength == 0) {
        CORE_LOG_E("byteLength must be non-zero");
        return false;
    }

    if (attrMeta.byteOffset > SIZE_MAX - attrMeta.byteLength) {
        CORE_LOG_E("byteOffset + byteLength overflow");
        return false;
    }

    BASE_NS::CloneData(&attrMeta.patchNum, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
    pointer += sizeof(uint32_t);

    if (attrMeta.patchNum > GS::MAX_UWAGS_PATCH_NUM) {
        CORE_LOG_E("patchNum %zu exceeds limit", static_cast<size_t>(attrMeta.patchNum));
        return false;
    }

    if (attrMeta.patchNum > 1) {
        if ((pointer + sizeof(uint16_t) + sizeof(uint32_t)) > size) {
            return false;
        }
        uint16_t flags;
        BASE_NS::CloneData(&flags, sizeof(uint16_t), &dataStream[pointer], sizeof(uint16_t));
        pointer += sizeof(uint16_t);

        attrMeta.patchGlobalEnableIndexFlag = static_cast<uint8_t>((flags >> 15u) & 0x1);
        attrMeta.patchGlobalEnableSizeFlag = static_cast<uint8_t>((flags >> 14u) & 0x1);
        attrMeta.patchGlobalEnableQuantBitFlag = static_cast<uint8_t>((flags >> 13u) & 0x1);
        attrMeta.patchGlobalEnableQuantMinMaxFlag = static_cast<uint8_t>((flags >> 12u) & 0x1);
        attrMeta.patchGlobalEnable2DmapingFlag = static_cast<uint8_t>((flags >> 11u) & 0x1);

        if (attrMeta.patchGlobalEnableSizeFlag == 0) {
            BASE_NS::CloneData(&attrMeta.lastPatchSize, sizeof(uint32_t), &dataStream[pointer], sizeof(uint32_t));
            pointer += sizeof(uint32_t);
        }

        for (size_t ii = 0; ii < attrMeta.patchNum; ii++) {
            GS_DECODER::UwaGsPatchMeta pathMeta;
            if (GS_DECODER::ParseUwaGsPatch(dataStream, size, pointer, attrMeta, pathMeta)) {
                attrMeta.patchMetas.push_back(pathMeta);
            } else {
                return false;
            }
        }
    }

    return true;
}

size_t GS_DECODER::GetDataTypeSize(uint8_t dataType)
{
    switch (dataType) {
        case 1u:
            return sizeof(int8_t);
        case 2u:
            return sizeof(uint8_t);
        case 3u:
            return sizeof(int16_t);
        case 4u:
            return sizeof(uint16_t);
        case 5u:
            // float16 is not directly supported in C++, so we use float
            return sizeof(float);
        case 6u:
            return sizeof(int32_t);
        case 7u:
            return sizeof(uint32_t);
        case 8u:
            return sizeof(float);
        case 9u:
            return sizeof(int64_t);
        case 10u:
            return sizeof(uint64_t);
        case 11u:
            return sizeof(double);
        default: {
            CORE_LOG_E("Unsupported data type");
            return 0;
        }
    }
}

bool GS_DECODER::ParseBasicInfo(
    const uint8_t* fileData, size_t size, GS_DECODER::GsBasicInfo& info, GS_DECODER::UwaGsHeader& header)
{
    const std::string uwaFormatMagicStr = "gsct";
    const std::vector<uint8_t> uwaFormatMagicBytes(uwaFormatMagicStr.begin(), uwaFormatMagicStr.end());

    if (size >= uwaFormatMagicBytes.size() &&
        std::equal(uwaFormatMagicBytes.begin(), uwaFormatMagicBytes.end(), fileData)) {
        size_t pointer = 0;

        // check magic bytes
        const std::string magicBytes = "gsct";

        // check buffer length out of bound
        if (size < magicBytes.size() + 4 + sizeof(uint32_t) * 3u) {
            CORE_LOG_E("header parsing error!");
            return false;
        }
        assert(std::equal(magicBytes.begin(), magicBytes.end(), fileData));
        pointer += magicBytes.size();

        // parse version
        header.version = { fileData[pointer], fileData[pointer + 1u], fileData[pointer + 2u] };
        pointer += 3u;

        // parse total byte length, super compression scheme, and reserved fields
        BASE_NS::CloneData(&header.totalByteLength, sizeof(uint32_t), &fileData[pointer], sizeof(uint32_t));
        pointer += sizeof(uint32_t);
        if (header.totalByteLength == 0 || header.totalByteLength > GS::MAX_UWAGS_BYTE_LENGTH) {
            CORE_LOG_E("TOO LARGE OR INVALID BYTELENGTH");
            return false;
        }
        header.superCompressionScheme = fileData[pointer++];
        if (header.superCompressionScheme > 1u) {
            CORE_LOG_E("SuperCompressionScheme %d is not supported ", static_cast<int>(header.superCompressionScheme));
            return false;
        }
        uint32_t reserved;
        BASE_NS::CloneData(&reserved, sizeof(uint32_t), &fileData[pointer], sizeof(uint32_t));
        pointer += sizeof(uint32_t);

        // parse number of points and attributes
        BASE_NS::CloneData(&header.numPoints, sizeof(uint32_t), &fileData[pointer], sizeof(uint32_t));
        pointer += sizeof(uint32_t);
        if (header.numPoints == 0) {
            CORE_LOG_E("INVALID Gs Num");
            return false;
        }

        header.numAttribute = fileData[pointer++];
        if (header.numAttribute == 0) {
            CORE_LOG_E("INVALID Attribute Num");
            return false;
        }

        CORE_LOG_I("Load compressed 3DGS data: Number = %zu; Attributes = %d", header.numPoints,
            static_cast<int>(header.numAttribute));

        info.numGS = header.numPoints;
        return true;
    }

    return true;
}

namespace {
bool DecodeQuantizedData(const BASE_NS::vector<uint8_t>& attributeStream,
    const GS_DECODER::UwaGsAttributeMeta& attributeInfo, size_t globalIdx, BASE_NS::vector<float>& outDecodedData)
{
    const size_t quantizedDataSize = (attributeInfo.quantizationBits > 8u) ? sizeof(uint16_t) : sizeof(uint8_t);
    outDecodedData.resize(attributeInfo.componentsCount);

    for (size_t jj = 0; jj < attributeInfo.componentsCount; ++jj) {
        const size_t offset =
            attributeInfo.byteOffset + (globalIdx * attributeInfo.componentsCount + jj) * quantizedDataSize;

        if ((offset + quantizedDataSize) > attributeStream.size()) {
            CORE_LOG_E("Out of range");
            return false;
        }

        uint16_t quantizedValue = 0;
        if (quantizedDataSize == sizeof(uint16_t)) {
            BASE_NS::CloneData(&quantizedValue, sizeof(uint16_t), &attributeStream[offset], sizeof(uint16_t));
        } else {
            uint8_t byte_value;
            BASE_NS::CloneData(&byte_value, sizeof(uint8_t), &attributeStream[offset], sizeof(uint8_t));
            quantizedValue = byte_value;
        }

        const float normalizedValue =
            static_cast<float>(quantizedValue) / static_cast<float>((1 << attributeInfo.quantizationBits) - 1);
        outDecodedData[jj] = normalizedValue;
    }

    return true;
}

bool DecodeRawData(const BASE_NS::vector<uint8_t>& attributeStream, const GS_DECODER::UwaGsAttributeMeta& attributeInfo,
    size_t globalIdx, BASE_NS::vector<float>& outDecodedData)
{
    if (attributeInfo.componentsCount < 0 || attributeInfo.componentsCount > 45u) {
        CORE_LOG_E("Invalid components count");
        return false;
    }

    // if no quantization, directly read the data
    if (attributeInfo.attributeQuantizationFlag == 0) {
        if (attributeInfo.attributeEncoderScheme != 0) {
            CORE_LOG_E("Non-quantized data must have encoder scheme 0");
            return false;
        }

        if (attributeInfo.uncompressedDataType != 8u) {
            CORE_LOG_E("Unsupported uncompressed dataType!");
            return false;
        }

        const size_t dataTypeSize = GS_DECODER::GetDataTypeSize(attributeInfo.uncompressedDataType);
        const size_t perVertexSize = static_cast<size_t>(attributeInfo.componentsCount) * dataTypeSize;
        const size_t vertexOffset = globalIdx * perVertexSize;
        if (vertexOffset / perVertexSize != globalIdx) {
            CORE_LOG_E("Offset overflow");
            return false;
        }
        if (attributeInfo.byteOffset > SIZE_MAX - vertexOffset) {
            CORE_LOG_E("Offset overflow");
            return false;
        }
        const size_t offset = attributeInfo.byteOffset + vertexOffset;
        outDecodedData.resize(attributeInfo.componentsCount);

        if ((offset + sizeof(float) * attributeInfo.componentsCount) > attributeStream.size()) {
            CORE_LOG_E("Out of range");
            return false;
        }

        BASE_NS::CloneData(outDecodedData.data(), sizeof(float) * attributeInfo.componentsCount,
            &attributeStream[offset], sizeof(float) * attributeInfo.componentsCount);
        return true;
    }

    // scheme 0: simple quantization decoding
    if (attributeInfo.attributeEncoderScheme == 0 && attributeInfo.attributeQuantizationFlag == 1) {
        return DecodeQuantizedData(attributeStream, attributeInfo, globalIdx, outDecodedData);
    }

    // scheme 2
    if (attributeInfo.attributeEncoderScheme == 2u) {
        outDecodedData.resize(attributeInfo.componentsCount);
        for (size_t jj = 0; jj < attributeInfo.componentsCount; ++jj) {
            outDecodedData[jj] = -1.0f * attributeInfo.quantMinValue[jj] /
                                 (attributeInfo.quantMaxValue[jj] - attributeInfo.quantMinValue[jj]);
        }
        return true;
    }

    CORE_LOG_E("Unsupported encoder scheme");
    return false;
}

bool DequantizeData(const GS_DECODER::UwaGsAttributeMeta& attributeInfo, size_t globalIdx, size_t numGS,
    BASE_NS::vector<float>& decodedData)
{
    if (attributeInfo.componentsCount < 0 || attributeInfo.componentsCount > 45u) {
        CORE_LOG_E("Invalid components count");
        return false;
    }

    if (attributeInfo.patchNum == 1u) {
        for (size_t jj = 0; jj < attributeInfo.componentsCount; ++jj) {
            decodedData[jj] = decodedData[jj] * (attributeInfo.quantMaxValue[jj] - attributeInfo.quantMinValue[jj]) +
                              attributeInfo.quantMinValue[jj];
        }
        return true;
    }

    if (attributeInfo.patchGlobalEnableSizeFlag == 0) {
        const size_t chunkSize =
            static_cast<size_t>(std::round(static_cast<double>(numGS - attributeInfo.lastPatchSize) /
                                           static_cast<double>(attributeInfo.patchNum - 1)));
        if (chunkSize == 0u) {
            return false;
        }
        size_t patchIdx = globalIdx / chunkSize;
        if (patchIdx >= attributeInfo.patchMetas.size()) {
            return false;
        }
        const auto& patch = attributeInfo.patchMetas[patchIdx];
        for (size_t jj = 0; jj < attributeInfo.componentsCount; ++jj) {
            decodedData[jj] = decodedData[jj] * (patch.patchQuantMaxValue[jj] - patch.patchQuantMinValue[jj]) +
                              patch.patchQuantMinValue[jj];
        }
        return true;
    }

    CORE_LOG_E("Unsupported Patch Size Mode!");
    return false;
}

bool WriteToVertexData(const GS_DECODER::UwaGsAttributeMeta& attributeInfo, const BASE_NS::vector<float>& decodedData,
    GS_DECODER::GsSplatData& gsVertex)
{
    switch (attributeInfo.attributeType) {
        case 0u: { // position
            for (size_t jj = 0; jj < 3u; ++jj) {
                gsVertex.position[jj] = decodedData[jj];
            }
            break;
        }
        case 1u: { // rotation
            for (size_t jj = 0; jj < 4u; ++jj) {
                gsVertex.rotation[jj] = decodedData[jj];
            }
            break;
        }
        case 2u: { // scale
            for (size_t jj = 0; jj < 3u; ++jj) {
                gsVertex.scaling[jj] = decodedData[jj];
            }
            break;
        }
        case 3u: { // DC and opacity
            if (attributeInfo.componentsCount < 1u) {
                CORE_LOG_E("Wrong Attribute Components Count");
                return false;
            }
            const size_t shCount = static_cast<size_t>(attributeInfo.componentsCount) - 1;
            BASE_NS::CloneData(gsVertex.shs, sizeof(float) * shCount, decodedData.data(), sizeof(float) * shCount);

            float op = BASE_NS::Math::clamp(decodedData[shCount], 0.001f, 0.999f);
            op = std::log(op / (1.0f - op)); // inverse sigmoid
            gsVertex.opacity = op;
            break;
        }
        case 4u: { // spherical harmonics
            BASE_NS::CloneData(gsVertex.shs + 3, attributeInfo.componentsCount * sizeof(float), decodedData.data(),
                attributeInfo.componentsCount * sizeof(float));
            break;
        }
        default:
            CORE_LOG_E("Wrong Attribute Type");
            return false;
    }

    return true;
}
} // namespace

bool GS_DECODER::DecodeAttribute(const BASE_NS::vector<uint8_t>& attributeStream,
    const GS_DECODER::UwaGsAttributeMeta& attributeInfo, GsSplatData& gsVertex, size_t globalIdx, size_t numGS)
{
    // check if the quantization flag is supported
    if (attributeInfo.attributeQuantizationFlag > 1) {
        CORE_LOG_E("Unsupported quantization flag");
        return false;
    }

    // decode raw data
    BASE_NS::vector<float> decodedData;
    if (!DecodeRawData(attributeStream, attributeInfo, globalIdx, decodedData)) {
        return false;
    }

    // inverse quantization
    if (attributeInfo.attributeQuantizationFlag == 1) {
        if (!DequantizeData(attributeInfo, globalIdx, numGS, decodedData)) {
            return false;
        }
    }

    // write the data to GS_DECODER::GsData
    if (!WriteToVertexData(attributeInfo, decodedData, gsVertex)) {
        return false;
    }

    return true;
}

bool GS_DECODER::ParseGsCompressedUwa(const uint8_t* dataStream, size_t size, GS_DECODER::CompressedGsData& gsData)
{
    GS_DECODER::UwaGsHeader& header = gsData.uwaHeader;
    GS_DECODER::GsBasicInfo info;
    if (!GS_DECODER::ParseBasicInfo(dataStream, size, info, header)) {
        return false;
    }
    size_t pointer = 0;
    const std::string magic_bytes = "gsct";
    pointer += magic_bytes.size() + 5u + 3u * sizeof(uint32_t); // skip the header

    CORE_LOG_I("Load compressed 3DGS data: Number = %zu; Attributes = %d", header.numPoints,
        static_cast<int>(header.numAttribute));

    // parse attribute metas
    gsData.shDegree = 0u;
    for (uint8_t ii = 0; ii < header.numAttribute; ii++) {
        GS_DECODER::UwaGsAttributeMeta attrMeta;
        if (GS_DECODER::ParseUWAGSAttributeMeta(dataStream, size, pointer, attrMeta)) {
            header.attributeMetas.push_back(attrMeta);
        } else {
            CORE_LOG_E("Parse Attributes Meta failed!");
            return false;
        }
        if (header.attributeMetas[ii].attributeType == 4u) {
            gsData.shDegree = static_cast<int>(
                static_cast<size_t>(BASE_NS::Math::sqrt(static_cast<float>(
                    static_cast<double>(header.attributeMetas[(size_t)ii].componentsCount) / 3.0 + 1.0))) -
                (size_t)1);
        }
    }

    BASE_NS::vector<uint8_t>& attributeDatas = gsData.attributeDatas;
    if (header.superCompressionScheme == 1) {
        // decoding data
        const size_t availSize = header.totalByteLength;
        attributeDatas.resize(availSize);
        if (GS_DECODER::DecompressZlib(dataStream + pointer, size - pointer,
                static_cast<unsigned char*>(attributeDatas.data()), availSize) != Z_OK) {
            CORE_LOG_E("DecompressZlib failed!");
            return false;
        }
    } else {
        attributeDatas.resize(size - pointer);
        BASE_NS::CloneData(static_cast<void*>(attributeDatas.data()), attributeDatas.size(), (dataStream + pointer),
            attributeDatas.size());
    }

    return true;
}

bool GS_DECODER::ParseGsCompressed(const uint8_t* fileData, size_t size, GS_DECODER::CompressedGsData& gsData)
{
    // define the magic bytes for PLY and UWA formats
    const std::string uwaFormatMagicStr = "gsct";
    const std::vector<uint8_t> uwaFormatMagicBytes(uwaFormatMagicStr.begin(), uwaFormatMagicStr.end());

    if (size >= uwaFormatMagicBytes.size() &&
        std::equal(uwaFormatMagicBytes.begin(), uwaFormatMagicBytes.end(), fileData)) {
        CORE_LOG_I("Parse compressed UWA format gs");
        return GS_DECODER::ParseGsCompressedUwa(fileData, size, gsData);
    }
    CORE_LOG_E("Parse attributes meta failed!");
    return false;
}