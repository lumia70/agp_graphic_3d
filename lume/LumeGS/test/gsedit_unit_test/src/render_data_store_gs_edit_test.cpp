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

#include <gtest/gtest.h>

#include <base/containers/unordered_map.h>
#include <base/containers/vector.h>
#include <base/math/matrix.h>
#include <base/math/vector.h>
#include <base/util/hash.h>
#include <plugin_gs/render/intf_render_data_store_gs_edit.h>

using namespace GS;
using namespace BASE_NS;

// ============================================================================
// GsEditAction struct tests (pure data, no GPU dependencies)
// ============================================================================
TEST(GsEditActionTest, ActionParamsSelectBox)
{
    auto params = make_shared<GS_EDIT_PARAMS::SelectBox>();
    params->min = Math::Vec2(0.1f, 0.2f);
    params->max = Math::Vec2(0.8f, 0.9f);

    GsEditAction action;
    action.type = GsEditActionType::SelectBox;
    action.params = params;

    EXPECT_EQ(action.type, GsEditActionType::SelectBox);
    auto* p = static_cast<GS_EDIT_PARAMS::SelectBox*>(action.params.get());
    EXPECT_FLOAT_EQ(p->min.x, 0.1f);
    EXPECT_FLOAT_EQ(p->max.y, 0.9f);
}

TEST(GsEditActionTest, ActionParamsSelect3DBox)
{
    auto params = make_shared<GS_EDIT_PARAMS::Select3DBox>();
    params->min = Math::Vec3(-1.0f, -2.0f, -3.0f);
    params->max = Math::Vec3(1.0f, 2.0f, 3.0f);

    GsEditAction action;
    action.type = GsEditActionType::Select3DBox;
    action.params = params;

    auto* p = static_cast<GS_EDIT_PARAMS::Select3DBox*>(action.params.get());
    EXPECT_FLOAT_EQ(p->min.x, -1.0f);
    EXPECT_FLOAT_EQ(p->max.z, 3.0f);
}

TEST(GsEditActionTest, ActionParamsTransform)
{
    auto params = make_shared<GS_EDIT_PARAMS::Transform>();
    params->transform = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f);
    params->useSelection = true;

    GsEditAction action;
    action.type = GsEditActionType::Transform;
    action.params = params;

    auto* p = static_cast<GS_EDIT_PARAMS::Transform*>(action.params.get());
    EXPECT_TRUE(p->useSelection);
    EXPECT_FLOAT_EQ(p->transform.data[0], 1.0f);
    EXPECT_FLOAT_EQ(p->transform.data[15], 1.0f);
}

TEST(GsEditActionTest, ActionParamsColorSelection)
{
    auto params = make_shared<GS_EDIT_PARAMS::ColorSelection>();
    params->color = Math::Vec4(1.0f, 0.0f, 0.0f, 1.0f);
    params->colorMode = 2u;

    GsEditAction action;
    action.type = GsEditActionType::ColorSelection;
    action.params = params;

    auto* p = static_cast<GS_EDIT_PARAMS::ColorSelection*>(action.params.get());
    EXPECT_FLOAT_EQ(p->color.x, 1.0f);
    EXPECT_EQ(p->colorMode, 2u);
}

TEST(GsEditActionTest, ActionParamsNoParams)
{
    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();

    GsEditAction action;
    action.type = GsEditActionType::ClearSelection;
    action.params = params;

    auto* p = static_cast<GS_EDIT_PARAMS::NoParams*>(action.params.get());
    EXPECT_NE(p, nullptr);
}

