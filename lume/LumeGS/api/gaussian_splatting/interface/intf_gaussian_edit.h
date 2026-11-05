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

#ifndef INTF_GAUSSIAN_EDIT_H
#define INTF_GAUSSIAN_EDIT_H

#include <functional>
#include <gaussian_splatting/base/namespace.h>
#include <meta/base/types.h>
#include <meta/base/interface_macros.h>
#include <meta/api/future.h>
#include <base/util/formats.h>

GS_ADDON_BEGIN_NAMESPACE()

enum class PaintMode {
    REPLACE,
    MULTIPLY,
    ADD,

};

enum class FloaterRemovalMode : uint32_t {
    SINGLE_VIEW = 0u,
    UNDEFINED = 0xFFFFFFFFu,
};

class IGaussianEdit : public CORE_NS::IInterface {
    META_INTERFACE(CORE_NS::IInterface, IGaussianEdit, "ee7ac3b4-83bf-46f8-92b0-330af5dd1a76")
public:
    using FloaterRemovalCallback = std::function<void(bool success)>;

    virtual void SetEditingModeEnabled(bool enabled) = 0;

    virtual void Select(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) = 0;
    virtual void Select(float minX, float minY, float maxX, float maxY) = 0;
    virtual void Select(const BASE_NS::vector<uint32_t>& indices) = 0;
    virtual void Select(uint32_t width, uint32_t height, uint32_t rowSize, BASE_NS::Format format, void* data) = 0;
    virtual void InvertSelection() = 0;

    virtual void Paint(BASE_NS::Color, PaintMode mode) = 0;

    virtual void ClearSelection() = 0;
    virtual void RemoveSelected() = 0;
    virtual void RemoveFloaters(float fx, float fy, float cx, float cy,
        uint32_t width, uint32_t height, FloaterRemovalCallback cb = nullptr) = 0;
    // New API: camera parameters are read from the current rendering camera.
    // Returns a unique task ID that can be used with GetFloaterRemovalProgress()
    // to poll the progress of this specific task.
    virtual uint64_t RemoveFloaters(FloaterRemovalMode mode, FloaterRemovalCallback cb = nullptr) = 0;
    // Returns the progress (0.0..1.0) of the currently active floater removal
    // pipeline, or the last completed one if none is active.
    virtual float GetFloaterRemovalProgress() const = 0;
    // Returns the task ID of the currently active floater removal pipeline.
    virtual uint64_t GetFloaterRemovalTaskId() const = 0;
    virtual void TransformSelected(const BASE_NS::Math::Mat4X4& mat) = 0;
    virtual void Undo() = 0;
};
GS_ADDON_END_NAMESPACE()

META_TYPE(GS_ADDON_NS::IGaussianEdit::WeakPtr);
META_TYPE(GS_ADDON_NS::IGaussianEdit::Ptr);

#endif // INTF_GAUSSIAN_EDIT_H
