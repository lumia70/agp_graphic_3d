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

#ifndef PLUGIN_GS_PARSING_H
#define PLUGIN_GS_PARSING_H

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <numeric>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <thread>
#include <unordered_set>

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

#include "gs_loading.h"
#include "gs_utils.h"

namespace {
#include "app/shaders/common/gs_common.h"
} // namespace

#include <map>

#include "spatial_buffer.h"
#include "spatial_codec_factory.h"
#include "spatial_meta.h"
#include "spatial_tag.h"

namespace GS {

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;

using namespace Spatial;

namespace {
struct AstcHeader {
    uint8_t magic[4]; // Magic number should be { 0x5CA1AB13 }
    uint8_t blockdimX;
    uint8_t blockdimY;
    uint8_t blockdimZ;
    uint8_t xsize[3u];
    uint8_t ysize[3u];
    uint8_t zsize[3u];
};

inline uint32_t ReadUint32(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0u] | (data[1u] << 8u) | (data[2u] << 16u) | (data[3u] << 24u));
}
} // namespace

inline void ProcessCullingData(
    const size_t index, const Math::Vec3 scaleExp, BASE_NS::vector<float>& gaussianMaxScales, bool enableScoreCulling)
{
    if (enableScoreCulling) {
        gaussianMaxScales[index] = Math::max(Math::max(scaleExp.x, scaleExp.y), scaleExp.z);
    }
}

bool CheckGltfAndResetSize(const uint8_t* data, size_t& size)
{
    constexpr size_t glbMagicLen = 12u;

    if (data == nullptr || size < glbMagicLen) {
        return false;
    }

    // "glTF" in little-endian hex
    constexpr uint32_t GLB_MAGIC = 0x46546C67u;
    uint32_t magicNumber = 0;
    BASE_NS::CloneData(&magicNumber, sizeof(uint32_t), data, sizeof(uint32_t));
    if (magicNumber != GLB_MAGIC) {
        return false;
    }

    // check version bytes 4-8
    uint32_t version = 0;
    BASE_NS::CloneData(&version, sizeof(uint32_t), data + 4u, sizeof(uint32_t));
    // only accept glTF 2.0 for now
    const uint32_t gltfVersion = 2u;
    if (version != gltfVersion) {
        return false;
    }

    // check size bytes 8-12
    uint32_t glbSize = 0u;
    BASE_NS::CloneData(&glbSize, sizeof(uint32_t), data + 8u, sizeof(uint32_t));
    if (glbSize > size) {
        return false;
    }
    // the fileByteLen may be larger than glbSize, and we only care about the glb part
    size = glbSize;

    // valid glb data
    return true;
}

const json::value ExtractGlbJson(
    const uint8_t* gltfData, const size_t gltfDataLen, uint32_t& jsonChunkLength, BASE_NS::vector<char>& jsonBuffer)
{
    if (gltfData == nullptr || gltfDataLen < 20u || ReadUint32(gltfData) != 0x46546C67u) {
        CORE_LOG_E("ExtractGlbJson: Invalid glb!");
        return nullptr;
    }

    // validate json chunk
    jsonChunkLength = ReadUint32(gltfData + 12u);
    if (ReadUint32(gltfData + 16u) != 0x4E4F534Au || gltfDataLen - 20u < jsonChunkLength) {
        CORE_LOG_E("ExtractGlbJson: Invalid json!");
        return nullptr;
    }

    jsonBuffer.resize(jsonChunkLength + 1);
    BASE_NS::CloneData(jsonBuffer.data(), jsonChunkLength, gltfData + 20u, jsonChunkLength);
    jsonBuffer[jsonChunkLength] = '\0';

    return json::parse(jsonBuffer.data());
}

const json::value* ExtractGlbExtensions(const json::value& gltfJson)
{
    if (!gltfJson) {
        CORE_LOG_E("ExtractGlbExtensions: Failed to parse json!");
        return nullptr;
    }

    // extract gs extensions
    const json::value* meshes = gltfJson.find("meshes");
    if (!meshes || !meshes->is_array() || meshes->array_.empty()) {
        CORE_LOG_E("ExtractGlbExtensions: meshes not found!");
        return nullptr;
    }

    const json::value* primitives = meshes->array_[0].find("primitives");
    if (!primitives || !primitives->is_array() || primitives->array_.empty()) {
        CORE_LOG_E("ExtractGlbExtensions: primitives not found!");
        return nullptr;
    }

    const json::value* extensions = primitives->array_[0].find("extensions");
    if (!extensions || !extensions->is_object()) {
        CORE_LOG_E("ExtractGlbExtensions: extensions not found!");
        return nullptr;
    }

    return BASE_NS::move(extensions);
}

BASE_NS::pair<bool, BASE_NS::string> GetGltfExtensions(const uint8_t* gltfData, const size_t gltfDataLen)
{
    BASE_NS::string extensionName = "";
    uint32_t jsonChunkLength = 0u;
    BASE_NS::vector<char> jsonBuffer;
    const json::value gltfJson = ExtractGlbJson(gltfData, gltfDataLen, jsonChunkLength, jsonBuffer);
    const json::value* extensions = ExtractGlbExtensions(gltfJson);
    if (!extensions) {
        return { false, extensionName };
    }

    // glb v1 (with ply data in a single BufferView)
    const json::value* gaussianSplatting = extensions->find("UWA_gaussian_splatting");
    if (gaussianSplatting && gaussianSplatting->is_object()) {
        extensionName = "UWA_gaussian_splatting";
        return { true, extensionName };
    }

    // glb v3 (with compressed data in a single BufferView)
    const json::value* primitiveCompression = extensions->find("UWA_primitive_3DGS_compression");
    if (primitiveCompression && primitiveCompression->is_object()) {
        extensionName = "UWA_primitive_3DGS_compression";
        return { true, extensionName };
    }

    // glb v4 (with 1.0 compressed data in a single BufferView)
    const json::value* primitiveCompressionV1 = extensions->find("UWA_gaussian_splatting_compression_EGSC");
    if (primitiveCompressionV1 && primitiveCompressionV1->is_object()) {
        extensionName = "UWA_gaussian_splatting_compression_EGSC";
        return { true, extensionName };
    }

    const json::value* PrimitiveUnCompression = extensions->find("KHR_gaussian_splatting");
    if (PrimitiveUnCompression && PrimitiveUnCompression->is_object()) {
        extensionName = "KHR_gaussian_splatting";
        return { true, extensionName };
    }

    // glb v2 (with properties in different BufferViews)
    return { true, extensionName };
}

ByteArray ExtractGltfPlyData(const uint8_t* gltfData, const size_t gltfDataLen)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "ExtractGltfPlyData", "", 0xFF00FF);

    uint32_t jsonChunkLength = 0u;
    BASE_NS::vector<char> jsonBuffer;
    const json::value gltfJson = ExtractGlbJson(gltfData, gltfDataLen, jsonChunkLength, jsonBuffer);
    const json::value* extensions = ExtractGlbExtensions(gltfJson);
    if (!extensions) {
        return ByteArray(0);
    }

    // pad to 4-bytes
    const size_t binChunkStart = static_cast<size_t>((20u + jsonChunkLength + 3u) & ~3u);

    // locate and validate binary chunk
    if (gltfDataLen < binChunkStart + 8u || ReadUint32(gltfData + binChunkStart + 4u) != 0x004E4942u ||
        gltfDataLen < binChunkStart + 8u + ReadUint32(gltfData + binChunkStart)) {
        CORE_LOG_E("ExtractGltfPlyData: Invalid bin chunk!");
        return ByteArray(0);
    }

    const char* binData = reinterpret_cast<const char*>(gltfData + binChunkStart + 8u);
    const uint32_t binChunkSize = ReadUint32(gltfData + binChunkStart);

    const json::value* gaussianSplatting = extensions->find("UWA_gaussian_splatting");
    if (!gaussianSplatting || !gaussianSplatting->is_object()) {
        CORE_LOG_E("ExtractGltfPlyData: UWA_gaussian_splatting not found!");
        return ByteArray(0);
    }

    const json::value* compressed3DGS = gaussianSplatting->find("compressed3DGS");
    if (!compressed3DGS || !compressed3DGS->is_object()) {
        CORE_LOG_E("ExtractGltfPlyData: compressed3DGS not found!");
        return ByteArray(0);
    }

    const json::value* bufferView = compressed3DGS->find("bufferView");
    if (!bufferView || !bufferView->is_number()) {
        CORE_LOG_E("ExtractGltfPlyData: bufferView not found!");
        return ByteArray(0);
    }

    // get buffer view
    const size_t bufferViewIndex = bufferView->as_number<size_t>();
    const json::value* bufferViews = gltfJson.find("bufferViews");
    if (!bufferViews || !bufferViews->is_array() || bufferViewIndex >= bufferViews->array_.size()) {
        CORE_LOG_E("ExtractGltfPlyData: bufferViews not found!");
        return ByteArray(0);
    }

    // extract data
    const json::value* bufferViewObj = &bufferViews->array_[bufferViewIndex];
    const json::value* byteLength = bufferViewObj->find("byteLength");
    if (!byteLength || !byteLength->is_number()) {
        CORE_LOG_E("ExtractGltfPlyData: byteLength not found!");
        return ByteArray(0);
    }

    const size_t size = byteLength->as_number<size_t>();
    const json::value* byteOffset = bufferViewObj->find("byteOffset");
    const size_t offset = (byteOffset && byteOffset->is_number()) ? byteOffset->as_number<size_t>() : 0;
    if (offset >= binChunkSize || size > binChunkSize - offset) {
        CORE_LOG_E("ExtractGltfPlyData: Invalid offset or size!");
        return ByteArray(0);
    }

    ByteArray plyData(size);
    BASE_NS::CloneData(&plyData.GetData()[0], size, binData + offset, size);

    return plyData;
}

ByteArray ExtractGltfCompressedData(const uint8_t* gltfData, const size_t gltfDataLen)
{
    uint32_t jsonChunkLength = 0u;
    BASE_NS::vector<char> jsonBuffer;
    const json::value gltfJson = ExtractGlbJson(gltfData, gltfDataLen, jsonChunkLength, jsonBuffer);
    const json::value* extensions = ExtractGlbExtensions(gltfJson);
    if (!extensions) {
        return ByteArray(0);
    }

    // pad to 4-bytes
    const size_t binChunkStart = (20u + static_cast<size_t>(jsonChunkLength) + 3u) & ~static_cast<size_t>(3u);

    // locate and validate binary chunk
    if (gltfDataLen < binChunkStart + 8u || ReadUint32(gltfData + binChunkStart + 4u) != 0x004E4942u ||
        gltfDataLen - binChunkStart - 8u < ReadUint32(gltfData + binChunkStart)) {
        CORE_LOG_E("ExtractGltfCompressedData: Invalid bin chunk!");
        return ByteArray(0);
    }

    const char* binData = reinterpret_cast<const char*>(gltfData + binChunkStart + 8u);
    const uint32_t binChunkSize = ReadUint32(gltfData + binChunkStart);

    const json::value* gaussianSplatting = extensions->find("UWA_primitive_3DGS_compression");
    if (!gaussianSplatting || !gaussianSplatting->is_object()) {
        CORE_LOG_E("ExtractGltfCompressedData: UWA_primitive_3DGS_compression not found!");
        return ByteArray(0);
    }

    const json::value* bufferView = gaussianSplatting->find("bufferView");
    if (!bufferView || !bufferView->is_number()) {
        CORE_LOG_E("ExtractGltfCompressedData: bufferView not found!");
        return ByteArray(0);
    }

    // get buffer view
    const size_t bufferViewIndex = bufferView->as_number<size_t>();
    const json::value* bufferViews = gltfJson.find("bufferViews");
    if (!bufferViews || !bufferViews->is_array() || bufferViewIndex >= bufferViews->array_.size()) {
        CORE_LOG_E("ExtractGltfCompressedData: bufferViews not found!");
        return ByteArray(0);
    }

    // extract data
    const json::value* bufferViewObj = &bufferViews->array_[bufferViewIndex];
    const json::value* byteLength = bufferViewObj->find("byteLength");
    if (!byteLength || !byteLength->is_number()) {
        CORE_LOG_E("ExtractGltfCompressedData: byteLength not found!");
        return ByteArray(0);
    }

    const size_t size = byteLength->as_number<size_t>();
    const json::value* byteOffset = bufferViewObj->find("byteOffset");
    const size_t offset = (byteOffset && byteOffset->is_number()) ? byteOffset->as_number<size_t>() : 0;
    if (offset >= binChunkSize || size > binChunkSize - offset) {
        CORE_LOG_E("ExtractGltfCompressedData: Invalid offset or size!");
        return ByteArray(0);
    }

    ByteArray compressedData(size);
    BASE_NS::CloneData(&compressedData.GetData()[0], size, binData + offset, size);

    return compressedData;
}

