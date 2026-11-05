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

#include "sog_parsing.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <zlib.h>

#include <base/containers/array_view.h>
#include <base/containers/string.h>
#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/float_packer.h>
#include <base/math/matrix.h>
#include <base/math/matrix_util.h>
#include <base/math/vector.h>
#include <base/math/vector_util.h>
#include <core/json/json.h>
#include <core/log.h>

#include "render/render_data_store_default_gs.h"
#include "util/gs_limits.h"
#include "util/gs_utils.h"

namespace {
#include "app/shaders/common/gs_common.h"
}  // namespace

namespace GS {

using namespace BASE_NS;
using namespace CORE_NS;

namespace {
static constexpr uint32_t kSogLocalFileHeaderSig = 0x04034b50u;
static constexpr uint32_t kSogCentralDirHeaderSig = 0x02014b50u;
static constexpr size_t kSogEocdMaxScan = 65557u;
static constexpr float kSogSqrt2 = 1.41421356237309504880f;
static constexpr float kSogQuatScale = kSogSqrt2 / 255.0f;
static constexpr float kSogQuatBias = kSogSqrt2 * 0.5f;

class SimpleZipReader {
public:
    struct FileEntry {
        BASE_NS::string filename;
        uint32_t compressedSize{0};
        uint32_t uncompressedSize{0};
        uint32_t localHeaderOffset{0};
        uint16_t compression{0};
    };

    struct ExtractedFile {
        BASE_NS::vector<uint8_t> storage;
        BASE_NS::array_view<const uint8_t> view;
        bool valid{false};
    };

    bool Open(const uint8_t* data, size_t size)
    {
        data_ = data;
        size_ = size;
        if (!ParseZipDirectory()) {
            return false;
        }

        index_.reserve(entries_.size());
        for (size_t ii = 0u; ii < entries_.size(); ii++) {
            index_.insert({BASE_NS::string(entries_[ii].filename.data(), entries_[ii].filename.size()), ii});
        }

        return true;
    }

    ExtractedFile ExtractFile(const BASE_NS::string& filename) const
    {
        CORE_CPU_PERF_SCOPE("LUMEGS", "ExtractFile", "", 0xFF00FF);
        const BASE_NS::string key(filename.data(), filename.size());
        const auto it = index_.find(key);

        if (it == index_.end()) {
            return {};
        }

        return ExtractEntry(entries_[it->second]);
    }

private:
    const uint8_t* data_{nullptr};
    size_t size_{0};
    BASE_NS::vector<FileEntry> entries_;
    BASE_NS::unordered_map<BASE_NS::string, size_t> index_;

    bool ParseZipDirectory()
    {
        if (size_ < 22u) {
            return false;
        }

        const size_t scanStart = size_ - 22u;
        const size_t scanLimit = (size_ > kSogEocdMaxScan) ? size_ - kSogEocdMaxScan : 0u;
        size_t eocdPos = scanStart;
        bool found = false;

        while (true) {
            if (data_[eocdPos] == 0x50u && data_[eocdPos + 1u] == 0x4bu && data_[eocdPos + 2u] == 0x05u &&
                data_[eocdPos + 3u] == 0x06u) {
                found = true;
                break;
            }

            if (eocdPos == scanLimit) {
                break;
            }

            eocdPos--;
        }

        if (!found) {
            return false;
        }

        uint32_t centralDirOffset { 0 };
        BASE_NS::CloneData(&centralDirOffset, sizeof(centralDirOffset),
            data_ + eocdPos + 16u, sizeof(centralDirOffset));
        uint16_t numEntries { 0 };
        BASE_NS::CloneData(&numEntries, sizeof(numEntries), data_ + eocdPos + 10u, sizeof(numEntries));

        entries_.reserve(static_cast<size_t>(numEntries));

        size_t pos = static_cast<size_t>(centralDirOffset);
        for (size_t ii = 0u; ii < static_cast<size_t>(numEntries) && pos < eocdPos; ii++) {
            if (pos + 46u > size_) {
                break;
            }
            uint32_t sig { 0 };
            BASE_NS::CloneData(&sig, sizeof(sig), data_ + pos, sizeof(sig));
            if (sig != kSogCentralDirHeaderSig) {
                break;
            }

            FileEntry entry;
            BASE_NS::CloneData(&entry.compression, sizeof(entry.compression),
                data_ + pos + 10u, sizeof(entry.compression));
            BASE_NS::CloneData(&entry.compressedSize, sizeof(entry.compressedSize),
                data_ + pos + 20u, sizeof(entry.compressedSize));
            BASE_NS::CloneData(&entry.uncompressedSize, sizeof(entry.uncompressedSize),
                data_ + pos + 24u, sizeof(entry.uncompressedSize));
            uint16_t nameLen;
            BASE_NS::CloneData(&nameLen, sizeof(nameLen), data_ + pos + 28u, sizeof(nameLen));
            uint16_t extraLen;
            BASE_NS::CloneData(&extraLen, sizeof(extraLen), data_ + pos + 30u, sizeof(extraLen));
            uint16_t commentLen;
            BASE_NS::CloneData(&commentLen, sizeof(commentLen), data_ + pos + 32u, sizeof(commentLen));
            BASE_NS::CloneData(&entry.localHeaderOffset, sizeof(entry.localHeaderOffset),
                data_ + pos + 42u, sizeof(entry.localHeaderOffset));

            if (pos + 46u + nameLen > size_) {
                break;
            }

            entry.filename =
                BASE_NS::string(reinterpret_cast<const char*>(data_ + pos + 46u), static_cast<size_t>(nameLen));
            entries_.push_back(entry);

            pos += 46u + static_cast<size_t>(nameLen) + static_cast<size_t>(extraLen) + static_cast<size_t>(commentLen);
        }

        return !entries_.empty();
    }

