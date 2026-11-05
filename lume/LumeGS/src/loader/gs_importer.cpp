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

#include "loader/gs_importer.h"

#include <string>

#include <3d/ecs/components/uri_component.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <core/ecs/intf_ecs.h>
#include <core/log.h>
#include <core/ecs/intf_entity_manager.h>
#include <core/threading/intf_thread_pool.h>

#include "loader/gs_loader.h"
#include "plugin_gs/ecs/components/gs_component.h"

namespace GS {
GsSceneImporter::GsSceneImporter(CORE_NS::IEcs& ecs, CORE_NS::IThreadPool* pool) : ecs_(ecs)
{}

GsSceneImporter::~GsSceneImporter() = default;

void GsSceneImporter::ImportResources(const CORE3D_NS::ISceneData::Ptr& data, CORE3D_NS::ResourceImportFlags flags)
{
    ImportResources(data, flags, nullptr);
}

void GsSceneImporter::ImportResources(
    const CORE3D_NS::ISceneData::Ptr& data, CORE3D_NS::ResourceImportFlags flags, Listener* listener)
{
    if (!data) {
        return;
    }
    auto sceneData = data->GetInterface<IGsSceneData>();
    if (!sceneData) {
        return;
    }
    const BASE_NS::array_view<const uint8_t>& plySceneData = static_cast<GsSceneData&>(*sceneData).GetUri();
    const int64_t gsLoadOffset = static_cast<GsSceneData&>(*sceneData).GetLoadOffset();

    const BASE_NS::string filePath =
        BASE_NS::string(reinterpret_cast<const char*>(plySceneData.data()), plySceneData.size());
    BASE_NS::string uri = BASE_NS::string(BASE_NS::to_string(gsLoadOffset)) + "|" + filePath;

    auto entity = ecs_.GetEntityManager().CreateReferenceCounted();
    auto uriManager = CORE_NS::GetManager<CORE3D_NS::IUriComponentManager>(ecs_);
    if (!uriManager) {
        CORE_LOG_E("GsSceneImporter::%s: uriManager is null", __func__);
        ecs_.GetEntityManager().Destroy(entity);
        return;
    }
    uriManager->Create(entity);
    auto uriHandle = uriManager->Write(entity);
    if (!uriHandle) {
        CORE_LOG_E("GsSceneImporter::%s: uriManager Write failed", __func__);
        ecs_.GetEntityManager().Destroy(entity);
        return;
    }
    uriHandle->uri = BASE_NS::move(uri);
    result_.data.meshes.push_back(entity);
}

bool GsSceneImporter::Execute(uint32_t timeBudget)
{
    return {};
}

void GsSceneImporter::Cancel()
{}

bool GsSceneImporter::IsCompleted() const
{
    return {};
}

const CORE3D_NS::ISceneImporter::Result& GsSceneImporter::GetResult() const
{
    return result_;
}

const CORE3D_NS::MeshData& GsSceneImporter::GetMeshData() const
{
    return meshData_;
}

CORE_NS::Entity GsSceneImporter::ImportScene(size_t sceneIndex)
{
    return ImportScene(sceneIndex, 0U, CORE3D_NS::SceneImportFlagBits::CORE_IMPORT_COMPONENT_FLAG_BITS_ALL);
}

CORE_NS::Entity GsSceneImporter::ImportScene(size_t sceneIndex, CORE3D_NS::SceneImportFlags flags)
{
    return ImportScene(sceneIndex, 0U, flags);
}

CORE_NS::Entity GsSceneImporter::ImportScene(size_t sceneIndex, CORE_NS::Entity parentEntity)
{
    return ImportScene(sceneIndex, parentEntity, CORE3D_NS::SceneImportFlagBits::CORE_IMPORT_COMPONENT_FLAG_BITS_ALL);
}

CORE_NS::Entity GsSceneImporter::ImportScene(
    size_t sceneIndex, CORE_NS::Entity parentEntity, CORE3D_NS::SceneImportFlags flags)
{
    auto uriManager = CORE_NS::GetManager<CORE3D_NS::IUriComponentManager>(ecs_);
    auto nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecs_);
    if (result_.data.meshes.empty() || !uriManager || !nodeSystem) {
        return {};
    }
    auto uriHandle = uriManager->Read(result_.data.meshes.back());
    if (!uriHandle) {
        return {};
    }
    auto node = nodeSystem->CreateNode();
    if (!node) {
        CORE_LOG_E("GsSceneImporter::%s: CreateNode failed", __func__);
        return {};
    }