bool CheckValidFileExtensions(const BASE_NS::string& path)
{
    const size_t dotPos = path.find_last_of('.');
    // no '.' or it's the last character
    if (dotPos == BASE_NS::string::npos || dotPos == path.length() - 1) {
        return false;
    }

    const BASE_NS::string extension = BASE_NS::string(path.substr(dotPos)).toLower();
    return extension == ".ply" || extension == ".glb" || extension == ".splat" || extension == ".mp4" ||
           extension == ".sog";
}

inline bool IsSogExtension(const BASE_NS::string& path)
{
    const size_t dotPos = path.find_last_of('.');
    if (dotPos == BASE_NS::string::npos || dotPos == path.length() - 1) {
        return false;
    }
    const BASE_NS::string extension = BASE_NS::string(path.substr(dotPos)).toLower();
    return extension == ".sog";
}

void ProcessHeaderLine(const BASE_NS::string& line, PlyHeader& header, bool& headerEnd)
{
    size_t pos = 0;
    size_t nextSpace = line.find(' ', pos);
    BASE_NS::string_view token = line.substr(pos, nextSpace - pos);

    if (token == "ply") {
        // format indicator
    } else if (token == "format") {
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        BASE_NS::string_view format = line.substr(pos, nextSpace - pos);
        header.format = BASE_NS::string(&format[0], format.length());
    } else if (token == "element") {
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        token = line.substr(pos, nextSpace - pos);

        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        BASE_NS::string_view countStr = line.substr(pos, nextSpace - pos);

        if (token == "vertex") {
            header.numVertices = static_cast<size_t>(std::atoi(countStr.data()));
        } else if (token == "face") {
            header.numFaces = static_cast<size_t>(std::atoi(countStr.data()));
        }
    } else if (token == "property") {
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        BASE_NS::string_view type = line.substr(pos, nextSpace - pos);

        pos = nextSpace + 1;
        BASE_NS::string_view name = line.substr(pos);

        PlyProperty prop;
        prop.type = BASE_NS::string(&type[0], type.length());
        prop.name = BASE_NS::string(&name[0], name.length());

        if (header.vertexProperties.size() < static_cast<size_t>(header.numVertices)) {
            header.vertexProperties.push_back(prop);
        } else {
            header.faceProperties.push_back(prop);
        }
    } else if (token == "end_header") {
        headerEnd = true;
    }
}

PlyHeader LoadPLYHeader(void* fileData, const size_t byteLen, size_t& headerEndIdx)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "GsSystem::LoadPLYHeader", "", 0xFF00FF);

    const char* data = static_cast<const char*>(fileData);
    size_t currentIndex = 0;

    BASE_NS::string line;
    bool headerEnd = false;
    PlyHeader header;

    while (currentIndex < byteLen) {
        char currentChar = data[currentIndex];
        if (currentChar == '\n' || currentChar == '\r') {
            if (!line.empty()) {
                ProcessHeaderLine(line, header, headerEnd);
                line.clear();
            }

            if (headerEnd) {
                currentIndex++;
                break;
            }
            if (currentChar == '\r' && currentIndex + 1 < byteLen && data[currentIndex + 1] == '\n') {
                currentIndex++;
            }
        } else {
            line += currentChar;
        }

        currentIndex++;
    }

    headerEndIdx = currentIndex;

    if (!headerEnd) {
        CORE_LOG_E("GS3D ply header not end, invalidating GS model.");
        header = {};
    }

    return header;
}

void ProcessHeaderLineCompressed(const BASE_NS::string& line, PlyHeader& header, bool& headerEnd)
{
    size_t pos = 0;
    size_t nextSpace = line.find(' ', pos);
    BASE_NS::string_view token = line.substr(pos, nextSpace - pos);

    if (token == "ply") {
        // format indicator
    } else if (token == "format") {
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        BASE_NS::string_view format = line.substr(pos, nextSpace - pos);
        header.format = BASE_NS::string(&format[0], format.length());
    } else if (token == "element") {
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        token = line.substr(pos, nextSpace - pos);
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        BASE_NS::string_view countStr = line.substr(pos, nextSpace - pos);

        if (token == "vertex") {
            header.numVertices = static_cast<size_t>(std::atoi(countStr.data()));
        } else if (token == "face") {
            header.numFaces = static_cast<size_t>(std::atoi(countStr.data()));
        } else if (token == "chunk") {
            header.numChunks = static_cast<size_t>(std::atoi(countStr.data()));
        } else if (token == "feature_rest") {
            header.numShCodebook = static_cast<size_t>(std::atoi(countStr.data()));
        } else if (token == "skybox") {
            header.numSkyBox = static_cast<size_t>(std::atoi(countStr.data()));
        }
    } else if (token == "property") {
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        BASE_NS::string_view type = line.substr(pos, nextSpace - pos);
        pos = nextSpace + 1;
        BASE_NS::string_view name = line.substr(pos);
        PlyProperty prop;
        prop.type = BASE_NS::string(&type[0], type.length());
        prop.name = BASE_NS::string(&name[0], name.length());

        if (prop.name.rfind("min_", 0) == 0 || prop.name.rfind("max_", 0) == 0) {
            header.chunkProperties.push_back(prop);
        } else if (prop.name.rfind("packed", 0) == 0) {
            header.vertexProperties.push_back(prop);
        } else if (prop.name.rfind("feature_rest_indices", 0) == 0) {
            header.shIndicesProperties.push_back(prop);
        } else if (prop.name.rfind("f_rest_", 0) == 0) {
            header.shCodeBookProperties.push_back(prop);
            header.numFeatures++;
        } else if (prop.name.rfind("f_dc_", 0) == 0) {
            header.numFeatures++;
        }
    } else if (token == "declare") {
        header.compress = 0.1f;
        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        token = line.substr(pos, nextSpace - pos);

        pos = nextSpace + 1;
        nextSpace = line.find(' ', pos);
        const BASE_NS::string_view value = line.substr(pos, nextSpace - pos);
        if (token == "compress") {
            header.compress = static_cast<float>(std::atof(value.data()));
        } else if (token == "feature_rest_zero_point") {
            header.featureRestZeroPoint = static_cast<float>(std::atof(value.data()));
        } else if (token == "feature_rest_scale") {
            header.featureRestScale = static_cast<float>(std::atof(value.data()));
        } else if (token == "sh_degree") {
            header.shDegree = static_cast<size_t>(std::atoi(value.data()));
        } else if (token == "feature_dc_min") {
            header.featureDcMin = static_cast<float>(std::atof(value.data()));
        } else if (token == "feature_dc_max") {
            header.featureDcMax = static_cast<float>(std::atof(value.data()));
        }
    } else if (token == "end_header") {
        headerEnd = true;
    }
}

PlyHeader LoadPLYHeaderCompressed(void* fileData, const size_t byteLen, size_t& headerEndIdx)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "GsSystem::LoadPLYHeaderCompressed", "", 0xFF00FF);

    const char* data = static_cast<const char*>(fileData);
    size_t currentIndex = 0;

    BASE_NS::string line;
    bool headerEnd = false;
    PlyHeader header;

    while (currentIndex < byteLen) {
        char currentChar = data[currentIndex];
        if (currentChar == '\n' || currentChar == '\r') {
            if (!line.empty()) {
                ProcessHeaderLineCompressed(line, header, headerEnd);
                line.clear();
            }

            if (headerEnd) {
                currentIndex++;
                break;
            }

            if (currentChar == '\r' && currentIndex + 1 < byteLen && data[currentIndex + 1] == '\n') {
                currentIndex++;
            }
        } else {
            line += currentChar;
        }

        currentIndex++;
    }

    headerEndIdx = currentIndex;

    if (!headerEnd) {
        CORE_LOG_E("GS3D ply header not end, invalidating GS model.");
        header = {};
    }

    return header;
}

void PrecomputeCovariances(const BASE_NS::Math::Vec3 scale, const float mod, const BASE_NS::Math::Vec4 rot,
    BASE_NS::Math::Vec4& covA, BASE_NS::Math::Vec4& covB)
{
    Math::Mat3X3 S(1.0);
    S[0u][0u] = mod * scale.x;
    S[1u][1u] = mod * scale.y;
    S[2u][2u] = mod * scale.z;

    // quat to rotmat
    const float rmat[9] = {
        1.0f - 2.0f * (rot[2u] * rot[2u] + rot[3u] * rot[3u]), // r00
        2.0f * (rot[1u] * rot[2u] + rot[0u] * rot[3u]),        // r10
        2.0f * (rot[1u] * rot[3u] - rot[0u] * rot[2u]),        // r20

        2.0f * (rot[1u] * rot[2u] - rot[0u] * rot[3u]),        // r01
        1.0f - 2.0f * (rot[1u] * rot[1u] + rot[3u] * rot[3u]), // r11
        2.0f * (rot[2u] * rot[3u] + rot[0u] * rot[1u]),        // r21

        2.0f * (rot[1u] * rot[3u] + rot[0u] * rot[2u]),       // r02
        2.0f * (rot[2u] * rot[3u] - rot[0u] * rot[1u]),       // r12
        1.0f - 2.0f * (rot[1u] * rot[1u] + rot[2u] * rot[2u]) // r22
    };

    const Math::Mat3X3 R(rmat);
    const Math::Mat3X3 M = S * R;

    // 3D covariance matrix sigma
    const float sigma[6u] = { M.data[0u] * M.data[0u] + M.data[3u] * M.data[3u] + M.data[6u] * M.data[6u],
        M.data[0u] * M.data[1u] + M.data[3u] * M.data[4u] + M.data[6u] * M.data[7u],
        M.data[0u] * M.data[2u] + M.data[3u] * M.data[5u] + M.data[6u] * M.data[8u],
        M.data[1u] * M.data[1u] + M.data[4u] * M.data[4u] + M.data[7u] * M.data[7u],
        M.data[1u] * M.data[2u] + M.data[4u] * M.data[5u] + M.data[7u] * M.data[8u],
        M.data[2u] * M.data[2u] + M.data[5u] * M.data[5u] + M.data[8u] * M.data[8u] };

    covA = Math::Vec4(sigma[0u], sigma[1u], sigma[2u], 1.0f); // .w contains a scale modifier
    covB = Math::Vec4(sigma[3u], sigma[4u], sigma[5u], 0.0);  // .w will contain the packed color
}

void ParseGSFullHarmonics(const size_t baseOffset, const size_t ii, const size_t headerEndIdx, uint8_t* plyFileData,
    GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling, GS::GsPrimitive& primitive,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    PlyVertexDataFullHarmonics vertexData;
    BASE_NS::CloneData(static_cast<void*>(&vertexData), sizeof(PlyVertexDataFullHarmonics),
        static_cast<void*>(plyFileData + (headerEndIdx + (ii - baseOffset) * sizeof(PlyVertexDataFullHarmonics))),
        sizeof(PlyVertexDataFullHarmonics));

    const Math::Vec3 scaleExp =
        Math::Vec3(std::exp(vertexData.scale[0u]), std::exp(vertexData.scale[1u]), std::exp(vertexData.scale[2]));
    const float opacity = 1.0f / (1.0f + std::exp(-vertexData.opacity)); // sigmoid

    // normalize the quaternion rotation
    Math::Vec4 rotNorm = Math::Vec4(vertexData.rotation);
    {
        float len = 0;
        for (size_t jj = 0; jj < 4u; jj++) {
            len += rotNorm[jj] * rotNorm[jj];
        }
        if (len > Math::EPSILON) {
            rotNorm /= std::sqrtf(len);
        } else {
            rotNorm = Math::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        }
    }

    GsObjectLoadData::GsSplatData data;

    aabbMin = Math::min(aabbMin, Math::Vec3(vertexData.position));
    aabbMax = Math::max(aabbMax, Math::Vec3(vertexData.position));

    // precompute the covariances since we are taking only the 0th degree spherical harmonics anyways
    PrecomputeCovariances(scaleExp, 1.0f, rotNorm, data.covAScale, data.covBColorPacked);

    // Only take the 0th degree of the spherical harmonics. This essentially removes all view-dependentant
    // lighting (e.g. reflections), but is way faster since we can recompute everything. Otherwise we would
    // need to pass all 16x vec3 harmonics to the GPU and compute the new color per-frame.
    const Math::Vec3 color = Math::Vec3(vertexData.shs) * SH_C0 + Math::Vec3(0.5f, 0.5f, 0.5f);
    uint32_t colorPacked = PackGSColor(color, opacity);

    // Transpose and load the spherical harmonics
    if (primitive.RequireSH()) {
        float shDataTransposed[GS_NUM_HARMONICS * 3u];

        for (size_t jj = 0; jj < 3u; jj++) {
            shDataTransposed[jj] = vertexData.shs[jj];
        }

        for (size_t jj = 1u; jj < GS_NUM_HARMONICS; jj++) {
            shDataTransposed[jj * 3u + 0u] = vertexData.shs[(jj - 1u) + 3u];
            shDataTransposed[jj * 3u + 1u] = vertexData.shs[(jj - 1u) + GS_NUM_HARMONICS + 2u];
            shDataTransposed[jj * 3u + 2u] = vertexData.shs[(jj - 1u) + GS_NUM_HARMONICS * 2u + 1u];
        }

        GS::GsHarmonicsData hsData;
        size_t shIdx = 0;
        for (size_t jj = 0; jj < GS_HARMONICS_PACKED_SIZE; jj++) {
            hsData.shCoefficients[jj] = PackHalf2x16(shDataTransposed[shIdx], shDataTransposed[shIdx + 1]);
            shIdx += 2u;
        }
        loadData->sphericalHarmonics[ii] = hsData;
    }

    data.covBColorPacked.w = BitCast<float>(colorPacked);
    data.positionOpacity = Math::Vec4(Math::Vec3(vertexData.position), opacity);
    loadData->gaussianPositions[ii] = data.positionOpacity;
    loadData->gsVerticesData.Set(ii, BASE_NS::move(data));
    ProcessCullingData(ii, scaleExp, loadData->gaussianMaxScales, enableScoreCulling);

    // sanity check, if everything was parsed correctly then these should all be zeros
    if (vertexData.normal[0u] != 0.0f || vertexData.normal[1u] != 0.0f || vertexData.normal[2u] != 0.0) {
        CORE_LOG_E("GS header format is invalid: non-zero normals!");
        primitive.isValid = false;
    }
}

