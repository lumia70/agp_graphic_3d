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

#ifndef PLUGIN_API_RENDER_DATA_STORE_GS_EDIT_H
#define PLUGIN_API_RENDER_DATA_STORE_GS_EDIT_H

#include <cstdint>
#include <functional>
#include <vector>

#include <3d/ecs/components/local_matrix_component.h>
#include <base/containers/array_view.h>
#include <base/containers/pair.h>
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

namespace RENDER_NS {
class IGpuResourceManager;
class IShaderManager;
} // namespace RENDER_NS

namespace GS {

struct IGsEditActionParams {
    virtual ~IGsEditActionParams() = default;
    // Used to distinguish param types without RTTI / dynamic_cast.
    virtual bool IsFloaterRemovalOptions() const { return false; }
};

// Action types
enum class GsEditActionType : uint32_t {
    SelectBox = 0u,
    Select3DBox,
    SelectIndividual, // TODO
    SelectByIndex,
    SelectByMask,
    ClearSelection,
    InvertSelection,
    RemoveSelected,
    Transform,
    ColorSelection,
    FloaterRemoval,

    NumActions, // not a valid action, used for acquiring the number of actions
};

// Each action defines its own params
namespace GS_EDIT_PARAMS {
struct SelectBox : IGsEditActionParams {
    BASE_NS::Math::Vec2 min;
    BASE_NS::Math::Vec2 max;
};

struct Select3DBox : IGsEditActionParams {
    BASE_NS::Math::Vec3 min;
    BASE_NS::Math::Vec3 max;
};

struct SelectByIndex : IGsEditActionParams {
    uint32_t* indices;
    size_t bufSize {};
    RENDER_NS::RenderHandleReference buffer;
};

struct SelectByMask : IGsEditActionParams {
    uint8_t* data;
    RENDER_NS::RenderHandleReference image;
    uint64_t width {};
    uint64_t height {};
    uint64_t rowSize {};
    BASE_NS::Format format { BASE_NS::Format::BASE_FORMAT_UNDEFINED };
    uint64_t formatByteSize {};
};

struct Transform : IGsEditActionParams {
    BASE_NS::Math::Mat4X4 transform;
    bool useSelection { true };
};

struct ColorSelection : IGsEditActionParams {
    BASE_NS::Math::Vec4 color;
    uint32_t colorMode { 0 }; // 0 = replace, 1 = multiply, 2 = add
};

struct CameraIntrinsics : IGsEditActionParams {
    float fx { 0.0f };
    float fy { 0.0f };
    float cx { 0.0f };
    float cy { 0.0f };
    uint32_t width { 0u };
    uint32_t height { 0u };
};

struct FloaterRemovalOptions : IGsEditActionParams {
    uint32_t mode { 0u }; // FloaterRemovalMode::SINGLE_VIEW
    uint64_t taskId { 0u }; // unique ID to correlate with the JS FloaterRemovalTask
    bool IsFloaterRemovalOptions() const override { return true; }
};

struct NoParams : IGsEditActionParams {};
} // namespace GS_EDIT_PARAMS

struct GsEditAction {
    GsEditActionType type;
    BASE_NS::shared_ptr<IGsEditActionParams> params;
    // camera view matrix at the time of the action, used for e.g. selection operations
    BASE_NS::Math::Mat4X4 cameraViewMatrix;
    BASE_NS::Math::Mat4X4 cameraViewProjMatrix;
};

static constexpr BASE_NS::string_view GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX = "RenderDataStoreGsEdit";

class IRenderDataStoreGsEdit : public RENDER_NS::IRenderDataStore {
public:
    static constexpr BASE_NS::Uid UID { "c4cb87e1-5182-4e1a-b8e7-b1646b226d05" };

    using ExportCb = std::function<void(bool success, BASE_NS::string_view msg)>;
    using Export3DBoxCb = std::function<void(bool finished, BASE_NS::string_view msg,
        const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& result)>;
    using FloaterRemovalCb = std::function<void(bool success)>;