// ============================================================================
// GsEditActionType enum coverage
// ============================================================================
TEST(GsEditActionTypeTest, AllActionTypesAreDistinctAndCounted)
{
    uint32_t values[] = {
        static_cast<uint32_t>(GsEditActionType::SelectBox),
        static_cast<uint32_t>(GsEditActionType::Select3DBox),
        static_cast<uint32_t>(GsEditActionType::SelectIndividual),
        static_cast<uint32_t>(GsEditActionType::SelectByIndex),
        static_cast<uint32_t>(GsEditActionType::SelectByMask),
        static_cast<uint32_t>(GsEditActionType::ClearSelection),
        static_cast<uint32_t>(GsEditActionType::InvertSelection),
        static_cast<uint32_t>(GsEditActionType::RemoveSelected),
        static_cast<uint32_t>(GsEditActionType::Transform),
        static_cast<uint32_t>(GsEditActionType::ColorSelection),
        static_cast<uint32_t>(GsEditActionType::FloaterRemoval),
    };
    // All values are unique
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        for (size_t j = i + 1; j < sizeof(values) / sizeof(values[0]); j++) {
            EXPECT_NE(values[i], values[j]);
        }
    }
    // NumActions sentinel equals the count of real actions
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::NumActions), 11u);
}

// ============================================================================
// hash<GsEditActionType>
// ============================================================================
TEST(GsEditActionTypeTest, HashProducesDeterministicResults)
{
    auto h1 = BASE_NS::hash(GsEditActionType::SelectBox);
    auto h2 = BASE_NS::hash(GsEditActionType::SelectBox);
    EXPECT_EQ(h1, h2);

    auto h3 = BASE_NS::hash(GsEditActionType::Transform);
    EXPECT_NE(h1, h3);

    // Hash equals the underlying uint32_t value
    EXPECT_EQ(h1, static_cast<uint64_t>(GsEditActionType::SelectBox));
    EXPECT_EQ(h3, static_cast<uint64_t>(GsEditActionType::Transform));

    // Usable in unordered_map (hash specialization provided in intf_render_data_store_gs_edit.h)
    BASE_NS::unordered_map<GsEditActionType, int> map;
    map[GsEditActionType::SelectBox] = 1;
    map[GsEditActionType::Transform] = 2;
    EXPECT_EQ(map[GsEditActionType::SelectBox], 1);
    EXPECT_EQ(map[GsEditActionType::Transform], 2);
}

// ============================================================================
// Simulated action stack logic (mirrors RenderDataStoreGsEdit behavior
// without requiring IRenderContext)
// ============================================================================
class GsEditActionStackSimulator {
public:
    uint32_t PushAction(const GsEditAction& action)
    {
        stack_.push_back(action);
        updated_ = true;
        return static_cast<uint32_t>(stack_.size() - 1u);
    }

    uint32_t PopAction()
    {
        stack_.pop_back();
        updated_ = true;
        return static_cast<uint32_t>(stack_.size());
    }

    const BASE_NS::vector<GsEditAction>& GetStack() const
    {
        return stack_;
    }

    void ClearStack()
    {
        stack_.clear();
    }

    bool HasBeenUpdated() const
    {
        return updated_;
    }

    void MarkAsExecuted()
    {
        updated_ = false;
    }

private:
    BASE_NS::vector<GsEditAction> stack_;
    bool updated_{false};
};

// ============================================================================
// Simulated action stack tests
// ============================================================================
TEST(GsEditActionStackTest, PushAndPop)
{
    GsEditActionStackSimulator stack;

    auto params = make_shared<GS_EDIT_PARAMS::SelectBox>();
    params->min = Math::Vec2(0.0f, 0.0f);
    params->max = Math::Vec2(1.0f, 1.0f);

    GsEditAction action;
    action.type = GsEditActionType::SelectBox;
    action.params = params;

    uint32_t idx = stack.PushAction(action);
    EXPECT_EQ(idx, 0u);
    EXPECT_EQ(stack.GetStack().size(), 1u);
    EXPECT_TRUE(stack.HasBeenUpdated());

    stack.MarkAsExecuted();
    EXPECT_FALSE(stack.HasBeenUpdated());

    uint32_t newIdx = stack.PopAction();
    EXPECT_EQ(newIdx, 0u);
    EXPECT_EQ(stack.GetStack().size(), 0u);
}

