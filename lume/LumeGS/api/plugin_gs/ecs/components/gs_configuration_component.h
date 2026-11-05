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

#if !defined(PLUGIN_API_GS_CONFIGURATION_COMPONENT_H) || defined(IMPLEMENT_MANAGER)
#define PLUGIN_API_GS_CONFIGURATION_COMPONENT_H

#ifndef IMPLEMENT_MANAGER
#include <cstdint>

#include <3d/namespace.h>
#include <base/containers/vector.h>
#include <core/ecs/component_struct_macros.h>
#include <core/ecs/entity.h>
#include <core/ecs/intf_component_manager.h>
#include <core/namespace.h>
#include <core/property/property_types.h>
#include <render/namespace.h>

namespace GS {
#endif

BEGIN_COMPONENT(IGsConfigurationComponentManager, GsConfigurationComponent)

    /** Custom shader for GS animations */
    DEFINE_PROPERTY(CORE_NS::Entity, customAnimationShader, "Custom shader for GS animations", 0, )

    /** Custom resources for GS animations */
    DEFINE_PROPERTY(BASE_NS::vector<CORE_NS::Entity>, customResources, "Custom resources for GS animations", 0, )

    /** Animation running state, set to true to run the animation */
    DEFINE_PROPERTY(bool, isAnimationRunning, "Animation running state, set to true to run the animation", 0, false)

    /** Toggle between animation playback nad pause */
    DEFINE_PROPERTY(bool, isAnimationPaused, "Toggle between animation playback nad pause", 0, false)

    /** Seek to seconds of the animation playback. Set to -1.0f if seeking is not desired */
    DEFINE_PROPERTY(float, animationSeekTime,
        "Seek to seconds of the animation playback. Set to -1.0f if seeking is not desired", 0, -1.0f)

    /** Read-only property, containing the current animation time sent to the shaders */
    DEFINE_PROPERTY(float, currentAnimationTime,
        "Read-only property, containing the current animation time sent to the shaders", 0, 0.0f)

    /** Use GPU sort */
    DEFINE_PROPERTY(bool, useGpuSort, "If true, use GPU sort for sorting the splats. False to use CPU sort", 0, false)

    /** Define the number of frames it will take to compute all spherical harmonics.
     * Larger numbers are often required for good performance on larger scenes. */
    DEFINE_PROPERTY(uint32_t, frameToComputeSphericalHarmonics,
        "Define the number of frames it will take to compute all spherical harmonics. Larger numbers are often "
        "required for good performance on larger scenes.",
        0, 6u)

    /** Enabe the GS real-time editor */
    DEFINE_PROPERTY(bool, enableGsEditor, "Enabe the GS real-time editor", 0, false)

    /** Clip space culling */
    DEFINE_PROPERTY(
        float, frustumScale, "Default only cull behind camera, else additionally cull x/y > w * frustumScale", 0, -1.0f)

    /** Enable GS Picking */
    DEFINE_PROPERTY(bool, enableGsPicking, "If true, enable GS raycast. False to disable", 0, true)

    /** Enable CustomAnimation */
    DEFINE_PROPERTY(bool, enableCustomAnimation, "If true, enable GS custom animation. False to disable", 0, true)

    /** Enable PrecomputePack
     * Only works when GpuSort and CustomAnimation is disabled. */
    DEFINE_PROPERTY(bool, enablePackedData, "If true, use packed GsSplatData and shader", 0, false)

    /** Enable TilePackedData
     * If true, use GsSplatDataTilePacked for tiled mode. */
    DEFINE_PROPERTY(bool, enableTilePackedData, "If true, use tile packed GsSplatData", 0, true)

    /** Cpu scheduling
        Fix the gaussian threads only runs on middle and small core */
    DEFINE_PROPERTY(bool, enableCpuScheduling, "If true, use middle and small core only", 0, false)

    /** Use importance score culling */
    DEFINE_PROPERTY(
        bool, enableScoreCulling, "If true, use importance score algorithm to cull GS, False to disable", 0, false)

    /** Enable DynamicResolution */
    DEFINE_PROPERTY(
        bool, enableDynamicResolution, "If true, resolution will reduce when camera view is moving", 0, false)

    /** Dynamic resolution scale */
    DEFINE_PROPERTY(float, dynamicResolutionScale, "dynamic resolution scale", 0, 1.1f)

    /** Enable 3DGS frame generation */
    DEFINE_PROPERTY(bool, enableFrameGeneration,
        "If true, frame generation for 3dgs rendering will be enabled. False to disable", 0, false)

    // set to true if swapchain is SRGB, overrides the value of GsComponent::convertGsColorSrgb
    DEFINE_PROPERTY(bool, convertGsColorSrgb, "Convert GS colors to linear when using sRGB render targets", 0, true)

    // when true, GPU sort uses 24 bits instead of 32bits. Currently only enabled for LSGS.
    DEFINE_PROPERTY(bool, use24BitGpuSort, "Use 24-bit GPU sort, improves sort performance by 25%", 0, false)

