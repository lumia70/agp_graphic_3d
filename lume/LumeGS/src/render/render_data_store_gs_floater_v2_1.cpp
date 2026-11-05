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

#include "render_data_store_gs_floater_v2_1.h"

namespace GS {

RenderDataStoreGsFloaterV21::RenderDataStoreGsFloaterV21(
    RENDER_NS::IRenderContext& context, BASE_NS::string_view name)
    : context_(context), name_(name)
{}

void RenderDataStoreGsFloaterV21::Ref()
{
    refCount_.fetch_add(1, std::memory_order_relaxed);
}

void RenderDataStoreGsFloaterV21::Unref()
{
    if (refCount_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete this;
    }
}

int32_t RenderDataStoreGsFloaterV21::GetRefCount()
{
    return refCount_.load(std::memory_order_relaxed);
}

BASE_NS::refcnt_ptr<RENDER_NS::IRenderDataStore> RenderDataStoreGsFloaterV21::Create(
    RENDER_NS::IRenderContext& context, char const* name)
{
    return BASE_NS::refcnt_ptr<RENDER_NS::IRenderDataStore>(
        new RenderDataStoreGsFloaterV21(context, name ? name : TYPE_NAME));
}

} // namespace GS