TEST(GsEditActionStackTest, PushMultiple)
{
    GsEditActionStackSimulator stack;

    for (uint32_t i = 0; i < 5; i++) {
        auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
        GsEditAction action;
        action.type = GsEditActionType::InvertSelection;
        action.params = params;
        uint32_t idx = stack.PushAction(action);
        EXPECT_EQ(idx, i);
    }

    EXPECT_EQ(stack.GetStack().size(), 5u);

    stack.PopAction();
    EXPECT_EQ(stack.GetStack().size(), 4u);
}

TEST(GsEditActionStackTest, ClearStack)
{
    GsEditActionStackSimulator stack;

    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::ClearSelection;
    action.params = params;

    stack.PushAction(action);
    stack.PushAction(action);
    EXPECT_EQ(stack.GetStack().size(), 2u);

    stack.ClearStack();
    EXPECT_EQ(stack.GetStack().size(), 0u);
}

TEST(GsEditActionStackTest, UpdateFlagLifecycle)
{
    GsEditActionStackSimulator stack;
    EXPECT_FALSE(stack.HasBeenUpdated());

    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::ClearSelection;
    action.params = params;

    stack.PushAction(action);
    EXPECT_TRUE(stack.HasBeenUpdated());

    stack.MarkAsExecuted();
    EXPECT_FALSE(stack.HasBeenUpdated());

    stack.PopAction();
    EXPECT_TRUE(stack.HasBeenUpdated());
}

// ============================================================================
// Camera update logic simulation (mirrors RenderDataStoreGsEdit::UpdateCameras)
// ============================================================================
class GsEditCameraUpdateSimulator {
public:
    uint32_t PushAction(const GsEditAction& action)
    {
        stack_.push_back(action);
        uint32_t idx = static_cast<uint32_t>(stack_.size() - 1u);
        queuedUpdates_.push_back(idx);
        return idx;
    }

    void UpdateCameras(const Math::Mat4X4& viewMatrix, const Math::Mat4X4& projMatrix)
    {
        for (const uint32_t idx : queuedUpdates_) {
            if (static_cast<size_t>(idx) >= stack_.size()) {
                continue;
            }
            stack_.at(idx).cameraViewMatrix = viewMatrix;
            stack_.at(idx).cameraViewProjMatrix = projMatrix * viewMatrix;
        }
        queuedUpdates_.clear();
    }

    const GsEditAction& GetAction(size_t idx) const
    {
        return stack_.at(idx);
    }

private:
    BASE_NS::vector<GsEditAction> stack_;
    BASE_NS::vector<uint32_t> queuedUpdates_;
};

TEST(GsEditCameraUpdateTest, UpdateCamerasForPushedAction)
{
    GsEditCameraUpdateSimulator sim;

    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::InvertSelection;
    action.params = params;

    sim.PushAction(action);

    Math::Mat4X4 view = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
        2.0f, 3.0f, 1.0f);
    Math::Mat4X4 proj = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f);

    sim.UpdateCameras(view, proj);

    const auto& updated = sim.GetAction(0);
    EXPECT_FLOAT_EQ(updated.cameraViewMatrix.data[12], 1.0f);
    EXPECT_FLOAT_EQ(updated.cameraViewMatrix.data[13], 2.0f);
    EXPECT_FLOAT_EQ(updated.cameraViewMatrix.data[14], 3.0f);
}

