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

#include "render_data_store_gs_edit.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <future>
#include <numeric>
#include <securec.h>

#include <3d/render/intf_render_data_store_default_camera.h>
#include <3d/render/intf_render_data_store_default_scene.h>
#include <3d/render/intf_render_node_scene_util.h>
#include <3d/render/render_data_defines_3d.h>
#include <base/containers/array_view.h>
#include <base/math/matrix_util.h>
#include <core/intf_engine.h>
#include <core/log.h>
#include <core/namespace.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/util/parallel_sort.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/resource_handle.h>

#include "app/shaders/common/gs_common.h"
#include "app/shaders/common/gs_struct_common.h"

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;

namespace GS {

const BASE_NS::string DATA_STORE_CAMERA_NAME { "RenderDataStoreDefaultCamera" };

RenderDataStoreGsEdit::RenderDataStoreGsEdit(IRenderContext& context, const string_view name)
    : fileManager_(context.GetEngine().GetFileManager()), context_(context), name_(name.data()),
      gpuResourceMgr_(context_.GetDevice().GetGpuResourceManager())
{}

RenderDataStoreGsEdit::~RenderDataStoreGsEdit() {}

void RenderDataStoreGsEdit::PostRender()
{
    Clear();
}

void RenderDataStoreGsEdit::Clear() {}

void RenderDataStoreGsEdit::Ref()
{
    refcnt_.fetch_add(1, std::memory_order_relaxed);
}

void RenderDataStoreGsEdit::Unref()
{
    if (std::atomic_fetch_sub_explicit(&refcnt_, 1, std::memory_order_release) == 1) {
        std::atomic_thread_fence(std::memory_order_acquire);
        delete this;
    }
}

int32_t RenderDataStoreGsEdit::GetRefCount()
{
    return refcnt_;
}

// for plugin / factory interface
refcnt_ptr<IRenderDataStore> RenderDataStoreGsEdit::Create(IRenderContext& context, char const* name)
{
    return refcnt_ptr<IRenderDataStore>(new RenderDataStoreGsEdit(context, name));
}

uint32_t RenderDataStoreGsEdit::PushGsEditAction(const GsEditAction editAction)
{
    editActionStack_.push_back(editAction);
    actionStackUpdated_ = true;

    const uint32_t actionIdx = static_cast<uint32_t>(editActionStack_.size() - 1u);
    queuedCameraUpdates_.push_back(actionIdx);

    return actionIdx;
}

uint32_t RenderDataStoreGsEdit::PopGsEditAction()
{
    editActionStack_.pop_back();
    actionStackUpdated_ = true;
    return static_cast<uint32_t>(editActionStack_.size());
}

const BASE_NS::vector<GsEditAction>& RenderDataStoreGsEdit::GetEditActionStack() const
{
    return editActionStack_;
}

bool RenderDataStoreGsEdit::HasActionStackBeendUpdated() const
{
    return actionStackUpdated_;
}

void RenderDataStoreGsEdit::MarkStackAsExecuted()
{
    actionStackUpdated_ = false;
}

void RenderDataStoreGsEdit::UpdateCameras(
    const BASE_NS::Math::Mat4X4 viewMatrix, const BASE_NS::Math::Mat4X4 projMatrix)
{
    for (const uint32_t updateIdx : queuedCameraUpdates_) {
        if (static_cast<size_t>(updateIdx) >= editActionStack_.size()) {
            CORE_LOG_E("RenderDataStoreGsEdit::UpdateCameraParametersForActions: index exceeds action stack size!");
            continue;
        }

        editActionStack_.at(updateIdx).cameraViewMatrix = viewMatrix;
        editActionStack_.at(updateIdx).cameraViewProjMatrix = projMatrix * viewMatrix;
    }

    queuedCameraUpdates_.clear();
}

void RenderDataStoreGsEdit::SetEditState(const bool state)
{
    editState_ = state;
}

bool RenderDataStoreGsEdit::GetEditState() const
{
    return editState_;
}

void RenderDataStoreGsEdit::SetExportModelCallback(ExportCb cb)
{
    exportModelCb_ = std::move(cb);
}

void RenderDataStoreGsEdit::TriggerExportModelCallback(bool success, BASE_NS::string_view msg)
{
    if (exportModelCb_) {
        exportModelCb_(success, msg);
        exportModelCb_ = nullptr;
    }
}

void RenderDataStoreGsEdit::TriggerGetRecommended3DBoxCallback(
    bool finished, BASE_NS::string_view msg, const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& result)
{
    if (getRecommendedBoxCb_) {
        getRecommendedBoxCb_(finished, msg, result);
        getRecommendedBoxCb_ = nullptr;
    }
}

void RenderDataStoreGsEdit::SetExportModelFlag(const bool flag)
{
    exportModelFlag_ = flag;
}

void RenderDataStoreGsEdit::SetRecommended3DBoxFlag(const bool flag)
{
    recommended3DBoxFlag_ = flag;
}

bool RenderDataStoreGsEdit::IsExportModelFlagEnabled() const
{
    return exportModelFlag_;
}

bool RenderDataStoreGsEdit::IsGetRecommended3DBoxFlagEnabled() const
{
    return recommended3DBoxFlag_;
}

BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> RenderDataStoreGsEdit::GetRecommended3DBox() const
{
    return recommended3DBox_;
}

void RenderDataStoreGsEdit::SetRecommended3DBox(
    const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& recommended3DBox)
{
    recommended3DBox_ = recommended3DBox;
}

void RenderDataStoreGsEdit::SetRecommended3DBoxRay(
    BASE_NS::Math::Vec3& rayOrigin, BASE_NS::Math::Vec3& rayDir, Export3DBoxCb cb)
{
    rayOriginDir_.first = rayOrigin;
    rayOriginDir_.second = rayDir;
    getRecommendedBoxCb_ = std::move(cb);
}

BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> RenderDataStoreGsEdit::GetRecommended3DBoxRay() const
{
    return rayOriginDir_;
}

void RenderDataStoreGsEdit::SetFloaterRemovalInProgress(bool inProgress)
{
    floaterRemovalInProgress_ = inProgress;
    if (inProgress) {
        floaterRemovalProgress_ = 0.0f;
    }
}

bool RenderDataStoreGsEdit::IsFloaterRemovalInProgress() const
{
    return floaterRemovalInProgress_;
}

void RenderDataStoreGsEdit::SetFloaterRemovalCallback(FloaterRemovalCb cb)
{
    floaterRemovalCb_ = BASE_NS::move(cb);
}

void RenderDataStoreGsEdit::TriggerFloaterRemovalCallback(bool success)
{
    if (floaterRemovalCb_) {
        auto cb = BASE_NS::move(floaterRemovalCb_);
        floaterRemovalCb_ = {};
        cb(success);
    }
}

void RenderDataStoreGsEdit::SetFloaterRemovalProgress(float progress)
{
    floaterRemovalProgress_ = progress;
}

float RenderDataStoreGsEdit::GetFloaterRemovalProgress() const
{
    return floaterRemovalProgress_;
}

void RenderDataStoreGsEdit::SetFloaterRemovalTaskId(uint64_t id)
{
    floaterRemovalTaskId_ = id;
}

uint64_t RenderDataStoreGsEdit::GetFloaterRemovalTaskId() const
{
    return floaterRemovalTaskId_;
}

void RenderDataStoreGsEdit::UpdateExportModelStatus()
{
    isExportModelDone_ = !exportModelFlag_;
}

bool RenderDataStoreGsEdit::IsGsExportDone() const
{
    return isExportModelDone_;
}

void RenderDataStoreGsEdit::SetExportModelPath(const BASE_NS::string& path)
{
    exportModelPath_ = path;
}

BASE_NS::string RenderDataStoreGsEdit::GetExportModelPath() const
{
    return exportModelPath_;
}

void RenderDataStoreGsEdit::ClearEditActionStack()
{
    editActionStack_.clear();
}

void RenderDataStoreGsEdit::SetAIEditResultAvailable()
{
    aiEditResultAvailable_.store(true, std::memory_order_relaxed);
}

bool RenderDataStoreGsEdit::HasAIEditResultAvailable() const
{
    return aiEditResultAvailable_.load(std::memory_order_relaxed);
}

void RenderDataStoreGsEdit::ClearAIEditResultAvailable()
{
    aiEditResultAvailable_.store(false, std::memory_order_relaxed);
}

bool RenderDataStoreGsEdit::IsAIEditDirtyFlag() const
{
    return aiEditDirtyFlag_.load(std::memory_order_relaxed);
}

void RenderDataStoreGsEdit::SetAIEditDirtyFlag()
{
    aiEditDirtyFlag_.store(true, std::memory_order_relaxed);
}

void RenderDataStoreGsEdit::ClearAIEditDirtyFlag()
{
    aiEditDirtyFlag_.store(false, std::memory_order_relaxed);
}

} // namespace GS
