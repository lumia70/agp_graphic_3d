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

#include "glb_parsing.h"

#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdint>
#include <limits>

#include <base/containers/string.h>
#include <base/containers/vector.h>
#include <base/math/float_packer.h>
#include <base/math/vector.h>
#include <base/math/vector_util.h>
#include <core/log.h>

#include "util/gs_utils.h"

#include "spatial_buffer.h"
#include "spatial_codec_factory.h"
#include "spatial_meta.h"
#include "spatial_tag.h"

namespace GS {

using namespace BASE_NS;
using namespace CORE_NS;
using namespace Spatial;

static inline const std::string SUPPORT_GLB_COMPRESSION = "UWA_gaussian_splatting_compression_EGSC";
constexpr size_t MAX_BLOCK_SIZE = 10000;
constexpr uint32_t MAX_SPLAT_COUNT = 10000000;

bool CheckDecVersion(std::vector<uint8_t>& fileDataVec, std::shared_ptr<SpatialCodecDemuxer>& demuxer,
    std::shared_ptr<SpatialMeta>& config)
{
    auto inputBuffer = SpatialCodecFactory::CreateBuffer(SpatialTag::UWAGS);
    if (inputBuffer == nullptr) {
        CORE_LOG_E("CheckDecVersion Failed to create GLB UWA 3DGS Buffer");
        return false;
    }
    inputBuffer->SetBinaryData(std::move(fileDataVec));
    // initialize demuxer
    demuxer = SpatialCodecFactory::CreateDemuxer(SpatialTag::UWAGS);
    if (demuxer == nullptr) {
        CORE_LOG_E("CheckDecVersion Failed to create GLB UWA 3DGS Demuxer");
        return false;
    }
    config = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
    if (config == nullptr) {
        CORE_LOG_E("CheckDecVersion Failed to create GLB UWA 3DGS MetaConfig");
        return false;
    }
    auto ret = demuxer->Initialize(inputBuffer, *config);
    if (ret != SpatialErrorCode::SUCCESS) {
        CORE_LOG_E("CheckDecVersion Failed to initialize Spatial Codec, error code: %d", static_cast<int>(ret));
        return false;
    }
    // get glb meta information
    std::vector<std::shared_ptr<const SpatialMeta>> primitiveMetas;
    ret = demuxer->GetMeta(SpatialTag::BASIC_INFO, primitiveMetas);
    if (ret != SpatialErrorCode::SUCCESS) {
        CORE_LOG_E("CheckDecVersion Failed to get metadata, error code: %d", static_cast<int>(ret));
        return false;
    }
    // get codec version through extension name
    if (primitiveMetas.empty()) {
        CORE_LOG_E("Get metadata failed");
        return false;
    }
    const auto& firstMeta = primitiveMetas[0];
    auto metaOpt = firstMeta->GetMeta(SpatialTag::COMPRESSION_EXTENSION_NAME);
    if (!metaOpt.has_value()) {
        CORE_LOG_E("CheckDecVersion GetMeta COMPRESSION_EXTENSION_NAME failed");
        return false;
    }
    const std::string* decVersionPtr = std::get_if<std::string>(&metaOpt.value());
    if (decVersionPtr == nullptr || *decVersionPtr != SUPPORT_GLB_COMPRESSION) {
        CORE_LOG_E("CheckDecVersion decVersion is not supported");
        return false;
    }
    return true;
}

bool GetModelAndMeta(const std::shared_ptr<SpatialCodecDemuxer>& demuxer, std::shared_ptr<SpatialMeta>& config,
    std::shared_ptr<SpatialModel>& model, std::shared_ptr<const SpatialMeta>& basicInfo)
{
    if (demuxer == nullptr || config == nullptr) {
        CORE_LOG_E("GetModelAndMeta Invalid input");
        return false;
    }
    std::shared_ptr<const SpatialBuffer> buffer;
    auto ret = demuxer->GetData("0", *config, buffer);
    if (ret != SpatialErrorCode::SUCCESS || buffer == nullptr) {
        CORE_LOG_E("GetModelAndMeta Failed to extract glTF stream data!");
        return false;
    }

    std::shared_ptr<const std::vector<uint8_t>> encodedData;
    auto binRet = buffer->GetBinaryData(encodedData);
    if (binRet != SpatialErrorCode::SUCCESS || encodedData == nullptr) {
        CORE_LOG_E("GetModelAndMeta Failed to get binary data");
        return false;
    }

    config->AddMeta(SpatialTag::COMPRESSION_EXTENSION_NAME, SUPPORT_GLB_COMPRESSION);
    auto decoder = SpatialCodecFactory::CreateDecoder(SpatialTag::UWAGS);
    if (decoder == nullptr) {
        CORE_LOG_E("GetModelAndMeta CreateDecoder failed");
        return false;
    }
    ret = decoder->Decode(*encodedData, *config, model);
    if (ret != SpatialErrorCode::SUCCESS || model == nullptr) {
        CORE_LOG_E("GetModelAndMeta decoding failed");
        return false;
    }
    ret = model->GetMeta(SpatialTag::BASIC_INFO, basicInfo);
    if (ret != SpatialErrorCode::SUCCESS || basicInfo == nullptr) {
        CORE_LOG_E("GetModelAndMeta basic info get failed");
        return false;
    }
    return true;
}

bool DecodeGlbFile(const uint8_t* fileData, size_t size, CachedSogData& cache, const bool decodeAuxiliary)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "DecodeGlbFile", "", 0xFF00FF);
    if (fileData == nullptr || size == 0) {
        CORE_LOG_E("DecodeGlbFile Invalid input size=%zu", size);
        return false;
    }
    std::vector<uint8_t> fileDataVec(fileData, fileData + size);
    std::shared_ptr<SpatialCodecDemuxer> demuxer = nullptr;
    std::shared_ptr<SpatialMeta> config = nullptr;
    if (!CheckDecVersion(fileDataVec, demuxer, config) || demuxer == nullptr || config == nullptr) {
        return false;
    }
    std::shared_ptr<SpatialModel> model = nullptr;
    std::shared_ptr<const SpatialMeta> basicInfo = nullptr;
    if (!GetModelAndMeta(demuxer, config, model, basicInfo) || model == nullptr || basicInfo == nullptr) {
        return false;
    }
    const uint32_t gsCount = (basicInfo->GetMetaAs<uint32_t>(SpatialTag::GS_POINTS_NUM)).value_or(0);
    if (gsCount == 0 || gsCount > MAX_SPLAT_COUNT) {
        CORE_LOG_E("DecodeGlbFile Invalid gsCount: %u (max: %u)", gsCount, MAX_SPLAT_COUNT);
        return false;
    }
    uint32_t baseBytes = 17u;  // 8-bit quantized attributes
    cache.glbAttributeDatasBytes = gsCount * baseBytes;

    cache.dataFormat = CachedSogData::DataFormat::GLB_EXPANDED;
    cache.glbModel = model;
    cache.hasPositions = true;
    cache.hasQuats = true;
    cache.hasScales = true;
    cache.hasSh0 = true;

    cache.shDegree = basicInfo->GetMetaAs<uint32_t>(SpatialTag::SH_DEGREE).value_or(0);

    // UWA 1.0 EGSC stores the SH rest as an ASTC-packed texture, not as float
    // coefficients. Parse its metadata here; the tiled render path must bind
    // the ASTC texture (Phase B) instead of the 96 B float SH buffer.
    if (cache.shDegree > 0u) {
        std::shared_ptr<const SpatialMeta> attrMeta;
        if (model->GetMeta(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER, attrMeta) ==
            SpatialErrorCode::SUCCESS) {
            const std::string texNumKey = "attribute2DTexNum";
            const uint32_t texNum = attrMeta->GetMetaAs<uint32_t>(texNumKey).value_or(0);
            const uint32_t codecId =
                attrMeta->GetMetaAs<uint32_t>(SpatialTag::PACKING_MAP_TEXTURE_CODEC_ID).value_or(0);
            cache.compressedShWidth = attrMeta->GetMetaAs<uint32_t>(SpatialTag::PACKING_MAP_WIDTH).value_or(0);
            cache.compressedShHeight = attrMeta->GetMetaAs<uint32_t>(SpatialTag::PACKING_MAP_HEIGHT).value_or(0);
            cache.attribute2DSingleWidth = attrMeta->GetMetaAs<uint32_t>(SpatialTag::REGION_WIDTH).value_or(0);
            cache.attribute2DSingleHeight = attrMeta->GetMetaAs<uint32_t>(SpatialTag::REGION_HEIGHT).value_or(0);
            cache.attribute2DTexNum = texNum;
            cache.codecVersion = 2u;
            cache.hasCompressedSh = (texNum > 0u && codecId > 0u && codecId <= 3u);

            const auto shNMaxs = attrMeta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MAX_VALUE);
            const auto shNMins = attrMeta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MIN_VALUE);
            if (shNMaxs.has_value() && shNMins.has_value()) {
                const size_t n = BASE_NS::Math::clamp(
                    shNMaxs.value().size() > 45 ? 15 : shNMaxs.value().size() / 3, size_t(0), size_t(16));
                for (size_t ii = 0; ii < n; ii++) {
                    cache.shRestMins_R[ii] = shNMins.value()[3 * ii];
                    cache.shRestMins_G[ii] = shNMins.value()[3 * ii + 1];
                    cache.shRestMins_B[ii] = shNMins.value()[3 * ii + 2];
                    cache.shRestMaxs_R[ii] = shNMaxs.value()[3 * ii];
                    cache.shRestMaxs_G[ii] = shNMaxs.value()[3 * ii + 1];
                    cache.shRestMaxs_B[ii] = shNMaxs.value()[3 * ii + 2];
                }
            }
            std::shared_ptr<const SpatialMeta> sh0Meta;
            if (model->GetMeta(SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0, sh0Meta) ==
                SpatialErrorCode::SUCCESS) {
                const auto dcMaxs = sh0Meta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MAX_VALUE);
                const auto dcMins = sh0Meta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MIN_VALUE);
                if (dcMaxs.has_value() && !dcMaxs->empty()) {
                    cache.shDcMaxs = *std::max_element(dcMaxs->begin(), dcMaxs->end());
                }
                if (dcMins.has_value() && !dcMins->empty()) {
                    cache.shDcMins = *std::min_element(dcMins->begin(), dcMins->end());
                }
            }
        }
    }

    CORE_LOG_I("DecodeGlbFile: gsCount=%u shDegree=%u codec=%u compressedSh=%d texNum=%u map=%ux%u region=%ux%u",
        gsCount, cache.shDegree, cache.codecVersion, cache.hasCompressedSh ? 1 : 0, cache.attribute2DTexNum,
        cache.compressedShWidth, cache.compressedShHeight, cache.attribute2DSingleWidth,
        cache.attribute2DSingleHeight);
    return true;
}