TEST(GsEditCameraUpdateTest, UpdateCamerasOnlyAffectsQueuedActions)
{
    GsEditCameraUpdateSimulator sim;

    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::InvertSelection;
    action.params = params;

    sim.PushAction(action);

    Math::Mat4X4 view1 = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 10.0f,
        20.0f, 30.0f, 1.0f);
    Math::Mat4X4 proj = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f);

    sim.UpdateCameras(view1, proj);

    sim.PushAction(action);

    Math::Mat4X4 view2 = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 40.0f,
        50.0f, 60.0f, 1.0f);

    sim.UpdateCameras(view2, proj);

    const auto& first = sim.GetAction(0);
    const auto& second = sim.GetAction(1);

    EXPECT_FLOAT_EQ(first.cameraViewMatrix.data[12], 10.0f);
    EXPECT_FLOAT_EQ(second.cameraViewMatrix.data[12], 40.0f);
}

// ============================================================================
// Export model state logic simulation (mirrors RenderDataStoreGsEdit)
// ============================================================================
class GsEditExportStateSimulator {
public:
    void SetExportModelFlag(bool flag)
    {
        exportFlag_ = flag;
    }

    bool IsExportModelFlagEnabled() const
    {
        return exportFlag_;
    }

    void UpdateExportModelStatus()
    {
        isDone_ = !exportFlag_;
    }

    bool IsGsExportDone() const
    {
        return isDone_;
    }

    void SetExportModelPath(const string& path)
    {
        path_ = path;
    }

    string GetExportModelPath() const
    {
        return path_;
    }

    void SetExportModelCallback(std::function<void(bool, string_view)> cb)
    {
        cb_ = std::move(cb);
    }

    void TriggerExportModelCallback(bool success, string_view msg)
    {
        if (cb_) {
            cb_(success, msg);
            cb_ = nullptr;
        }
    }

private:
    bool exportFlag_{false};
    bool isDone_{true};
    string path_;
    std::function<void(bool, string_view)> cb_;
};

TEST(GsEditExportStateTest, InitialStateAndExportFlow)
{
    GsEditExportStateSimulator sim;
    EXPECT_FALSE(sim.IsExportModelFlagEnabled());
    EXPECT_TRUE(sim.IsGsExportDone());

    sim.SetExportModelFlag(true);
    EXPECT_TRUE(sim.IsExportModelFlagEnabled());

    sim.UpdateExportModelStatus();
    EXPECT_FALSE(sim.IsGsExportDone());

    sim.SetExportModelPath("/data/test.ply");
    EXPECT_EQ(sim.GetExportModelPath(), "/data/test.ply");

    sim.SetExportModelFlag(false);
    sim.UpdateExportModelStatus();
    EXPECT_TRUE(sim.IsGsExportDone());
}

TEST(GsEditExportStateTest, ExportCallbackFiresAndClears)
{
    GsEditExportStateSimulator sim;
    bool callbackFired = false;
    bool callbackSuccess = false;

    sim.SetExportModelCallback([&](bool success, string_view msg) {
        callbackFired = true;
        callbackSuccess = success;
    });

    sim.TriggerExportModelCallback(true, "");
    EXPECT_TRUE(callbackFired);
    EXPECT_TRUE(callbackSuccess);

    // Second trigger should not fire (callback cleared)
    callbackFired = false;
    sim.TriggerExportModelCallback(false, "error");
    EXPECT_FALSE(callbackFired);
}

// ============================================================================
// Recommended 3D box state logic simulation
// ============================================================================
class GsEdit3DBoxSimulator {
public:
    void SetRecommended3DBoxFlag(bool flag)
    {
        flag_ = flag;
    }

    bool IsGetRecommended3DBoxFlagEnabled() const
    {
        return flag_;
    }

    void SetRecommended3DBox(const pair<Math::Vec3, Math::Vec3>& box)
    {
        box_ = box;
    }

    pair<Math::Vec3, Math::Vec3> GetRecommended3DBox() const
    {
        return box_;
    }

    void SetRecommended3DBoxRay(Math::Vec3& rayOrigin, Math::Vec3& rayDir,
        std::function<void(bool, string_view, const pair<Math::Vec3, Math::Vec3>&)> cb)
    {
        ray_ = {rayOrigin, rayDir};
        cb_ = std::move(cb);
    }