    ExtractedFile ExtractEntry(const FileEntry& entry) const
    {
        ExtractedFile result;
        if (static_cast<size_t>(entry.localHeaderOffset) + 30u > size_) {
            return result;
        }

        const size_t pos = static_cast<size_t>(entry.localHeaderOffset);
        uint32_t sig { 0 };
        BASE_NS::CloneData(&sig, sizeof(sig), data_ + pos, sizeof(sig));
        if (sig != kSogLocalFileHeaderSig) {
            return result;
        }

        uint16_t nameLen;
        BASE_NS::CloneData(&nameLen, sizeof(nameLen), data_ + pos + 26u, sizeof(nameLen));
        uint16_t extraLen;
        BASE_NS::CloneData(&extraLen, sizeof(extraLen), data_ + pos + 28u, sizeof(extraLen));

        const size_t dataOffset = pos + 30u + static_cast<size_t>(nameLen) + static_cast<size_t>(extraLen);
        if (dataOffset + static_cast<size_t>(entry.compressedSize) > size_) {
            return result;
        }

        const uint8_t* compressedData = data_ + dataOffset;

        if (entry.compression == 0u) {
            result.view =
                BASE_NS::array_view<const uint8_t>(compressedData, static_cast<size_t>(entry.uncompressedSize));
            result.valid = true;
            return result;
        } else if (entry.compression == 8u) {
            result.storage.resize(static_cast<size_t>(entry.uncompressedSize));

            z_stream strm = {};
            strm.next_in = const_cast<Bytef*>(reinterpret_cast<const Bytef*>(compressedData));
            strm.avail_in = static_cast<uInt>(entry.compressedSize);
            strm.next_out = reinterpret_cast<Bytef*>(result.storage.data());
            strm.avail_out = static_cast<uInt>(entry.uncompressedSize);

            if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) {
                result.storage.clear();
                return result;
            }
            const int ret = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);

            if (ret != Z_STREAM_END) {
                result.storage.clear();
                return result;
            }

            result.view = BASE_NS::array_view<const uint8_t>(result.storage.data(), result.storage.size());
            result.valid = true;
            return result;
        }

        return result;
    }
};
}  // namespace

void PrecomputeCovariancesSog(const BASE_NS::Math::Vec3 scale, const BASE_NS::Math::Vec4 rot,
    BASE_NS::Math::Vec4& covA, BASE_NS::Math::Vec4& covB)
{
    const float qw = rot[0u];
    const float qx = rot[1u];
    const float qy = rot[2u];
    const float qz = rot[3u];

    const float r0 = 1.0f - 2.0f * (qy * qy + qz * qz);
    const float r1 = 2.0f * (qx * qy + qw * qz);
    const float r2 = 2.0f * (qx * qz - qw * qy);
    const float r3 = 2.0f * (qx * qy - qw * qz);
    const float r4 = 1.0f - 2.0f * (qx * qx + qz * qz);
    const float r5 = 2.0f * (qy * qz + qw * qx);
    const float r6 = 2.0f * (qx * qz + qw * qy);
    const float r7 = 2.0f * (qy * qz - qw * qx);
    const float r8 = 1.0f - 2.0f * (qx * qx + qy * qy);

    BASE_NS::Math::Mat3X3 S(1.0f);
    S[0u][0u] = scale.x;
    S[1u][1u] = scale.y;
    S[2u][2u] = scale.z;

    const float rmat[9u] = {r0, r1, r2, r3, r4, r5, r6, r7, r8};
    const BASE_NS::Math::Mat3X3 R(rmat);
    const BASE_NS::Math::Mat3X3 M = S * R;

    const float m0 = M.data[0u];
    const float m1 = M.data[1u];
    const float m2 = M.data[2u];
    const float m3 = M.data[3u];
    const float m4 = M.data[4u];
    const float m5 = M.data[5u];
    const float m6 = M.data[6u];
    const float m7 = M.data[7u];
    const float m8 = M.data[8u];

    covA = BASE_NS::Math::Vec4(
        m0 * m0 + m3 * m3 + m6 * m6, m0 * m1 + m3 * m4 + m6 * m7, m0 * m2 + m3 * m5 + m6 * m8, 1.0f);
    covB = BASE_NS::Math::Vec4(
        m1 * m1 + m4 * m4 + m7 * m7, m1 * m2 + m4 * m5 + m7 * m8, m2 * m2 + m5 * m5 + m8 * m8, 0.0f);
}

CORE_NS::IImageContainer::Ptr DecodeWebP(
    const BASE_NS::array_view<const uint8_t> webpData, CORE_NS::IImageLoaderManager& imgMgr)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "DecodeWebP", "", 0xFF00FF);
    if (webpData.empty()) {
        CORE_LOG_E("SOG DecodeWebP: empty input");
        return {};
    }

    const BASE_NS::array_view<const uint8_t> view(webpData.data(), webpData.size());
    auto result = imgMgr.LoadImage(view, 0u);
    if (!result.success || !result.image) {
        CORE_LOG_E("SOG DecodeWebP: IImageLoaderManager::LoadImage failed");
        return {};
    }

    return BASE_NS::move(result.image);
}

