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

#include <scene/interface/intf_light.h>
#include <scene/interface/intf_render_configuration.h>
#include <scene/interface/intf_scene.h>
#include <scene/interface/intf_scene_manager.h>

#include <3d/ecs/components/light_component.h>

#include "scene/scene_component_test.h"

SCENE_BEGIN_NAMESPACE()
namespace UTest {

class API_ScenePluginLightComponentTest : public ScenePluginComponentTest<CORE3D_NS::ILightComponentManager> {
protected:
    template<class T>
    void TestILightPropertyGetters(T* light)
    {
        EXPECT_TRUE(light->Color());
        EXPECT_TRUE(light->Intensity());
        EXPECT_TRUE(light->NearPlane());
        EXPECT_TRUE(light->ShadowEnabled());
        EXPECT_TRUE(light->ShadowStrength());
        EXPECT_TRUE(light->ShadowDepthBias());
        EXPECT_TRUE(light->ShadowNormalBias());
        EXPECT_TRUE(light->SpotInnerAngle());
        EXPECT_TRUE(light->SpotOuterAngle());
        EXPECT_TRUE(light->Type());
        EXPECT_TRUE(light->AdditionalFactor());
        EXPECT_TRUE(light->LightLayerMask());
        EXPECT_TRUE(light->ShadowLayerMask());
    }

    void TestILightFunctions(ILight* light)
    {
        ASSERT_TRUE(light);
        TestILightPropertyGetters<const ILight>(light);
        TestILightPropertyGetters<ILight>(light);
        TestInvalidComponentPropertyInit(light);
    }
};

/**
 * @tc.name: Members
 * @tc.desc: Tests for Members. [AUTO-GENERATED]
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, Members, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<BASE_NS::Color>("Color", {0.5, 0.6, 0.7, 0.0}, nativeComponent.color);
    TestEngineProperty<float>("Intensity", 0.5, nativeComponent.intensity);
    TestEngineProperty<float>("NearPlane", 1.2345f, nativeComponent.nearPlane);
    TestEngineProperty<bool>("ShadowEnabled", true, nativeComponent.shadowEnabled);
    TestEngineProperty<float>("ShadowStrength", 0.5, nativeComponent.shadowStrength);
    TestEngineProperty<float>("ShadowDepthBias", 0.5, nativeComponent.shadowDepthBias);
    TestEngineProperty<float>("ShadowNormalBias", 0.5, nativeComponent.shadowNormalBias);
    TestEngineProperty<float>("SpotInnerAngle", 0.5, nativeComponent.spotInnerAngle);
    TestEngineProperty<float>("SpotOuterAngle", 0.5, nativeComponent.spotOuterAngle);
    TestEngineProperty<LightType>("Type", LightType::SPOT, nativeComponent.type);
    TestEngineProperty<BASE_NS::Math::Vec4>("AdditionalFactor", {0.5, 1, 2, 0.6}, nativeComponent.additionalFactor);
    TestEngineProperty<uint64_t>("LightLayerMask", 4, nativeComponent.lightLayerMask);
    TestEngineProperty<uint64_t>("ShadowLayerMask", 128, nativeComponent.shadowLayerMask);
}

/**
 * @tc.name: Functions
 * @tc.desc: Tests for Functions. [AUTO-GENERATED]
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, Functions, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestILightFunctions(interface_cast<ILight>(node));
    TestILightFunctions(interface_cast<ILight>(object));
    TestILightFunctions(interface_cast<ILight>(node));
    TestILightFunctions(interface_cast<ILight>(object));
}

/**
 * @tc.name: Type_Directional
 * @tc.desc: Verify LightComponent Type property read/write with DIRECTIONAL.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, Type_Directional, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    // Default type is DIRECTIONAL, pre-set to SPOT so TestEngineProperty can verify DIRECTIONAL differs from old.
    auto p = GetProperty<LightType>("Type");
    ASSERT_TRUE(p);
    p->SetValue(LightType::SPOT);
    UpdateComponentMembers();

    TestEngineProperty<LightType>("Type", LightType::DIRECTIONAL, nativeComponent.type);
}

/**
 * @tc.name: Type_Point
 * @tc.desc: Verify LightComponent Type property read/write with POINT.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, Type_Point, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<LightType>("Type", LightType::POINT, nativeComponent.type);
}

/**
 * @tc.name: Type_Rect
 * @tc.desc: Verify LightComponent Type property read/write with RECT.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, Type_Rect, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<LightType>("Type", LightType::RECT, nativeComponent.type);
}

/**
 * @tc.name: PointLightRange_SetValue
 * @tc.desc: Verify PointLight Range property read/write with normal value.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, PointLightRange_SetValue, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<float>("Range", 10.0f, nativeComponent.range);
}

/**
 * @tc.name: RectLight_Width_SetValue
 * @tc.desc: Verify RectLight RectWidth property read/write with normal value.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, RectLight_Width_SetValue, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<float>("RectWidth", 2.0f, nativeComponent.rectLight.width);
}

/**
 * @tc.name: RectLight_Width_SetZero
 * @tc.desc: Verify RectLight RectWidth property read/write with value zero.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, RectLight_Width_SetZero, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<float>("RectWidth", 0.0f, nativeComponent.rectLight.width);
}

/**
 * @tc.name: RectLight_Height_SetValue
 * @tc.desc: Verify RectLight RectHeight property read/write with normal value.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, RectLight_Height_SetValue, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<float>("RectHeight", 3.0f, nativeComponent.rectLight.height);
}

/**
 * @tc.name: RectLight_Height_SetZero
 * @tc.desc: Verify RectLight RectHeight property read/write with value zero.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, RectLight_Height_SetZero, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<float>("RectHeight", 0.0f, nativeComponent.rectLight.height);
}

/**
 * @tc.name: RectLight_TwoSided_SetTrue
 * @tc.desc: Verify RectLight RectTwoSided property set to true.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, RectLight_TwoSided_SetTrue, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    TestEngineProperty<bool>("RectTwoSided", true, nativeComponent.rectLight.twoSided);
}

/**
 * @tc.name: RectLight_TwoSided_SetFalse
 * @tc.desc: Verify RectLight RectTwoSided property set to false.
 * @tc.type: FUNC
 */
UNIT_TEST_F(API_ScenePluginLightComponentTest, RectLight_TwoSided_SetFalse, testing::ext::TestSize.Level1)
{
    auto scene = CreateEmptyScene();
    auto node = scene->CreateNode("//test", ClassId::LightNode).GetResult();
    SetComponent(node, "LightComponent");

    // Default twoSided is false, pre-set to true so TestEngineProperty can verify false differs from old.
    auto p = GetProperty<bool>("RectTwoSided");
    ASSERT_TRUE(p);
    p->SetValue(true);
    UpdateComponentMembers();

    TestEngineProperty<bool>("RectTwoSided", false, nativeComponent.rectLight.twoSided);
}

}  // namespace UTest

SCENE_END_NAMESPACE()