    pair<Math::Vec3, Math::Vec3> GetRecommended3DBoxRay() const
    {
        return ray_;
    }

    void TriggerCallback(bool finished, string_view msg, const pair<Math::Vec3, Math::Vec3>& result)
    {
        if (cb_) {
            cb_(finished, msg, result);
            cb_ = nullptr;
        }
    }

private:
    bool flag_{false};
    pair<Math::Vec3, Math::Vec3> box_{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    pair<Math::Vec3, Math::Vec3> ray_{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}};
    std::function<void(bool, string_view, const pair<Math::Vec3, Math::Vec3>&)> cb_;
};

TEST(GsEdit3DBoxTest, SetAndGet3DBox)
{
    GsEdit3DBoxSimulator sim;
    pair<Math::Vec3, Math::Vec3> box(Math::Vec3(-1.0f, -2.0f, -3.0f), Math::Vec3(1.0f, 2.0f, 3.0f));
    sim.SetRecommended3DBox(box);
    auto result = sim.GetRecommended3DBox();
    EXPECT_FLOAT_EQ(result.first.x, -1.0f);
    EXPECT_FLOAT_EQ(result.second.z, 3.0f);
}

TEST(GsEdit3DBoxTest, SetRayAndCallback)
{
    GsEdit3DBoxSimulator sim;
    bool callbackFired = false;
    Math::Vec3 origin(1.0f, 2.0f, 3.0f);
    Math::Vec3 dir(0.0f, 1.0f, 0.0f);

    sim.SetRecommended3DBoxRay(origin, dir,
        [&](bool finished, string_view msg, const pair<Math::Vec3, Math::Vec3>& result) { callbackFired = true; });

    auto ray = sim.GetRecommended3DBoxRay();
    EXPECT_FLOAT_EQ(ray.first.x, 1.0f);
    EXPECT_FLOAT_EQ(ray.second.y, 1.0f);

    pair<Math::Vec3, Math::Vec3> resultBox(Math::Vec3(-1, -1, -1), Math::Vec3(1, 1, 1));
    sim.TriggerCallback(true, "", resultBox);
    EXPECT_TRUE(callbackFired);

    callbackFired = false;
    sim.TriggerCallback(false, "error", {});
    EXPECT_FALSE(callbackFired);
}

TEST(GsEdit3DBoxTest, FlagState)
{
    GsEdit3DBoxSimulator sim;
    EXPECT_FALSE(sim.IsGetRecommended3DBoxFlagEnabled());

    sim.SetRecommended3DBoxFlag(true);
    EXPECT_TRUE(sim.IsGetRecommended3DBoxFlagEnabled());

    sim.SetRecommended3DBoxFlag(false);
    EXPECT_FALSE(sim.IsGetRecommended3DBoxFlagEnabled());
}

// ============================================================================
// Edit state logic simulation
// ============================================================================
class GsEditStateSimulator {
public:
    void SetEditState(bool state)
    {
        editState_ = state;
    }
    bool GetEditState() const
    {
        return editState_;
    }

private:
    bool editState_{false};
};

TEST(GsEditStateTest, DefaultAndSetState)
{
    GsEditStateSimulator sim;
    EXPECT_FALSE(sim.GetEditState());

    sim.SetEditState(true);
    EXPECT_TRUE(sim.GetEditState());

    sim.SetEditState(false);
    EXPECT_FALSE(sim.GetEditState());
}

// ============================================================================
// GsEditAction struct - camera matrices
// ============================================================================
TEST(GsEditActionTest, ActionCameraViewMatrix)
{
    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::ClearSelection;
    action.params = params;
    action.cameraViewMatrix = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        5.0f, 10.0f, 15.0f, 1.0f);

    EXPECT_FLOAT_EQ(action.cameraViewMatrix.data[12], 5.0f);
    EXPECT_FLOAT_EQ(action.cameraViewMatrix.data[13], 10.0f);
    EXPECT_FLOAT_EQ(action.cameraViewMatrix.data[14], 15.0f);
}