namespace {
using JsonValue = CORE_NS::json::value;
using GsSplatData = GS::GsObjectLoadData::GsSplatData;

const JsonValue* FindObject(const JsonValue& root, const char* name)
{
    const JsonValue* value = root.find(name);
    return value != nullptr && value->is_object() ? value : nullptr;
}

bool ReadRequiredUInt(const JsonValue& root, const char* name, uint32_t& output)
{
    const JsonValue* value = root.find(name);
    if (value == nullptr || !value->is_number()) {
        CORE_LOG_E("ParseSogMeta: %s missing or not a number", name);
        return false;
    }
    output = value->as_number<uint32_t>();
    return true;
}

template<typename Type, typename Container>
void AppendNumberArray(const JsonValue& object, const char* name, Container& output)
{
    const JsonValue* values = object.find(name);
    if (values == nullptr || !values->is_array()) {
        return;
    }
    for (size_t ii = 0u; ii < values->array_.size(); ii++) {
        output.emplace_back(values->array_[ii].as_number<Type>());
    }
}

template<typename Container>
void AppendStringArray(const JsonValue& object, const char* name, Container& output)
{
    const JsonValue* values = object.find(name);
    if (values == nullptr || !values->is_array()) {
        return;
    }
    for (size_t ii = 0u; ii < values->array_.size(); ii++) {
        output.emplace_back(values->array_[ii].string_);
    }
}

template<typename Target>
void ParseCodebookFiles(const JsonValue& root, const char* name, Target& target)
{
    const JsonValue* object = FindObject(root, name);
    if (object == nullptr) {
        return;
    }
    AppendNumberArray<float>(*object, "codebook", target.codebook);
    // Codebook is indexed by uint8_t (0-255) at decode time; pad to 256 entries to prevent OOB
    if (target.codebook.size() < 256u) {
        CORE_LOG_E("CodeBook size is not 256");
        target.codebook.resize(256u, 0.0f);
    }
    AppendStringArray(*object, "files", target.files);
}

template<typename Target>
void ParseFiles(const JsonValue& root, const char* name, Target& target)
{
    const JsonValue* object = FindObject(root, name);
    if (object != nullptr) {
        AppendStringArray(*object, "files", target.files);
    }
}

void ParseMeans(const JsonValue& root, SogMeta& meta)
{
    const JsonValue* means = FindObject(root, "means");
    if (means == nullptr) {
        return;
    }
    AppendNumberArray<float>(*means, "mins", meta.means.mins);
    AppendNumberArray<float>(*means, "maxs", meta.means.maxs);
    AppendStringArray(*means, "files", meta.means.files);
}

void ParseShN(const JsonValue& root, SogMeta& meta)
{
    const JsonValue* shN = FindObject(root, "shN");
    if (shN == nullptr) {
        return;
    }

    const JsonValue* count = shN->find("count");
    if (count != nullptr && count->is_number()) {
        meta.shN.count = count->as_number<uint32_t>();
    }

    const JsonValue* bands = shN->find("bands");
    if (bands != nullptr && bands->is_number()) {
        meta.shN.bands = bands->as_number<uint32_t>();
    }

    AppendNumberArray<float>(*shN, "codebook", meta.shN.codebook);
    AppendStringArray(*shN, "files", meta.shN.files);
}

struct SogDecodeNeeds {
    bool positions{};
    bool quats{};
    bool scales{};
    bool sh0{};
    bool shN{};
};

SogDecodeNeeds GetSogDecodeNeeds(const SogMeta& meta, bool decodeAuxiliary)
{
    SogDecodeNeeds needs;
    needs.positions = meta.means.files.size() >= 2u;
    needs.quats = !meta.quats.files.empty();
    needs.scales = !meta.scales.files.empty() && !meta.scales.codebook.empty();
    needs.sh0 = !meta.sh0.files.empty() && !meta.sh0.codebook.empty();
    needs.shN = decodeAuxiliary && meta.shN.bands > 0u && meta.shN.files.size() >= 2u && !meta.shN.codebook.empty();
    return needs;
}

CORE_NS::IImageContainer::Ptr LoadSogImage(
    const SimpleZipReader& zip, const BASE_NS::string& file, const char* name, CORE_NS::IImageLoaderManager& imgMgr)
{
    const SimpleZipReader::ExtractedFile source = zip.ExtractFile(file);
    if (!source.valid) {
        CORE_LOG_E("SOG: failed to extract %s image", name);
        return {};
    }

    auto img = DecodeWebP(source.view, imgMgr);
    if (!img) {
        CORE_LOG_E("SOG: failed to decode %s image", name);
    }

    return img;
}

void BuildExpScales(CachedSogData& cache)
{
    const auto& cb = cache.meta.scales.codebook;
    const size_t scalesSize = BASE_NS::Math::min<size_t>(cb.size(), 256u);
    for (size_t ii = 0u; ii < scalesSize; ii++) {
        cache.expScales[ii] = std::exp(cb[ii]);
    }

    for (size_t ii = scalesSize; ii < 256u; ii++) {
        cache.expScales[ii] = 1.0f;
    }
}

bool DecodeSogImages(
    const SimpleZipReader& zip, const SogDecodeNeeds& needs, CachedSogData& cache, CORE_NS::IImageLoaderManager& imgMgr)
{
    const uint32_t count = cache.meta.count;
    const size_t stride = CachedSogData::kSplatStride;

    const bool wantsSplats = needs.positions || needs.quats || needs.scales || needs.sh0;
    if (wantsSplats) {
        cache.packedSplats.resize(static_cast<size_t>(count) * stride);
    }

    uint8_t* const splats = wantsSplats ? cache.packedSplats.data() : nullptr;

    auto validateImagePixels = [count](const CORE_NS::IImageContainer::Ptr& img, const char* name) -> bool {
        const size_t pixelCount = img->GetData().size();
        const size_t needCount = static_cast<size_t>(count) * 4u;
        if (pixelCount < needCount) {
            CORE_LOG_E("SOG: %s image has %zu pixels, need %zu", name, pixelCount, needCount);
            return false;
        }
        return true;
    };

    if (needs.positions) {
        auto meansL = LoadSogImage(zip, cache.meta.means.files[0u], "meansL", imgMgr);
        if (!meansL || !validateImagePixels(meansL, "meansL")) {
            return false;
        }

        const uint8_t* lo = meansL->GetData().data();
        for (uint32_t ii = 0u; ii < count; ii++) {
            uint8_t* dst = splats + static_cast<size_t>(ii) * stride + CachedSogData::kPosOffset;
            const size_t off = static_cast<size_t>(ii) * 4u;
            dst[0u] = lo[off + 0u];
            dst[2u] = lo[off + 1u];
            dst[4u] = lo[off + 2u];
        }
        meansL.reset();

        auto meansU = LoadSogImage(zip, cache.meta.means.files[1u], "meansU", imgMgr);
        if (!meansU || !validateImagePixels(meansU, "meansU")) {
            return false;
        }

        const uint8_t* hi = meansU->GetData().data();
        for (uint32_t ii = 0u; ii < count; ii++) {
            uint8_t* dst = splats + static_cast<size_t>(ii) * stride + CachedSogData::kPosOffset;
            const size_t off = static_cast<size_t>(ii) * 4u;
            dst[1u] = hi[off + 0u];
            dst[3u] = hi[off + 1u];
            dst[5u] = hi[off + 2u];
        }

        cache.hasPositions = true;
    }

    if (needs.quats) {
        auto quats = LoadSogImage(zip, cache.meta.quats.files[0u], "quats", imgMgr);
        if (!quats || !validateImagePixels(quats, "quats")) {
            return false;
        }

        const uint8_t* src = quats->GetData().data();
        for (uint32_t ii = 0u; ii < count; ii++) {
            uint8_t* dst = splats + static_cast<size_t>(ii) * stride + CachedSogData::kQuatOffset;
            const size_t off = static_cast<size_t>(ii) * 4u;
            dst[0u] = src[off + 0u];
            dst[1u] = src[off + 1u];
            dst[2u] = src[off + 2u];
            dst[3u] = src[off + 3u];
        }
        cache.hasQuats = true;
    }

    if (needs.scales) {
        auto scales = LoadSogImage(zip, cache.meta.scales.files[0u], "scales", imgMgr);
        if (!scales || !validateImagePixels(scales, "scales")) {
            return false;
        }

        const uint8_t* src = scales->GetData().data();
        for (uint32_t ii = 0u; ii < count; ii++) {
            uint8_t* dst = splats + static_cast<size_t>(ii) * stride + CachedSogData::kScaleOffset;
            const size_t off = static_cast<size_t>(ii) * 4u;
            dst[0u] = src[off + 0u];
            dst[1u] = src[off + 1u];
            dst[2u] = src[off + 2u];
        }

        cache.hasScales = true;
        BuildExpScales(cache);
    }

    if (needs.sh0) {
        auto sh0 = LoadSogImage(zip, cache.meta.sh0.files[0u], "sh0", imgMgr);
        if (!sh0 || !validateImagePixels(sh0, "sh0")) {
            return false;
        }

        const uint8_t* src = sh0->GetData().data();
        for (uint32_t ii = 0u; ii < count; ii++) {
            uint8_t* dst = splats + static_cast<size_t>(ii) * stride + CachedSogData::kSh0Offset;
            const size_t off = static_cast<size_t>(ii) * 4u;
            dst[0u] = src[off + 0u];
            dst[1u] = src[off + 1u];
            dst[2u] = src[off + 2u];
            dst[3u] = src[off + 3u];
        }

        cache.hasSh0 = true;
    }

    if (needs.shN) {
        auto centroids = LoadSogImage(zip, cache.meta.shN.files[0u], "centroids", imgMgr);
        if (!centroids) {
            return false;
        }

        const uint32_t cw = centroids->GetImageDesc().width;
        const uint32_t ch = centroids->GetImageDesc().height;
        cache.centroidsWidth = cw;
        cache.shDegree = cache.meta.shN.bands;

        const size_t centroidPixels = static_cast<size_t>(cw) * static_cast<size_t>(ch);
        cache.packedCentroids.resize(centroidPixels * 3u);
        const uint8_t* csrc = centroids->GetData().data();
        for (size_t ii = 0u; ii < centroidPixels; ii++) {
            const size_t srcOff = ii * 4u;
            const size_t dstOff = ii * 3u;
            cache.packedCentroids[dstOff + 0u] = csrc[srcOff + 0u];
            cache.packedCentroids[dstOff + 1u] = csrc[srcOff + 1u];
            cache.packedCentroids[dstOff + 2u] = csrc[srcOff + 2u];
        }
        centroids.reset();

        auto labels = LoadSogImage(zip, cache.meta.shN.files[1u], "labels", imgMgr);
        if (!labels || !validateImagePixels(labels, "labels")) {
            return false;
        }

        cache.packedLabels.resize(count);
        const uint8_t* lsrc = labels->GetData().data();
        for (uint32_t ii = 0u; ii < count; ii++) {
            const size_t off = static_cast<size_t>(ii) * 4u;
            cache.packedLabels[ii] =
                static_cast<uint16_t>(lsrc[off + 0u] | (static_cast<uint16_t>(lsrc[off + 1u]) << 8u));
        }
    }

    return true;
}

struct SogExtractContext {
    const CachedSogData& cache;
    const SogMeta& meta;

