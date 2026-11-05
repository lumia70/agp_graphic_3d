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

#if !defined(PLUGIN_API_GS_COMPONENT_H) || defined(IMPLEMENT_MANAGER)
#define PLUGIN_API_GS_COMPONENT_H

#ifndef IMPLEMENT_MANAGER
#include <cstdint>

#include <3d/ecs/components/local_matrix_component.h>
#include <3d/namespace.h>
#include <base/containers/vector.h>
#include <base/math/matrix.h>
#include <base/math/vector.h>
#include <core/ecs/component_struct_macros.h>
#include <core/ecs/entity.h>
#include <core/ecs/intf_component_manager.h>
#include <core/namespace.h>
#include <core/property/property_types.h>
#include <render/datastore/intf_render_data_store.h>
#include <render/namespace.h>
#include <render/render_data_structures.h>

namespace GS {
#endif

BEGIN_COMPONENT(IGsComponentManager, GsComponent)

    DEFINE_PROPERTY(BASE_NS::string, gsPath, "GS3D Ply or GLB Path", 0, "")

    // the offset glb embeded in mp4
    DEFINE_PROPERTY(int64_t, gsDataOffset, "GS3D file data offset to start reading from", 0, 0)

    // whether to use the fade-in effect or not
    DEFINE_PROPERTY(bool, useFadeAnimation, "Use fade-in animation", 0, false)
    // use this position as the origin position of the fade
    DEFINE_PROPERTY(
        BASE_NS::Math::Vec3, fadeInStartPosition, "Fade animation start position", 0, BASE_NS::Math::Vec3(0, 0, 0))
    // the fade speed, as meters/second
    DEFINE_PROPERTY(float, fadeSpeed, "Fade animation speed (m/s)", 0, 1.0f)
    // time to wait before starting the effect. Recommended to be at least 1s.
    DEFINE_PROPERTY(float, fadeWaitOffset, "Fade start wait time (s)", 0, 1.0f)
    // time to wait before starting to fade in the full gaussians and not just the points.
    DEFINE_PROPERTY(float, timeToFadeFullPoints, "Time to fade all points", 0, 2.0f)

    // if true, automatically find a good culling value.
    DEFINE_PROPERTY(bool, useAutomaticCulling, "Use automatic pre-culling", 0, false)
    // an absolute value for culling transparent gaussians. If non-zero, automatic culling can never exceed this
    // absolute value.
    DEFINE_PROPERTY(float, transparentCullingOpacityAbsolute, "Pre-culling max opacity threshold", 0, 0.1f)

    // superresolution scaling factor
    DEFINE_PROPERTY(uint32_t, gsScaleFactor, "GS superresolution scale factor", 0, 2)

    // enable or disable the depth pass. Necessary for hybrid rendering. Incurs a performance penalty.
    DEFINE_PROPERTY(bool, gsEnableDepthPass, "Enable GS depth pass", 0, false)

    // enable or disable the use of crop-box
    DEFINE_PROPERTY(bool, gsUseCropBox, "Use GS crop-box", 0, false)
    // AABB min of the crop-box
    DEFINE_PROPERTY(BASE_NS::Math::Vec3, cropBoxAABBMin, "GS crop-box AABB min", 0, BASE_NS::Math::Vec3(-1, -1, -1))
    // AABB max of the crop-box
    DEFINE_PROPERTY(BASE_NS::Math::Vec3, cropBoxAABBMax, "GS crop-box AABB max", 0, BASE_NS::Math::Vec3(1, 1, 1))

    // if true, use 16bit surface targest. 8bit if false.
    DEFINE_PROPERTY(bool, use16bitSurfaceTarget, "Use 16-bit surface target", 0, false)

    // override for the maximum allowed spherical harmonics degree. 3 = use all.
    DEFINE_PROPERTY(uint32_t, maxShDegreeOverride, "Spherical Harmonics max degree override", 0, 3)

    // whether to use the LOD system
    DEFINE_PROPERTY(bool, computeLods, "Use LOD system", 0, false)

    // LOD bias, 1 = no bias, 0 = LOD0
    DEFINE_PROPERTY(float, lodBias, "LOD bias (1 = no bias, 0 = LOD0)", 0, 1.0f)

    // set to true if swapchain is SRGB
    DEFINE_PROPERTY(bool, convertGsColorSrgb, "Convert GS colors to linear when using sRGB render targets", 0, true)

    // whether to render this component or not
    DEFINE_PROPERTY(bool, enabled, "Is rendering enabled", 0, true)

    // GsSystem sets this to true after the model has been loaded succesfully
    DEFINE_PROPERTY(bool, isLoaded, "Indicates whether the model has been loaded succesfully", 0, false)

    // Save gaussian splats position
    DEFINE_PROPERTY(BASE_NS::vector<float>, gsPosData, "gsPosData", 0,)
	
    //save motion data
    DEFINE_PROPERTY(BASE_NS::vector<float>, cameraMotion2d, "A motion 2-dimensional float array", 0,)

    // GsSystem sets this to true after the primitive is valid
    DEFINE_PROPERTY(bool, isPrimitiveValid, "Indicates whether the primitive data is valid", 0, false)

END_COMPONENT(IGsComponentManager, GsComponent, "a1711202-59ed-4735-a795-8b13e7f0af4d")

#ifndef IMPLEMENT_MANAGER
}
#endif
#endif // PLUGIN_API_GS_COMPONENT_H
