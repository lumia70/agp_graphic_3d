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

#ifndef GAUSSIAN_SPLATTING_NODE_H
#define GAUSSIAN_SPLATTING_NODE_H

#include <gaussian_splatting/builtin_objects.h>
#include <gaussian_splatting/interface/intf_gaussian_edit.h>
#include <gaussian_splatting/interface/intf_gaussian_splatting.h>
#include <scene/base/namespace.h>
#include <scene/ext/ecs_lazy_property.h>
#include <scene/ext/intf_create_entity.h>
#include <scene/ext/node_fwd.h>
#include <scene/ext/scene_property.h>

#include <meta/ext/implementation_macros.h>
#include <meta/ext/object_fwd.h>

GS_ADDON_BEGIN_NAMESPACE()

class GSNode : public META_NS::IntroduceInterfaces<SCENE_NS::NodeFwd, SCENE_NS::ICreateEntity, IGaussianSplatting,
                   IGaussianEdit> {
    META_OBJECT(GSNode, GS_ADDON_NS::ClassId::GSNode, IntroduceInterfaces)

public:
    // clang-format off
    META_BEGIN_STATIC_DATA()

    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, BASE_NS::string, Path, "GsComponent.gsPath")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, size_t, DataOffset, "GsComponent.gsDataOffset")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, UseFadeAnimation, "GsComponent.useFadeAnimation")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, BASE_NS::Math::Vec3, FadeInStartPosition,
        "GsComponent.fadeInStartPosition")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, float, FadeSpeed, "GsComponent.fadeSpeed")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, float, FadeWaitOffset, "GsComponent.fadeWaitOffset")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, float, TimeToFadeFullPoints,
        "GsComponent.timeToFadeFullPoints")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, AutomaticCulling, "GsComponent.useAutomaticCulling")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, float, CullingThreshold,
        "GsComponent.transparentCullingOpacityAbsolute")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, uint32_t, ScaleFactor, "GsComponent.gsScaleFactor")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, EnableDepthPass, "GsComponent.gsEnableDepthPass")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, UseCropBox, "GsComponent.gsUseCropBox")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, BASE_NS::Math::Vec3, CropBoxAABBMin,
        "GsComponent.cropBoxAABBMin")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, BASE_NS::Math::Vec3, CropBoxAABBMax,
        "GsComponent.cropBoxAABBMax")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, Use16bitSurfaceTarget,
        "GsComponent.use16bitSurfaceTarget")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, uint32_t, MaxShDegreeOverride,
        "GsComponent.maxShDegreeOverride")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, ComputeLods, "GsComponent.computeLods")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, float, LodBias, "GsComponent.lodBias")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, ConvertGsColorSrgb, "GsComponent.convertGsColorSrgb")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, RenderingEnabled, "GsComponent.enabled")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, IsLoaded, "GsComponent.isLoaded")
    SCENE_STATIC_DYNINIT_PROPERTY_DATA(IGaussianSplatting, bool, IsPrimitiveValid, "GsComponent.isPrimitiveValid")

    META_END_STATIC_DATA()
    // clang-format on

    META_IMPLEMENT_PROPERTY(BASE_NS::string, Path);
    META_IMPLEMENT_PROPERTY(size_t, DataOffset)
    META_IMPLEMENT_PROPERTY(bool, UseFadeAnimation)
    META_IMPLEMENT_PROPERTY(BASE_NS::Math::Vec3, FadeInStartPosition)
    META_IMPLEMENT_PROPERTY(float, FadeSpeed)
    META_IMPLEMENT_PROPERTY(float, FadeWaitOffset)
    META_IMPLEMENT_PROPERTY(float, TimeToFadeFullPoints)
    META_IMPLEMENT_PROPERTY(bool, AutomaticCulling)
    META_IMPLEMENT_PROPERTY(float, CullingThreshold)
    META_IMPLEMENT_PROPERTY(uint32_t, ScaleFactor)
    META_IMPLEMENT_PROPERTY(bool, EnableDepthPass)
    META_IMPLEMENT_PROPERTY(bool, UseCropBox)
    META_IMPLEMENT_PROPERTY(BASE_NS::Math::Vec3, CropBoxAABBMin)
    META_IMPLEMENT_PROPERTY(BASE_NS::Math::Vec3, CropBoxAABBMax)
    META_IMPLEMENT_PROPERTY(bool, Use16bitSurfaceTarget)
    META_IMPLEMENT_PROPERTY(uint32_t, MaxShDegreeOverride)
    META_IMPLEMENT_PROPERTY(bool, ComputeLods)
    META_IMPLEMENT_PROPERTY(float, LodBias)
    META_IMPLEMENT_PROPERTY(bool, ConvertGsColorSrgb)
    META_IMPLEMENT_PROPERTY(bool, RenderingEnabled)
    META_IMPLEMENT_PROPERTY(bool, IsLoaded)
    META_IMPLEMENT_PROPERTY(bool, IsPrimitiveValid)

    CORE_NS::Entity CreateEntity(const SCENE_NS::IInternalScene::Ptr& scene) override;

    void SetEditingModeEnabled(bool enabled) override;
    void Select(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) override;
    void Select(float minX, float minY, float maxX, float maxY) override;
    void Select(uint32_t width, uint32_t height, uint32_t rowSize, BASE_NS::Format format, void* data) override;
    void Select(const BASE_NS::vector<uint32_t>& indices) override;
    void InvertSelection() override;

    void Paint(BASE_NS::Color, PaintMode mode) override;

    void ClearSelection() override;
    void RemoveSelected() override;
    void RemoveFloaters(float fx, float fy, float cx, float cy,
        uint32_t width, uint32_t height, FloaterRemovalCallback cb) override;
    uint64_t RemoveFloaters(FloaterRemovalMode mode, FloaterRemovalCallback cb) override;
    float GetFloaterRemovalProgress() const override;
    uint64_t GetFloaterRemovalTaskId() const override;
    void TransformSelected(const BASE_NS::Math::Mat4X4& mat) override;
    void Undo() override;

private:
};

GS_ADDON_END_NAMESPACE()

#endif  // GAUSSIAN_SPLATTING_NODE_H
