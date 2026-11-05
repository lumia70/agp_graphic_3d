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

#ifndef GS_LOADER_H
#define GS_LOADER_H

#include <plugin_gs/util/intf_gs_loader.h>

#include <3d/loaders/intf_scene_loader.h>
#include <core/plugin/intf_interface_helper.h>

CORE_BEGIN_NAMESPACE()
class IFileManager;
CORE_END_NAMESPACE()

RENDER_BEGIN_NAMESPACE()
class IRenderContext;
RENDER_END_NAMESPACE()

namespace GS {
class IGsSceneData : public CORE_NS::IInterface {
public:
    static constexpr auto UID = BASE_NS::Uid { "3f57ac26-319f-4bd9-8756-a030254b56f1" };
};

class GsSceneData final : public CORE_NS::IInterfaceHelper<CORE3D_NS::ISceneData, IGsSceneData> {
public:
    GsSceneData(BASE_NS::vector<uint8_t>&&);
    GsSceneData(BASE_NS::vector<uint8_t>&&, int64_t);
    ~GsSceneData() = default;

    size_t GetDefaultSceneIndex() const override;
    size_t GetSceneCount() const override;
    BASE_NS::array_view<const uint8_t> GetUri() const;
    int64_t GetLoadOffset() const;

private:
    BASE_NS::vector<uint8_t> data_;
    int64_t offset_ { 0 };
};

class GsSceneLoader final : public CORE_NS::IInterfaceHelper<CORE3D_NS::ISceneLoader, IGsLoader> {
public:
    GsSceneLoader(RENDER_NS::IRenderContext&);
    // allows for partial initialization. (used by tests)
    explicit GsSceneLoader(CORE_NS::IFileManager& fileManager);
    CORE3D_NS::ISceneLoader::Result Load(BASE_NS::string_view uri) override;
    CORE3D_NS::ISceneLoader::Result Load(const BASE_NS::string_view uri, const int64_t offset) override;

    CORE3D_NS::ISceneImporter::Ptr CreateSceneImporter(CORE_NS::IEcs& ecs) override;
    CORE3D_NS::ISceneImporter::Ptr CreateSceneImporter(CORE_NS::IEcs& ecs, CORE_NS::IThreadPool& pool) override;

    BASE_NS::array_view<const BASE_NS::string_view> GetSupportedExtensions() const override;

    // Prevent deleting the instance based on reference counter.
    void Ref() override {}
    void Unref() override {}

private:
    RENDER_NS::IRenderContext* renderContext_;
    CORE_NS::IFileManager& fileManager_;
};
} // namespace GS

#endif // GS_LOADER_H