    bool havePositions{};
    bool haveQuats{};
    bool haveScales{};
    bool haveSH0{};
    bool haveSHN{};
    float xMin{};
    float yMin{};
    float zMin{};
    float xRange{};
    float yRange{};
    float zRange{};
    uint32_t shCoeffs{};
    size_t shPackedCount{};
    bool useTilePacked {false};

    const uint8_t* splats{nullptr};
    const uint16_t* labels{nullptr};
    const uint8_t* centroids{nullptr};

    explicit SogExtractContext(const CachedSogData& cache) : cache(cache), meta(cache.meta)
    {}
};

struct SogExtractState {
    BASE_NS::vector<float> shDataTransposed;
    BASE_NS::Math::Vec3 aabbMin{
        std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
    BASE_NS::Math::Vec3 aabbMax{std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest()};
};

bool GetSogExtractRange(const SogMeta& meta, size_t offset, size_t& count, uint32_t& start, uint32_t& end)
{
    if (count == 0u) {
        count = meta.count;
    }
    if (offset >= meta.count) {
        CORE_LOG_E("SOG ExtractSplats: offset %zu >= meta.count %u", offset, meta.count);
        return false;
    }
    if (offset + count > meta.count) {
        CORE_LOG_E("SOG ExtractSplats: offset+count (%zu+%zu) > meta.count %u", offset, count, meta.count);
        return false;
    }

    start = static_cast<uint32_t>(offset);
    end = static_cast<uint32_t>(offset + count);
    return true;
}

void ResizeSogOutputs(
    GsObjectLoadData& loadData, const size_t currentSize, const uint32_t loadNum, const bool extractAuxiliary)
{
    loadData.gsVerticesData.resize(currentSize + loadNum);
    if (!extractAuxiliary) {
        return;
    }

    loadData.gaussianPositions.resize(currentSize + loadNum);
    loadData.gaussianIndices.resize(currentSize + loadNum);
    loadData.sphericalHarmonics.resize(currentSize + loadNum);
}

void InitSogPositionRanges(SogExtractContext& ctx)
{
    if (!ctx.havePositions) {
        return;
    }

    ctx.xMin = ctx.meta.means.mins[0u];
    ctx.yMin = ctx.meta.means.mins[1u];
    ctx.zMin = ctx.meta.means.mins[2u];
    ctx.xRange = (ctx.meta.means.maxs[0u] - ctx.xMin) / 65535.0f;
    ctx.yRange = (ctx.meta.means.maxs[1u] - ctx.yMin) / 65535.0f;
    ctx.zRange = (ctx.meta.means.maxs[2u] - ctx.zMin) / 65535.0f;
}

void InitSogShPacking(SogExtractContext& ctx)
{
    constexpr uint32_t coeffsPerDegree[4u] = {0u, 3u, 8u, 15u};
    ctx.shCoeffs = ctx.haveSHN ? coeffsPerDegree[std::min(ctx.cache.shDegree, 3u)] : 0u;
    ctx.shPackedCount = (static_cast<size_t>(ctx.shCoeffs) + 1u) * 3u / 2u;
}

void InitSogViews(SogExtractContext& ctx)
{
    ctx.splats = !ctx.cache.packedSplats.empty() ? ctx.cache.packedSplats.data() : nullptr;
    ctx.labels = ctx.haveSHN ? ctx.cache.packedLabels.data() : nullptr;
    ctx.centroids = ctx.haveSHN ? ctx.cache.packedCentroids.data() : nullptr;
}

bool InitSogExtractContext(SogExtractContext& ctx)
{
    ctx.havePositions = ctx.cache.hasPositions;
    ctx.haveQuats = ctx.cache.hasQuats;
    ctx.haveScales = ctx.haveQuats && ctx.cache.hasScales;
    ctx.haveSH0 = ctx.cache.hasSh0;
    ctx.haveSHN = ctx.haveSH0 && !ctx.cache.packedLabels.empty() && !ctx.cache.packedCentroids.empty();

    if (ctx.havePositions && (ctx.meta.means.mins.size() < 3U || ctx.meta.means.maxs.size() < 3U)) {
        CORE_LOG_E("SOG: means mins/maxs arrays are too short");
        return false;
    }

    InitSogPositionRanges(ctx);
    InitSogShPacking(ctx);
    InitSogViews(ctx);
    return true;
}

inline void DecodeSogPositionFast(const SogExtractContext& ctx, const uint8_t* splat, GsSplatData& data,
    SogExtractState& scratch, const bool skipAabb)
{
    const uint8_t* p = splat + CachedSogData::kPosOffset;
    const uint16_t xVal = static_cast<uint16_t>(p[0u]) | (static_cast<uint16_t>(p[1u]) << 8u);
    const uint16_t yVal = static_cast<uint16_t>(p[2u]) | (static_cast<uint16_t>(p[3u]) << 8u);
    const uint16_t zVal = static_cast<uint16_t>(p[4u]) | (static_cast<uint16_t>(p[5u]) << 8u);

    const float x = ctx.xMin + xVal * ctx.xRange;
    const float y = ctx.yMin + yVal * ctx.yRange;
    const float z = ctx.zMin + zVal * ctx.zRange;
    const float px = InvLogTransform(x);
    const float py = InvLogTransform(y);
    const float pz = InvLogTransform(z);

    data.positionOpacity = BASE_NS::Math::Vec4(px, py, pz, 1.0f);
    if (!skipAabb) {
        scratch.aabbMin = BASE_NS::Math::min(scratch.aabbMin, BASE_NS::Math::Vec3(px, py, pz));
        scratch.aabbMax = BASE_NS::Math::max(scratch.aabbMax, BASE_NS::Math::Vec3(px, py, pz));
    }
}

inline BASE_NS::Math::Vec4 MakeSogQuat(const int mode, const float a, const float b, const float c, const float d)
{
    switch (mode) {
        case 0:
            return BASE_NS::Math::Vec4(d, a, b, c);
        case 1:
            return BASE_NS::Math::Vec4(a, d, b, c);
        case 2:
            return BASE_NS::Math::Vec4(a, b, d, c);
        case 3:
            return BASE_NS::Math::Vec4(a, b, c, d);
        default:
            return BASE_NS::Math::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
    }
}

inline BASE_NS::Math::Vec4 DecodeSogQuatFast(const uint8_t* splat)
{
    const uint8_t* q = splat + CachedSogData::kQuatOffset;
    const uint8_t px = q[0u];
    const uint8_t py = q[1u];
    const uint8_t pz = q[2u];
    const uint8_t tag = q[3u];

    if (tag < 252u) {
        return BASE_NS::Math::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
    }

    const float a = px * kSogQuatScale - kSogQuatBias;
    const float b = py * kSogQuatScale - kSogQuatBias;
    const float c = pz * kSogQuatScale - kSogQuatBias;
    const float d2 = 1.0f - (a * a + b * b + c * c);
    const float d = d2 > 0.0f ? BASE_NS::Math::sqrt(d2) : 0.0f;

    BASE_NS::Math::Vec4 quat = MakeSogQuat(static_cast<int>(tag) - 252, a, b, c, d);
    const float lenSq = quat.x * quat.x + quat.y * quat.y + quat.z * quat.z + quat.w * quat.w;
    if (lenSq > 0.0f) {
        const float len = BASE_NS::Math::sqrt(lenSq);
        quat.x /= len;
        quat.y /= len;
        quat.z /= len;
        quat.w /= len;
    }

    return quat;
}

inline void DecodeSogCovarianceFast(
    const SogExtractContext& ctx, const uint8_t* splat, const BASE_NS::Math::Vec4& rotNorm, GsSplatData& data)
{
    const uint8_t* s = splat + CachedSogData::kScaleOffset;
    const BASE_NS::Math::Vec3 scaleExp(
        ctx.cache.expScales[s[0u]], ctx.cache.expScales[s[1u]], ctx.cache.expScales[s[2u]]);
    PrecomputeCovariancesSog(scaleExp, rotNorm, data.covAScale, data.covBColorPacked);
}

inline BASE_NS::Math::Vec3 DecodeSogSh0Fast(
    const SogExtractContext& ctx, const uint8_t* splat, GsSplatData& data, BASE_NS::Math::Vec4* gaussianPosition)
{
    const uint8_t* s = splat + CachedSogData::kSh0Offset;
    const float opacity = s[3u] * (1.0f / 255.0f);
    data.positionOpacity.w = opacity;
    if (gaussianPosition) {
        *gaussianPosition = data.positionOpacity;
    }

    const BASE_NS::Math::Vec3 rgb(
        ctx.meta.sh0.codebook[s[0u]], ctx.meta.sh0.codebook[s[1u]], ctx.meta.sh0.codebook[s[2u]]);
    const BASE_NS::Math::Vec3 color = rgb * GS::SH_C0 + BASE_NS::Math::Vec3(0.5f, 0.5f, 0.5f);
    const uint32_t colorPacked = PackGSColor(color, opacity);
    data.covBColorPacked.w = BitCast<float>(colorPacked);
    return rgb;
}

inline void DecodeSogShNFast(const SogExtractContext& ctx, const BASE_NS::Math::Vec3& rgb, const uint32_t ii,
    SogExtractState& scratch, GS::GsHarmonicsData& hsData)
{
    const uint16_t paletteIdx = ctx.labels[ii];
    if (paletteIdx >= ctx.meta.shN.count) {
        return;
    }

    float* shBuf = scratch.shDataTransposed.data();
    shBuf[0u] = rgb.x;
    shBuf[1u] = rgb.y;
    shBuf[2u] = rgb.z;

    const uint32_t cyRow = paletteIdx / 64u;
    const uint32_t cxBase = (paletteIdx % 64u) * ctx.shCoeffs;
    const uint8_t* centroids = ctx.centroids;
    const float* codebook = ctx.meta.shN.codebook.data();

    for (uint32_t jj = 0u; jj < ctx.shCoeffs; jj++) {
        const uint32_t cOff = (cyRow * ctx.cache.centroidsWidth + cxBase + jj) * 3u;
        shBuf[(jj + 1u) * 3u + 0u] = codebook[centroids[cOff + 0u]];
        shBuf[(jj + 1u) * 3u + 1u] = codebook[centroids[cOff + 1u]];
        shBuf[(jj + 1u) * 3u + 2u] = codebook[centroids[cOff + 2u]];
    }

    size_t shIdx = 0u;
    for (size_t jj = 0u; jj < ctx.shPackedCount; jj++) {
        hsData.shCoefficients[jj] = PackHalf2x16(shBuf[shIdx], shBuf[shIdx + 1u]);
        shIdx += 2u;
    }
}

inline void ExtractSogSplatTo(const SogExtractContext& ctx, const uint32_t ii, GsSplatData& dst,
    BASE_NS::Math::Vec4* auxGaussianPos, GS::GsHarmonicsData* auxHarmonics, SogExtractState& scratch,
    const bool skipAabb)
{
    const uint8_t* splat = ctx.splats + static_cast<size_t>(ii) * CachedSogData::kSplatStride;

    if (ctx.havePositions) {
        DecodeSogPositionFast(ctx, splat, dst, scratch, skipAabb);
    }

    if (ctx.haveQuats) {
        const BASE_NS::Math::Vec4 rotNorm = DecodeSogQuatFast(splat);
        if (ctx.haveScales) {
            DecodeSogCovarianceFast(ctx, splat, rotNorm, dst);
        }
    }

    if (ctx.haveSH0) {
        const BASE_NS::Math::Vec3 rgb = DecodeSogSh0Fast(ctx, splat, dst, auxGaussianPos);
        if (auxHarmonics && ctx.haveSHN) {
            DecodeSogShNFast(ctx, rgb, ii, scratch, *auxHarmonics);
        }
    }
}

void PackGsVertexData(const GsObjectLoadData::GsSplatData& gsData, GsObjectLoadData::GsSplatDataTilePacked& packedData)
{
    packedData.positionOpacityPacked = gsData.positionOpacity;
    float packedColor = gsData.covBColorPacked.w;

    packedData.covColorPacked = BASE_NS::Math::UVec4(
            PackHalf2x16(SqrtWithSign(gsData.covAScale.x), SqrtWithSign(gsData.covAScale.y)),
            PackHalf2x16(SqrtWithSign(gsData.covAScale.z), SqrtWithSign(gsData.covBColorPacked.x)),
            PackHalf2x16(
                    SqrtWithSign(gsData.covBColorPacked.y), SqrtWithSign(gsData.covBColorPacked.z)),
            *reinterpret_cast<uint32_t*>(&packedColor));
}

bool ExtractSogChunk(const SogExtractContext& ctx, const uint32_t start, const uint32_t end, uint8_t* gsDstBytes,
    BASE_NS::Math::Vec4* gaussianPositions, GS::GsHarmonicsData* sphericalHarmonics, SogExtractState& scratch,
    const std::atomic_bool& cancelled, const bool skipAabb)
{
    if (ctx.haveSHN && sphericalHarmonics && scratch.shDataTransposed.empty()) {
        scratch.shDataTransposed.resize((static_cast<size_t>(ctx.shCoeffs) + 1u) * 3u, 0.0f);
    }

    const size_t stride = ctx.useTilePacked ? sizeof(GsObjectLoadData::GsSplatDataTilePacked) : sizeof(GsSplatData);
    for (uint32_t ii = start; ii < end; ii++) {
        if (((ii - start) & 0xFFFFu) == 0u && cancelled.load()) {
            return false;
        }

        const uint32_t outIdx = ii - start;
        GsSplatData splat{};
        BASE_NS::Math::Vec4* auxPos = gaussianPositions ? &gaussianPositions[outIdx] : nullptr;
        GS::GsHarmonicsData* auxSh = sphericalHarmonics ? &sphericalHarmonics[outIdx] : nullptr;
        ExtractSogSplatTo(ctx, ii, splat, auxPos, auxSh, scratch, skipAabb);
        if (ctx.useTilePacked) {
            GsObjectLoadData::GsSplatDataTilePacked tilePackedSplat {};
            PackGsVertexData(splat, tilePackedSplat);
            BASE_NS::CloneData(gsDstBytes + static_cast<size_t>(outIdx) * stride, stride, &tilePackedSplat, stride);
        } else {
            BASE_NS::CloneData(gsDstBytes + static_cast<size_t>(outIdx) * stride, stride, &splat, stride);
        }
    }

    return true;
}

}  // namespace

bool ParseSogMeta(const BASE_NS::array_view<const uint8_t> jsonData, SogMeta& meta)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "ParseSogMeta", "", 0xFF00FF);
    BASE_NS::string jsonString(jsonData.size(), '\0');
    if (!BASE_NS::CloneData(jsonString.data(), jsonString.size(), jsonData.data(), jsonData.size())) {
        CORE_LOG_E("ParseSogMeta: CloneData failed");
        return false;
    }

    const JsonValue root = CORE_NS::json::parse(jsonString.data());
    if (!ReadRequiredUInt(root, "version", meta.version) || !ReadRequiredUInt(root, "count", meta.count)) {
        return false;
    }

    if (static_cast<size_t>(meta.count) > GS::MAX_SOG_SPLAT_COUNT) {
        CORE_LOG_E("ParseSogMeta: count %u exceeds limit %zu", meta.count, GS::MAX_SOG_SPLAT_COUNT);
        return false;
    }

    ParseMeans(root, meta);
    ParseCodebookFiles(root, "scales", meta.scales);
    ParseFiles(root, "quats", meta.quats);
    ParseCodebookFiles(root, "sh0", meta.sh0);
    ParseShN(root, meta);

    return true;
}

