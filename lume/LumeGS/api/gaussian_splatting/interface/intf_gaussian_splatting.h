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

#ifndef INTF_GAUSSIAN_SPLATTING_H
#define INTF_GAUSSIAN_SPLATTING_H

#include <scene/interface/intf_material.h>
#include <scene/interface/intf_node.h>
#include <scene/interface/intf_mesh.h>
#include <gaussian_splatting/base/namespace.h>

#include <meta/base/types.h>

GS_ADDON_BEGIN_NAMESPACE()

class IGaussianSplatting : public CORE_NS::IInterface {
    META_INTERFACE(CORE_NS::IInterface, IGaussianSplatting, "b2677f35-19b6-4141-9e94-a7947a01f0ab")
public:
    // GS3D Ply or GLB Path
    META_PROPERTY(BASE_NS::string, Path);

    // The offset glb embeded in mp4
    META_PROPERTY(size_t, DataOffset)

    // Whether to use the fade-in effect or not
    META_PROPERTY(bool, UseFadeAnimation)

    // use this position as the origin position of the fade
    META_PROPERTY(BASE_NS::Math::Vec3, FadeInStartPosition)

    // The fade speed, as meters/second
    META_PROPERTY(float, FadeSpeed)

    // Time to wait before starting the effect. Recommended to be at least 1s.
    META_PROPERTY(float, FadeWaitOffset)

    // Time to wait before starting to fade in the full gaussians and not just the points.
    META_PROPERTY(float, TimeToFadeFullPoints)

    // If true, automatically find a good culling value.
    META_PROPERTY(bool, AutomaticCulling)

    // An absolute value for culling transparent gaussians. If non-zero, automatic culling can never exceed this
    // absolute value.
    META_PROPERTY(float, CullingThreshold)

    // Superresolution scaling factor
    META_PROPERTY(uint32_t, ScaleFactor)

    // Enable or disable the depth pass. Necessary for hybrid rendering. Incurs a performance penalty.
    META_PROPERTY(bool, EnableDepthPass)

    // Wnable or disable the use of crop-box
    META_PROPERTY(bool, UseCropBox)

    // AABB min of the crop-box
    META_PROPERTY(BASE_NS::Math::Vec3, CropBoxAABBMin)

    // AABB max of the crop-box
    META_PROPERTY(BASE_NS::Math::Vec3, CropBoxAABBMax)

    // If true, use 16bit surface targest. 8bit if false.
    META_PROPERTY(bool, Use16bitSurfaceTarget)

    // Override for the maximum allowed spherical harmonics degree. 3 = use all.
    META_PROPERTY(uint32_t, MaxShDegreeOverride)

    // Whether to use the LOD system
    META_PROPERTY(bool, ComputeLods)

    // LOD bias, 1 = no bias, 0 = LOD0
    META_PROPERTY(float, LodBias)

    // Set to true if swapchain is SRGB
    META_PROPERTY(bool, ConvertGsColorSrgb)

    // Whether to render this component or not
    META_PROPERTY(bool, RenderingEnabled)

    // GsSystem sets this to true after the model has been loaded succesfully
    META_PROPERTY(bool, IsLoaded)

    // Indicates whether the primitive data is valid
    META_PROPERTY(bool, IsPrimitiveValid)
};
GS_ADDON_END_NAMESPACE()

META_TYPE(GS_ADDON_NS::IGaussianSplatting::WeakPtr);
META_TYPE(GS_ADDON_NS::IGaussianSplatting::Ptr);

#endif // INTF_GAUSSIAN_SPLATTING_H