struct GlbBlockInfo {
    size_t startBlock { 0 };
    size_t endBlock { 0 };
    size_t blockSize { 0 };
    size_t splatSize { 0 };
    size_t gsCount { 0 };
    uint32_t shDegree { 0 };
    uint32_t numHarmonics { 0 };
    GS::GsHarmonicsData* shPtr { nullptr };
    bool compressedSh { false };
    uint32_t astcSlot { 0u };
    float shDcMins { -1.0f };
    float shDcMaxs { 1.0f };
    size_t reqStartInBlock { 0 };
    size_t reqEndInBlock { 0 };
    uint8_t* dstPtr {nullptr};
    size_t writtenCount { 0 };
    BASE_NS::Math::Vec3 aabbMin { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max() };
    BASE_NS::Math::Vec3 aabbMax { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest() };
};

bool InitExtractInfo(const CachedSogData& cache, const TileExtractBuffer& tileBuffer,
    std::shared_ptr<SpatialModel>& model, GlbBlockInfo& blockInfo)
{
    if (cache.dataFormat != CachedSogData::DataFormat::GLB_EXPANDED || cache.glbModel == nullptr) {
        CORE_LOG_E("InitExtractInfo Invalid cache or model not loaded");
        return false;
    }
    if (tileBuffer.offset > MAX_SPLAT_COUNT || tileBuffer.count > MAX_SPLAT_COUNT) {
        CORE_LOG_E("InitExtractInfo offset/count exceeds limit: %zu %zu", tileBuffer.offset, tileBuffer.count);
        return false;
    }
    // Get model from cache
    model = std::static_pointer_cast<SpatialModel>(cache.glbModel);
    if (model == nullptr) {
        CORE_LOG_E("InitExtractInfo Failed to cast cache.glbModel to SpatialModel");
        return false;
    }
    std::shared_ptr<const SpatialMeta> basicInfo;
    auto ret = model->GetMeta(SpatialTag::BASIC_INFO, basicInfo);
    if (ret != SpatialErrorCode::SUCCESS || basicInfo == nullptr) {
        CORE_LOG_E("InitExtractInfo basic info get failed");
        return false;
    }
    blockInfo.gsCount = static_cast<size_t>((basicInfo->GetMetaAs<uint32_t>(SpatialTag::GS_POINTS_NUM)).value_or(0));
    if (blockInfo.gsCount == 0 || blockInfo.gsCount > MAX_SPLAT_COUNT || tileBuffer.count > blockInfo.gsCount) {
        CORE_LOG_E("InitExtractInfo gsCount exceeds limit: %zu", blockInfo.gsCount);
        return false;
    }
    // Tag value matches SpatialDecoderTag::PARALLEL_BLOCK_SIZE
    // (spatial_codec/src/decoder/include/common/spatial_decoder_tag.h).
    // The GSBSDemuxer writes block size under this key, not "block_points_num".
    size_t size = static_cast<size_t>((basicInfo->GetMetaAs<uint32_t>("parallelBlockSize")).value_or(0));
    if (size == 0 || size > MAX_BLOCK_SIZE) {
        CORE_LOG_E("InitExtractInfo Invalid block size: %zu", size);
        return false;
    }
    blockInfo.blockSize = size * size;
    blockInfo.splatSize = tileBuffer.useTilePacked ? sizeof(GsObjectLoadData::GsSplatDataTilePacked) :
        sizeof(GsObjectLoadData::GsSplatData);
    const size_t requiredBytes = tileBuffer.count * blockInfo.splatSize;
    if (tileBuffer.dst == nullptr || tileBuffer.dstBytes < requiredBytes) {
        CORE_LOG_E("InitExtractInfo check dst failed: dstBytes=%zu required=%zu", tileBuffer.dstBytes, requiredBytes);
        return false;
    }
    blockInfo.startBlock = tileBuffer.offset / blockInfo.blockSize;
    blockInfo.endBlock = (tileBuffer.offset + tileBuffer.count + blockInfo.blockSize - 1) / blockInfo.blockSize;

    // Compressed EGSC SH is an ASTC texture, not float SHN; keep it DC until
    // the tiled ASTC path is wired (Phase B).
    blockInfo.shDegree = cache.hasCompressedSh ? 0u : cache.shDegree;
    blockInfo.compressedSh = cache.hasCompressedSh;
    blockInfo.astcSlot = tileBuffer.astcSlot;
    blockInfo.shDcMins = cache.shDcMins;
    blockInfo.shDcMaxs = cache.shDcMaxs;
    if (blockInfo.shDegree > 0u) {
        blockInfo.numHarmonics =
            3u * ((blockInfo.shDegree + 1u) * (blockInfo.shDegree + 1u) - 1u); // 9/24/45
        const size_t shBytes = tileBuffer.count * sizeof(GS::GsHarmonicsData);
        if (!tileBuffer.shDst || tileBuffer.shDstBytes < shBytes) {
            CORE_LOG_E("InitExtractInfo SH dst too small: %zu < %zu", tileBuffer.shDstBytes, shBytes);
            return false;
        }
        blockInfo.shPtr = reinterpret_cast<GS::GsHarmonicsData*>(tileBuffer.shDst);
    }
    return true;
}