TEST(GsEditActionTest, ActionCameraViewProjMatrix)
{
    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::InvertSelection;
    action.params = params;
    action.cameraViewProjMatrix = Math::Mat4X4(2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);

    EXPECT_FLOAT_EQ(action.cameraViewProjMatrix.data[0], 2.0f);
    EXPECT_FLOAT_EQ(action.cameraViewProjMatrix.data[5], 3.0f);
    EXPECT_FLOAT_EQ(action.cameraViewProjMatrix.data[10], 4.0f);
}

// ============================================================================
// GsEditAction shared_ptr ownership
// ============================================================================
TEST(GsEditActionTest, ActionParamsSharedOwnership)
{
    auto params = make_shared<GS_EDIT_PARAMS::SelectBox>();
    params->min = Math::Vec2(0.0f, 0.0f);
    params->max = Math::Vec2(1.0f, 1.0f);

    GsEditAction action1;
    action1.type = GsEditActionType::SelectBox;
    action1.params = params;

    GsEditAction action2;
    action2.type = GsEditActionType::SelectBox;
    action2.params = params;

    // Both actions share the same params
    auto* p1 = static_cast<GS_EDIT_PARAMS::SelectBox*>(action1.params.get());
    auto* p2 = static_cast<GS_EDIT_PARAMS::SelectBox*>(action2.params.get());
    EXPECT_EQ(p1, p2);
    EXPECT_FLOAT_EQ(p1->min.x, 0.0f);
    EXPECT_FLOAT_EQ(p2->max.x, 1.0f);
}

// ============================================================================
// All param types coverage
// ============================================================================
TEST(GsEditActionTest, SelectByIndexParams)
{
    auto params = make_shared<GS_EDIT_PARAMS::SelectByIndex>();
    uint32_t indices[] = {0, 1, 2, 3};
    params->indices = indices;
    params->bufSize = 4u;

    GsEditAction action;
    action.type = GsEditActionType::SelectByIndex;
    action.params = params;

    auto* p = static_cast<GS_EDIT_PARAMS::SelectByIndex*>(action.params.get());
    EXPECT_EQ(p->indices, indices);
    EXPECT_EQ(p->bufSize, 4u);
}

TEST(GsEditActionTest, SelectByMaskParams)
{
    auto params = make_shared<GS_EDIT_PARAMS::SelectByMask>();
    params->width = 64u;
    params->height = 64u;
    params->rowSize = 8u;
    params->formatByteSize = 1u;

    GsEditAction action;
    action.type = GsEditActionType::SelectByMask;
    action.params = params;

    auto* p = static_cast<GS_EDIT_PARAMS::SelectByMask*>(action.params.get());
    EXPECT_EQ(p->width, 64u);
    EXPECT_EQ(p->height, 64u);
    EXPECT_EQ(p->rowSize, 8u);
    EXPECT_EQ(p->formatByteSize, 1u);
}

// ============================================================================
// GsEditActionType value checks
// ============================================================================
TEST(GsEditActionTypeTest, EnumValuesStartAtZero)
{
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::SelectBox), 0u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::Select3DBox), 1u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::SelectIndividual), 2u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::SelectByIndex), 3u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::SelectByMask), 4u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::ClearSelection), 5u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::InvertSelection), 6u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::RemoveSelected), 7u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::Transform), 8u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::ColorSelection), 9u);
    EXPECT_EQ(static_cast<uint32_t>(GsEditActionType::FloaterRemoval), 10u);
}

// ============================================================================
// Additional export state tests
// ============================================================================
TEST(GsEditExportStateTest, ExportPathManipulation)
{
    GsEditExportStateSimulator sim;

    sim.SetExportModelPath("/data/local/tmp/model.ply");
    EXPECT_EQ(sim.GetExportModelPath(), "/data/local/tmp/model.ply");

    sim.SetExportModelPath("/sdcard/export.glb");
    EXPECT_EQ(sim.GetExportModelPath(), "/sdcard/export.glb");

    sim.SetExportModelPath("");
    EXPECT_EQ(sim.GetExportModelPath(), "");
}

