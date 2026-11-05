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

#ifndef PLUGIN_API_RENDER_DATA_STORE_GS_AI_EDIT_H
#define PLUGIN_API_RENDER_DATA_STORE_GS_AI_EDIT_H

#include <cstdint>
#include <vector>

#include <3d/ecs/components/local_matrix_component.h>
#include <base/containers/array_view.h>
#include <base/containers/shared_ptr.h>
#include <base/containers/string.h>
#include <base/containers/string_view.h>
#include <base/containers/vector.h>
#include <base/math/matrix.h>
#include <base/math/vector.h>
#include <core/ecs/entity.h>
#include <core/ecs/entity_reference.h>
#include <render/datastore/intf_render_data_store.h>
#include <render/namespace.h>
#include <render/render_data_structures.h>

#include <future>
namespace RENDER_NS {
class IGpuResourceManager;
class IShaderManager;
} // namespace RENDER_NS

namespace GS {

enum class GsAIEditOpType
{
    Non = 0,
    Extract,
    Preview,
    Confirm,
    Undo,
    Discard,
    GetRecommendedBox
};
struct GsAIEditOp
{
    std::shared_ptr<std::promise<std::vector<BASE_NS::Math::UVec4>>> maskData = nullptr;
    std::shared_ptr<std::promise<std::vector<BASE_NS::Math::Vec3>>> boxData = nullptr;
    std::shared_ptr<std::promise<int>> extractResult = nullptr;
    CORE_NS::Entity entity;
    float X; 
    float Y;
    BASE_NS::Math::Vec3 ori_;
    BASE_NS::Math::Vec3 dir_;
    GsAIEditOpType opType = GsAIEditOpType::Non;
};

static constexpr BASE_NS::string_view GS_AI_EDIT_RENDER_DATA_STORE_NAME_POST_FIX = "RenderDataStoreGsAIEdit";

class IRenderDataStoreGsAIEdit : public RENDER_NS::IRenderDataStore {
public:
    static constexpr BASE_NS::Uid UID { "c4cb87e1-5182-4e1a-b8e7-b1646b226d06" };
    virtual uint32_t PushOperation(const GsAIEditOp editOp) = 0;
    virtual uint32_t PopOperation() = 0;
    virtual GsAIEditOp GetFrontOperation() = 0;
    virtual bool checkOperationQueueEmpty() const =0;

    virtual void setECS( CORE_NS::IEcs& ecs)=0;
    virtual CORE_NS::IEcs& getECS()=0;
};

} // namespace GS

#endif // PLUGIN_API_RENDER_DATA_STORE_GS_AI_EDIT_H