    const BASE_NS::string& fullUri = uriHandle->uri;
    size_t pipePos = fullUri.find('|');

    BASE_NS::string gsPath;
    int64_t gsDataOffset = 0;

    if (pipePos != BASE_NS::string::npos) {
        BASE_NS::string offsetStr = BASE_NS::string(fullUri.substr(0, pipePos));
        gsDataOffset = std::stoll(offsetStr.c_str());

        gsPath = fullUri.substr(pipePos + 1);
    } else {
        // no offset, use the entire string
        gsPath = fullUri;
        gsDataOffset = 0;
    }

    if (CORE_NS::EntityUtil::IsValid(parentEntity)) {
        if (auto parentNode = nodeSystem->GetNode(parentEntity)) {
            node->SetParent(*parentNode);
        }
    }
    auto gsManager = CORE_NS::GetManager<IGsComponentManager>(ecs_);
    gsManager->Create(node->GetEntity());
    auto gsHandle = gsManager->Write(node->GetEntity());
    if (!gsHandle) {
        CORE_LOG_E("GsSceneImporter::%s: gsManager Write failed", __func__);
        return {};
    }
    gsHandle->gsPath = gsPath;
    gsHandle->gsDataOffset = gsDataOffset;
    return node->GetEntity();
}

CORE_NS::Entity GsSceneImporter::ImportScene(size_t sceneIndex, uint32_t sceneId, CORE3D_NS::SceneImportFlags flags)
{
    if (result_.data.meshes.empty()) {
        return {};
    }
    auto uriManager = CORE_NS::GetManager<CORE3D_NS::IUriComponentManager>(ecs_);
    if (!uriManager) {
        CORE_LOG_E("GsSceneImporter::%s: uriManager is null", __func__);
        return {};
    }
    auto uriHandle = uriManager->Read(result_.data.meshes.back());
    if (!uriHandle) {
        CORE_LOG_E("GsSceneImporter::%s: uriManager Read failed", __func__);
        return {};
    }

    const BASE_NS::string& fullUri = uriHandle->uri;
    size_t pipePos = fullUri.find('|');

    BASE_NS::string gsPath;
    int64_t gsDataOffset = 0;

    if (pipePos != BASE_NS::string::npos) {
        BASE_NS::string offsetStr = BASE_NS::string(fullUri.substr(0, pipePos));
        gsDataOffset = std::stoll(offsetStr.c_str());

        gsPath = fullUri.substr(pipePos + 1);
    } else {
        // no offset, use the entire string
        gsPath = fullUri;
        gsDataOffset = 0;
    }

    auto nodeSystem = CORE_NS::GetSystem<CORE3D_NS::INodeSystem>(ecs_);
    if (!nodeSystem) {
        CORE_LOG_E("GsSceneImporter::%s: nodeSystem is null", __func__);
        return {};
    }
    auto node = nodeSystem->CreateNode();
    auto gsManager = CORE_NS::GetManager<IGsComponentManager>(ecs_);
    if (!node || !gsManager) {
        CORE_LOG_E("GsSceneImporter::%s: CreateNode or gsManager failed", __func__);
        return {};
    }
    node->SetSceneId(sceneId);
    gsManager->Create(node->GetEntity());
    auto gsHandle = gsManager->Write(node->GetEntity());
    if (!gsHandle) {
        CORE_LOG_E("GsSceneImporter::%s: gsManager Write failed", __func__);
        return {};
    }
    gsHandle->gsPath = gsPath;
    gsHandle->gsDataOffset = gsDataOffset;
    return node->GetEntity();
}
}  // namespace GS