void ParseGSRestrictedHarmonics(const size_t baseOffset, const size_t ii, const size_t headerEndIdx,
    uint8_t* plyFileData, GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling, GS::GsPrimitive& primitive,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    PlyVertexDataRestrictedHarmonics vertexData;
    BASE_NS::CloneData(static_cast<void*>(&vertexData), sizeof(PlyVertexDataRestrictedHarmonics),
        static_cast<void*>(plyFileData + (headerEndIdx + (ii - baseOffset) * sizeof(PlyVertexDataRestrictedHarmonics))),
        sizeof(PlyVertexDataRestrictedHarmonics));

    const Math::Vec3 scaleExp =
        Math::Vec3(std::exp(vertexData.scale[0]), std::exp(vertexData.scale[1]), std::exp(vertexData.scale[2]));
    const float opacity = 1.0f / (1.0f + std::exp(-vertexData.opacity)); // sigmoid

    // normalize the quaternion rotation
    Math::Vec4 rotNorm = Math::Vec4(vertexData.rotation);
    {
        float len = 0;
        for (size_t jj = 0; jj < 4u; jj++) {
            len += rotNorm[jj] * rotNorm[jj];
        }
        if (len > Math::EPSILON) {
            rotNorm /= std::sqrtf(len);
        } else {
            rotNorm = Math::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        }
    }

    GsObjectLoadData::GsSplatData data;

    aabbMin = Math::min(aabbMin, Math::Vec3(vertexData.position));
    aabbMax = Math::max(aabbMax, Math::Vec3(vertexData.position));

    // precompute the covariances since we are taking only the 0th degree spherical harmonics anyways
    PrecomputeCovariances(scaleExp, 1.0f, rotNorm, data.covAScale, data.covBColorPacked);

    // Only take the 0th degree of the spherical harmonics. This essentially removes all view-dependentant
    // lighting (e.g. reflections), but is way faster since we can recompute everything. Otherwise we would
    // need to pass all 16x vec3 harmonics to the GPU and compute the new color per-frame.
    const Math::Vec3 color = Math::Vec3(vertexData.shs) * SH_C0 + Math::Vec3(0.5f, 0.5f, 0.5f);
    uint32_t colorPacked = PackGSColor(color, opacity);

    data.covBColorPacked.w = BitCast<float>(colorPacked);
    data.positionOpacity = Math::Vec4(Math::Vec3(vertexData.position), opacity);
    loadData->gaussianPositions[ii] = data.positionOpacity;
    loadData->gsVerticesData.Set(ii, BASE_NS::move(data));
    ProcessCullingData(ii, scaleExp, loadData->gaussianMaxScales, enableScoreCulling);

    // sanity check, if everything was parsed correctly then these should all be zeros
    if (vertexData.normal[0u] != 0.0f || vertexData.normal[1u] != 0.0f || vertexData.normal[2u] != 0.0) {
        CORE_LOG_E("GS header format is invalid: non-zero normals!");
        primitive.isValid = false;
    }
}

template <typename VertexDataT>
void ParseGSRestrictedHarmonicsMinimal(const size_t baseOffset, const size_t ii, const size_t headerEndIdx,
    uint8_t* plyFileData, GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling, GS::GsPrimitive& primitive,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    VertexDataT vertexData;
    BASE_NS::CloneData(static_cast<void*>(&vertexData), sizeof(VertexDataT),
        static_cast<void*>(plyFileData + (headerEndIdx + (ii - baseOffset) * sizeof(VertexDataT))),
        sizeof(VertexDataT));

    const Math::Vec3 scaleExp =
        Math::Vec3(std::exp(vertexData.scale[0]), std::exp(vertexData.scale[1]), std::exp(vertexData.scale[2]));
    const float opacity = 1.0f / (1.0f + std::exp(-vertexData.opacity)); // sigmoid

    // normalize the quaternion rotation
    Math::Vec4 rotNorm = Math::Vec4(vertexData.rotation);
    {
        float len = 0;
        for (size_t jj = 0; jj < 4u; jj++) {
            len += rotNorm[jj] * rotNorm[jj];
        }
        if (len > Math::EPSILON) {
            rotNorm /= std::sqrtf(len);
        } else {
            rotNorm = Math::Vec4(1.0f, 0.0f, 0.0f, 0.0f);
        }
    }

    GsObjectLoadData::GsSplatData data;

    aabbMin = Math::min(aabbMin, Math::Vec3(vertexData.position));
    aabbMax = Math::max(aabbMax, Math::Vec3(vertexData.position));

    // precompute the covariances since we are taking only the 0th degree spherical harmonics anyways
    PrecomputeCovariances(scaleExp, 1.0f, rotNorm, data.covAScale, data.covBColorPacked);

    // Only take the 0th degree of the spherical harmonics. This essentially removes all view-dependentant
    // lighting (e.g. reflections), but is way faster since we can recompute everything. Otherwise we would
    // need to pass all 16x vec3 harmonics to the GPU and compute the new color per-frame.
    const Math::Vec3 color = Math::Vec3(vertexData.shs) * SH_C0 + Math::Vec3(0.5f, 0.5f, 0.5f);
    uint32_t colorPacked = PackGSColor(color, opacity);

    data.covBColorPacked.w = BitCast<float>(colorPacked);
    data.positionOpacity = Math::Vec4(Math::Vec3(vertexData.position), opacity);
    loadData->gaussianPositions[ii] = data.positionOpacity;
    loadData->gsVerticesData.Set(ii, BASE_NS::move(data));
    ProcessCullingData(ii, scaleExp, loadData->gaussianMaxScales, enableScoreCulling);
}

void ParseGSVertex(const PlyFormatType formatType, const size_t ii, const size_t headerEndIdx,
    uint8_t* plyFileData, GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling, GS::GsPrimitive& primitive,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    switch (formatType) {
        case PlyFormatType::FullHarmonics:
            ParseGSFullHarmonics(0, ii, headerEndIdx, plyFileData, loadData, enableScoreCulling, primitive, aabbMin,
                aabbMax);
            break;
        case PlyFormatType::RestrictedHarmonicsMinimal:
            ParseGSRestrictedHarmonicsMinimal<PlyVertexDataRestrictedHarmonicsMinimal>(
                0, ii, headerEndIdx, plyFileData, loadData, enableScoreCulling, primitive, aabbMin, aabbMax);
            break;
        case PlyFormatType::RestrictedHarmonicsMinimalV2:
            ParseGSRestrictedHarmonicsMinimal<PlyVertexDataRestrictedHarmonicsMinimalV2>(
                0, ii, headerEndIdx, plyFileData, loadData, enableScoreCulling, primitive, aabbMin, aabbMax);
            break;
        case PlyFormatType::RestrictedHarmonicsMinimalV3:
            ParseGSRestrictedHarmonicsMinimal<PlyVertexDataRestrictedHarmonicsMinimalV3>(
                0, ii, headerEndIdx, plyFileData, loadData, enableScoreCulling, primitive, aabbMin, aabbMax);
            break;
        case PlyFormatType::RestrictedHarmonics:
            ParseGSRestrictedHarmonics(0, ii, headerEndIdx, plyFileData, loadData, enableScoreCulling,
                primitive, aabbMin, aabbMax);
            break;
        default:
            break;
    }
}

inline Math::Vec3 Unpack111011(const uint32_t& bits)
{
    return Math::Vec3(static_cast<float>(bits >> 21u) / 2047.0f, static_cast<float>((bits >> 11u) & 0x3FFu) / 1023.0f,
        static_cast<float>(bits & 0x7FFu) / 2047.0f);
}

inline Math::Vec4 Unpack8888(const uint32_t& bits)
{
    return Math::Vec4(static_cast<float>(bits >> 24) / 255.0f, static_cast<float>((bits >> 16) & 0xFFu) / 255.0f,
        static_cast<float>((bits >> 8) & 0xFFu) / 255.0f, static_cast<float>(bits & 0xFFu) / 255.0f);
}

inline Math::Vec3 UnpackRotationComponents(uint32_t bits)
{
    const float norm = 1.0f / (Math::sqrt(2.0f) * 0.5f);
    const float a = (static_cast<float>((bits >> 20) & 0x3FFu) / 1023.0f - 0.5f) * norm;
    const float b = (static_cast<float>((bits >> 10) & 0x3FFu) / 1023.0f - 0.5f) * norm;
    const float c = (static_cast<float>(bits & 0x3FFu) / 1023.0f - 0.5f) * norm;
    return Math::Vec3(a, b, c);
}

inline Math::Vec4 UnpackRotation(uint32_t bits)
{
    const Math::Vec3 abc = UnpackRotationComponents(bits);
    const float m = Math::sqrt(1.0f - (abc.x * abc.x + abc.y * abc.y + abc.z * abc.z));
    const uint32_t mode = bits >> 30u;

    switch (mode) {
        case 0u:
            return Math::Vec4(m, abc.x, abc.y, abc.z);
        case 1u:
            return Math::Vec4(abc.x, m, abc.y, abc.z);
        case 2u:
            return Math::Vec4(abc.x, abc.y, m, abc.z);
        default:
            return Math::Vec4(abc.x, abc.y, abc.z, m);
    }
}

void ReadGSDataCompressedOld(const PlyHeader& gsHeader, const size_t headerEndIdx, uint8_t* plyFileData,
    BASE_NS::vector<GsChunkData>& chunks, BASE_NS::vector<GsPackedData>& packedData,
    BASE_NS::vector<uint32_t>& shIndices, BASE_NS::vector<int8_t>& shs)
{
    size_t ptr = headerEndIdx;

    // Read chunks
    chunks.resize(gsHeader.numChunks);
    BASE_NS::CloneData(static_cast<void*>(chunks.data()), sizeof(GsChunkData) * gsHeader.numChunks,
        static_cast<void*>(plyFileData + ptr), sizeof(GsChunkData) * gsHeader.numChunks);
    ptr += sizeof(GsChunkData) * gsHeader.numChunks;

    // Read packed data
    packedData.resize(gsHeader.numVertices);
    BASE_NS::CloneData(static_cast<void*>(packedData.data()), sizeof(GsPackedData) * gsHeader.numVertices,
        static_cast<void*>(plyFileData + ptr), sizeof(GsPackedData) * gsHeader.numVertices);
    ptr += sizeof(GsPackedData) * gsHeader.numVertices;

    // Read SH indices
    if (gsHeader.shIndicesProperties.size() > 0) {
        shIndices.resize(gsHeader.numVertices);
        BASE_NS::CloneData(static_cast<void*>(shIndices.data()), sizeof(uint32_t) * gsHeader.numVertices,
            static_cast<void*>(plyFileData + ptr), sizeof(uint32_t) * gsHeader.numVertices);
        ptr += sizeof(uint32_t) * gsHeader.numVertices;
    }

    // Read SHs
    const size_t numShs = (gsHeader.shDegree + 1u) * (gsHeader.shDegree + 1u) * 3u - 3u;
    if (numShs > 0u) {
        shs.resize(gsHeader.numShCodebook * numShs);
        BASE_NS::CloneData(static_cast<void*>(shs.data()), sizeof(int8_t) * gsHeader.numShCodebook * numShs,
            static_cast<void*>(plyFileData + ptr), sizeof(int8_t) * gsHeader.numShCodebook * numShs);
    }
}