bool DecodeSogFile(const uint8_t* fileData, size_t size, CachedSogData& cache, CORE_NS::IImageLoaderManager& imgMgr,
    const bool decodeAuxiliary)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "DecodeSogFile", "", 0xFF00FF);
    SimpleZipReader zip;
    if (!zip.Open(fileData, size)) {
        CORE_LOG_E("SOG: failed to open ZIP archive");
        return false;
    }

    auto metaExtracted = zip.ExtractFile("meta.json");
    if (!metaExtracted.valid || metaExtracted.view.empty()) {
        CORE_LOG_E("SOG: meta.json not found in archive");
        return false;
    }

    if (!ParseSogMeta(metaExtracted.view, cache.meta)) {
        CORE_LOG_E("SOG: failed to parse meta.json");
        return false;
    }

    if (cache.meta.version < 2u) {
        CORE_LOG_E("SOG: version %u < 2 is not supported", cache.meta.version);
        return false;
    }

    CORE_LOG_I("SOG DecodeSogFile: version=%u count=%u", cache.meta.version, cache.meta.count);

    const SogDecodeNeeds needs = GetSogDecodeNeeds(cache.meta, decodeAuxiliary);
    return DecodeSogImages(zip, needs, cache, imgMgr);
}

bool ExtractSplatsFromCache(const CachedSogData& cache, const size_t offset, size_t count, GsObjectLoadData& loadData,
    GsPrimitive& primitive, const std::atomic_bool& cancelled, const bool extractAuxiliary, const bool skipAabb)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "ExtractSplatsFromCache", "", 0xFF00FF);

    uint32_t start = 0u;
    uint32_t end = 0u;
    if (!GetSogExtractRange(cache.meta, offset, count, start, end)) {
        return false;
    }

    const uint32_t loadNum = end - start;
    const size_t currentSize = loadData.gsVerticesData.size();
    ResizeSogOutputs(loadData, currentSize, loadNum, extractAuxiliary);
    primitive.numGs += loadNum;

    SogExtractContext ctx(cache);
    if (!InitSogExtractContext(ctx)) {
        return false;
    }

    if (ctx.haveSH0 && cache.shDegree > 0u) {
        primitive.shDegree = cache.shDegree;
    }

    GsSplatData* gsDst = loadData.gsVerticesData.data() + currentSize;
    BASE_NS::Math::Vec4* gaussianPositions =
        extractAuxiliary ? loadData.gaussianPositions.data() + currentSize : nullptr;
    GS::GsHarmonicsData* sphericalHarmonics =
        (extractAuxiliary && ctx.haveSHN) ? loadData.sphericalHarmonics.data() + currentSize : nullptr;

    SogExtractState scratch;
    if (!ExtractSogChunk(ctx,
            start,
            end,
            reinterpret_cast<uint8_t*>(gsDst),
            gaussianPositions,
            sphericalHarmonics,
            scratch,
            cancelled,
            skipAabb)) {
        return false;
    }

    if (!skipAabb) {
        primitive.aabbMin = BASE_NS::Math::min(primitive.aabbMin, scratch.aabbMin);
        primitive.aabbMax = BASE_NS::Math::max(primitive.aabbMax, scratch.aabbMax);
    }
    return true;
}

