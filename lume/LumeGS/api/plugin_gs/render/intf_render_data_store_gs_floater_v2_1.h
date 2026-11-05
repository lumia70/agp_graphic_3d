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

#ifndef PLUGIN_API_RENDER_DATA_STORE_GS_FLOATER_V2_1_H
#define PLUGIN_API_RENDER_DATA_STORE_GS_FLOATER_V2_1_H

#include <cstdint>

#include <base/containers/string_view.h>
#include <base/util/uid.h>
#include <render/datastore/intf_render_data_store.h>
#include <render/namespace.h>

namespace GS {

static constexpr BASE_NS::string_view GS_FLOATER_V2_1_RENDER_DATA_STORE_NAME_POST_FIX =
    "RenderDataStoreGsFloaterV21";

// Passive buffer holder for the v2.1 floater-removal pipeline.
// All scoring, request/result, and configuration state has been internalized
// into the RenderNode (per D4/D19). This interface exists only so the engine
// can create, reference, and identify the DataStore instance.
class IRenderDataStoreGsFloaterV21 : public RENDER_NS::IRenderDataStore {
public:
    static constexpr BASE_NS::Uid UID { "a66b5116-eaac-4c6e-8901-4639a7456181" };
};

} // namespace GS

#endif // PLUGIN_API_RENDER_DATA_STORE_GS_FLOATER_V2_1_H
