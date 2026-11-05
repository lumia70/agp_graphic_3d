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

#ifndef RENDER_DATA_STORE_GS_AI_EDIT_H
#define RENDER_DATA_STORE_GS_AI_EDIT_H

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

#include "plugin_gs/render/intf_render_data_store_gs_ai_edit.h"
#include <queue>

namespace RENDER_NS {
class IRenderContext;
class IGpuResourceManager;
class IShaderManager;
class IRenderDataStoreManager;

} // namespace RENDER_NS

namespace GS {

/**
RenderDataStoreGsAIEdit

 - Handles/Managers gs edit
*/
class RenderDataStoreGsAIEdit final : public IRenderDataStoreGsAIEdit {
public:
    RenderDataStoreGsAIEdit(RENDER_NS::IRenderContext &engine, const BASE_NS::string_view name);
    ~RenderDataStoreGsAIEdit() override;

    uint32_t PushOperation(const GsAIEditOp editOp) override
    {
        aiEditOperations_.emplace(editOp);
        return aiEditOperations_.size();
    }
    uint32_t PopOperation() override
    {
        if (aiEditOperations_.empty() == false) {
            aiEditOperations_.pop();
        }
        return aiEditOperations_.size();
    }
    virtual GsAIEditOp GetFrontOperation() override
    {
        return aiEditOperations_.front();
    }
    bool checkOperationQueueEmpty() const override
    {
        return aiEditOperations_.empty();
    }
    // IRenderDataStore
    void PreRender() override
    {
    }
    // Reset and start indexing from the beginning. i.e. frame boundary reset.
    void PostRender() override;
    void PreRenderBackend() override
    {
    }
    void PostRenderBackend() override
    {
    }
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

    const BASE_NS::Uid &GetUid() const override
    {
        return UID;
    }

    void Ref() override;
    void Unref() override;
    int32_t GetRefCount() override;

    // for plugin / factory interface
    static constexpr char const *TYPE_NAME = "RenderDataStoreGsAIEdit";
    static BASE_NS::refcnt_ptr<IRenderDataStore> Create(RENDER_NS::IRenderContext &renderContext, char const *name);

    // void SetEditState(const bool state) override;
    // bool GetEditState() const override;

    void setECS(CORE_NS::IEcs &ecs) override
    {
        ecs_ = &ecs;
    }
    virtual CORE_NS::IEcs &getECS() override
    {
        return *ecs_;
    }

private:
    CORE_NS::IFileManager &fileManager_;
    RENDER_NS::IRenderContext &context_;
    BASE_NS::string name_;
    RENDER_NS::IGpuResourceManager &gpuResourceMgr_;

    std::atomic_int32_t refcnt_{0};

    // BASE_NS::vector<GsEditAction> editActionStack_;
    std::queue<GsAIEditOp> aiEditOperations_;

    BASE_NS::string exportModelPath_;
    BASE_NS::vector<uint32_t> queuedCameraUpdates_;

    CORE_NS::IEcs* ecs_;
};
} // namespace GS

#endif // RENDER_DATA_STORE_GS_EDIT_H