TEST(GsEditExportStateTest, ExportCallbackReceivesMessage)
{
    GsEditExportStateSimulator sim;
    string_view receivedMsg;

    sim.SetExportModelCallback([&](bool success, string_view msg) { receivedMsg = msg; });

    sim.TriggerExportModelCallback(true, "export complete");
    EXPECT_EQ(receivedMsg, "export complete");
}

TEST(GsEditExportStateTest, ExportFlagToggling)
{
    GsEditExportStateSimulator sim;

    sim.SetExportModelFlag(true);
    EXPECT_TRUE(sim.IsExportModelFlagEnabled());

    sim.SetExportModelFlag(false);
    EXPECT_FALSE(sim.IsExportModelFlagEnabled());

    sim.SetExportModelFlag(true);
    EXPECT_TRUE(sim.IsExportModelFlagEnabled());
}

// ============================================================================
// Additional 3D box tests
// ============================================================================
TEST(GsEdit3DBoxTest, BoxDefaultValues)
{
    GsEdit3DBoxSimulator sim;
    auto box = sim.GetRecommended3DBox();
    EXPECT_FLOAT_EQ(box.first.x, 0.0f);
    EXPECT_FLOAT_EQ(box.first.y, 0.0f);
    EXPECT_FLOAT_EQ(box.first.z, 0.0f);
    EXPECT_FLOAT_EQ(box.second.x, 0.0f);
    EXPECT_FLOAT_EQ(box.second.y, 0.0f);
    EXPECT_FLOAT_EQ(box.second.z, 0.0f);
}

TEST(GsEdit3DBoxTest, BoxOverwrite)
{
    GsEdit3DBoxSimulator sim;
    pair<Math::Vec3, Math::Vec3> box1(Math::Vec3(-1.0f, -1.0f, -1.0f), Math::Vec3(1.0f, 1.0f, 1.0f));
    sim.SetRecommended3DBox(box1);
    auto result1 = sim.GetRecommended3DBox();
    EXPECT_FLOAT_EQ(result1.first.x, -1.0f);

    pair<Math::Vec3, Math::Vec3> box2(Math::Vec3(-5.0f, -5.0f, -5.0f), Math::Vec3(5.0f, 5.0f, 5.0f));
    sim.SetRecommended3DBox(box2);
    auto result2 = sim.GetRecommended3DBox();
    EXPECT_FLOAT_EQ(result2.first.x, -5.0f);
    EXPECT_FLOAT_EQ(result2.second.x, 5.0f);
}

TEST(GsEdit3DBoxTest, CallbackReceivesResult)
{
    GsEdit3DBoxSimulator sim;
    pair<Math::Vec3, Math::Vec3> receivedResult;

    Math::Vec3 origin(0.0f, 0.0f, 0.0f);
    Math::Vec3 dir(1.0f, 0.0f, 0.0f);
    sim.SetRecommended3DBoxRay(origin, dir,
        [&](bool finished, string_view msg, const pair<Math::Vec3, Math::Vec3>& result) { receivedResult = result; });

    pair<Math::Vec3, Math::Vec3> expected(Math::Vec3(-2.0f, -2.0f, -2.0f), Math::Vec3(2.0f, 2.0f, 2.0f));
    sim.TriggerCallback(true, "", expected);
    EXPECT_FLOAT_EQ(receivedResult.first.x, -2.0f);
    EXPECT_FLOAT_EQ(receivedResult.second.z, 2.0f);
}