void GetBlockInfo(const TileExtractBuffer& tileBuffer, const size_t blockIdx, GlbBlockInfo& blockInfo)
{
    // Calculate this block's global range
    size_t blockStart = blockIdx * blockInfo.blockSize;
    size_t blockEnd = std::min(blockStart + blockInfo.blockSize, blockInfo.gsCount);
    size_t blockCount = blockEnd - blockStart;
    // Calculate how much of this block is in our request
    size_t reqStartInBlock = 0;
    size_t reqEndInBlock = blockCount;
    if (blockStart < tileBuffer.offset) {
        reqStartInBlock = tileBuffer.offset - blockStart;
    }
    if (blockEnd > tileBuffer.offset + tileBuffer.count) {
        reqEndInBlock = tileBuffer.offset + tileBuffer.count - blockStart;
    }
    blockInfo.reqStartInBlock = reqStartInBlock;
    blockInfo.reqEndInBlock = reqEndInBlock;
}

bool GetBlockData(const std::vector<const float*>& attributes, const uint32_t* shUv,
    const std::atomic_bool& cancelled, const TileExtractBuffer& tileBuffer, GlbBlockInfo& blockInfo)
{
    if (attributes.size() < 5u || blockInfo.dstPtr == nullptr) {
        return false;
    }
    const auto& positionArr = attributes[0];
    const auto& scaleArr = attributes[1];
    const auto& sh0Arr = attributes[2u];
    const auto& opacityArr = attributes[3u];
    const auto& rotationArr = attributes[4u];
    const float* shNArr = attributes.size() >= 6u ? attributes[5u] : nullptr;
    for (size_t jj = blockInfo.reqStartInBlock; jj < blockInfo.reqEndInBlock; jj++) {
        if (cancelled.load()) {
            return false;
        }
        const Math::Vec3 position(positionArr[3 * jj], positionArr[3u * jj + 1], positionArr[3u * jj + 2u]);
        if (!tileBuffer.skipAabb) {
            blockInfo.aabbMin = Math::min(blockInfo.aabbMin, position);
            blockInfo.aabbMax = Math::max(blockInfo.aabbMax, position);
        }
        const Math::Vec4 rotNorm = Math::Vec4(rotationArr[4u * jj], rotationArr[4u * jj + 1],
            rotationArr[4u * jj + 2u], rotationArr[4u * jj + 3u]);
        const Math::Vec3 color(0.5f + SH_C0 * sh0Arr[3u * jj], 0.5f + SH_C0 * sh0Arr[3u * jj + 1],
            0.5f + SH_C0 * sh0Arr[3u * jj + 2u]);
        const float opacity = 1.0f / (1.0f + std::exp(-1.0f * opacityArr[1 * jj]));
        Math::Vec3 scaleExp = opacity < 0.001f ? Math::Vec3{ 0.001f, 0.001f, 0.001f } :
            Math::Vec3{ std::exp(scaleArr[3u * jj]), std::exp(scaleArr[3u * jj + 1]),
            std::exp(scaleArr[3u * jj + 2u]) };
        uint32_t colorPacked = PackGSColor(color, opacity);
        if (tileBuffer.useTilePacked) {
            auto* dstSplat = reinterpret_cast<GsObjectLoadData::GsSplatDataTilePacked*>(blockInfo.dstPtr);
            BASE_NS::Math::Vec4 covAScale;
            BASE_NS::Math::Vec4 covBColorPacked;
            PrecomputeCovariancesSog(scaleExp, rotNorm, covAScale, covBColorPacked);
            dstSplat->positionOpacityPacked = { position.x, position.y, position.z, opacity };
            dstSplat->covColorPacked = BASE_NS::Math::UVec4(PackHalf2x16(SqrtWithSign(covAScale.x),
                SqrtWithSign(covAScale.y)), PackHalf2x16(SqrtWithSign(covAScale.z),
                SqrtWithSign(covBColorPacked.x)), PackHalf2x16(SqrtWithSign(covBColorPacked.y),
                SqrtWithSign(covBColorPacked.z)), *reinterpret_cast<uint32_t*>(&colorPacked));
        } else {
            auto* dstSplat = reinterpret_cast<GsObjectLoadData::GsSplatData*>(blockInfo.dstPtr);
            PrecomputeCovariancesSog(scaleExp, rotNorm, dstSplat->covAScale, dstSplat->covBColorPacked);
            dstSplat->covBColorPacked.w = BitCast<float>(colorPacked);
            dstSplat->positionOpacity = { position.x, position.y, position.z, opacity };
            if (blockInfo.compressedSh && shUv != nullptr) {
                dstSplat->astcOffsetColor = { shUv[2u * jj], shUv[2u * jj + 1u],
                    PackGSDcWithMinMax(Math::Vec3(sh0Arr[3u * jj], sh0Arr[3u * jj + 1u], sh0Arr[3u * jj + 2u]),
                        blockInfo.shDcMins, blockInfo.shDcMaxs),
                    blockInfo.astcSlot };
            }
        }

        if (shNArr != nullptr && blockInfo.shPtr != nullptr && blockInfo.numHarmonics > 0u) {
            GS::GsHarmonicsData hs;
            hs.shCoefficients[0] = PackHalf2x16(sh0Arr[3u * jj], sh0Arr[3u * jj + 1u]);
            hs.shCoefficients[1] = PackHalf2x16(sh0Arr[3u * jj + 2u], shNArr[jj * blockInfo.numHarmonics]);
            size_t shIdx = jj * blockInfo.numHarmonics + 1u;
            for (size_t kk = 2u; kk < blockInfo.numHarmonics / 2u + 2u; kk++, shIdx += 2u) {
                if (kk * 2u - 3u > blockInfo.numHarmonics) {
                    hs.shCoefficients[kk] = PackHalf2x16(shNArr[shIdx], 0.0f);
                } else {
                    hs.shCoefficients[kk] = PackHalf2x16(shNArr[shIdx], shNArr[shIdx + 1u]);
                }
            }
            *blockInfo.shPtr++ = hs;
        }
        blockInfo.dstPtr += blockInfo.splatSize;
    }
    return true;
}