void ProcessVertexDataCompressedOld(const PlyHeader& gsHeader, const GsPackedData& packData, const GsChunkData& chunk,
    const BASE_NS::vector<int8_t>& shs, uint32_t shIndex, const size_t baseOffset, const size_t ii,
    GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling, const GS::GsPrimitive& primitive,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    const Math::Vec4 rotation = UnpackRotation(packData.rotation);
    Math::Vec4 rotNorm(rotation);
    float len = 0.0f;
    for (size_t jj = 0; jj < 4u; jj++) {
        len += rotNorm[jj] * rotNorm[jj];
    }
    rotNorm /= Math::sqrt(len);

    const Math::Vec3 pos = Unpack111011(packData.position);
    const Math::Vec3 position(pos.x * (chunk.maxX - chunk.minX) + chunk.minX,
        pos.y * (chunk.maxY - chunk.minY) + chunk.minY, pos.z * (chunk.maxZ - chunk.minZ) + chunk.minZ);

    GsObjectLoadData::GsSplatData data;

    aabbMin = Math::min(aabbMin, position);
    aabbMax = Math::max(aabbMax, position);

    const Math::Vec3 packScale = Unpack111011(packData.scale);
    const Math::Vec3 scaleExp(std::exp(packScale.x * (chunk.maxScaleX - chunk.minScaleX) + chunk.minScaleX),
        std::exp(packScale.y * (chunk.maxScaleY - chunk.minScaleY) + chunk.minScaleY),
        std::exp(packScale.z * (chunk.maxScaleZ - chunk.minScaleZ) + chunk.minScaleZ));

    PrecomputeCovariances(scaleExp, 1.0f, rotNorm, data.covAScale, data.covBColorPacked);

    const Math::Vec4 dcOpacity = Unpack8888(packData.color);
    const Math::Vec3 sh0 = Math::Vec3(dcOpacity) * (gsHeader.featureDcMax - gsHeader.featureDcMin) +
                           Math::Vec3(gsHeader.featureDcMin, gsHeader.featureDcMin, gsHeader.featureDcMin);
    const Math::Vec3 color = sh0 * SH_C0 + Math::Vec3(0.5f, 0.5f, 0.5f);
    uint32_t colorPacked = PackGSColor(color, dcOpacity.w);

    // Transpose and load the spherical harmonics
    if (gsHeader.shIndicesProperties.size() > 0 && primitive.RequireSH()) {
        const size_t numShs = (gsHeader.shDegree + 1u) * (gsHeader.shDegree + 1u) * 3u - 3u;
        float shDataTransposed[GS_NUM_HARMONICS * 3u];
        shDataTransposed[0u] = sh0.x;
        shDataTransposed[1u] = sh0.y;
        shDataTransposed[2u] = sh0.z;

        if (shIndex >= gsHeader.numShCodebook) {
            return;
        }
        for (size_t jj = 0; jj < GS_NUM_HARMONICS - 1; jj++) {
            for (size_t kk = 0; kk < 3u; kk++) {
                const size_t hsIdx = shIndex * numShs + jj * 3u + kk;
                if (hsIdx >= shs.size()) {
                    return;
                }
                shDataTransposed[(jj + 1u) * 3u + kk] =
                    (static_cast<float>(static_cast<int>(shs[hsIdx])) - gsHeader.featureRestZeroPoint) *
                    gsHeader.featureRestScale;
            }
        }

        GS::GsHarmonicsData hsData;
        for (size_t jj = 0; jj < GS_HARMONICS_PACKED_SIZE; jj++) {
            hsData.shCoefficients[jj] = PackHalf2x16(shDataTransposed[jj * 2u], shDataTransposed[jj * 2u + 1u]);
        }
        loadData->sphericalHarmonics[baseOffset + ii] = hsData;
    }

    data.covBColorPacked.w = BitCast<float>(colorPacked);
    data.positionOpacity = Math::Vec4(position, dcOpacity.w);
    loadData->gaussianPositions[baseOffset + ii] = data.positionOpacity;
    loadData->gsVerticesData.Set(baseOffset + ii, BASE_NS::move(data));
    ProcessCullingData(baseOffset + ii, scaleExp, loadData->gaussianMaxScales, enableScoreCulling);
}

void ParseGSCompressedOld(const PlyHeader& gsHeader, const size_t headerEndIdx, uint8_t* plyFileData,
    GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling, GS::GsPrimitive& primitive, IThreadPool* threadPool)
{
    // Read data
    BASE_NS::vector<GsChunkData> chunks;
    BASE_NS::vector<GsPackedData> packedData;
    BASE_NS::vector<uint32_t> shIndices;
    BASE_NS::vector<int8_t> shs;
    ReadGSDataCompressedOld(gsHeader, headerEndIdx, plyFileData, chunks, packedData, shIndices, shs);

    std::mutex aabbSyncMtx;
    const size_t numThreads = std::max(threadPool->GetNumberOfThreads(), 1u);

    BASE_NS::vector<IThreadPool::IResult::Ptr> results;
    results.reserve(numThreads);

    if (gsHeader.numSkyBox > gsHeader.numVertices) {
        return;
    }
    const size_t chunkSize = (gsHeader.numVertices - gsHeader.numSkyBox) / numThreads;
    if (loadData->gsVerticesData.Size() < primitive.numGs) {
        return;
    }
    const uint32_t baseOffset = static_cast<uint32_t>(loadData->gsVerticesData.Size()) - primitive.numGs;

    for (size_t ii = 0; ii < numThreads; ii++) {
        const size_t startIdx = ii * chunkSize;
        const size_t endIdx =
            (ii == numThreads - 1) ? (gsHeader.numVertices - gsHeader.numSkyBox) : startIdx + chunkSize;

        auto task = CreateFunctionTask([&, startIdx, endIdx]() {
            BASE_NS::Math::Vec3 aabbMin { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max() };
            BASE_NS::Math::Vec3 aabbMax { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest() };

            for (size_t ii = startIdx; ii < endIdx; ii++) {
                const size_t chunkIdx = ii / 256u;
                if (chunkIdx >= chunks.size()) {
                    continue;
                }
                const GsChunkData& chunk = chunks[chunkIdx];
                const GsPackedData& pd = packedData[ii];
                const uint32_t shIndex = shIndices.empty() ? 0 : shIndices[ii];

                ProcessVertexDataCompressedOld(gsHeader, pd, chunk, shs, shIndex, baseOffset, ii, loadData,
                    enableScoreCulling, primitive, aabbMin, aabbMax);
            }

            std::lock_guard<std::mutex> lock(aabbSyncMtx);
            primitive.aabbMin = Math::min(primitive.aabbMin, aabbMin);
            primitive.aabbMax = Math::max(primitive.aabbMax, aabbMax);
        });

        results.push_back(threadPool->Push(BASE_NS::move(task)));
    }

    for (auto& result : results) {
        result->Wait();
    }
}

BASE_NS::Format BlockDimToFormat(uint8_t blockDim)
{
    switch (blockDim) {
        case 4u:
            return Format::BASE_FORMAT_ASTC_4x4_UNORM_BLOCK;
        case 5u:
            return Format::BASE_FORMAT_ASTC_5x5_UNORM_BLOCK;
        case 6u:
            return Format::BASE_FORMAT_ASTC_6x6_UNORM_BLOCK;
        case 8u:
            return Format::BASE_FORMAT_ASTC_8x8_UNORM_BLOCK;
        case 10u:
            return Format::BASE_FORMAT_ASTC_10x10_UNORM_BLOCK;
        case 12u:
            return Format::BASE_FORMAT_ASTC_12x12_UNORM_BLOCK;
        default:
            CORE_LOG_E("BlockDimToFormat: ASTC block format not supported!");
            return Format::BASE_FORMAT_UNDEFINED;
    }
}

static bool ProcessASTCAttributeItem(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive,
    GS::GsRenderData* renderData, RENDER_NS::IGpuResourceManager& gpuResourceMgr, IRenderContext* renderContext,
    size_t ii, GpuImageDesc& descImg)
{
    auto descRaw = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
    if (descRaw == nullptr) {
        CORE_LOG_E("ProcessASTCAttributeItem CreateMeta failed");
        return false;
    }
    std::string names = SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER;
    std::shared_ptr<const ModelAttribute> positionsRaw;
    auto ret = model->GetAttribute(names, *descRaw, positionsRaw);
    if (ret != SpatialErrorCode::SUCCESS) {
        CORE_LOG_E("Failed to fetch astc raw data");
        return false;
    }
    auto [shHigherRawDataPtr, shHigherRawCount] = positionsRaw->GetAs<uint8_t>();
    if (!shHigherRawDataPtr || shHigherRawCount < 16u) {
        CORE_LOG_E("Failed to fetch astc raw data");
        return false;
    }
    AstcHeader astcHeaderAttribute;
    BASE_NS::CloneData(reinterpret_cast<char*>(&astcHeaderAttribute), 16u, shHigherRawDataPtr, 16u);
    const uint32_t width = (static_cast<uint32_t>(astcHeaderAttribute.xsize[2u]) << 16u) |
                      (static_cast<uint32_t>(astcHeaderAttribute.xsize[1]) << 8u) |
                      static_cast<uint32_t>(astcHeaderAttribute.xsize[0]);
    const uint32_t height = (static_cast<uint32_t>(astcHeaderAttribute.ysize[2u]) << 16u) |
                       (static_cast<uint32_t>(astcHeaderAttribute.ysize[1]) << 8u) |
                       static_cast<uint32_t>(astcHeaderAttribute.ysize[0]);
    descImg.width = width;
    descImg.height = height;
    descImg.format = BlockDimToFormat(astcHeaderAttribute.blockdimX);
    if (descImg.format == Format::BASE_FORMAT_UNDEFINED) {
        return false;
    }
    const RENDER_NS::FormatProperties textureProperties =
        renderContext->GetDevice().GetFormatProperties(descImg.format);
    if (textureProperties.bufferFeatures == 0u && textureProperties.bytesPerPixel == 0u &&
        textureProperties.linearTilingFeatures == 0u && textureProperties.optimalTilingFeatures == 0u) {
        CORE_LOG_ONCE_E("AstcPlatformSupport The platform ",
            "does not support BASE_FORMAT_ASTC_UNORM_BLOCK, spherical harmonics are disabled!");
        primitive.platformsSupportsBlockFormat = false;
        primitive.codecVersion = 0u;
        return true;
    }
    if (primitive.textIdOffset + ii < renderData->gsShASTCDataRefs.size()) {
        renderData->gsShASTCDataRefs[primitive.textIdOffset + ii] = gpuResourceMgr.Create(
            descImg, BASE_NS::array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(shHigherRawDataPtr) + 16u,
                         shHigherRawCount * sizeof(uint8_t) - 16u));
        renderData->astcSizes[primitive.textIdOffset + ii] = Math::UVec2(descImg.width, descImg.height);
    } else {
        CORE_LOG_E("CreateASTCImage: Attempted to write out-of-bounds for ASTC storage!");
    }
    return true;
}

