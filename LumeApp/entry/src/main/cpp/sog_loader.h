/*
 * sog_loader.h - SOG v2 (Self-Organizing Gaussians) loader for 3DGS.
 *
 * A .sog file is a ZIP container (entries stored, not deflated) with:
 *   meta.json          codebooks, quantization ranges, gaussian count
 *   means_l.webp       position, low byte  (16-bit across l/u, per RGB axis)
 *   means_u.webp       position, high byte
 *   scales.webp        RGB = index into the 256-entry log-scale codebook
 *   quats.webp         RGB = three smallest quat components, A = largest tag
 *   sh0.webp           RGB = index into the DC codebook, A = opacity
 *   shN_centroids.webp 64*shCoeffs wide palette, RGB = codebook indices
 *   shN_labels.webp    R|G<<8 = palette row for each gaussian
 *
 * Positions are stored log-transformed: pos = invLog(min + span * v / 65535)
 * with invLog(x) = sign(x) * (exp(|x|) - 1). Scales are log-space (exp()).
 * Quaternions use the "smallest three" packing (tag 252..255 selects the
 * reconstructed component). SH rest is palette-quantized and channel-major in
 * the palette, while this project stores coefficient-major f_rest.
 *
 * WebP decoding uses the system ImageKit NDK (libimage_source / libpixelmap),
 * so the app must link those two libraries.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <future>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#ifndef LOGI
#include <hilog/log.h>
#undef LOG_DOMAIN
#define LOG_DOMAIN 0x4C4D /* "LM" */
#undef LOG_TAG
#define LOG_TAG "LumeApp"
#define LOGI(...) OH_LOG_Print(LOG_APP, LOG_INFO, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#define LOGW(...) OH_LOG_Print(LOG_APP, LOG_WARN, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) OH_LOG_Print(LOG_APP, LOG_ERROR, LOG_DOMAIN, LOG_TAG, __VA_ARGS__)
#endif

#include <multimedia/image_framework/image/image_source_native.h>
#include <multimedia/image_framework/image/pixelmap_native.h>

#include "splat_loader.h"

namespace sog {

// ---------------------------------------------------------------------------
// Minimal JSON reader (only what meta.json needs)
// ---------------------------------------------------------------------------

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0.0;
    std::string text;
    std::vector<Json> items;
    std::vector<std::pair<std::string, Json>> fields;

    const Json* get(const char* key) const
    {
        for (size_t i = 0; i < fields.size(); i++) {
            if (fields[i].first == key) {
                return &fields[i].second;
            }
        }
        return nullptr;
    }
    double num(size_t i, double fallback = 0.0) const
    {
        return i < items.size() ? items[i].number : fallback;
    }
};

class JsonParser {
public:
    JsonParser(const char* begin, const char* end) : p_(begin), end_(end) {}

    bool Parse(Json& out)
    {
        SkipSpace();
        const bool ok = ParseValue(out);
        SkipSpace();
        return ok;
    }

private:
    const char* p_;
    const char* end_;