// ============================================================================
// Additional action stack tests
// ============================================================================
TEST(GsEditActionStackTest, PushPopMultipleTypes)
{
    GsEditActionStackSimulator stack;

    auto sbParams = make_shared<GS_EDIT_PARAMS::SelectBox>();
    sbParams->min = Math::Vec2(0.1f, 0.2f);
    sbParams->max = Math::Vec2(0.9f, 0.8f);
    GsEditAction sbAction;
    sbAction.type = GsEditActionType::SelectBox;
    sbAction.params = sbParams;
    stack.PushAction(sbAction);

    auto npParams = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction invAction;
    invAction.type = GsEditActionType::InvertSelection;
    invAction.params = npParams;
    stack.PushAction(invAction);

    EXPECT_EQ(stack.GetStack().size(), 2u);
    EXPECT_EQ(stack.GetStack()[0].type, GsEditActionType::SelectBox);
    EXPECT_EQ(stack.GetStack()[1].type, GsEditActionType::InvertSelection);

    stack.PopAction();
    EXPECT_EQ(stack.GetStack().size(), 1u);
    EXPECT_EQ(stack.GetStack()[0].type, GsEditActionType::SelectBox);
}

TEST(GsEditActionStackTest, ClearEmptyStack)
{
    GsEditActionStackSimulator stack;
    EXPECT_EQ(stack.GetStack().size(), 0u);

    stack.ClearStack();
    EXPECT_EQ(stack.GetStack().size(), 0u);
}

TEST(GsEditActionStackTest, UpdateFlagAfterClear)
{
    GsEditActionStackSimulator stack;
    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::ClearSelection;
    action.params = params;

    stack.PushAction(action);
    stack.MarkAsExecuted();
    EXPECT_FALSE(stack.HasBeenUpdated());

    stack.ClearStack();
    // ClearStack does not set updated flag
    EXPECT_FALSE(stack.HasBeenUpdated());
}

// ============================================================================
// Additional camera update tests
// ============================================================================
TEST(GsEditCameraUpdateTest, ViewProjMatrixComposition)
{
    GsEditCameraUpdateSimulator sim;

    auto params = make_shared<GS_EDIT_PARAMS::NoParams>();
    GsEditAction action;
    action.type = GsEditActionType::InvertSelection;
    action.params = params;
    sim.PushAction(action);

    // Non-identity projection: scale x by 2
    Math::Mat4X4 view = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f,
        2.0f, 3.0f, 1.0f);
    Math::Mat4X4 proj = Math::Mat4X4(2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f);

    sim.UpdateCameras(view, proj);

    const auto& updated = sim.GetAction(0);
    EXPECT_FLOAT_EQ(updated.cameraViewMatrix.data[12], 1.0f);
    // viewProj = proj * view, so translation component scaled
    EXPECT_FLOAT_EQ(updated.cameraViewProjMatrix.data[12], 2.0f);
    EXPECT_FLOAT_EQ(updated.cameraViewProjMatrix.data[13], 2.0f);
}

// ============================================================================
// Edit state additional tests
// ============================================================================
TEST(GsEditStateTest, ToggleMultipleTimes)
{
    GsEditStateSimulator sim;
    EXPECT_FALSE(sim.GetEditState());

    sim.SetEditState(true);
    EXPECT_TRUE(sim.GetEditState());

    sim.SetEditState(false);
    EXPECT_FALSE(sim.GetEditState());

    sim.SetEditState(true);
    EXPECT_TRUE(sim.GetEditState());

    sim.SetEditState(true);
    EXPECT_TRUE(sim.GetEditState());
}

// ============================================================================
// GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX
// ============================================================================
TEST(GsEditConstantTest, RenderDataStoreNamePostFix)
{
    EXPECT_EQ(string(GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX), "RenderDataStoreGsEdit");
    EXPECT_FALSE(string(GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).empty());
    EXPECT_GT(string(GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).length(), 0u);
    EXPECT_GE(string(GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).length(), 1u);
    EXPECT_EQ(string(GS_EDIT_RENDER_DATA_STORE_NAME_POST_FIX).length(), 21u);
}