bool CreateASTCImage(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive,
    GS::GsRenderData* renderData, RENDER_NS::IGpuResourceManager& gpuResourceMgr, IRenderContext* renderContext)
{
    if (primitive.shDegree == 0 || primitive.codecVersion == 0)
        return true;
    GpuImageDesc descImg;
    descImg.usageFlags = ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT |
                         ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT;
    descImg.imageTiling = ImageTiling::CORE_IMAGE_TILING_OPTIMAL;
    descImg.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    descImg.engineCreationFlags = 0;

    for (size_t ii = 0; ii < primitive.attribute2DTexNum; ii++) {
        if (!ProcessASTCAttributeItem(model, primitive, renderData, gpuResourceMgr, renderContext, ii, descImg)) {
            return false;
        }
    }
    return true;
}
bool CreateASTCImage(const GS_DECODER::CompressedGsData& gsData, size_t shIdx, GS::GsPrimitive& primitive,
    GS::GsRenderData* renderData, RENDER_NS::IGpuResourceManager& gpuResourceMgr, IRenderContext* renderContext)
{
    size_t dataPointer = gsData.uwaHeader.attributeMetas[shIdx].byteOffset;
    GpuImageDesc descImg;
    descImg.usageFlags = ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_SRC_BIT |
                         ImageUsageFlagBits::CORE_IMAGE_USAGE_TRANSFER_DST_BIT |
                         ImageUsageFlagBits::CORE_IMAGE_USAGE_SAMPLED_BIT;
    descImg.imageTiling = ImageTiling::CORE_IMAGE_TILING_OPTIMAL;
    descImg.memoryPropertyFlags = MemoryPropertyFlagBits::CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    descImg.engineCreationFlags = 0;

    for (size_t ii = 0; ii < gsData.uwaHeader.attributeMetas[shIdx].attribute2DTexNum; ii++) {
        BASE_NS::vector<uint8_t> shASTCDataAttribute;
        const size_t currentTextureSize = gsData.uwaHeader.attributeMetas[shIdx].attribute2DTexSizes[ii];
        if (dataPointer + currentTextureSize > gsData.attributeDatas.size()) {
            CORE_LOG_E("CreateASTCImage: ASTC texture size exceeds allocated!");
            return false;
        }
        shASTCDataAttribute.resize(currentTextureSize);
        BASE_NS::CloneData(shASTCDataAttribute.data(), shASTCDataAttribute.size(), &gsData.attributeDatas[dataPointer],
            shASTCDataAttribute.size());

        AstcHeader astcHeaderAttribute;
        BASE_NS::CloneData(reinterpret_cast<char*>(&astcHeaderAttribute), 16u, shASTCDataAttribute.data(), 16u);
        const uint32_t width = (static_cast<uint32_t>(astcHeaderAttribute.xsize[2u]) << 16u) |
                               (static_cast<uint32_t>(astcHeaderAttribute.xsize[1]) << 8u) |
                               static_cast<uint32_t>(astcHeaderAttribute.xsize[0]);
        const uint32_t height = (static_cast<uint32_t>(astcHeaderAttribute.ysize[2u]) << 16u) |
                                (static_cast<uint32_t>(astcHeaderAttribute.ysize[1]) << 8u) |
                                static_cast<uint32_t>(astcHeaderAttribute.ysize[0]);
        descImg.width = width;
        descImg.height = height;
        dataPointer += shASTCDataAttribute.size();
        descImg.format = BlockDimToFormat(astcHeaderAttribute.blockdimX);
        if (descImg.format == Format::BASE_FORMAT_UNDEFINED) {
            return false;
        }

        // make sure the platform supports BASE_FORMAT_ASTC_4x4_UNORM_BLOCK
        const RENDER_NS::FormatProperties textureProperties =
            renderContext->GetDevice().GetFormatProperties(descImg.format);
        if (textureProperties.bufferFeatures == 0u && textureProperties.bytesPerPixel == 0u &&
            textureProperties.linearTilingFeatures == 0u && textureProperties.optimalTilingFeatures == 0u) {
            CORE_LOG_ONCE_E("AstcPlatformSupport",
                "The platform does not support BASE_FORMAT_ASTC_UNORM_BLOCK, spherical harmonics are disabled!");
            primitive.platformsSupportsBlockFormat = false;
            continue;
        }
        if (primitive.textIdOffset + ii < renderData->gsShASTCDataRefs.size()) {
            renderData->gsShASTCDataRefs[primitive.textIdOffset + ii] = gpuResourceMgr.Create(
                descImg, BASE_NS::array_view<uint8_t>((uint8_t*)(shASTCDataAttribute.data() + 16u),
                             shASTCDataAttribute.size() * sizeof(uint8_t) - 16u));

            renderData->astcSizes[primitive.textIdOffset + ii] = Math::UVec2(descImg.width, descImg.height);
        } else {
            CORE_LOG_E("CreateASTCImage: Attempted to write out-of-bounds for ASTC storage!");
        }
    }

    return true;
}

static bool ParseASTCMetaData(const std::shared_ptr<SpatialModel>& model,
    const std::shared_ptr<const SpatialMeta>& attributeMeta, GS::GsPrimitive& primitive)
{
    // NOTE: Tag value matches SpatialDecoderTag::ATTRIBUTE_2D_TEX_NUM
    // in spatial_codec/src/decoder/include/common/spatial_decoder_tag.h
    // Not accessible via SpatialTag because it's a non-UWA-standard internal decoder tag.
    const std::string texNumKey = "attribute2DTexNum";
    const uint32_t texNum = (attributeMeta->GetMetaAs<uint32_t>(texNumKey)).value_or(0);
    const uint32_t codecId = attributeMeta->GetMetaAs<uint32_t>(SpatialTag::PACKING_MAP_TEXTURE_CODEC_ID).value_or(0);
    if (texNum == 0 || codecId == 0u || codecId > 3u) {
        primitive.codecVersion = 0u;
        primitive.attribute2DTexNum = 0u;
        return true;
    }
    std::shared_ptr<const SpatialMeta> shNMeta;
    std::shared_ptr<const SpatialMeta> impMeta;
    auto ret = model->GetMeta(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER, shNMeta);
    if (ret != SpatialErrorCode::SUCCESS) {
        CORE_LOG_E("PARSING SHN META DATA FAILED");
        return false;
    }
    auto ret2 = model->GetMeta(SpatialTag::IMPORTANCE, impMeta);
    if (ret2 == SpatialErrorCode::SUCCESS) {
        const auto impMaxs = impMeta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MAX_VALUE);
        const auto impMins = impMeta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MIN_VALUE);
        if (impMaxs.has_value() && impMaxs.value().size() > 0) {
            primitive.impMax = impMaxs.value()[0];
            CORE_LOG_E("has_value %f", primitive.impMax);
        }
        if (impMins.has_value() && impMins.value().size() > 0) {
            primitive.impMin = impMins.value()[0];
            CORE_LOG_E("has_value %f", primitive.impMin);
        }
    }
    const auto shNMaxs = shNMeta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MAX_VALUE);
    const auto shNMins = shNMeta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MIN_VALUE);
    if (!shNMaxs.has_value() || !shNMins.has_value()) {
        CORE_LOG_E("Missing quantization parameters");
        return false;
    }
    const size_t shNMaxsSize =
        BASE_NS::Math::clamp(shNMaxs.value().size() > 45 ? 15 : shNMaxs.value().size() / 3, size_t(0), size_t(16));
    for (size_t ii = 0u; ii < shNMaxsSize;
         ii++) { // <- NOTE: could be a conflict; ii < shNMaxsSize and above seems to produce 15?
        primitive.shRestMins_R[ii] = shNMins.value()[3 * ii];
        primitive.shRestMins_G[ii] = shNMins.value()[3 * ii + 1];
        primitive.shRestMins_B[ii] = shNMins.value()[3 * ii + 2];
        primitive.shRestMaxs_R[ii] = shNMaxs.value()[3 * ii];
        primitive.shRestMaxs_G[ii] = shNMaxs.value()[3 * ii + 1];
        primitive.shRestMaxs_B[ii] = shNMaxs.value()[3 * ii + 2];
    }

    primitive.compressedShWidth = attributeMeta->GetMetaAs<uint32_t>(SpatialTag::PACKING_MAP_WIDTH).value_or(0);
    primitive.compressedShHeight = attributeMeta->GetMetaAs<uint32_t>(SpatialTag::PACKING_MAP_HEIGHT).value_or(0);
    primitive.attribute2DSingleWidth = (attributeMeta->GetMetaAs<uint32_t>(SpatialTag::REGION_WIDTH)).value_or(0);
    primitive.attribute2DSingleHeight = (attributeMeta->GetMetaAs<uint32_t>(SpatialTag::REGION_HEIGHT)).value_or(0);
    primitive.attribute2DTexNum = texNum;

    if (primitive.attribute2DSingleWidth > primitive.compressedShWidth) {
        CORE_LOG_E("ParseASTCData: ASTC texture information error!");
        return false;
    }
    if (primitive.textIdOffset + primitive.attribute2DTexNum > GS_MAX_NUM_GS_TEXTURES) {
        CORE_LOG_E("ParseASTCData: Too many ASTC textures requested!");
        primitive.codecVersion = 0u;
        primitive.attribute2DTexNum = 0u;
    }
    return true;
}

bool ParseASTCData(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive)
{
    if (primitive.shDegree == 0u || primitive.codecVersion == 0u)
        return true;
    std::shared_ptr<const SpatialMeta> attributeMeta;
    auto ret = model->GetMeta(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER, attributeMeta);
    if (ret == SpatialErrorCode::ERROR_MODEL_NO_META) {
        CORE_LOG_I("no shn meta");
        primitive.shDegree = 0u;
        primitive.codecVersion = 0u;
        primitive.attribute2DTexNum = 0u;
    } else if (ret != SpatialErrorCode::SUCCESS) {
        CORE_LOG_E("parse astcmeta failed");
        return false;
    } else {
        if (!ParseASTCMetaData(model, attributeMeta, primitive)) {
            return false;
        }
    }
    return true;
}
bool ParseASTCData(const GS_DECODER::CompressedGsData& gsData, size_t shIdx, GS::GsPrimitive& primitive, size_t version)
{
    if (shIdx >= gsData.uwaHeader.attributeMetas.size()) {
        CORE_LOG_E("ParseASTCData: shIdx %zu out of range", shIdx);
        return false;
    }
    const auto& shMeta = gsData.uwaHeader.attributeMetas[shIdx];
    if (shMeta.quantMinValue.empty() || shMeta.quantMaxValue.empty()) {
        CORE_LOG_E("ParseASTCData: quantMinValue/quantMaxValue is empty");
        return false;
    }
    primitive.shRestMins = gsData.uwaHeader.attributeMetas[shIdx].quantMinValue[0];
    primitive.shRestMaxs = gsData.uwaHeader.attributeMetas[shIdx].quantMaxValue[0];
    BASE_NS::vector<uint8_t> shASTCData;
    shASTCData.resize(16u);
    if (gsData.uwaHeader.attributeMetas[shIdx].byteOffset + shASTCData.size() >= gsData.attributeDatas.size()) {
        CORE_LOG_E("ParseASTCData: ASTC parsing failed!");
        return false;
    }
    BASE_NS::CloneData(shASTCData.data(), shASTCData.size(),
        &gsData.attributeDatas[gsData.uwaHeader.attributeMetas[shIdx].byteOffset], shASTCData.size());

    // get image size
    AstcHeader astcHeader;
    BASE_NS::CloneData(reinterpret_cast<char*>(&astcHeader), 16u, shASTCData.data(), 16u);
    const size_t astcWidth =
        static_cast<size_t>((astcHeader.xsize[2u] << 16u) | (astcHeader.xsize[1] << 8u) | astcHeader.xsize[0]);
    const size_t astcHeight =
        static_cast<size_t>((astcHeader.ysize[2u] << 16u) | (astcHeader.ysize[1] << 8u) | astcHeader.ysize[0]);
    CORE_ASSERT(astcHeader.blockdimX == astcHeader.blockdimY);
    const size_t astcBlockSize = astcHeader.blockdimX;
    primitive.compressedShAstcBlock = static_cast<uint32_t>(astcBlockSize);
    primitive.compressedShWidth = static_cast<uint32_t>(astcWidth);
    primitive.compressedShHeight = static_cast<uint32_t>(astcHeight);
    primitive.attribute2DSingleWidth =
        static_cast<uint32_t>(gsData.uwaHeader.attributeMetas[shIdx].attribute2DSingleWidth);
    primitive.codecVersion = static_cast<uint32_t>(version);
    primitive.attribute2DTexNum = static_cast<uint32_t>(gsData.uwaHeader.attributeMetas[shIdx].attribute2DTexNum);

    if (primitive.attribute2DSingleWidth > primitive.compressedShWidth) {
        CORE_LOG_E("ParseASTCData: ASTC texture information error!");
        return false;
    }
    if (primitive.textIdOffset + primitive.attribute2DTexNum > GS_MAX_NUM_GS_TEXTURES) {
        CORE_LOG_E("ParseASTCData: Too many ASTC textures requested!");
        primitive.shDegree = 0u;
        primitive.attribute2DTexNum = 0u;
        primitive.textIdOffset = GS_MAX_NUM_GS_TEXTURES;
    }

    return true;
}

void LoadShN(std::map<std::string, std::pair<const float*, size_t>> attributes, const vec3& sh0, const size_t jj,
    const size_t globalIdx, const size_t num_harmonics, GS::GsObjectLoadDataExt* loadData)
{
    GS::GsHarmonicsData hsData;
    hsData.shCoefficients[0] = PackHalf2x16(sh0.x, sh0.y);
    hsData.shCoefficients[1] =
        PackHalf2x16(sh0.z, attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER].first[jj * num_harmonics]);
    size_t shIdx = jj * num_harmonics + 1;
    for (size_t kk = 2; kk < (num_harmonics) / 2 + 2; kk++, shIdx += 2u) {
        if (kk * 2 - 3 > num_harmonics) {
            hsData.shCoefficients[kk] =
                PackHalf2x16(attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER].first[shIdx], 0.0f);
        } else {
            hsData.shCoefficients[kk] =
                PackHalf2x16(attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER].first[shIdx],
                    attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER].first[shIdx + 1]);
        }
    }
    loadData->sphericalHarmonics[globalIdx] = hsData;
}

