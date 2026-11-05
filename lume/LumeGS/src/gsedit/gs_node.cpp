/*
 * Copyright (C) 2026 Huawei Device Co., Ltd.
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

#include "gaussian_splatting/gs_node.h"

#include <atomic>
#include <plugin_gs/ecs/components/gs_component.h>
#include <plugin_gs/ecs/components/gs_configuration_component.h>
#include <plugin_gs/ecs/systems/intf_gs_edit_system.h>
#include <scene/api/ecs_scene.h>
#include <scene/ext/intf_ecs_object_access.h>
#include <string>

#include <meta/interface/intf_containable.h>

GS_ADDON_BEGIN_NAMESPACE()

void ExecuteCommand(SCENE_NS::IScene::Ptr scene, BASE_NS::string_view command, bool isAsync = true)
{
    if (isAsync) {
        auto ecsScene = SCENE_NS::EcsScene(scene);
        ecsScene.AddEcsThreadTask([ecs = ecsScene.GetEcs(), cmd = BASE_NS::string{command}] {
            if (auto ges = CORE_NS::GetSystem<GS::IGsEditSystem>(*ecs)) {
                ges->PushEditAction(cmd);
            } else {
            }
        });
    } else {
        auto internalScene = scene->GetInternalScene();
        if (!internalScene) {
            CORE_LOG_E("GSEditJS::%s: internalScene is null", __func__);
            return;
        }
        internalScene
            ->AddTaskOrRunDirectly([internalScene, command]() -> META_NS::IAny::Ptr {
                auto ecs = internalScene->GetEcsContext().GetNativeEcs();
                if (!ecs) {
                    CORE_LOG_E("GSEditJS::%s: invalid ecs", __func__);
                    return META_NS::IAny::Ptr{};
                }
                if (auto gsEditSys = CORE_NS::GetSystem<GS::IGsEditSystem>(*ecs)) {
                    gsEditSys->PushEditAction(command.data());
                } else {
                    CORE_LOG_E("GSEditJS::%s: invalid system", __func__);
                    return META_NS::IAny::Ptr{};
                }
                return META_NS::IAny::Ptr{};
            })
            .Wait();
    }
}

template<typename First, typename... Args>
BASE_NS::string JoinToString(First&& first, Args&&... args)
{
    BASE_NS::string result;
    result += std::to_string(first).c_str();
    ((result.append(BASE_NS::string(", ")).append(std::to_string(args).c_str())), ...);
    return result;
}

constexpr size_t FormatByteSize(BASE_NS::Format format)
{
    switch (format) {
        case BASE_NS::Format::BASE_FORMAT_UNDEFINED:
            return 0;
        case BASE_NS::Format::BASE_FORMAT_R8_UNORM:
            return 1;
        case BASE_NS::Format::BASE_FORMAT_R5G6B5_UNORM_PACK16:
            return 2;
        case BASE_NS::Format::BASE_FORMAT_R8G8B8_UNORM:
            return 3;
        case BASE_NS::Format::BASE_FORMAT_R8G8B8A8_UNORM:
        case BASE_NS::Format::BASE_FORMAT_B8G8R8A8_UNORM:
        case BASE_NS::Format::BASE_FORMAT_A2R10G10B10_UNORM_PACK32:
            return 4;
        case BASE_NS::Format::BASE_FORMAT_R16G16B16A16_SFLOAT:
            return 8;
    }
    return 0;
}
CORE_NS::Entity GSNode::CreateEntity(const SCENE_NS::IInternalScene::Ptr& scene)
{
    const auto& ecs = scene->GetEcsContext();
    auto gcm = CORE_NS::GetManager<GS::IGsComponentManager>(*ecs.GetNativeEcs());
    auto gccm = CORE_NS::GetManager<GS::IGsConfigurationComponentManager>(*ecs.GetNativeEcs());
    if (!gcm || !gccm) {
        return {};
    }

    auto ent = ecs.GetNativeEcs()->GetEntityManager().Create();
    ecs.AddDefaultComponents(ent);

    GS::GsComponent gc;
    gcm->Set(ent, gc);

    GS::GsConfigurationComponent gcc;
    gcc.useGpuSort = true;
    gccm->Set(ent, gcc);

    return ent;
}

void GSNode::SetEditingModeEnabled(bool enabled)
{
    auto scene = GetScene();
    if (!scene) {
        CORE_LOG_E("GSEditJS::%s: scene is null", __func__);
        return;
    }
    auto ecsScene = SCENE_NS::EcsScene(scene);

    auto internalScene = scene->GetInternalScene();
    if (!internalScene) {
        CORE_LOG_E("GSEditJS::%s: internalScene is null", __func__);
        return;
    }

    auto ecsObject = SCENE_NS::EcsObject(GetSelf());
    if (!ecsObject || ecsObject.GetEntity() == CORE_NS::Entity{}) {
        CORE_LOG_E("GSEditJS::%s: invalid ecsObject", __func__);
        return;
    }

    auto ecs = internalScene->GetEcsContext().GetNativeEcs();
    if (!ecs) {
        CORE_LOG_E("GSEditJS::%s: invalid ecs", __func__);
        return;
    }

    ecsScene.AddEcsThreadTask([ecs = ecsScene.GetEcs(), entity = ecsObject.GetEntity(), enabled] {
        if (!ecs) {
            CORE_LOG_E("GSEditJS::%s: invalid ecs", __func__);
            return META_NS::IAny::Ptr{};
        }
        if (auto ges = CORE_NS::GetSystem<GS::IGsEditSystem>(*ecs)) {
            auto ett = ges->GetGsConfigEntity();
            if (auto gsConfigManager = CORE_NS::GetManager<GS::IGsConfigurationComponentManager>(*ecs)) {
                if (!CORE_NS::EntityUtil::IsValid(ett)) {
                    gsConfigManager->Create(entity);
                    ett = entity;
                }
                CORE_NS::ScopedHandle<GS::GsConfigurationComponent> data(gsConfigManager->GetData(ett));
                if (!data) {
                    CORE_LOG_E("GSEditJS::%s: config data null", __func__);
                    return META_NS::IAny::Ptr{};
                }
                data->useGpuSort = true;
                data->enableGsEditor = enabled;
            } else {
                CORE_LOG_E("GSEditJS::%s: invalid gsConfigMgr", __func__);
                return META_NS::IAny::Ptr{};
            }
            ges->InitDataStore();
        } else {
            CORE_LOG_E("GSEditJS::%s: invalid ges", __func__);
        }
        return META_NS::IAny::Ptr{};
    });
}

void GSNode::Select(float minX, float minY, float minZ, float maxX, float maxY, float maxZ)
{
    BASE_NS::string command = "select_3d_box [" + JoinToString(minX, minY, minZ, maxX, maxY, maxZ) + "]";

    ExecuteCommand(GetScene(), command);
}

void GSNode::Select(float minX, float minY, float maxX, float maxY)
{
    BASE_NS::string command = "select_box [" + JoinToString(minX, minY, maxX, maxY) + "]";

    ExecuteCommand(GetScene(), command);
}

void GSNode::Select(uint32_t width, uint32_t height, uint32_t rowSize, BASE_NS::Format format, void* data)
{
    BASE_NS::string command = "select_by_mask [" +
                              JoinToString(reinterpret_cast<uintptr_t>(data),
                                  width,
                                  height,
                                  rowSize,
                                  static_cast<uint32_t>(format),
                                  FormatByteSize(format)) +
                              "]";
    ExecuteCommand(GetScene(), command, false);
}

void GSNode::Select(const BASE_NS::vector<uint32_t>& indices)
{
    BASE_NS::string command =
        "select_by_index [" + JoinToString(reinterpret_cast<uintptr_t>(indices.data()), indices.size()) + "]";
    /*
     * to ensure the command data remains valid until the system
     * processes it, we must send the command synchronously.
     */
    ExecuteCommand(GetScene(), command, false);
}

