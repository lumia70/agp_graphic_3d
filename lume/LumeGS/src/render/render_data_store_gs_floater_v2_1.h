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

#ifndef RENDER_DATA_STORE_GS_FLOATER_V2_1_H
#define RENDER_DATA_STORE_GS_FLOATER_V2_1_H

#include <atomic>
#include <cstdint>

#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/util/uid.h>
#include <render/datastore/intf_render_data_store.h>
#include <render/intf_render_context.h>

#include "plugin_gs/render/intf_render_data_store_gs_floater_v2_1.h"

namespace GS {

/*
 * Passive data store for the v2.1 floater-removal pipeline.
 * All scoring, request/result, and configuration state lives inside the
 * RenderNode (per D4/D19). This DataStore exists only so the engine can
 * create, reference, and identify the instance.
 * Single-threaded render-thread access - no mutex required.
 */
class RenderDataStoreGsFloaterV21 final : public IRenderDataStoreGsFloaterV21 {
public:
    RenderDataStoreGsFloaterV21(RENDER_NS::IRenderContext& context, BASE_NS::string_view name);
    ~RenderDataStoreGsFloaterV21() override = default;

    // IRenderDataStore
    void PreRender() override {}
    void PostRender() override {}
    void PreRenderBackend() override {}
    void PostRenderBackend() override {}
    void Clear() override {}

    uint32_t GetFlags() const override
    {
        return 0u;
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
    static constexpr char const *TYPE_NAME = "RenderDataStoreGsFloaterV21";
    static BASE_NS::refcnt_ptr<RENDER_NS::IRenderDataStore> Create(
        RENDER_NS::IRenderContext& context, char const *name);

private:
    RENDER_NS::IRenderContext& context_;
    BASE_NS::string name_;
    std::atomic_int32_t refCount_ { 0 };
};

} // namespace GS

#endif // RENDER_DATA_STORE_GS_FLOATER_V2_1_H
