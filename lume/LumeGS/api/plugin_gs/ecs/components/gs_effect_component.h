/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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

#if !defined(API_PLUGINGSEFFECT_ECS_COMPONENTS_GS_EFFECT_COMPONENT_H) || defined(IMPLEMENT_MANAGER)
#define API_PLUGINGSEFFECT_ECS_COMPONENTS_GS_EFFECT_COMPONENT_H

#if !defined(IMPLEMENT_MANAGER)
#include <plugin_gs/namespace.h>

#include <base/util/color.h>
#include <core/ecs/component_struct_macros.h>
#include <core/ecs/intf_component_manager.h>
#include <core/namespace.h>

PLUGINGS_BEGIN_NAMESPACE()
#endif

BEGIN_COMPONENT(IGsEffectComponentManager, GsEffectComponent)

    // Retro Effect Configuration
    DEFINE_PROPERTY(bool, retroEffectEnabled, "Retro Effect Enabled", 0, false)
    /** Number of distinct colors to quantize the image to. */
    DEFINE_PROPERTY(uint32_t, retroEffectColorNum, "Retro Effect Color Num", 0, 8u)
    /** Size of each pixelated block in screen pixels. */
    DEFINE_PROPERTY(uint32_t, retroEffectPixelSize, "Retro Effect Pixel Size", 0, 4u)
    /** Toggle between enabling mask blending. */
    DEFINE_PROPERTY(bool, retroEffectBlending, "Retro Effect Mask Blending", 0, true)
    /** Barrel distortion vignette intensity, mimics CRT monitor curvature around screen edges. */
    DEFINE_PROPERTY(float, retroEffectCurve, "Retro Effect Curve", 0, 0.25f)

    // Comic Effect Configuration
    DEFINE_PROPERTY(bool, comicEffectEnabled, "Comic Effect Enabled", 0, false)
    /** Edge detection sensitivity for generating comic book -style outlines. */
    DEFINE_PROPERTY(float, comicEffectLineThreshold, "Comic Effect Line Threshold", 0, 0.2f)
    /** RGB color for drawing the comic outline strokes around edges. */
    DEFINE_PROPERTY(
        BASE_NS::Color, comicEffectLineColor, "Comic Effect Line Color", 0, BASE_NS::Color(0.0f, 0.0f, 0.0f))

    // Watercolor Effect Configuration
    DEFINE_PROPERTY(bool, watercolorEffectEnabled, "Watercolor Effect Enabled", 0, false)
    /** Radius in pixels for watercolor brush effect, i.e. stroke size. */
    DEFINE_PROPERTY(float, watercolorEffectRadius, "Watercolor Effect Radius", 0, 5.0f)
    /** Threshold controlling how aggressively colors blend together. */
    DEFINE_PROPERTY(float, watercolorEffectThreshold, "Watercolor Effect Threshold", 0, 0.5f)

    // Obra-Dinn Effect Configuration
    DEFINE_PROPERTY(bool, obradinnEffectEnabled, "Obra-Dinn Effect Enabled", 0, false)
    /** Intensity of the dithering noise pattern. */
    DEFINE_PROPERTY(float, obradinnEffectNoiseStrength, "Obra-Dinn Effect Noise Strength", 0, 0.3f)
    /** Luminance threshold for converting pixels to two-tone dithered effect. */
    DEFINE_PROPERTY(float, obradinnEffectThreshold, "Obra-Dinn Effect Threshold", 0, 0.4f)
    /** Primary color used for bright/foreground areas. */
    DEFINE_PROPERTY(
        BASE_NS::Color, obradinnEffectForegroundColor, "Obra-Dinn Effect Color1", 0, BASE_NS::Color(0xE5FFFFu))
    /** Secondary color used for dark/background areas. */
    DEFINE_PROPERTY(
        BASE_NS::Color, obradinnEffectBackgroundColor, "Obra-Dinn Effect Color2", 0, BASE_NS::Color(0x333319u))

    // Color Editing Effect Configuration
    DEFINE_PROPERTY(bool, colorEditingEnabled, "Color Editing Enabled", 0, false)
    /** Exposure adjustment in stops. Positive values brighten, negative values darken the image. */
    DEFINE_PROPERTY(float, colorEditingExposure, "Color Editing Exposure", 0, 0.0f)
    /** Contrast multiplier. Values above 1.0f increase contrast, below 1.0f decrease contrast. */
    DEFINE_PROPERTY(float, colorEditingContrast, "Color Editing Contrast", 0, 1.0f)
    /** Color temperature shift. Positive values make warmer (red), negative values cooler (blue). */
    DEFINE_PROPERTY(float, colorEditingTemperature, "Color Editing Temperature", 0, 0.0f)
    /** Tint adjustment. Positive values add magenta, negative values add green to the image. */
    DEFINE_PROPERTY(float, colorEditingTint, "Color Editing Tint", 0, 0.0f)
    /** Color saturation multiplier. Above 1.0f increases vibrancy, below 1.0f makes more muted. */
    DEFINE_PROPERTY(float, colorEditingSaturation, "Color Editing Saturation", 0, 1.0f)
    /** Saturation boost. Enhances muted colors while maintaining saturated colors. */
    DEFINE_PROPERTY(float, colorEditingVibrance, "Color Editing Vibrance", 0, 0.0f)
END_COMPONENT(IGsEffectComponentManager, GsEffectComponent, "9f3c12e7-84a1-4cda-bb73-2e6d5d87a8f4")

#if !defined(IMPLEMENT_MANAGER)
PLUGINGS_END_NAMESPACE()
#endif

#endif // API_PLUGINGSEFFECT_ECS_COMPONENTS_GS_EFFECT_COMPONENT_H
