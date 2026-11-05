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

#include "loader/gs_loader.h"

#include <core/intf_engine.h>
#include <core/io/intf_file_manager.h>
#include <render/intf_render_context.h>

#include "loader/gs_importer.h"

namespace GS {
namespace {
constexpr BASE_NS::string_view EXTENSIONS[] = {".ply", ".glb", ".splat", ".mp4"};
}  // namespace

GsSceneData::GsSceneData(BASE_NS::vector<uint8_t>&& data) : data_(data)
{}
GsSceneData::GsSceneData(BASE_NS::vector<uint8_t>&& data, int64_t offset) : data_(data), offset_(offset)
{}

size_t GsSceneData::GetDefaultSceneIndex() const
{
    return 0U;
}

size_t GsSceneData::GetSceneCount() const
{
    return data_.empty() ? 0U : 1U;
}

BASE_NS::array_view<const uint8_t> GsSceneData::GetUri() const
{
    return data_;
}

int64_t GsSceneData::GetLoadOffset() const
{
    return offset_;
}

GsSceneLoader::GsSceneLoader(RENDER_NS::IRenderContext& context)
    : renderContext_(&context), fileManager_(context.GetEngine().GetFileManager())
{}
GsSceneLoader::GsSceneLoader(CORE_NS::IFileManager& fileManager) : renderContext_(nullptr), fileManager_(fileManager)
{}

CORE3D_NS::ISceneLoader::Result GsSceneLoader::Load(BASE_NS::string_view uri)
{
    if (uri.empty()) {
        return {-1, "Empty URI", {}};
    }
    auto file = fileManager_.OpenFile(uri);
    if (!file) {
        return {-1, "Failed to open file", {}};
    }
    file->Close();

    // TODO actual parsing of the file. now just passing the URI forward if the file exists.
    return {0,
        {},
        BASE_NS::refcnt_ptr<CORE3D_NS::ISceneData>{new GsSceneData(BASE_NS::vector<uint8_t>(
            reinterpret_cast<const uint8_t*>(uri.data()), reinterpret_cast<const uint8_t*>(uri.data() + uri.size())))}};
}

CORE3D_NS::ISceneLoader::Result GsSceneLoader::Load(const BASE_NS::string_view uri, const int64_t offset)
{
    if (uri.empty()) {
        return {-1, "Empty URI", {}};
    }
    auto file = fileManager_.OpenFile(uri);
    if (!file) {
        return {-1, "Failed to open file", {}};
    }
    file->Close();

    return {0,
        {},
        BASE_NS::refcnt_ptr<CORE3D_NS::ISceneData>{
            new GsSceneData(BASE_NS::vector<uint8_t>(reinterpret_cast<const uint8_t*>(uri.data()),
                                reinterpret_cast<const uint8_t*>(uri.data() + uri.size())),
                offset)}};
}

CORE3D_NS::ISceneImporter::Ptr GsSceneLoader::CreateSceneImporter(CORE_NS::IEcs& ecs)
{
    auto tp = ecs.GetThreadPool();
    if (!tp) {
        return nullptr;
    }
    return CORE3D_NS::ISceneImporter::Ptr{new GsSceneImporter(ecs, nullptr)};
}

CORE3D_NS::ISceneImporter::Ptr GsSceneLoader::CreateSceneImporter(CORE_NS::IEcs& ecs, CORE_NS::IThreadPool& pool)
{
    return CORE3D_NS::ISceneImporter::Ptr{new GsSceneImporter(ecs, &pool)};
}

BASE_NS::array_view<const BASE_NS::string_view> GsSceneLoader::GetSupportedExtensions() const
{
    return EXTENSIONS;
}
}  // namespace GS