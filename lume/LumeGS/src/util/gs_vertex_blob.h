/*
 * Copyright (c) 2026 Huawei Device Co., Ltd.
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

#ifndef PLUGIN_GS_VERTEX_BLOB_H
#define PLUGIN_GS_VERTEX_BLOB_H

#include <algorithm>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>

#include <render/device/intf_gpu_resource_manager.h>

#include "gpu_vector.h"

namespace GS {
class GsVertexBlob {
private:
    GpuVector<GsObjectLoadData::GsSplatData> data_;
    GpuVector<GsObjectLoadData::GsSplatDataPacked> dataPacked_;
    bool initialized_ { false };
    bool isPacked_ { false };

public:
    GsVertexBlob() = default;
    ~GsVertexBlob() = default;

    void SetPackingEnabled(bool enable);
    void SetGPUAllocator(RENDER_NS::IGpuResourceManager& gpuMgr);

    GsVertexBlob(const GsVertexBlob&) = delete;
    GsVertexBlob& operator=(const GsVertexBlob&) = delete;

    GsVertexBlob(GsVertexBlob&& other) noexcept;
    GsVertexBlob& operator=(GsVertexBlob&& other) noexcept;

    // use setter and getter instead of operator
    void Set(size_t index, const GsObjectLoadData::GsSplatData& data);
    void Set(size_t index, GsObjectLoadData::GsSplatData&& data);

    GsObjectLoadData::GsSplatData Get(size_t index) const;

    size_t Size() const noexcept;

    bool Empty() const noexcept;

    size_t Capacity() const noexcept;

    const void* Data() const noexcept;

    void* Data() noexcept;

    void PushBack(const GsObjectLoadData::GsSplatData& value);

    void Resize(size_t count);

    void Reserve(size_t count);

    void ShrinkToFit();

    // This creates a redundant copy and introduces additional latency when processing packed data internally
    // Called only for offline LOD generation, which is not currently used anywhere
    BASE_NS::vector<GsObjectLoadData::GsSplatData> GetOriginVector() const;

    RENDER_NS::RenderHandleReference FinishGPUAlloc() noexcept;
};
} // namespace GS

#endif // PLUGIN_GS_VERTEX_BLOB_H
