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

#ifndef RENDER_DATA_STORE_GS_EDIT_H
#define RENDER_DATA_STORE_GS_EDIT_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include <base/containers/array_view.h>
#include <base/containers/pair.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/vector.h>
#include <base/util/uid.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/namespace.h>
#include <core/threading/intf_thread_pool.h>
#include <render/datastore/intf_render_data_store.h>
#include <render/datastore/intf_render_data_store_default_staging.h>
#include <render/intf_render_context.h>
#include <render/resource_handle.h>

#include "plugin_gs/render/intf_render_data_store_gs_edit.h"

namespace RENDER_NS {
class IRenderContext;
class IGpuResourceManager;
class IShaderManager;
class IRenderDataStoreManager;

} // namespace RENDER_NS

namespace GS {

/**
RenderDataStoreGsEdit

 - Handles/Managers gs edit
*/
class RenderDataStoreGsEdit final : public IRenderDataStoreGsEdit {
public:
    RenderDataStoreGsEdit(RENDER_NS::IRenderContext& engine, const BASE_NS::string_view name);
    ~RenderDataStoreGsEdit() override;

    // IRenderDataStore
    void PreRender() override {}
    // Reset and start indexing from the beginning. i.e. frame boundary reset.
    void PostRender() override;
    void PreRenderBackend() override {}
    void PostRenderBackend() override {}
    void Clear() override;

    uint32_t GetFlags() const override
    {
        return 0;
    }

    BASE_NS::string_view GetTypeName() const override
    {
        return TYPE_NAME;
    }

    BASE_NS::string_view GetName() const override
    {
        return name_;
    }

    const BASE_NS::Uid& GetUid() const override
    {
        return UID;
    }

    void Ref() override;
    void Unref() override;
    int32_t GetRefCount() override;

    // for plugin / factory interface
    static constexpr char const *TYPE_NAME = "RenderDataStoreGsEdit";
    static BASE_NS::refcnt_ptr<IRenderDataStore> Create(RENDER_NS::IRenderContext& renderContext, char const* name);

    uint32_t PushGsEditAction(const GsEditAction editAction) override;
    uint32_t PopGsEditAction() override;
    const BASE_NS::vector<GsEditAction>& GetEditActionStack() const override;
    void ClearEditActionStack() override;
    bool HasActionStackBeendUpdated() const override;
    void MarkStackAsExecuted() override;

    // called from GsEditSystem::Update to guarantee camera data update
    void UpdateCameras(const BASE_NS::Math::Mat4X4 viewMatrix, const BASE_NS::Math::Mat4X4 projMatrix) override;

    void SetEditState(const bool state) override;
    bool GetEditState() const override;

    void SetExportModelFlag(const bool flag) override;
    void TriggerExportModelCallback(bool success, BASE_NS::string_view msg) override;
    void TriggerGetRecommended3DBoxCallback(bool finished, BASE_NS::string_view msg,
        const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& result) override;
    void SetExportModelCallback(ExportCb cb) override;
    bool IsExportModelFlagEnabled() const override;
    void SetRecommended3DBoxFlag(const bool flag) override;
    bool IsGetRecommended3DBoxFlagEnabled() const override;
    void UpdateExportModelStatus() override;
    bool IsGsExportDone() const override;
    void SetExportModelPath(const BASE_NS::string& path) override;
    BASE_NS::string GetExportModelPath() const override;

    void SetAIEditResultAvailable() override;
    bool HasAIEditResultAvailable() const override;
    void ClearAIEditResultAvailable() override;

    bool IsAIEditDirtyFlag() const override;
    void SetAIEditDirtyFlag() override;
    void ClearAIEditDirtyFlag() override;

    BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> GetRecommended3DBox() const override;
    void SetRecommended3DBox(const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& recommended3DBox) override;
    void SetRecommended3DBoxRay(BASE_NS::Math::Vec3& rayOrigin, BASE_NS::Math::Vec3& rayDir, Export3DBoxCb cb) override;
    BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> GetRecommended3DBoxRay() const override;

    void SetFloaterRemovalInProgress(bool inProgress) override;
    bool IsFloaterRemovalInProgress() const override;
    void SetFloaterRemovalCallback(FloaterRemovalCb cb) override;
    void TriggerFloaterRemovalCallback(bool success) override;
    void SetFloaterRemovalProgress(float progress) override;
    float GetFloaterRemovalProgress() const override;
    void SetFloaterRemovalTaskId(uint64_t id) override;
    uint64_t GetFloaterRemovalTaskId() const override;

private:
    CORE_NS::IFileManager& fileManager_;
    RENDER_NS::IRenderContext& context_;
    BASE_NS::string name_;
    RENDER_NS::IGpuResourceManager& gpuResourceMgr_;

    std::atomic_int32_t refcnt_ { 0 };

    BASE_NS::vector<GsEditAction> editActionStack_;

    bool actionStackUpdated_ { false };
    bool exportModelFlag_ { false };
    bool recommended3DBoxFlag_ { false };
    bool isExportModelDone_ { false };
    std::atomic<bool> aiEditResultAvailable_ { false };
    std::atomic<bool> aiEditDirtyFlag_ { false };
    BASE_NS::Math::Vec2 mousePos_ { 0.0f, 0.0f };
    BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> recommended3DBox_ { { 0.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f } };
    BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> rayOriginDir_ { { 0.0f, 0.0f, 0.0f },

        { 0.0f, 0.0f, 0.0f } };
    BASE_NS::string exportModelPath_;
    BASE_NS::vector<uint32_t> queuedCameraUpdates_;
    ExportCb exportModelCb_ {};
    Export3DBoxCb getRecommendedBoxCb_ {};

    bool editState_ { false };
    bool floaterRemovalInProgress_ { false };
    FloaterRemovalCb floaterRemovalCb_ {};
    float floaterRemovalProgress_ { 0.0f };
    uint64_t floaterRemovalTaskId_ { 0u };
};
} // namespace GS

#endif // RENDER_DATA_STORE_GS_EDIT_H