void GSNode::InvertSelection()
{
    ExecuteCommand(GetScene(), "invert_selection");
}

void GSNode::Paint(BASE_NS::Color color, PaintMode mode)
{
    BASE_NS::string command =
        "color_selection [" + JoinToString(color.r, color.g, color.b, color.a, static_cast<uint32_t>(mode)) + "]";
    ExecuteCommand(GetScene(), command);
}

void GSNode::ClearSelection()
{
    ExecuteCommand(GetScene(), "clear_selection");
}

void GSNode::RemoveSelected()
{
    ExecuteCommand(GetScene(), "remove_selected");
}

void GSNode::RemoveFloaters(float fx, float fy, float cx, float cy, uint32_t width, uint32_t height,
    FloaterRemovalCallback cb)
{
    BASE_NS::string command = "remove_floaters [" + JoinToString(fx, fy, cx, cy, width, height) + "]";
    auto scene = GetScene();
    if (cb) {
        auto ecsScene = SCENE_NS::EcsScene(scene);
        ecsScene.AddEcsThreadTask([ecs = ecsScene.GetEcs(), cmd = BASE_NS::move(command),
                                      callback = BASE_NS::move(cb)]() mutable {
            if (auto ges = CORE_NS::GetSystem<GS::IGsEditSystem>(*ecs)) {
                if (auto ds = ges->GetEditDataStore()) {
                    ds->SetFloaterRemovalCallback(BASE_NS::move(callback));
                }
                ges->PushEditAction(cmd);
            }
        });
    } else {
        ExecuteCommand(scene, command);
    }
}