void MergeBlockToGlobal(std::map<std::string, std::pair<const float*, size_t>> attributes,
    std::map<std::string, std::pair<const uint32_t*, size_t>> uv, GS::GsPrimitive& primitive,
    const size_t baseOffset, const size_t ii, const size_t blockSize, GS::GsObjectLoadDataExt* loadData,
    bool enableScoreCulling, const size_t num_harmonics, BASE_NS::Math::Vec3& aabbMin,
    BASE_NS::Math::Vec3& aabbMax) {
    if (baseOffset + ii * blockSize + blockSize > loadData->gsVerticesData.Size()) {
        return;
    }
    // transform attributes to renderer structure
    for (size_t jj = 0; jj < blockSize; jj++) {
        GsObjectLoadData::GsSplatData data;
        const vec3 position = vec3(attributes[SpatialTag::POSITION].first[3 * jj],
                                   attributes[SpatialTag::POSITION].first[3 * jj + 1],
                                   attributes[SpatialTag::POSITION].first[3 * jj + 2]);
        aabbMin = Math::min(aabbMin, position);
        aabbMax = Math::max(aabbMax, position);
        const Math::Vec4 rotNorm = Math::Vec4(
            attributes[SpatialTag::ROTATION].first[4 * jj], attributes[SpatialTag::ROTATION].first[4 * jj + 1],
            attributes[SpatialTag::ROTATION].first[4 * jj + 2], attributes[SpatialTag::ROTATION].first[4 * jj + 3]);
        const vec3 scaleExp = vec3(std::exp(attributes[SpatialTag::SCALE].first[3 * jj]),
                                   std::exp(attributes[SpatialTag::SCALE].first[3 * jj + 1]),
                                   std::exp(attributes[SpatialTag::SCALE].first[3 * jj + 2]));
        const vec3 sh0 = vec3(attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0].first[3 * jj],
                              attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0].first[3 * jj + 1],
                              attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0].first[3 * jj + 2]);
        const vec3 color = vec3(0.5f + SH_C0 * sh0[0], 0.5f + SH_C0 * sh0[1], 0.5f + SH_C0 * sh0[2u]);
        const float opacity = 1.0f / (1.0f + std::exp(-1.0f * attributes[SpatialTag::OPACITY].first[1 * jj]));
        PrecomputeCovariances(opacity < 0.001f ? vec3(0.001f, 0.001f, 0.001f) : scaleExp, 1.0f, rotNorm, data.covAScale,
                              data.covBColorPacked);

        if (uv.find(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER) != uv.end()) {
            data.astcOffsetColor.x =
                static_cast<uint32_t>(uv[SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER].first[2 * jj]);
            data.astcOffsetColor.y =
                static_cast<uint32_t>(uv[SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER].first[2 * jj + 1]);
        }
        if (attributes.find(SpatialTag::IMPORTANCE) != attributes.end()) {
            data.astcOffsetColor.w = static_cast<uint32_t>(
                std::round((attributes[SpatialTag::IMPORTANCE].first[jj] - primitive.impMin)
                    / (primitive.impMax - primitive.impMin + 1e-6f) * 14.0f + 1.0f));
        } else
            data.astcOffsetColor.w = 15u;

        uint32_t colorPacked = PackGSColor(Math::Vec3(color.x, color.y, color.z), opacity);
        data.covBColorPacked.w = BitCast<float>(colorPacked);
        data.astcOffsetColor.z = PackGSDcWithMinMax(sh0, primitive.shDcMins, primitive.shDcMaxs);
        data.positionOpacity = Math::Vec4(position.x, position.y, position.z, opacity);
        size_t globalIdx = baseOffset + ii * blockSize + jj;
        loadData->gaussianPositions[globalIdx] = data.positionOpacity;
        loadData->gsVerticesData.Set(globalIdx, BASE_NS::move(data));
        ProcessCullingData(globalIdx, scaleExp, loadData->gaussianMaxScales, enableScoreCulling);
        if (primitive.shDegree == 0 || primitive.codecVersion > 0)
            continue;
        LoadShN(attributes, sh0, jj, globalIdx, num_harmonics, loadData);
    }
}

static bool FetchBlockAttributes(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive, size_t ii,
    size_t blockSize, std::unordered_map<std::string, std::shared_ptr<const ModelAttribute>>& attrMaps,
    std::unordered_map<std::string, std::shared_ptr<const ModelAttribute>>& uvMaps)
{
    std::vector<std::string> uvNames = {};
    if (primitive.codecVersion > 0 && primitive.shDegree > 0)
        uvNames.push_back(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER);

    std::vector<std::string> attributeNames = {
        SpatialTag::POSITION, SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0,
        SpatialTag::OPACITY, SpatialTag::SCALE, SpatialTag::ROTATION};

    if (primitive.shDegree > 0) {
        std::shared_ptr<const SpatialMeta> attributeMeta;
        if (model->GetMeta(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER, attributeMeta) ==
            SpatialErrorCode::SUCCESS && primitive.codecVersion == 0u) {
            attributeNames.push_back(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER);
        }
        std::shared_ptr<const SpatialMeta> attributeMeta2;
        if (primitive.codecVersion > 1 &&
            model->GetMeta(SpatialTag::IMPORTANCE, attributeMeta2) == SpatialErrorCode::SUCCESS) {
            attributeNames.push_back(SpatialTag::IMPORTANCE);
        }
    }

    const std::unordered_set<std::string> attrList(attributeNames.begin(), attributeNames.end());
    const std::unordered_set<std::string> uvList(uvNames.begin(), uvNames.end());

    for (const auto& attrName : attributeNames) {
        auto outputDesc = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
        if (outputDesc == nullptr) {
            CORE_LOG_E("FetchBlockAttributes CreateMeta failed");
            return false;
        }
        outputDesc->AddMeta(SpatialTag::BLOCK_INDEX, static_cast<uint32_t>(ii));
        outputDesc->AddMeta(SpatialTag::BLOCK_POINTS_NUM, static_cast<uint32_t>(blockSize));
        outputDesc->AddMeta(SpatialTag::RECONSTRUCTED, static_cast<uint32_t>(1));
        std::shared_ptr<const ModelAttribute> blockAttr;
        auto ret = model->GetAttribute(attrName, *outputDesc, blockAttr);
        if (ret != SpatialErrorCode::SUCCESS) {
            CORE_LOG_E("Failed to dequantize attribute %s", attrName.c_str());
            return false;
        }
        attrMaps[attrName] = blockAttr;
    }

    if (!uvNames.empty()) {
        auto outputDesc2 = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
        if (outputDesc2 == nullptr) {
            CORE_LOG_E("FetchBlockAttributes CreateMeta (uv) failed");
            return false;
        }
        outputDesc2->AddMeta(SpatialTag::BLOCK_INDEX, static_cast<uint32_t>(ii));
        outputDesc2->AddMeta(SpatialTag::BLOCK_POINTS_NUM, static_cast<uint32_t>(blockSize));
        outputDesc2->AddMeta(SpatialTag::UV, static_cast<uint32_t>(1));
        std::shared_ptr<const ModelAttribute> blockAttr;
        auto ret = model->GetAttribute(uvNames[0], *outputDesc2, blockAttr);

        if (ret != SpatialErrorCode::SUCCESS) {
            CORE_LOG_E("Failed to get uv, processing block: %d", static_cast<uint32_t>(ii));
            return false;
        }
        uvMaps[uvNames[0]] = blockAttr;
    }
    return true;
}

static void ProcessAndMergeBlockData(
    const std::unordered_map<std::string, std::shared_ptr<const ModelAttribute>>& attrMaps,
    const std::unordered_map<std::string, std::shared_ptr<const ModelAttribute>>& uvMaps, GS::GsPrimitive& primitive,
    size_t blockSize, size_t baseOffset, size_t ii, GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    std::map<std::string, std::pair<const float*, size_t>> attributes;
    std::map<std::string, std::pair<const uint32_t*, size_t>> uv;
    size_t num_harmonics = 0;

    for (const auto& [attrName, modelAttr] : attrMaps) {
        auto [pointPtr, pointCount] = modelAttr->GetAs<float>();
        attributes[attrName] = std::make_pair(pointPtr, pointCount);
        if (attrName == SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER) {
            num_harmonics = pointCount / blockSize < 45 ? pointCount / blockSize : 45;
        }
    }

    for (const auto& [attrName, modelAttr] : uvMaps) {
        auto [pointPtr, pointCount] = modelAttr->GetAs<uint32_t>();
        uv[attrName] = std::make_pair(pointPtr, pointCount);
    }

    if (attributes[SpatialTag::POSITION].second / 3 != blockSize ||
        attributes[SpatialTag::ROTATION].second / 4 != blockSize ||
        attributes[SpatialTag::SCALE].second / 3 != blockSize ||
        attributes[SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0].second / 3 != blockSize ||
        attributes[SpatialTag::OPACITY].second / 1 != blockSize ||
        (attributes.find(SpatialTag::IMPORTANCE) != attributes.end() &&
         attributes[SpatialTag::IMPORTANCE].second != blockSize) ||
        (uv.find(SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER) != uv.end() &&
         uv[SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER].second / 2 != blockSize)) {
        CORE_LOG_E("Attribute size is not CORRECT");
        return;
    }

    MergeBlockToGlobal(attributes, uv, primitive, baseOffset, ii, blockSize, loadData, enableScoreCulling,
        num_harmonics, aabbMin, aabbMax);
}

void ProcessVertexDataCompressed(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive,
    const size_t baseOffset, const size_t ii, const size_t blockSize, GS::GsObjectLoadDataExt* loadData,
    bool enableScoreCulling, BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    std::unordered_map<std::string, std::shared_ptr<const ModelAttribute>> attrMaps;
    std::unordered_map<std::string, std::shared_ptr<const ModelAttribute>> uvMaps;
    if (!FetchBlockAttributes(model, primitive, ii, blockSize, attrMaps, uvMaps)) {
        return;
    }
    ProcessAndMergeBlockData(
        attrMaps, uvMaps, primitive, blockSize, baseOffset, ii, loadData, enableScoreCulling, aabbMin, aabbMax);
}
void ProcessVertexDataCompressed(const GS_DECODER::CompressedGsData& gsData, const GS::GsPrimitive& primitive,
    const size_t baseOffset, const size_t ii, GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    GS_DECODER::GsSplatData gsVertex;
    GsObjectLoadData::GsSplatData data;
    if (gsData.shDegree > 0) {
        const uint32_t blockSize = primitive.compressedShAstcBlock * primitive.compressedShAstcBlock;
        const uint32_t blockNum = static_cast<uint32_t>(ii) / blockSize;
        const uint32_t inNum = static_cast<uint32_t>(ii) % blockSize;
        const uint32_t blockSides = primitive.attribute2DSingleWidth / primitive.compressedShAstcBlock;
        const uint32_t astcOffsetx =
            blockNum % blockSides * primitive.compressedShAstcBlock + inNum % primitive.compressedShAstcBlock;
        const uint32_t astcOffsety =
            blockNum / blockSides * primitive.compressedShAstcBlock + inNum / primitive.compressedShAstcBlock;
        data.astcOffsetColor.x = static_cast<uint32_t>(astcOffsetx);
        data.astcOffsetColor.y = static_cast<uint32_t>(astcOffsety);
    }

    for (const auto& attr_meta : gsData.uwaHeader.attributeMetas) {
        bool succeed =
            GS_DECODER::DecodeAttribute(gsData.attributeDatas, attr_meta, gsVertex, ii, gsData.uwaHeader.numPoints);
        if (!succeed) {
            CORE_LOG_E("ProcessVertexDataCompressed: Decoding attributes failed!");
            return;
        }
    }
    const vec3 position = vec3(gsVertex.position[0], gsVertex.position[1], gsVertex.position[2]);
    const Math::Vec4 rotNorm =
        Math::Vec4(gsVertex.rotation[0], gsVertex.rotation[1], gsVertex.rotation[2], gsVertex.rotation[3]);

    const vec3 scaleExp =
        vec3(std::exp(gsVertex.scaling[0u]), std::exp(gsVertex.scaling[1u]), std::exp(gsVertex.scaling[2u]));

    const vec3 sh0 = vec3(gsVertex.shs[0], gsVertex.shs[1], gsVertex.shs[2u]);
    const vec3 color = vec3(0.5f + SH_C0 * sh0[0], 0.5f + SH_C0 * sh0[1], 0.5f + SH_C0 * sh0[2u]);
    const float opacity = 1.0f / (1.0f + std::exp(-1.0f * gsVertex.opacity));

    aabbMin = Math::min(aabbMin, position);
    aabbMax = Math::max(aabbMax, position);
    PrecomputeCovariances(scaleExp, 1.0f, rotNorm, data.covAScale, data.covBColorPacked);

    uint32_t colorPacked = PackGSColor(Math::Vec3(color.x, color.y, color.z), opacity);
    uint32_t dcPacked = PackGSDcWithMinMax(sh0, primitive.shDcMins, primitive.shDcMaxs);
    data.covBColorPacked.w = BitCast<float>(colorPacked);
    data.astcOffsetColor.z = dcPacked;
    data.positionOpacity = Math::Vec4(position.x, position.y, position.z, opacity);
    loadData->gaussianPositions[baseOffset + ii] = data.positionOpacity;
    loadData->gsVerticesData.Set(baseOffset + ii, BASE_NS::move(data));
    ProcessCullingData(baseOffset + ii, scaleExp, loadData->gaussianMaxScales, enableScoreCulling);
}