bool FetchBlockData(const std::shared_ptr<SpatialModel>& model, const std::shared_ptr<SpatialMeta>& outputDesc,
    const std::atomic_bool& cancelled, const TileExtractBuffer& tileBuffer, GlbBlockInfo& blockInfo)
{
    std::vector<std::string> attributeNames = { SpatialTag::POSITION, SpatialTag::SCALE,
        SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0, SpatialTag::OPACITY,
        SpatialTag::ROTATION };
    if (blockInfo.shDegree > 0u) {
        attributeNames.push_back(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER);
    }
    for (size_t blockIdx = blockInfo.startBlock; blockIdx < blockInfo.endBlock; blockIdx++) {
        if (cancelled.load()) {
            return false;
        }
        GetBlockInfo(tileBuffer, blockIdx, blockInfo);
        if (blockInfo.reqStartInBlock > blockInfo.reqEndInBlock || blockInfo.reqEndInBlock > blockInfo.gsCount ||
            (blockInfo.reqEndInBlock - blockInfo.reqStartInBlock) > blockInfo.blockSize) {
            return false;
        }
        std::vector<std::shared_ptr<const ModelAttribute>> attrMaps(attributeNames.size());
        std::vector<const float*> attributes(attributeNames.size());
        outputDesc->AddMeta(SpatialTag::BLOCK_INDEX, static_cast<uint32_t>(blockIdx));
        for (size_t i = 0; i < attributeNames.size(); ++i) {
            if (cancelled.load()) {
                return false;
            }
            const std::string& attrName = attributeNames[i];
            auto ret = model->GetAttribute(attrName, *outputDesc, attrMaps[i]);
            if (ret != SpatialErrorCode::SUCCESS || attrMaps[i] == nullptr) {
                CORE_LOG_E("Failed to dequantize attribute at index %zu", i);
                return false;
            }
            auto [pointPtr, pointCount] = attrMaps[i]->GetAs<float>();
            size_t countPerData = 1;
            if (i < 3u) {
                countPerData = 3u;
            } else if (i == 4u) {
                countPerData = 4u;
            } else if (i == 5u) {
                countPerData = blockInfo.numHarmonics;
            }
            if (pointCount < blockInfo.reqEndInBlock * countPerData) {
                CORE_LOG_E("Attribute %zu pointCount %zu < reqEndInBlock %zu", i, pointCount, blockInfo.reqEndInBlock);
                return false;
            }
            attributes[i] = pointPtr;
        }

        const uint32_t* shUv = nullptr;
        if (blockInfo.compressedSh) {
            auto uvDesc = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
            if (uvDesc == nullptr) {
                return false;
            }
            uvDesc->AddMeta(SpatialTag::BLOCK_INDEX, static_cast<uint32_t>(blockIdx));
            uvDesc->AddMeta(SpatialTag::BLOCK_POINTS_NUM, static_cast<uint32_t>(blockInfo.blockSize));
            uvDesc->AddMeta(SpatialTag::UV, static_cast<uint32_t>(1));
            std::shared_ptr<const ModelAttribute> uvAttr;
            if (model->GetAttribute(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER, *uvDesc, uvAttr) !=
                    SpatialErrorCode::SUCCESS ||
                uvAttr == nullptr) {
                CORE_LOG_E("Failed to fetch compressed SH UV");
                return false;
            }
            auto [uvPtr, uvCount] = uvAttr->GetAs<uint32_t>();
            if (uvPtr == nullptr || uvCount < blockInfo.reqEndInBlock * 2u) {
                CORE_LOG_E("Compressed SH UV count too small");
                return false;
            }
            shUv = uvPtr;
        }

        if (!GetBlockData(attributes, shUv, cancelled, tileBuffer, blockInfo)) {
            return false;
        }
        blockInfo.writtenCount += (blockInfo.reqEndInBlock - blockInfo.reqStartInBlock);
        std::vector<std::shared_ptr<const ModelAttribute>>().swap(attrMaps);
        std::vector<const float*>().swap(attributes);
    }
    return true;
}