uint64_t GSNode::RemoveFloaters(FloaterRemovalMode mode, FloaterRemovalCallback cb)
{
    // Generate a unique task ID for this floater removal request.
    static std::atomic<uint64_t> sNextTaskId { 1u };
    const uint64_t taskId = sNextTaskId.fetch_add(1, std::memory_order_relaxed);

    // Encode: "remove_floaters [mode, taskId]" — both as floats (ParseType::FLOAT).
    BASE_NS::string command = "remove_floaters [";
    command += BASE_NS::string(std::to_string(static_cast<uint32_t>(mode)).c_str());
    command += ", ";
    command += BASE_NS::string(std::to_string(static_cast<double>(taskId)).c_str());
    command += "]";

    auto scene = GetScene();
    auto ecsScene = SCENE_NS::EcsScene(scene);
    ecsScene.AddEcsThreadTask([ecs = ecsScene.GetEcs(), cmd = BASE_NS::move(command),
                                  callback = BASE_NS::move(cb)]() mutable {
        if (auto ges = CORE_NS::GetSystem<GS::IGsEditSystem>(*ecs)) {
            if (auto ds = ges->GetEditDataStore()) {
                ds->SetFloaterRemovalCallback(BASE_NS::move(callback));
            }
            ges->PushEditAction(cmd);
        }
    });
    return taskId;
}

float GSNode::GetFloaterRemovalProgress() const
{
    auto scene = GetScene();
    auto internalScene = scene->GetInternalScene();
    if (!internalScene) {
        return 0.0f;
    }
    float progress = 0.0f;
    internalScene
        ->AddTaskOrRunDirectly([internalScene, &progress]() -> META_NS::IAny::Ptr {
            auto ecs = internalScene->GetEcsContext().GetNativeEcs();
            if (ecs) {
                if (auto ges = CORE_NS::GetSystem<GS::IGsEditSystem>(*ecs)) {
                    if (auto ds = ges->GetEditDataStore()) {
                        progress = ds->GetFloaterRemovalProgress();
                    }
                }
            }
            return META_NS::IAny::Ptr{};
        })
        .Wait();
    return progress;
}

uint64_t GSNode::GetFloaterRemovalTaskId() const
{
    auto scene = GetScene();
    auto internalScene = scene->GetInternalScene();
    if (!internalScene) {
        return 0u;
    }
    uint64_t taskId = 0u;
    internalScene
        ->AddTaskOrRunDirectly([internalScene, &taskId]() -> META_NS::IAny::Ptr {
            auto ecs = internalScene->GetEcsContext().GetNativeEcs();
            if (ecs) {
                if (auto ges = CORE_NS::GetSystem<GS::IGsEditSystem>(*ecs)) {
                    if (auto ds = ges->GetEditDataStore()) {
                        taskId = ds->GetFloaterRemovalTaskId();
                    }
                }
            }
            return META_NS::IAny::Ptr{};
        })
        .Wait();
    return taskId;
}

void GSNode::TransformSelected(const BASE_NS::Math::Mat4X4& mat4)
{
    BASE_NS::string command = "transform [" +
                              JoinToString(mat4.data[0],
                                  mat4.data[1],
                                  mat4.data[2],
                                  mat4.data[3],
                                  mat4.data[4],
                                  mat4.data[5],
                                  mat4.data[6],
                                  mat4.data[7],
                                  mat4.data[8],
                                  mat4.data[9],
                                  mat4.data[10],
                                  mat4.data[11],
                                  mat4.data[12],
                                  mat4.data[13],
                                  mat4.data[14],
                                  mat4.data[15],
                                  0) +
                              "]";

    ExecuteCommand(GetScene(), command);
}

void GSNode::Undo()
{
    ExecuteCommand(GetScene(), "pop_last");
}

GS_ADDON_END_NAMESPACE()