    // The covariance standard deviation multiplier. 3.0f is 99.9999% coverage, 2.0f is ~90% coverage. Higher value
    // means worse performance, do not exceed 3.0f.
    DEFINE_PROPERTY(float, renderCovarianceStddev, "", 0, 2.0f)

    // GS rendering reconstruction sharpenining coefficient
    DEFINE_PROPERTY(float, renderSharpeningCoefficient, "", 0, 0.25f)

    /** LSGS: If true, all out-of-frustum tiles are preloaded at the preload LOD. If false, preload only tiles inside an
     * expanded frustum+preload sphere. */
    DEFINE_PROPERTY(bool, lsgsPreloadFarLod,
        "LSGS: If true, preload all tiles at the preload LOD, false to use frustum+sphere based preload.", 0, false)

    /** LSGS: If true, when the accumulated visible splats exceed lsgsMaxLodRenderCount, low-prio tiles are demoted to
     * coverage LOD. */
    DEFINE_PROPERTY(bool, lsgsEnableSplatBudget,
        "LSGS: If true, enable LSGS splat budget, demote tiles when exceeding lsgsMaxLodRenderCount", 0, true)

    /** LSGS: Minimum total splat count after budget demotions, stop demotion when the projected total would drop below
     * this. */
    DEFINE_PROPERTY(uint32_t, lsgsMinLodRenderCount,
        "LSGS: Minimum splat count after LSGS budget demotion, demotion will not reduce below this value.", 0, 100000u)

    /** LSGS: Enforce at-most this many splats in the frustum. */
    DEFINE_PROPERTY(
        uint32_t, lsgsMaxLodRenderCount, "LSGS: Enforce at-most this many splats in the frustum.", 0, 1000000u)

    /** LSGS: Number of frames to re-check the resident state of a resident tile. */
    DEFINE_PROPERTY(uint32_t, lsgsResidentRecheckFrameInterval,
        "LSGS: Number of frames to re-check the resident state of a resident tile.", 0, 8u)

    /** LSGS: If true, log splat counts each frame. */
    DEFINE_PROPERTY(bool, lsgsLogSplatCounts, "If true, log splat counts each frame.", 0, false)

    /** LSGS: Fov (degrees) of the preload frustum. Set to less than camera FOV to disable. */
    DEFINE_PROPERTY(float, lsgsPreloadYFovDegrees,
        "LSGS: Fov (degrees) of the preload frustum. Set to less than camera FOV to disable.", 0, 110.0f)

    /** LSGS: Radius of the preload sphere. Set to 0.0f to disable sphere preload. */
    DEFINE_PROPERTY(float, lsgsPreloadSphereRadius,
        "LSGS: Radius of the preload sphere. Set to 0.0f to disable sphere preload.", 0, 100.0f)

    /** LSGS: Lod bias scale, multiplier for aabb distance calculation used to select lod levels.
     * common range 0.5 - 2.0. Larger values result in blurrier display, as lower detail lods are used. */
    DEFINE_PROPERTY(float, lsgsLodBias,
        "LSGS: Lod bias scale. Smaller values prefer higher quality lods.", 0, 1.8f)

    /** Enable XEG spatial upscaling. If true, XEG upscaling path is used for rendering. */
    DEFINE_PROPERTY(bool, enableXegUpscale,
        "Enable XEG spatial upscaling. If true, XEG upscaling path is used for rendering.", 0, false)

    /** Spatial upscale factor (e.g., 2.0 for 2x).*/
    DEFINE_PROPERTY(float, xegUpscaleFactor,
        "Spatial upscale factor (e.g., 2.0 for 2x).", 0, 1.0f)

    /** Adhoc frustum culling scale in clip space. Negative value disables GPU culling. */
    DEFINE_PROPERTY(float, adhocCullingScale,
        "Adhoc frustum culling scale in clip space, negative to disable GPU culling", 0, 1.2f)

    DEFINE_PROPERTY(uint32_t, enableGsOpt,
        "Solution for optimizing the sparse Gaussian model. 0: close, 1:open", 0, 0)

    /** LSGS: Render preference preset. 0 = SMOOTH, 1 = BALANCED, 2 = DETAILED. Default is
     * BALANCED. When the value changes, the engine applies the matching preset: SOG cache
     * memory, GPU pool memory, lsgsMaxLodRenderCount and lsgsMinLodRenderCount. Invalid
     * values fall back to the BALANCED preset. */
    DEFINE_PROPERTY(uint32_t, lsgsLevel,
        "LSGS: Render preference preset, 0=SMOOTH, 1=BALANCED, 2=DETAILED. Changes apply the "
        "corresponding memory/gpu/splat-count preset.", 0, 1u)

END_COMPONENT(IGsConfigurationComponentManager, GsConfigurationComponent, "cb54fdec-b8a5-4c87-bc2e-dff71fb498db")

#ifndef IMPLEMENT_MANAGER
}
#endif
#endif // PLUGIN_API_GS_CONFIGURATION_COMPONENT_H
