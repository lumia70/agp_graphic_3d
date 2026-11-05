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

#include "render_data_store_gs_ai_edit.h"

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

const BASE_NS::string DATA_STORE_CAMERA_NAME{"RenderDataStoreDefaultCamera"};

RenderDataStoreGsAIEdit::RenderDataStoreGsAIEdit(IRenderContext &context, const string_view name)
    : fileManager_(context.GetEngine().GetFileManager()),
      context_(context),
      name_(name.data()),
      gpuResourceMgr_(context_.GetDevice().GetGpuResourceManager())
{
}

RenderDataStoreGsAIEdit::~RenderDataStoreGsAIEdit()
{
}

void RenderDataStoreGsAIEdit::PostRender()
{
    Clear();
}

void RenderDataStoreGsAIEdit::Clear()
{
}

void RenderDataStoreGsAIEdit::Ref()
{
    refcnt_.fetch_add(1, std::memory_order_relaxed);
}

void RenderDataStoreGsAIEdit::Unref()
{
    if (std::atomic_fetch_sub_explicit(&refcnt_, 1, std::memory_order_release) == 1) {
        std::atomic_thread_fence(std::memory_order_acquire);
        delete this;
    }
}

int32_t RenderDataStoreGsAIEdit::GetRefCount()
{
    return refcnt_;
}

// for plugin / factory interface
refcnt_ptr<IRenderDataStore> RenderDataStoreGsAIEdit::Create(IRenderContext &context, char const* name)
{
    return refcnt_ptr<IRenderDataStore>(new RenderDataStoreGsAIEdit(context, name));
}

} // namespace GS