    void SkipSpace()
    {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\n' || *p_ == '\r' || *p_ == '\t')) {
            ++p_;
        }
    }

    bool Literal(const char* lit)
    {
        const size_t n = strlen(lit);
        if ((size_t)(end_ - p_) < n || memcmp(p_, lit, n) != 0) {
            return false;
        }
        p_ += n;
        return true;
    }

    bool ParseString(std::string& out)
    {
        if (p_ >= end_ || *p_ != '"') {
            return false;
        }
        ++p_;
        out.clear();
        while (p_ < end_) {
            const char c = *p_++;
            if (c == '"') {
                return true;
            }
            if (c == '\\' && p_ < end_) {
                const char e = *p_++;
                switch (e) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        // meta.json is ASCII; skip the escape payload.
                        if (end_ - p_ >= 4) {
                            p_ += 4;
                        }
                        out.push_back('?');
                        break;
                    }
                    default: out.push_back(e); break;
                }
                continue;
            }
            out.push_back(c);
        }
        return false;
    }

    bool ParseValue(Json& out)
    {
        SkipSpace();
        if (p_ >= end_) {
            return false;
        }
        switch (*p_) {
            case '{': {
                out.type = Json::Object;
                ++p_;
                SkipSpace();
                if (p_ < end_ && *p_ == '}') {
                    ++p_;
                    return true;
                }
                while (p_ < end_) {
                    std::string key;
                    SkipSpace();
                    if (!ParseString(key)) {
                        return false;
                    }
                    SkipSpace();
                    if (p_ >= end_ || *p_ != ':') {
                        return false;
                    }
                    ++p_;
                    Json value;
                    if (!ParseValue(value)) {
                        return false;
                    }
                    out.fields.emplace_back(std::move(key), std::move(value));
                    SkipSpace();
                    if (p_ < end_ && *p_ == ',') {
                        ++p_;
                        continue;
                    }
                    if (p_ < end_ && *p_ == '}') {
                        ++p_;
                        return true;
                    }
                    return false;
                }
                return false;
            }
            case '[': {
                out.type = Json::Array;
                ++p_;
                SkipSpace();
                if (p_ < end_ && *p_ == ']') {
                    ++p_;
                    return true;
                }
                while (p_ < end_) {
                    Json value;
                    if (!ParseValue(value)) {
                        return false;
                    }
                    out.items.push_back(std::move(value));
                    SkipSpace();
                    if (p_ < end_ && *p_ == ',') {
                        ++p_;
                        continue;
                    }
                    if (p_ < end_ && *p_ == ']') {
                        ++p_;
                        return true;
                    }
                    return false;
                }
                return false;
            }
            case '"':
                out.type = Json::String;
                return ParseString(out.text);
            case 't':
                out.type = Json::Bool;
                out.boolean = true;
                return Literal("true");
            case 'f':
                out.type = Json::Bool;
                out.boolean = false;
                return Literal("false");
            case 'n':
                out.type = Json::Null;
                return Literal("null");
            default: {
                char* stop = nullptr;
                out.type = Json::Number;
                out.number = strtod(p_, &stop);
                if (stop == p_) {
                    return false;
                }
                p_ = stop;
                return true;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Minimal ZIP reader (stored entries only)
// ---------------------------------------------------------------------------

struct ZipEntry {
    std::string name;
    uint32_t localOffset = 0;
    uint32_t compressedSize = 0;
    uint32_t uncompressedSize = 0;
    uint16_t method = 0;
};

inline uint16_t ReadU16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
inline uint32_t ReadU32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline bool ReadZipEntries(const uint8_t* data, size_t size, std::vector<ZipEntry>& out)
{
    // End of central directory: scan the last 64 KB.
    const size_t scanStart = size > 65536 ? size - 65536 : 0;
    size_t eocd = size;
    for (size_t i = size >= 22 ? size - 22 : 0; i + 4 <= size; i--) {
        if (ReadU32(data + i) == 0x06054b50u) {
            eocd = i;
            break;
        }
        if (i == scanStart) {
            break;
        }
    }
    if (eocd == size) {
        return false;
    }

    const uint32_t entryCount = ReadU16(data + eocd + 10);
    const uint32_t cdOffset = ReadU32(data + eocd + 16);
    if (cdOffset + 46u > size) {
        return false;
    }

    size_t p = cdOffset;
    for (uint32_t i = 0; i < entryCount; i++) {
        if (p + 46 > size || ReadU32(data + p) != 0x02014b50u) {
            return false;
        }
        ZipEntry e;
        e.method = ReadU16(data + p + 10);
        e.compressedSize = ReadU32(data + p + 20);
        e.uncompressedSize = ReadU32(data + p + 24);
        const uint16_t nameLen = ReadU16(data + p + 28);
        const uint16_t extraLen = ReadU16(data + p + 30);
        const uint16_t commentLen = ReadU16(data + p + 32);
        e.localOffset = ReadU32(data + p + 42);
        if (p + 46 + nameLen > size) {
            return false;
        }
        e.name.assign(reinterpret_cast<const char*>(data + p + 46), nameLen);
        out.push_back(std::move(e));
        p += 46u + nameLen + extraLen + commentLen;
    }
    return true;
}

// Returns the uncompressed bytes of a stored entry, or nullptr.
inline const uint8_t* ZipEntryData(const uint8_t* data, size_t size, const ZipEntry& entry)
{
    const size_t hdr = entry.localOffset;
    if (hdr + 30 > size || ReadU32(data + hdr) != 0x04034b50u) {
        return nullptr;
    }
    if (entry.method != 0) {
        return nullptr;  // deflated entries are not supported
    }
    const uint16_t nameLen = ReadU16(data + hdr + 26);
    const uint16_t extraLen = ReadU16(data + hdr + 28);
    const size_t start = hdr + 30u + nameLen + extraLen;
    if (start + entry.compressedSize > size) {
        return nullptr;
    }
    return data + start;
}

// ---------------------------------------------------------------------------
// WebP -> RGBA8888 via the system ImageKit NDK
// ---------------------------------------------------------------------------

struct RgbaImage {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowStride = 0;
    std::vector<uint8_t> pixels;

    const uint8_t* At(uint32_t x, uint32_t y) const
    {
        return pixels.data() + (size_t)y * rowStride + (size_t)x * 4u;
    }
};

inline bool DecodeWebp(const uint8_t* data, size_t size, RgbaImage& out, const char* name)
{
    OH_ImageSourceNative* source = nullptr;
    if (OH_ImageSourceNative_CreateFromData(const_cast<uint8_t*>(data), size, &source) != IMAGE_SUCCESS ||
        source == nullptr) {
        LOGE("SOG: cannot open webp '%{public}s'", name);
        return false;
    }

    OH_DecodingOptions* options = nullptr;
    OH_DecodingOptions_Create(&options);
    OH_DecodingOptions_SetPixelFormat(options, PIXEL_FORMAT_RGBA_8888);

    OH_PixelmapNative* pixelmap = nullptr;
    const Image_ErrorCode decodeResult = OH_ImageSourceNative_CreatePixelmap(source, options, &pixelmap);
    OH_DecodingOptions_Release(options);
    OH_ImageSourceNative_Release(source);
    if (decodeResult != IMAGE_SUCCESS || pixelmap == nullptr) {
        LOGE("SOG: webp decode failed for '%{public}s' (%{public}d)", name, (int)decodeResult);
        return false;
    }

    OH_Pixelmap_ImageInfo* info = nullptr;
    OH_PixelmapImageInfo_Create(&info);
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowStride = 0;
    int32_t format = 0;
    OH_PixelmapNative_GetImageInfo(pixelmap, info);
    OH_PixelmapImageInfo_GetWidth(info, &width);
    OH_PixelmapImageInfo_GetHeight(info, &height);
    OH_PixelmapImageInfo_GetRowStride(info, &rowStride);
    OH_PixelmapImageInfo_GetPixelFormat(info, &format);
    OH_PixelmapImageInfo_Release(info);

    if (width == 0 || height == 0) {
        LOGE("SOG: webp '%{public}s' has empty size", name);
        OH_PixelmapNative_Release(pixelmap);
        return false;
    }
    if (rowStride < width * 4u) {
        rowStride = width * 4u;
    }

    out.width = width;
    out.height = height;
    out.rowStride = rowStride;
    out.pixels.resize((size_t)rowStride * height);
    size_t bufferSize = out.pixels.size();
    const Image_ErrorCode readResult = OH_PixelmapNative_ReadPixels(pixelmap, out.pixels.data(), &bufferSize);
    OH_PixelmapNative_Release(pixelmap);
    if (readResult != IMAGE_SUCCESS) {
        LOGE("SOG: webp read pixels failed for '%{public}s' (%{public}d)", name, (int)readResult);
        return false;
    }
    if (format != PIXEL_FORMAT_RGBA_8888) {
        LOGE("SOG: webp '%{public}s' decoded as format %{public}d, expected RGBA8888", name, (int)format);
    }

    // The platform decoder returns PREMULTIPLIED alpha. SOG stores data in all
    // four channels (opacity, quaternion tag), so premultiplying corrupts RGB.
    // Undo it: p = floor(v * a / 255)  =>  v ~= (p * 255 + a/2) / a.
    for (uint32_t y = 0; y < out.height; y++) {
        uint8_t* row = out.pixels.data() + (size_t)y * out.rowStride;
        for (uint32_t x = 0; x < out.width; x++) {
            uint8_t* px = row + (size_t)x * 4u;
            const uint32_t alpha = px[3];
            if (alpha == 0 || alpha == 255) {
                continue;
            }
            for (int c = 0; c < 3; c++) {
                const uint32_t v = ((uint32_t)px[c] * 255u + alpha / 2u) / alpha;
                px[c] = (uint8_t)(v > 255u ? 255u : v);
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// SOG loading
// ---------------------------------------------------------------------------

// Inverse of logTransform(x) = sign(x) * log(|x| + 1).
inline float InvLogTransform(float v)
{
    const float e = std::exp(std::fabs(v)) - 1.0f;
    return v < 0.0f ? -e : e;
}

inline float SigmoidInverse(float y)
{
    const float e = std::min(1.0f - 1e-6f, std::max(1e-6f, y));
    return std::log(e / (1.0f - e));
}

// Smallest-three quaternion packing. QUAT_IDX maps (largest component slot,
// slot+1, slot+2) to the stored component indices; result is (w, x, y, z).
inline void UnpackQuat(uint8_t px, uint8_t py, uint8_t pz, uint8_t tag, float& qw, float& qx, float& qy,
    float& qz)
{
    static const int kQuatIdx[12] = { 1, 2, 3, 0, 2, 3, 0, 1, 3, 0, 1, 2 };
    const float kSqrt2 = 1.41421356237f;
    const int maxComp = (int)tag - 252;
    const float a = ((px / 255.0f) * 2.0f - 1.0f) / kSqrt2;
    const float b = ((py / 255.0f) * 2.0f - 1.0f) / kSqrt2;
    const float c = ((pz / 255.0f) * 2.0f - 1.0f) / kSqrt2;

    float q[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    const int base = maxComp * 3;
    q[kQuatIdx[base + 0]] = a;
    q[kQuatIdx[base + 1]] = b;
    q[kQuatIdx[base + 2]] = c;
    q[maxComp] = std::sqrt(std::max(0.0f, 1.0f - (a * a + b * b + c * c)));
    qw = q[0];
    qx = q[1];
    qy = q[2];
    qz = q[3];
}

// SH coefficient counts per band count (bands: 0..3).
inline int ShCoeffCount(int bands)
{
    static const int kCoeffs[4] = { 0, 3, 8, 15 };
    return (bands >= 0 && bands <= 3) ? kCoeffs[bands] : 0;
}

inline bool LoadSogFile(const char* path, SplatData& out, float worldScale = 100.0f, bool loadSh = true)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        LOGE("SOG: cannot open '%{public}s'", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    const long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize <= 0) {
        fclose(f);
        return false;
    }
    std::vector<uint8_t> file((size_t)fileSize);
    const size_t read = fread(file.data(), 1, file.size(), f);
    fclose(f);
    if (read != file.size()) {
        LOGE("SOG: short read on '%{public}s'", path);
        return false;
    }

    std::vector<ZipEntry> entries;
    if (!ReadZipEntries(file.data(), file.size(), entries)) {
        LOGE("SOG: '%{public}s' is not a readable ZIP container (expected a .sog with a "
             "central directory)", path);
        return false;
    }

    // ---- meta.json ----
    const ZipEntry* metaEntry = nullptr;
    for (const auto& e : entries) {
        if (e.name == "meta.json") {
            metaEntry = &e;
        }
    }
    if (metaEntry == nullptr) {
        LOGE("SOG: meta.json missing");
        return false;
    }
    const uint8_t* metaBytes = ZipEntryData(file.data(), file.size(), *metaEntry);
    if (metaBytes == nullptr) {
        LOGE("SOG: meta.json is not a stored entry");
        return false;
    }
    const size_t metaSize = metaEntry->uncompressedSize > 0 ? metaEntry->uncompressedSize
                                                             : metaEntry->compressedSize;
    std::string metaText(reinterpret_cast<const char*>(metaBytes), metaSize);
    Json meta;
    JsonParser parser(metaText.c_str(), metaText.c_str() + metaText.size());
    if (!parser.Parse(meta)) {
        LOGE("SOG: meta.json parse failed");
        return false;
    }

    const Json* versionJson = meta.get("version");
    const int version = versionJson ? (int)(versionJson->number + 0.5) : 0;
    if (version != 2) {
        // v1 used a different per-channel min/max scheme and no codebooks, so the
        // v2 decode paths below would misread it. Fail loudly instead.
        LOGE("SOG: meta.json version %{public}d is not supported (this loader implements "
             "SOG v2 with codebooks); re-export as v2", version);
        return false;
    }

    const Json* countJson = meta.get("count");
    const uint32_t count = countJson ? (uint32_t)countJson->number : 0u;
    if (count == 0) {
        LOGE("SOG: meta.json has no gaussian count");
        return false;
    }
    LOGI("SOG: %{public}u gaussians, version %{public}d", count, version);

    const Json* means = meta.get("means");
    const Json* scales = meta.get("scales");
    const Json* quats = meta.get("quats");
    const Json* sh0 = meta.get("sh0");
    if (!means || !scales || !quats || !sh0) {
        LOGE("SOG: meta.json missing means/scales/quats/sh0");
        return false;
    }

    auto findEntry = [&entries](const std::string& name) -> const ZipEntry* {
        for (const auto& e : entries) {
            if (e.name == name) {
                return &e;
            }
        }
        return nullptr;
    };

    auto decodeEntry = [&](const std::string& name, RgbaImage& image) {
        const ZipEntry* entry = findEntry(name);
        if (entry == nullptr) {
            LOGE("SOG: entry '%{public}s' missing", name.c_str());
            return false;
        }
        const uint8_t* bytes = ZipEntryData(file.data(), file.size(), *entry);
        if (bytes == nullptr) {
            if (entry->method != 0) {
                LOGE("SOG: entry '%{public}s' uses ZIP method %{public}u; only stored "
                     "(method 0) entries are supported",
                    name.c_str(), (unsigned)entry->method);
            } else {
                LOGE("SOG: entry '%{public}s' points outside the file (corrupt local header)",
                    name.c_str());
            }
            return false;
        }
        return DecodeWebp(bytes, entry->compressedSize, image, name.c_str());
    };

    auto fileList = [](const Json& node, size_t index) -> std::string {
        const Json* files = node.get("files");
        if (!files || index >= files->items.size()) {
            return {};
        }
        return files->items[index].text;
    };

    // ---- means (16-bit position, log-transformed) ----
    const Json* meansFiles = means->get("files");
    if (!meansFiles || meansFiles->items.size() < 2) {
        LOGE("SOG: means needs two textures (low + high byte), found %{public}u",
            meansFiles ? (unsigned)meansFiles->items.size() : 0u);
        return false;
    }
    RgbaImage meansLo;
    RgbaImage meansHi;
    RgbaImage scaleImg;
    RgbaImage quatImg;
    RgbaImage sh0Img;
    {
        // The five textures of a tile are independent; decoding them concurrently
        // is the difference between ~0.8 s and ~0.3 s per tile.
        auto fMeansLo = std::async(std::launch::async, [&] {
            return decodeEntry(fileList(*means, 0), meansLo);
        });
        auto fMeansHi = std::async(std::launch::async, [&] {
            return decodeEntry(fileList(*means, 1), meansHi);
        });
        auto fScales = std::async(std::launch::async, [&] {
            return decodeEntry(fileList(*scales, 0), scaleImg);
        });
        auto fQuats = std::async(std::launch::async, [&] {
            return decodeEntry(fileList(*quats, 0), quatImg);
        });
        auto fSh0 = std::async(std::launch::async, [&] {
            return decodeEntry(fileList(*sh0, 0), sh0Img);
        });
        const bool okAll = fMeansLo.get() && fMeansHi.get() && fScales.get() && fQuats.get() &&
                           fSh0.get();
        if (!okAll) {
            return false;
        }
    }
    if ((size_t)meansLo.width * meansLo.height < count || (size_t)meansHi.width * meansHi.height < count) {
        LOGE("SOG: means texture too small for %{public}u gaussians", count);
        return false;
    }
    const Json* mins = means->get("mins");
    const Json* maxs = means->get("maxs");
    if (!mins || !maxs || mins->items.size() < 3 || maxs->items.size() < 3) {
        LOGE("SOG: means has no mins/maxs");
        return false;
    }
    float posMin[3];
    float posSpan[3];
    for (int c = 0; c < 3; c++) {
        posMin[c] = (float)mins->num((size_t)c);
        const float span = (float)(maxs->num((size_t)c) - mins->num((size_t)c));
        posSpan[c] = span != 0.0f ? span : 1.0f;
    }

    // ---- scales (log space, codebook) ----
    const Json* scaleCodeJson = scales->get("codebook");
    if (!scaleCodeJson || scaleCodeJson->items.size() < 256 ||
        (size_t)scaleImg.width * scaleImg.height < count) {
        LOGE("SOG: invalid scales codebook/texture");
        return false;
    }

    // ---- quats (smallest three) ----
    if ((size_t)quatImg.width * quatImg.height < count) {
        LOGE("SOG: quats texture too small");
        return false;
    }

    // ---- sh0 (DC codebook + opacity) ----
    const Json* sh0Code = sh0->get("codebook");
    if (!sh0Code || sh0Code->items.size() < 256 || (size_t)sh0Img.width * sh0Img.height < count) {
        LOGE("SOG: invalid sh0 codebook/texture");
        return false;
    }

    // ---- shN (optional palette-quantized rest) ----
    const Json* shN = meta.get("shN");
    int shBands = 0;
    int shCoeffs = 0;
    std::vector<float> shCode;
    RgbaImage centroids;
    RgbaImage labels;
    uint32_t centroidW = 0;
    uint32_t centroidH = 0;
    uint32_t paletteCount = 0;
    if (loadSh && shN != nullptr) {
        shBands = (int)(shN->get("bands") ? shN->get("bands")->number : 0.0);
        if (shBands < 1 || shBands > 3) {
            LOGE("SOG: shN.bands %{public}d out of range (expected 1..3), SH rest ignored",
                shBands);
            shBands = 0;
        }
        shCoeffs = ShCoeffCount(shBands);
        const Json* shCodeJson = shN->get("codebook");
        paletteCount = shN->get("count") ? (uint32_t)shN->get("count")->number : 0u;
        if (shCoeffs > 0 && shCodeJson != nullptr && paletteCount > 0) {
            shCode.resize(256);
            for (size_t i = 0; i < 256 && i < shCodeJson->items.size(); i++) {
                shCode[i] = (float)shCodeJson->items[i].number;
            }
            if (decodeEntry(fileList(*shN, 0), centroids) && decodeEntry(fileList(*shN, 1), labels)) {
                centroidW = centroids.width;
                centroidH = centroids.height;
                if (centroidW != (uint32_t)(64 * shCoeffs) ||
                    (size_t)labels.width * labels.height < count) {
                    LOGE("SOG: unexpected shN palette layout (%{public}ux%{public}u)", centroidW, centroidH);
                    shCoeffs = 0;
                }
            } else {
                shCoeffs = 0;
            }
        } else {
            shCoeffs = 0;
        }
        LOGI("SOG: SH bands=%{public}d coeffs=%{public}d palette=%{public}u", shBands, shCoeffs,
            paletteCount);
    }

    // ---- decode ----
    constexpr float SH_C0 = 0.28209479177387814f;
    out.gaussians.clear();
    out.gaussians.resize(count);
    out.sh.clear();
    if (shCoeffs > 0) {
        out.sh.assign((size_t)count * 48u, 0.0f);
    }
    out.boundsMin[0] = out.boundsMin[1] = out.boundsMin[2] = 1e30f;
    out.boundsMax[0] = out.boundsMax[1] = out.boundsMax[2] = -1e30f;

    float scaleCode[256];
    float sh0Table[256];
    for (int i = 0; i < 256; i++) {
        scaleCode[i] = (float)scaleCodeJson->num((size_t)i);
        sh0Table[i] = (float)sh0Code->num((size_t)i);
    }

    for (uint32_t g = 0; g < count; g++) {
        const uint32_t x = g % meansLo.width;
        const uint32_t y = g / meansLo.width;
        const uint8_t* lo = meansLo.At(x, y);
        const uint8_t* hi = meansHi.At(x, y);

        GaussianSplatGPU& dst = out.gaussians[g];
        dst = GaussianSplatGPU {};
        float pos[3];
        for (int c = 0; c < 3; c++) {
            const uint32_t q = (uint32_t)lo[c] | ((uint32_t)hi[c] << 8);
            const float v = posMin[c] + posSpan[c] * ((float)q / 65535.0f);
            pos[c] = InvLogTransform(v) * worldScale;
        }
        dst.px = pos[0];
        dst.py = pos[1];
        dst.pz = pos[2];

        const uint32_t sx = g % scaleImg.width;
        const uint32_t sy = g / scaleImg.width;
        const uint8_t* s = scaleImg.At(sx, sy);
        dst.sx = std::exp(scaleCode[s[0]]) * worldScale;
        dst.sy = std::exp(scaleCode[s[1]]) * worldScale;
        dst.sz = std::exp(scaleCode[s[2]]) * worldScale;

        const uint32_t qx = g % quatImg.width;
        const uint32_t qy = g / quatImg.width;
        const uint8_t* q = quatImg.At(qx, qy);
        if (q[3] >= 252) {
            UnpackQuat(q[0], q[1], q[2], q[3], dst.qw, dst.qx, dst.qy, dst.qz);
        } else {
            dst.qw = 1.0f;
            dst.qx = dst.qy = dst.qz = 0.0f;
        }

        const uint32_t cx = g % sh0Img.width;
        const uint32_t cy = g / sh0Img.width;
        const uint8_t* c = sh0Img.At(cx, cy);
        dst.cr = 0.5f + SH_C0 * sh0Table[c[0]];
        dst.cg = 0.5f + SH_C0 * sh0Table[c[1]];
        dst.cb = 0.5f + SH_C0 * sh0Table[c[2]];
        dst.ca = 1.0f;
        dst.opacity = (float)c[3] / 255.0f;

        if (shCoeffs > 0) {
            const uint32_t lx = g % labels.width;
            const uint32_t ly = g / labels.width;
            const uint8_t* l = labels.At(lx, ly);
            const uint32_t label = (uint32_t)l[0] | ((uint32_t)l[1] << 8);
            float* sh = &out.sh[(size_t)g * 48u];
            if (label < paletteCount) {
                const uint32_t row = label / 64u;
                const uint32_t colBase = (label % 64u) * (uint32_t)shCoeffs;
                for (int j = 0; j < shCoeffs; j++) {
                    const uint32_t col = colBase + (uint32_t)j;
                    if (row >= centroidH || col >= centroidW) {
                        continue;
                    }
                    const uint8_t* cen = centroids.At(col, row);
                    sh[j * 3 + 0] = shCode[cen[0]];
                    sh[j * 3 + 1] = shCode[cen[1]];
                    sh[j * 3 + 2] = shCode[cen[2]];
                }
            }
        }

        if (dst.px < out.boundsMin[0]) out.boundsMin[0] = dst.px;
        if (dst.py < out.boundsMin[1]) out.boundsMin[1] = dst.py;
        if (dst.pz < out.boundsMin[2]) out.boundsMin[2] = dst.pz;
        if (dst.px > out.boundsMax[0]) out.boundsMax[0] = dst.px;
        if (dst.py > out.boundsMax[1]) out.boundsMax[1] = dst.py;
        if (dst.pz > out.boundsMax[2]) out.boundsMax[2] = dst.pz;
    }

    out.gaussianCount = count;
    LOGI("SOG loaded: %{public}u gaussians, sh=%{public}d, bounds=(%{public}.2f,%{public}.2f,%{public}.2f)-(%{public}.2f,%{public}.2f,%{public}.2f)",
        out.gaussianCount, shCoeffs, out.boundsMin[0], out.boundsMin[1], out.boundsMin[2], out.boundsMax[0],
        out.boundsMax[1], out.boundsMax[2]);
    return true;
}

} // namespace sog
