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

#ifndef GS_IMPORTER_H
#define GS_IMPORTER_H

#include <3d/loaders/intf_scene_loader.h>
#include <core/plugin/intf_interface_helper.h>

namespace GS {
class GsSceneImporter final : public CORE_NS::IInterfaceHelper<CORE3D_NS::ISceneImporter> {
public:
    GsSceneImporter(CORE_NS::IEcs& ecs, CORE_NS::IThreadPool* pool);
    ~GsSceneImporter();

    void ImportResources(const CORE3D_NS::ISceneData::Ptr& data, CORE3D_NS::ResourceImportFlags flags) override;
    void ImportResources(
        const CORE3D_NS::ISceneData::Ptr& data, CORE3D_NS::ResourceImportFlags flags, Listener* listener) override;

    bool Execute(uint32_t timeBudget) override;

    void Cancel() override;

    bool IsCompleted() const override;

    const Result& GetResult() const override;

    const CORE3D_NS::MeshData& GetMeshData() const override;

    CORE_NS::Entity ImportScene(size_t sceneIndex) override;
    CORE_NS::Entity ImportScene(size_t sceneIndex, CORE3D_NS::SceneImportFlags flags) override;
    CORE_NS::Entity ImportScene(size_t sceneIndex, CORE_NS::Entity parentEntity) override;
    CORE_NS::Entity ImportScene(
        size_t sceneIndex, CORE_NS::Entity parentEntity, CORE3D_NS::SceneImportFlags flags) override;
    CORE_NS::Entity ImportScene(size_t sceneIndex, uint32_t sceneId, CORE3D_NS::SceneImportFlags flags) override;

private:
    CORE_NS::IEcs& ecs_;

    CORE3D_NS::ISceneImporter::Result result_;
    CORE3D_NS::MeshData meshData_;
};
} // namespace GS

#endif // GS_IMPORTER_H