static bool DecodeSpatialModel(
    const std::vector<uint8_t>& encodedData, const std::string& decVersion, std::shared_ptr<SpatialModel>& model)
{
    auto decodeConfig = SpatialCodecFactory::CreateMeta(SpatialTag::UWAGS);
    if (decodeConfig == nullptr) {
        CORE_LOG_E("DecodeSpatialModel CreateMeta failed");
        return false;
    }
    decodeConfig->AddMeta(SpatialTag::COMPRESSION_EXTENSION_NAME, decVersion);

    auto decoder = SpatialCodecFactory::CreateDecoder(SpatialTag::UWAGS);
    if (decoder == nullptr) {
        CORE_LOG_E("DecodeSpatialModel CreateDecoder failed");
        return false;
    }
    auto ret = decoder->Decode(encodedData, *decodeConfig, model);
    if (ret != SpatialErrorCode::SUCCESS || !model) {
        CORE_LOG_E("DECODER stage 1 decoding failed");
        return false;
    }
    return true;
}

static bool ParseBasicInfo(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive,
    std::shared_ptr<const Spatial::SpatialMeta>& basicInfo)
{
    auto ret = model->GetMeta(SpatialTag::BASIC_INFO, basicInfo);
    if (ret != SpatialErrorCode::SUCCESS) {
        CORE_LOG_E("basic info get failed");
        return false;
    }
    primitive.shDegree = basicInfo->GetMetaAs<uint32_t>(SpatialTag::SH_DEGREE).value_or(0);
    std::shared_ptr<const SpatialMeta> sh0Meta;
    ret = model->GetMeta(SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0, sh0Meta);
    if (ret != SpatialErrorCode::SUCCESS) {
        CORE_LOG_E("PARSING SH0 META DATA FAILED");
        return false;
    }
    const auto shDcMaxs = sh0Meta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MAX_VALUE);
    const auto shDcMins = sh0Meta->GetMetaAs<std::vector<float>>(SpatialTag::QUANTIZATION_MIN_VALUE);
    if (!shDcMaxs.has_value() || !shDcMins.has_value()) {
        CORE_LOG_E("Missing quantization parameters");
        return false;
    }
    primitive.shDcMins = *std::min_element(shDcMins.value().begin(), shDcMins.value().end());
    primitive.shDcMaxs = *std::max_element(shDcMaxs.value().begin(), shDcMaxs.value().end());
    return true;
}

static void ProcessBlockRange(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive,
    GS::GsRenderData* renderData, GS::GsObjectLoadDataExt* loadData, size_t baseOffset, size_t actualBlockSize,
    size_t startIdx, size_t endIdx, GsLoadContext loadCtx, BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
#ifdef __OHOS_PLATFORM__
    cpu_set_t currCpuSet;
    CPU_ZERO(&currCpuSet);
    if (renderData->enableCpuScheduling) {
        if (sched_getaffinity(0, sizeof(cpu_set_t), &currCpuSet) < 0) {
            CORE_LOG_E("GSPlugin Getschedule Cpu failed!");
        }
        cpu_set_t MiddleOrSmallCpus;
        CPU_ZERO(&MiddleOrSmallCpus);
        static constexpr size_t BINDING_CPU_CORE_INDICES = 6;
        for (size_t i = 0; i < BINDING_CPU_CORE_INDICES; ++i) {
            CPU_SET(i, &MiddleOrSmallCpus);
        }
        if (sched_setaffinity(0, sizeof(cpu_set_t), &MiddleOrSmallCpus) < 0) {
            CORE_LOG_E("GSPlugin SetSchedule Cpu failed!");
        }
    }
#endif

    aabbMin = { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
        std::numeric_limits<float>::max() };
    aabbMax = { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
        std::numeric_limits<float>::lowest() };

    for (size_t ii = startIdx; ii < endIdx; ii++) {
        ProcessVertexDataCompressed(model, primitive, baseOffset, ii, actualBlockSize, loadData,
            renderData->enableScoreCulling, aabbMin, aabbMax);

        if (loadCtx.shuttingDown_->load()) {
            break;
        }
    }
#ifdef __OHOS_PLATFORM__
    if (renderData->enableCpuScheduling) {
        if (sched_setaffinity(0, sizeof(cpu_set_t), &currCpuSet) < 0) {
            CORE_LOG_E("GSPlugin Schedule Cpu failed!");
        }
    }
#endif
}

static bool LaunchParallelVertexProcessing(const std::shared_ptr<SpatialModel>& model, GS::GsPrimitive& primitive,
    GS::GsRenderData* renderData, GS::GsObjectLoadDataExt* loadData, IThreadPool* threadPool, GsLoadContext loadCtx,
    size_t gsCount, uint32_t baseOffset, size_t actualBlockSize)
{
    if (primitive.RequireSH() && loadData->sphericalHarmonics.size() < primitive.numGs) {
        loadData->sphericalHarmonics.resize(primitive.numGs);
    }
    std::mutex aabbSyncMtx;
    const size_t numThreads = std::max(threadPool->GetNumberOfThreads(), 1u);
    BASE_NS::vector<IThreadPool::IResult::Ptr> results;
    results.reserve(numThreads);

    const size_t blockNum = (gsCount + actualBlockSize - 1) / actualBlockSize;
    const size_t chunkSize = size_t(blockNum + numThreads - 1) / numThreads;

    for (size_t ii = 0; ii < numThreads; ii++) {
        const size_t startIdx = ii * chunkSize;
        const size_t endIdx = (ii == numThreads - 1) ? (blockNum) : startIdx + chunkSize;

        auto task = CreateFunctionTask([&, startIdx, endIdx, loadCtx]() {
            BASE_NS::Math::Vec3 aabbMin;
            BASE_NS::Math::Vec3 aabbMax;
            ProcessBlockRange(model, primitive, renderData, loadData, baseOffset, actualBlockSize, startIdx, endIdx,
                loadCtx, aabbMin, aabbMax);
            std::lock_guard<std::mutex> lock(aabbSyncMtx);
            primitive.aabbMin = Math::min(primitive.aabbMin, aabbMin);
            primitive.aabbMax = Math::max(primitive.aabbMax, aabbMax);
        });

        results.push_back(threadPool->Push(BASE_NS::move(task)));
    }

    for (auto& result : results) {
        result->Wait();
    }
    return true;
}

bool ParseCompressedBasicInfo(const std::vector<uint8_t>& encodedData, std::string decVersion,
    GS::GsPrimitive& primitive, std::shared_ptr<SpatialModel>& model,
    std::shared_ptr<const SpatialMeta>& basicInfo)
{
    if (!DecodeSpatialModel(encodedData, decVersion, model)) {
        return false;
    }

    if (!ParseBasicInfo(model, primitive, basicInfo)) {
        return false;
    }
    return true;
}

bool ParseGSCompressed(std::shared_ptr<SpatialModel>& model, std::shared_ptr<const SpatialMeta>& basicInfo,
    std::string decVersion, GS::GsRenderData* renderData, GS::GsObjectLoadDataExt* loadData, GS::GsPrimitive& primitive,
    IThreadPool* threadPool, RENDER_NS::IGpuResourceManager& gpuResourceMgr, IRenderContext* renderContext,
    GsLoadContext loadCtx)
{
    if (!ParseASTCData(model, primitive)) {
        CORE_LOG_E("parse astc failed");
        return false;
    }
    if (!CreateASTCImage(model, primitive, renderData, gpuResourceMgr, renderContext)) {
        CORE_LOG_E("create astc failed");
        return false;
    }

    const size_t gsCount = static_cast<size_t>((basicInfo->GetMetaAs<uint32_t>(SpatialTag::GS_POINTS_NUM)).value_or(0));
    if (loadData->gsVerticesData.Size() < primitive.numGs) {
        return false;
    }
    const uint32_t baseOffset = static_cast<uint32_t>(loadData->gsVerticesData.Size()) - primitive.numGs;
    // NOTE: Tag value matches SpatialDecoderTag::PARALLEL_BLOCK_SIZE
    // in spatial_codec/src/decoder/include/common/spatial_decoder_tag.h
    // Not accessible via SpatialTag because it's a non-UWA-standard internal decoder tag.
    const std::string parallelBlockSize = "parallelBlockSize";
    const size_t blockSize =
        static_cast<size_t>((basicInfo->GetMetaAs<uint32_t>(parallelBlockSize)).value_or(0));
    const size_t actualBlockSize = blockSize * blockSize;
    if (!LaunchParallelVertexProcessing(
            model, primitive, renderData, loadData, threadPool, loadCtx, gsCount, baseOffset, actualBlockSize)) {
        return false;
    }
    return true;
}

void ConvertVertexData(size_t ii, const size_t shChannel, PlyVertexDataFullHarmonics& vertexData,
    GS::GsObjectLoadDataExt* loadData, bool enableScoreCulling, GS::GsPrimitive& primitive,
    BASE_NS::Math::Vec3& aabbMin, BASE_NS::Math::Vec3& aabbMax)
{
    const Math::Vec3 scaleExp =
        Math::Vec3(vertexData.scale[0u], vertexData.scale[1u], vertexData.scale[2]);
    const float opacity = vertexData.opacity; // sigmoid
    // normalize the quaternion rotation
    Math::Vec4 rotNorm = Math::Vec4(vertexData.rotation);
    {
        float len = 1e-6;
        for (size_t jj = 0; jj < 4u; jj++) {
            len += rotNorm[jj] * rotNorm[jj];
        }
        rotNorm /= std::sqrtf(len);
    }
    GsObjectLoadData::GsSplatData data;

    aabbMin = Math::min(aabbMin, Math::Vec3(vertexData.position));
    aabbMax = Math::max(aabbMax, Math::Vec3(vertexData.position));
    // precompute the covariances since we are taking only the 0th degree spherical harmonics anyways
    PrecomputeCovariances(scaleExp, 1.0f, rotNorm, data.covAScale, data.covBColorPacked);
    // Only take the 0th degree of the spherical harmonics. This essentially removes all view-dependentant
    // lighting (e.g. reflections), but is way faster since we can recompute everything. Otherwise we would
    // need to pass all 16x vec3 harmonics to the GPU and compute the new color per-frame.
    const Math::Vec3 color = Math::Vec3(vertexData.shs[0u], vertexData.shs[16u], vertexData.shs[32u])
        * SH_C0 + Math::Vec3(0.5f, 0.5f, 0.5f);
    uint32_t colorPacked = PackGSColor(color, opacity);

    // Transpose and load the spherical harmonics
    if (primitive.RequireSH()) {
        float shDataTransposed[shChannel * 3u];

        for (size_t jj = 0u; jj < shChannel; jj++) {
            shDataTransposed[jj * 3u + 0u] = vertexData.shs[jj];
            shDataTransposed[jj * 3u + 1u] = vertexData.shs[jj + GS_NUM_HARMONICS];
            shDataTransposed[jj * 3u + 2u] = vertexData.shs[jj + GS_NUM_HARMONICS * 2u];
        }

        GS::GsHarmonicsData hsData;
        size_t shIdx = 0;
        for (size_t jj = 0; jj < shChannel; jj++) {
            hsData.shCoefficients[jj] = PackHalf2x16(shDataTransposed[shIdx], shDataTransposed[shIdx + 1]);
            shIdx += 2u;
        }
        loadData->sphericalHarmonics[ii] = hsData;
    }
    data.covBColorPacked.w = *reinterpret_cast<float*>(&colorPacked);
    data.positionOpacity = Math::Vec4(Math::Vec3(vertexData.position), opacity);
    loadData->gaussianPositions[ii] = data.positionOpacity;
    loadData->gsVerticesData.Set(ii, BASE_NS::move(data));
    ProcessCullingData(ii, scaleExp, loadData->gaussianMaxScales, enableScoreCulling);
}

void GltfAOSLoading(size_t ii, const size_t allChannel, std::shared_ptr<const std::vector<uint8_t>>&
    uncompressedData, std::vector<std::string>& attributeNames, std::vector<uint32_t>& attrOffsets,
    PlyVertexDataFullHarmonics& vertexData)
{
    const float *floatData = reinterpret_cast<const float *>(uncompressedData->data());
    for (size_t i = 0; i < attributeNames.size(); ++i) {
        const uint32_t base = attrOffsets[i] + ii * allChannel;
        if (attributeNames[i] == "POSITION") {
            for (uint32_t j = 0u; j < 3u; j++)
                vertexData.position[j] = floatData[base + j];
        } else if (attributeNames[i] == "KHR_gaussian_splatting:SCALE") {
            for (uint32_t j = 0u; j < 3u; j++)
                vertexData.scale[j] = floatData[base + j];
        } else if (attributeNames[i] == "KHR_gaussian_splatting:ROTATION") {
            static constexpr uint32_t reorder[] = {3u, 0u, 1u, 2u};
            for (uint32_t j = 0; j < 4u; j++)
                vertexData.rotation[j] = floatData[base + reorder[j]];
        } else if (attributeNames[i] == "KHR_gaussian_splatting:OPACITY") {
            vertexData.opacity = floatData[base];
        } else if (attributeNames[i].compare(0, 23u, "KHR_gaussian_splatting:") == 0) {
            int degree;
            int coef;
            int b = -1;
            if (sscanf_s(attributeNames[i].c_str() + 23u, "SH_DEGREE_%d_COEF_%d", &degree, &coef) == 2u) {
                b = degree * degree + coef;
            }
            if (b >= 0 && b < static_cast<int>(GS_NUM_HARMONICS)) {
                vertexData.shs[b]       = floatData[base];
                vertexData.shs[b + 16u] = floatData[base + 1u];
                vertexData.shs[b + 32u] = floatData[base + 2u];
            }
        }
    }
}