bool ExtractSplatsToBuffer(const CachedSogData& cache, const size_t offset, size_t count, uint8_t* dst,
    const size_t dstBytes, GsPrimitive& primitive, const std::atomic_bool& cancelled, const bool skipAabb,
    bool useTilePacked)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "ExtractSplatsToBuffer", "", 0xFF00FF);

    uint32_t start = 0u;
    uint32_t end = 0u;
    if (!GetSogExtractRange(cache.meta, offset, count, start, end)) {
        return false;
    }

    const uint32_t loadNum = end - start;
    const size_t required = useTilePacked
                                ? static_cast<size_t>(loadNum) * sizeof(GsObjectLoadData::GsSplatDataTilePacked)
                                : static_cast<size_t>(loadNum) * sizeof(GsSplatData);
    if (dst == nullptr || dstBytes < required) {
        return false;
    }

    SogExtractContext ctx(cache);
    if (!InitSogExtractContext(ctx)) {
        return false;
    }
    ctx.useTilePacked = useTilePacked;

    if (ctx.haveSH0 && cache.shDegree > 0u) {
        primitive.shDegree = cache.shDegree;
    }

    SogExtractState scratch;
    if (!ExtractSogChunk(ctx, start, end, dst, nullptr, nullptr, scratch, cancelled, skipAabb)) {
        return false;
    }

    primitive.numGs += loadNum;

    if (!skipAabb) {
        primitive.aabbMin = BASE_NS::Math::min(primitive.aabbMin, scratch.aabbMin);
        primitive.aabbMax = BASE_NS::Math::max(primitive.aabbMax, scratch.aabbMax);
    }

    return true;
}

bool ParseGSSog(const uint8_t* fileData, size_t size, GS::GsObjectLoadData& loadData, GS::GsPrimitive& primitive,
    CORE_NS::IImageLoaderManager& imgMgr, size_t offset, size_t count, const std::atomic_bool& cancel)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "ParseGSSog", "", 0xFF00FF);
    CachedSogData tempCache;
    if (!DecodeSogFile(fileData, size, tempCache, imgMgr)) {
        return false;
    }

    return ExtractSplatsFromCache(tempCache, offset, count, loadData, primitive, cancel);
}
}  // namespace GS