    virtual uint32_t PushGsEditAction(const GsEditAction editAction) = 0;
    virtual uint32_t PopGsEditAction() = 0;
    virtual void ClearEditActionStack() = 0;

    virtual const BASE_NS::vector<GsEditAction>& GetEditActionStack() const = 0;
    virtual void UpdateCameras(const BASE_NS::Math::Mat4X4 viewMatrix, const BASE_NS::Math::Mat4X4 projMatrix) = 0;

    virtual bool HasActionStackBeendUpdated() const = 0;
    virtual void MarkStackAsExecuted() = 0;

    virtual void SetEditState(const bool state) = 0;
    virtual bool GetEditState() const = 0;

    virtual void SetExportModelFlag(const bool flag) = 0;
    virtual void SetExportModelCallback(ExportCb cb) = 0;
    virtual void TriggerExportModelCallback(bool success, BASE_NS::string_view msg) = 0;
    virtual void TriggerGetRecommended3DBoxCallback(bool finished, BASE_NS::string_view msg,
        const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& result) = 0;
    virtual bool IsExportModelFlagEnabled() const = 0;
    virtual void SetRecommended3DBoxFlag(const bool flag) = 0;
    virtual bool IsGetRecommended3DBoxFlagEnabled() const = 0;
    virtual void UpdateExportModelStatus() = 0;
    virtual bool IsGsExportDone() const = 0;
    virtual void SetExportModelPath(const BASE_NS::string& path) = 0;
    virtual BASE_NS::string GetExportModelPath() const = 0;

    virtual void SetAIEditResultAvailable() = 0;
    virtual bool HasAIEditResultAvailable() const = 0;
    virtual void ClearAIEditResultAvailable() = 0;

    virtual bool IsAIEditDirtyFlag() const = 0;
    virtual void SetAIEditDirtyFlag() = 0;
    virtual void ClearAIEditDirtyFlag() = 0;

    virtual BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> GetRecommended3DBox() const = 0;
    virtual void SetRecommended3DBox(
        const BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3>& recommended3DBox) = 0;
    virtual void SetRecommended3DBoxRay(
        BASE_NS::Math::Vec3& rayOrigin, BASE_NS::Math::Vec3& rayDir, Export3DBoxCb cb) = 0;

    virtual BASE_NS::pair<BASE_NS::Math::Vec3, BASE_NS::Math::Vec3> GetRecommended3DBoxRay() const = 0;

    // Floater removal pipeline spans multiple frames (PREPARE -> SORT -> PEEL x15
    // -> COMPACT). When in progress, SaveToPLY must defer the export until the
    // pipeline completes and edits are applied.
    virtual void SetFloaterRemovalInProgress(bool inProgress) = 0;
    virtual bool IsFloaterRemovalInProgress() const = 0;

    // Callback invoked when the floater removal pipeline completes.
    // success=true means floaters were found and removal actions were pushed;
    // success=false means no floaters were found or an error occurred.
    virtual void SetFloaterRemovalCallback(FloaterRemovalCb cb) = 0;
    virtual void TriggerFloaterRemovalCallback(bool success) = 0;

    // Progress reporting (0.0 .. 1.0) for the currently active pipeline.
    // Written by the render node each frame.
    virtual void SetFloaterRemovalProgress(float progress) = 0;
    virtual float GetFloaterRemovalProgress() const = 0;

    // Task ID for the currently active floater removal pipeline.  The render
    // node sets this when the pipeline starts; JS tasks compare it with their
    // own ID to know whether the reported progress belongs to them.
    virtual void SetFloaterRemovalTaskId(uint64_t id) = 0;
    virtual uint64_t GetFloaterRemovalTaskId() const = 0;
};
} // namespace GS

// hasher for GsEditActionType
namespace Base {
template<>
inline uint64_t hash<GS::GsEditActionType>(const GS::GsEditActionType& value)
{
    return static_cast<uint64_t>(value);
}
} // namespace Base

#endif // PLUGIN_API_RENDER_DATA_STORE_GS_EDIT_H