void GltfSOALoading(size_t ii, std::shared_ptr<const std::vector<uint8_t>>& uncompressedData,
    std::vector<std::string>& attributeNames, std::vector<uint32_t>& attrOffsets,
    PlyVertexDataFullHarmonics& vertexData)
{
    const float *floatData = reinterpret_cast<const float *>(uncompressedData->data());
    for (size_t i = 0; i < attributeNames.size(); ++i) {
        const uint32_t base = attrOffsets[i];
        if (attributeNames[i] == "POSITION") {
            for (uint32_t j = 0u; j < 3u; j++)
                vertexData.position[j] = floatData[base + ii*3u + j];
        } else if (attributeNames[i] == "KHR_gaussian_splatting:SCALE") {
            for (uint32_t j = 0u; j < 3u; j++)
                vertexData.scale[j] = floatData[base + ii*3u + j];
        } else if (attributeNames[i] == "KHR_gaussian_splatting:ROTATION") {
            static constexpr uint32_t reorder[] = {3u, 0u, 1u, 2u};
            for (uint32_t j = 0; j < 4u; j++)
                vertexData.rotation[j] = floatData[base + ii*4u + reorder[j]];
        } else if (attributeNames[i] == "KHR_gaussian_splatting:OPACITY") {
            vertexData.opacity = floatData[base + ii];
        } else if (attributeNames[i].compare(0, 23u, "KHR_gaussian_splatting:") == 0) {
            int degree;
            int coef;
            int b = -1;
            if (sscanf_s(attributeNames[i].c_str() + 23u, "SH_DEGREE_%d_COEF_%d", &degree, &coef) == 2u) {
                b = degree * degree + coef;
            }
            if (b >= 0 && b < static_cast<int>(GS_NUM_HARMONICS)) {
                vertexData.shs[b]       = floatData[base + ii*3u];
                vertexData.shs[b + 16u] = floatData[base + ii*3u + 1u];
                vertexData.shs[b + 32u] = floatData[base + ii*3u + 2u];
            }
        }
    }
}

uint32_t GetAttributeChannle(std::string name)
{
    if (name == "POSITION") {
        return 3u;
    } else if (name == "KHR_gaussian_splatting:SCALE") {
        return 3u;
    } else if (name == "KHR_gaussian_splatting:ROTATION") {
        return 4u;
    } else if (name == "KHR_gaussian_splatting:OPACITY") {
        return 1u;
    } else if (name.compare(0, 23u, "KHR_gaussian_splatting:") == 0) {
        int degree;
        int coef;
        if (sscanf_s(name.c_str() + 23u, "SH_DEGREE_%d_COEF_%d", &degree, &coef) == 2) {
            return 3u;
        }
    }
    return 0u;
}

bool UnCompressedMetaCheck(std::vector<std::string>& attributeNames, std::vector<uint32_t>& attrOffsets,
    const size_t numGaussians, size_t bufferSize, std::vector<uint32_t>& attrOffsetsU)
{
    attrOffsetsU.resize(attributeNames.size());
    for (size_t ef = 0u; ef < attributeNames.size(); ef++) {
        if (attrOffsets[ef] + GetAttributeChannle(attributeNames[ef]) * numGaussians * 4u > bufferSize) {
            CORE_LOG_E("uncompressed glb buffer is less than required");
            return false;
        }
        attrOffsetsU[ef] = static_cast<uint32_t>(attrOffsets[ef] / sizeof(float));
    }
    const size_t allChannel = 14u + (attributeNames.size() - 5) * 3;
    const size_t shChannel = 1u + (attributeNames.size() - 5);
    if (attributeNames.size() < 5u || allChannel * numGaussians * 4u > bufferSize) {
        CORE_LOG_E("uncompressed glb buffer is less than required");
        return false;
    }
    return true;
}

bool ParseGSUnCompressed(std::shared_ptr<const std::vector<uint8_t>>& uncompressedData,
    std::vector<std::shared_ptr<const SpatialMeta>>& primitiveMetas, const size_t numGaussians,
    GS::GsRenderData*  renderData, GS::GsObjectLoadDataExt* loadData, GS::GsPrimitive& primitive, GsLoadContext loadCtx)
{
    // get basic information for parsing
    auto dataFormat = primitiveMetas[0]->GetMetaAs<std::string>(SpatialTag::TYPE);
    auto attributeNames = primitiveMetas[0]->GetMetaAs<std::vector<std::string>>(SpatialTag::PRIMITIVE_ATTRIBUTES_NAME);
    auto attrOffsets = primitiveMetas[0]->GetMetaAs<std::vector<uint32_t>>(SpatialTag::BYTEOFFSET);
    std::vector<uint32_t> attrOffsetsU;
    if (!attributeNames.has_value() || !attrOffsets.has_value() || !UnCompressedMetaCheck(attributeNames.value(),
            attrOffsets.value(), numGaussians, uncompressedData->size(), attrOffsetsU)) {
        CORE_LOG_E("uncompressed glb meta parsing failed");
        return false;
    }
    const size_t allChannel = 14u + (attributeNames.value().size() - 5) * 3;
    const size_t shChannel = 1u + (attributeNames.value().size() - 5);

    // parsing each point from buffer
    const size_t numThreads = loadCtx.threadPool_->GetNumberOfThreads();
    BASE_NS::vector<IThreadPool::IResult::Ptr> results;
    results.reserve(numThreads);
    const size_t chunkSize = numGaussians / numThreads;
    std::mutex aabbSyncMtx;
    for (size_t ii = 0; ii < numThreads; ii++) {
        const size_t start = ii * chunkSize;
        const size_t end = (ii == numThreads - 1) ? numGaussians : start + chunkSize;
        auto task = CreateFunctionTask([&, start, end]() {
            BASE_NS::Math::Vec3 aabbMin { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max() };
            BASE_NS::Math::Vec3 aabbMax { std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest() };
            PlyVertexDataFullHarmonics vertexData;
            if (dataFormat.has_value() && dataFormat.value() == SpatialTag::KHR_GAUSSIAN_SPLATTING_AOS) {
                for (size_t ci = start; ci < end; ci++) {
                    GltfAOSLoading(ci, allChannel, uncompressedData, attributeNames.value(), attrOffsetsU, vertexData);
                    ConvertVertexData(ci, shChannel, vertexData, loadData,
                        renderData->enableScoreCulling, primitive, aabbMin, aabbMax);
                }
            } else if (dataFormat.has_value() && dataFormat.value() == SpatialTag::KHR_GAUSSIAN_SPLATTING_SOA) {
                for (size_t ci = start; ci < end; ci++) {
                    GltfSOALoading(ci, uncompressedData, attributeNames.value(), attrOffsetsU, vertexData);
                    ConvertVertexData(ci, shChannel, vertexData, loadData,
                        renderData->enableScoreCulling, primitive, aabbMin, aabbMax);
                }
            }
            std::lock_guard<std::mutex> lock(aabbSyncMtx);
            primitive.aabbMin = min(primitive.aabbMin, aabbMin);
            primitive.aabbMax = max(primitive.aabbMax, aabbMax);
        });
        results.push_back(loadCtx.threadPool_->Push(BASE_NS::move(task)));
    }

    for (auto& result : results) {
        result->Wait();
    }
    return true;
}
bool ParseGSCompressed(const uint8_t* fileData, size_t size, GS::GsRenderData* renderData,
    GS::GsObjectLoadDataExt* loadData, GS::GsPrimitive& primitive, IThreadPool* threadPool,
    RENDER_NS::IGpuResourceManager& gpuResourceMgr, IRenderContext* renderContext, GsLoadContext loadCtx)
{
    GS_DECODER::CompressedGsData gsData;
    if (!GS_DECODER::ParseGsCompressed(fileData, size, gsData)) {
        return false;
    }

    size_t codecVersion = 0;
    size_t shIdx = 0;
    size_t dcIdx = 0;
    bool foundDc = false;

    for (size_t aid = 0; aid < gsData.uwaHeader.attributeMetas.size(); aid++) {
        if ((gsData.uwaHeader.attributeMetas[aid].attributeEncoderScheme == 2) &&
            (gsData.uwaHeader.attributeMetas[aid].attributeType == 4u)) {
            codecVersion = 1;
            shIdx = aid;
        }
        if (gsData.uwaHeader.attributeMetas[aid].attributeType == 3u) {
            dcIdx = aid;
            foundDc = true;
        }
    }
    if (!foundDc || dcIdx >= gsData.uwaHeader.attributeMetas.size()) {
        return false;
    }
    primitive.shDegree = Math::min(primitive.shDegree, static_cast<uint32_t>(gsData.shDegree));

    if (codecVersion >= 1 && primitive.shDegree > 0) {
        if (!ParseASTCData(gsData, shIdx, primitive, codecVersion)) {
            return false;
        }
        if (!CreateASTCImage(gsData, shIdx, primitive, renderData, gpuResourceMgr, renderContext)) {
            return false;
        }
    }
    if (gsData.uwaHeader.attributeMetas[dcIdx].quantMinValue.empty() ||
        gsData.uwaHeader.attributeMetas[dcIdx].quantMaxValue.empty()) {
        CORE_LOG_E("DC attribute quantMinValue/quantMaxValue is empty");
        return false;
    }
    primitive.shDcMins = gsData.uwaHeader.attributeMetas[dcIdx].quantMinValue[0];
    primitive.shDcMaxs = gsData.uwaHeader.attributeMetas[dcIdx].quantMaxValue[0];

    std::mutex aabbSyncMtx;
    const size_t numThreads = std::max(threadPool->GetNumberOfThreads(), 1u);
    BASE_NS::vector<IThreadPool::IResult::Ptr> results;
    results.reserve(numThreads);
    const size_t chunkSize = size_t(primitive.numGs + numThreads - 1) / numThreads;

    if (loadData->gsVerticesData.Size() < primitive.numGs) {
        return false;
    }
    const uint32_t baseOffset = static_cast<uint32_t>(loadData->gsVerticesData.Size()) - primitive.numGs;
    for (size_t ii = 0; ii < numThreads; ii++) {
        const size_t startIdx = ii * chunkSize;
        const size_t endIdx = (ii == numThreads - 1) ? (primitive.numGs) : startIdx + chunkSize;

        auto task = CreateFunctionTask([&, startIdx, endIdx, loadCtx]() {
#ifdef __OHOS_PLATFORM__
            cpu_set_t currCpuSet;
            CPU_ZERO(&currCpuSet);
            if (renderData->enableCpuScheduling) {
                if (sched_getaffinity(0, sizeof(cpu_set_t), &currCpuSet) < 0) {
                    CORE_LOG_E("GSPlugin Getschedule Cpu failed!");
                }
                cpu_set_t MiddleOrSmallCpus;
                CPU_ZERO(&MiddleOrSmallCpus); // write into MiddleOrSmallCpus
                static constexpr size_t BINDING_CPU_CORE_INDICES = 6;
                for (size_t i = 0; i < BINDING_CPU_CORE_INDICES; ++i) {
                    CPU_SET(i, &MiddleOrSmallCpus);
                }
                if (sched_setaffinity(0, sizeof(cpu_set_t), &MiddleOrSmallCpus) < 0) {
                    CORE_LOG_E("GSPlugin SetSchedule Cpu failed!");
                }
            }
#endif

            BASE_NS::Math::Vec3 aabbMin { std::numeric_limits<float>::max(), std::numeric_limits<float>::max(),
                std::numeric_limits<float>::max() };
            BASE_NS::Math::Vec3 aabbMax { std::numeric_limits<float>::lowest(), std::numeric_limits<float>::lowest(),
                std::numeric_limits<float>::lowest() };

            for (size_t ii = startIdx; ii < endIdx; ii++) {
                ProcessVertexDataCompressed(
                    gsData, primitive, baseOffset, ii, loadData, renderData->enableScoreCulling, aabbMin, aabbMax);

                if (loadCtx.shuttingDown_->load()) { // check exit flag
                    break;
                }
            }
            std::lock_guard<std::mutex> lock(aabbSyncMtx);
            primitive.aabbMin = Math::min(primitive.aabbMin, aabbMin);
            primitive.aabbMax = Math::max(primitive.aabbMax, aabbMax);
#ifdef __OHOS_PLATFORM__
            if (renderData->enableCpuScheduling) {
                if (sched_setaffinity(0, sizeof(cpu_set_t), &currCpuSet) < 0) {
                    CORE_LOG_E("GSPlugin Schedule Cpu failed!");
                }
            }
#endif
        });

        results.push_back(threadPool->Push(BASE_NS::move(task)));
    }

    for (auto& result : results) {
        result->Wait();
    }
    return true;
}
} // namespace GS

#endif // PLUGIN_GS_PARSING_H