bool ExtractGlbSplatsToBuffer(const CachedSogData& cache, const std::atomic_bool& cancelled,
    const TileExtractBuffer& tileBuffer, GsPrimitive& primitive)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "ExtractGlbSplatsToBuffer", "", 0xFF00FF);
    std::shared_ptr<SpatialModel> model;
    GlbBlockInfo blockInfo;
    if (!InitExtractInfo(cache, tileBuffer, model, blockInfo) || model == nullptr) {
        return false;
    }
    std::vector<std::string> attributeNames = { SpatialTag::POSITION, SpatialTag::SCALE,
        SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0, SpatialTag::OPACITY,
        SpatialTag::ROTATION };
    auto outputDesc = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
    if (outputDesc == nullptr) {
        return false;
    }
    outputDesc->AddMeta(SpatialTag::BLOCK_POINTS_NUM, static_cast<uint32_t>(blockInfo.blockSize));
    outputDesc->AddMeta(SpatialTag::RECONSTRUCTED, static_cast<uint32_t>(1));
    blockInfo.dstPtr = tileBuffer.dst;
    if (!FetchBlockData(model, outputDesc, cancelled, tileBuffer, blockInfo)) {
        return false;
    }
    primitive.numGs += blockInfo.writtenCount;
    primitive.shDegree = cache.shDegree;
    primitive.codecVersion = cache.hasCompressedSh ? cache.codecVersion : 0u;
    if (cache.hasCompressedSh) {
        primitive.compressedShWidth = cache.compressedShWidth;
        primitive.compressedShHeight = cache.compressedShHeight;
        primitive.compressedShAstcBlock = cache.compressedShAstcBlock;
        primitive.attribute2DSingleWidth = cache.attribute2DSingleWidth;
        primitive.attribute2DSingleHeight = cache.attribute2DSingleHeight;
        primitive.attribute2DTexNum = GS_MAX_NUM_GS_TEXTURES;
        primitive.textIdOffset = 0u;
        primitive.shDcMins = cache.shDcMins;
        primitive.shDcMaxs = cache.shDcMaxs;
        for (size_t ii = 0; ii < 16u; ii++) {
            primitive.shRestMins_R[ii] = cache.shRestMins_R[ii];
            primitive.shRestMins_G[ii] = cache.shRestMins_G[ii];
            primitive.shRestMins_B[ii] = cache.shRestMins_B[ii];
            primitive.shRestMaxs_R[ii] = cache.shRestMaxs_R[ii];
            primitive.shRestMaxs_G[ii] = cache.shRestMaxs_G[ii];
            primitive.shRestMaxs_B[ii] = cache.shRestMaxs_B[ii];
        }
    }
    if (!tileBuffer.skipAabb) {
        primitive.aabbMin = Math::min(primitive.aabbMin, blockInfo.aabbMin);
        primitive.aabbMax = Math::max(primitive.aabbMax, blockInfo.aabbMax);
    }
    return blockInfo.writtenCount == tileBuffer.count;
}

}