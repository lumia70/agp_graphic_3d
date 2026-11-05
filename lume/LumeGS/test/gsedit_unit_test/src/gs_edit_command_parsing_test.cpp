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

#include <charconv>
#include <cmath>
#include <cstdint>

#include <gtest/gtest.h>

#include <base/containers/allocator.h>
#include <base/containers/string.h>
#include <base/containers/unordered_map.h>
#include <base/util/hash.h>
#include <plugin_gs/render/intf_render_data_store_gs_edit.h>

#include "util/gs_edit_command_parsing.h"

using namespace GS_EDIT;
using namespace GS;
using namespace BASE_NS;

// ============================================================================
// IsWhitespace
// ============================================================================
TEST(GsEditCommandParsingTest, IsWhitespace_AllRecognizedChars)
{
    EXPECT_TRUE(IsWhitespace(' '));
    EXPECT_TRUE(IsWhitespace('\t'));
    EXPECT_TRUE(IsWhitespace('\n'));
    EXPECT_TRUE(IsWhitespace('\r'));
    EXPECT_FALSE(IsWhitespace('a'));
    EXPECT_FALSE(IsWhitespace('0'));
    EXPECT_FALSE(IsWhitespace(','));
    EXPECT_FALSE(IsWhitespace('['));
    EXPECT_FALSE(IsWhitespace(']'));
}

TEST(GsEditCommandParsingTest, IsWhitespace_ControlAndSpecialChars)
{
    EXPECT_FALSE(IsWhitespace('\0'));
    EXPECT_FALSE(IsWhitespace('\1'));
    EXPECT_FALSE(IsWhitespace('\x7F'));
    EXPECT_FALSE(IsWhitespace('!'));
    EXPECT_FALSE(IsWhitespace('@'));
    EXPECT_FALSE(IsWhitespace('#'));
    EXPECT_FALSE(IsWhitespace('$'));
    EXPECT_FALSE(IsWhitespace('%'));
    EXPECT_FALSE(IsWhitespace('^'));
    EXPECT_FALSE(IsWhitespace('&'));
    EXPECT_FALSE(IsWhitespace('*'));
    EXPECT_FALSE(IsWhitespace('('));
    EXPECT_FALSE(IsWhitespace(')'));
    EXPECT_FALSE(IsWhitespace('_'));
    EXPECT_FALSE(IsWhitespace('-'));
    EXPECT_FALSE(IsWhitespace('+'));
    EXPECT_FALSE(IsWhitespace('='));
    EXPECT_FALSE(IsWhitespace('|'));
    EXPECT_FALSE(IsWhitespace('\\'));
    EXPECT_FALSE(IsWhitespace('/'));
}

// ============================================================================
// Load<T>
// ============================================================================
TEST(GsEditCommandParsingTest, LoadFloatAndUintTypes)
{
    float fval = 3.14f;
    auto loadedF = Load<float>(&fval);
    EXPECT_FLOAT_EQ(loadedF, 3.14f);

    uint32_t u32val = 42u;
    auto loadedU32 = Load<uint32_t>(&u32val);
    EXPECT_EQ(loadedU32, 42u);

    uint64_t u64val = 123456789ULL;
    auto loadedU64 = Load<uint64_t>(&u64val);
    EXPECT_EQ(loadedU64, 123456789ULL);

    float negZero = -0.0f;
    auto loadedNegZero = Load<float>(&negZero);
    uint32_t origBits = 0u;
    uint32_t loadedBits = 0u;
    (void)CloneData(&origBits, sizeof(float), &negZero, sizeof(float));
    (void)CloneData(&loadedBits, sizeof(float), &loadedNegZero, sizeof(float));
    EXPECT_EQ(origBits, loadedBits);
}

TEST(GsEditCommandParsingTest, LoadFloatSpecialValues)
{
    float posInf = std::numeric_limits<float>::infinity();
    auto loadedInf = Load<float>(&posInf);
    EXPECT_TRUE(std::isinf(loadedInf));

    float nanVal = std::numeric_limits<float>::quiet_NaN();
    auto loadedNan = Load<float>(&nanVal);
    EXPECT_TRUE(std::isnan(loadedNan));

    float denorm = 1.0e-38f;
    auto loadedDenorm = Load<float>(&denorm);
    EXPECT_FLOAT_EQ(loadedDenorm, denorm);

    float maxFloat = std::numeric_limits<float>::max();
    auto loadedMax = Load<float>(&maxFloat);
    EXPECT_FLOAT_EQ(loadedMax, maxFloat);

    float minFloat = std::numeric_limits<float>::lowest();
    auto loadedMin = Load<float>(&minFloat);
    EXPECT_FLOAT_EQ(loadedMin, minFloat);
}

TEST(GsEditCommandParsingTest, LoadUintBoundaryValues)
{
    uint32_t u32Zero = 0u;
    EXPECT_EQ(Load<uint32_t>(&u32Zero), 0u);

    uint32_t u32Max = std::numeric_limits<uint32_t>::max();
    EXPECT_EQ(Load<uint32_t>(&u32Max), u32Max);

    uint64_t u64Zero = 0ULL;
    EXPECT_EQ(Load<uint64_t>(&u64Zero), 0ULL);

    uint64_t u64Max = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(Load<uint64_t>(&u64Max), u64Max);

    int32_t i32Neg = -1;
    auto loadedNeg = Load<int32_t>(&i32Neg);
    EXPECT_EQ(loadedNeg, -1);
}

// ============================================================================
// ParseFloatParameters
// ============================================================================
TEST(GsEditCommandParsingTest, ParseFloatParameters_SingleValue)
{
    auto result = ParseFloatParameters("1.5");
    ASSERT_EQ(result.size(), sizeof(float));
    float value = 0.0f;
    (void)CloneData(&value, sizeof(float), result.data(), sizeof(float));
    EXPECT_FLOAT_EQ(value, 1.5f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_MultipleValues)
{
    auto result = ParseFloatParameters("1.0, 2.0, 3.0, 4.0");
    ASSERT_EQ(result.size(), 4u * sizeof(float));
    float values[4] = {};
    (void)CloneData(values, sizeof(values), result.data(), sizeof(values));
    EXPECT_FLOAT_EQ(values[0], 1.0f);
    EXPECT_FLOAT_EQ(values[1], 2.0f);
    EXPECT_FLOAT_EQ(values[2], 3.0f);
    EXPECT_FLOAT_EQ(values[3], 4.0f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_NegativeValues)
{
    auto result = ParseFloatParameters("-1.5, -2.5");
    ASSERT_EQ(result.size(), 2u * sizeof(float));
    float values[2] = {};
    (void)CloneData(values, sizeof(values), result.data(), sizeof(values));
    EXPECT_FLOAT_EQ(values[0], -1.5f);
    EXPECT_FLOAT_EQ(values[1], -2.5f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_WhitespaceHandling)
{
    auto result = ParseFloatParameters("  1.0 ,  2.0  ");
    ASSERT_EQ(result.size(), 2u * sizeof(float));
    float values[2] = {};
    (void)CloneData(values, sizeof(values), result.data(), sizeof(values));
    EXPECT_FLOAT_EQ(values[0], 1.0f);
    EXPECT_FLOAT_EQ(values[1], 2.0f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_EmptyAndInvalid)
{
    EXPECT_TRUE(ParseFloatParameters("").empty());
    EXPECT_TRUE(ParseFloatParameters("   ").empty());
    EXPECT_TRUE(ParseFloatParameters("abc").empty());
    // ",," is treated as empty segments between commas, still empty
    EXPECT_TRUE(ParseFloatParameters(",,").empty());
    // "1.0,,2.0" parses 1.0 and 2.0, skipping empty segment
    EXPECT_FALSE(ParseFloatParameters("1.0,,2.0").empty());
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_ScientificNotation)
{
    auto result = ParseFloatParameters("1e2");
    ASSERT_EQ(result.size(), sizeof(float));
    float value = 0.0f;
    (void)CloneData(&value, sizeof(float), result.data(), sizeof(float));
    EXPECT_FLOAT_EQ(value, 100.0f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_IntegerParsedAsFloat)
{
    auto result = ParseFloatParameters("42");
    ASSERT_EQ(result.size(), sizeof(float));
    float value = 0.0f;
    (void)CloneData(&value, sizeof(float), result.data(), sizeof(float));
    EXPECT_FLOAT_EQ(value, 42.0f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_TrailingComma)
{
    auto result = ParseFloatParameters("1.0,");
    ASSERT_EQ(result.size(), sizeof(float));
    float value = 0.0f;
    (void)CloneData(&value, sizeof(float), result.data(), sizeof(float));
    EXPECT_FLOAT_EQ(value, 1.0f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_ManyValues)
{
    auto result = ParseFloatParameters("0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8");
    ASSERT_EQ(result.size(), 8u * sizeof(float));
    float values[8] = {};
    (void)CloneData(values, sizeof(values), result.data(), sizeof(values));
    EXPECT_FLOAT_EQ(values[0], 0.1f);
    EXPECT_FLOAT_EQ(values[3], 0.4f);
    EXPECT_FLOAT_EQ(values[7], 0.8f);
}

TEST(GsEditCommandParsingTest, ParseFloatParameters_ZeroValues)
{
    auto result = ParseFloatParameters("0.0, -0.0");
    ASSERT_EQ(result.size(), 2u * sizeof(float));
    float values[2] = {};
    (void)CloneData(values, sizeof(values), result.data(), sizeof(values));
    EXPECT_FLOAT_EQ(values[0], 0.0f);
    EXPECT_FLOAT_EQ(values[1], 0.0f);
}

// ============================================================================
// ParseUint32Parameters
// ============================================================================
TEST(GsEditCommandParsingTest, ParseUint32Parameters_SingleValue)
{
    auto result = ParseUint32Parameters("42");
    ASSERT_EQ(result.size(), sizeof(uint32_t));
    uint32_t value = 0u;
    (void)CloneData(&value, sizeof(uint32_t), result.data(), sizeof(uint32_t));
    EXPECT_EQ(value, 42u);
}

TEST(GsEditCommandParsingTest, ParseUint32Parameters_MultipleValues)
{
    auto result = ParseUint32Parameters("1, 2, 3");
    ASSERT_EQ(result.size(), 3u * sizeof(uint32_t));
    uint32_t values[3] = {};
    (void)CloneData(values, sizeof(values), result.data(), sizeof(values));
    EXPECT_EQ(values[0], 1u);
    EXPECT_EQ(values[1], 2u);
    EXPECT_EQ(values[2], 3u);
}

TEST(GsEditCommandParsingTest, ParseUint32Parameters_EmptyAndInvalid)
{
    EXPECT_TRUE(ParseUint32Parameters("").empty());
    EXPECT_TRUE(ParseUint32Parameters("abc").empty());
    EXPECT_TRUE(ParseUint32Parameters("   ").empty());
    EXPECT_TRUE(ParseUint32Parameters(",,").empty());
    EXPECT_TRUE(ParseUint32Parameters("1.5").empty());
}

// ============================================================================
// ParseUint64Parameters
// ============================================================================
TEST(GsEditCommandParsingTest, ParseUint64Parameters_SingleValue)
{
    auto result = ParseUint64Parameters("123456789");
    ASSERT_EQ(result.size(), sizeof(uint64_t));
    uint64_t value = 0ULL;
    (void)CloneData(&value, sizeof(uint64_t), result.data(), sizeof(uint64_t));
    EXPECT_EQ(value, 123456789ULL);
}

TEST(GsEditCommandParsingTest, ParseUint64Parameters_MultipleValues)
{
    auto result = ParseUint64Parameters("100, 200");
    ASSERT_EQ(result.size(), 2u * sizeof(uint64_t));
    uint64_t values[2] = {};
    (void)CloneData(values, sizeof(values), result.data(), sizeof(values));
    EXPECT_EQ(values[0], 100ULL);
    EXPECT_EQ(values[1], 200ULL);
}

TEST(GsEditCommandParsingTest, ParseUint64Parameters_EmptyAndInvalid)
{
    EXPECT_TRUE(ParseUint64Parameters("").empty());
    EXPECT_TRUE(ParseUint64Parameters("xyz").empty());
    EXPECT_TRUE(ParseUint64Parameters("   ").empty());
    EXPECT_TRUE(ParseUint64Parameters(",,").empty());
    // "1.5" parses "1" as uint64 before the dot, so result is not empty
    EXPECT_FALSE(ParseUint64Parameters("1.5").empty());
}

// ============================================================================
// ParseGsEditActionString - pop_last
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_PopLast)
{
    auto r1 = ParseGsEditActionString("pop_last");
    EXPECT_TRUE(r1.isPopAction);
    EXPECT_FALSE(r1.editAction.has_value());

    auto r2 = ParseGsEditActionString("  pop_last  ");
    EXPECT_TRUE(r2.isPopAction);
    EXPECT_FALSE(r2.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - select_box
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_SelectBox_Valid)
{
    auto result = ParseGsEditActionString("select_box [0.1, 0.2, 0.8, 0.9]");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::SelectBox);
    EXPECT_FALSE(result.isPopAction);
    auto* p = static_cast<GS_EDIT_PARAMS::SelectBox*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->min.x, 0.1f);
    EXPECT_FLOAT_EQ(p->min.y, 0.2f);
    EXPECT_FLOAT_EQ(p->max.x, 0.8f);
    EXPECT_FLOAT_EQ(p->max.y, 0.9f);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_SelectBox_ZeroAndLargeValues)
{
    auto result = ParseGsEditActionString("select_box [0.0, 0.0, 0.0, 0.0]");
    ASSERT_TRUE(result.editAction.has_value());
    auto* p = static_cast<GS_EDIT_PARAMS::SelectBox*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->min.x, 0.0f);
    EXPECT_FLOAT_EQ(p->min.y, 0.0f);
    EXPECT_FLOAT_EQ(p->max.x, 0.0f);
    EXPECT_FLOAT_EQ(p->max.y, 0.0f);

    result = ParseGsEditActionString("select_box [9999.5, -9999.5, 10000.0, -10000.0]");
    ASSERT_TRUE(result.editAction.has_value());
    p = static_cast<GS_EDIT_PARAMS::SelectBox*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->min.x, 9999.5f);
    EXPECT_FLOAT_EQ(p->min.y, -9999.5f);
    EXPECT_FLOAT_EQ(p->max.x, 10000.0f);
    EXPECT_FLOAT_EQ(p->max.y, -10000.0f);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_SelectBox_WrongParamCount)
{
    auto result = ParseGsEditActionString("select_box [1.0, 2.0]");
    EXPECT_FALSE(result.editAction.has_value());
    EXPECT_FALSE(result.isPopAction);

    result = ParseGsEditActionString("select_box [1.0]");
    EXPECT_FALSE(result.editAction.has_value());

    result = ParseGsEditActionString("select_box [1.0, 2.0, 3.0, 4.0, 5.0]");
    EXPECT_FALSE(result.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - select_3d_box
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_Select3DBox_Valid)
{
    auto result = ParseGsEditActionString("select_3d_box [-1.0, -2.0, -3.0, 1.0, 2.0, 3.0]");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::Select3DBox);
    auto* p = static_cast<GS_EDIT_PARAMS::Select3DBox*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->min.x, -1.0f);
    EXPECT_FLOAT_EQ(p->min.y, -2.0f);
    EXPECT_FLOAT_EQ(p->min.z, -3.0f);
    EXPECT_FLOAT_EQ(p->max.x, 1.0f);
    EXPECT_FLOAT_EQ(p->max.y, 2.0f);
    EXPECT_FLOAT_EQ(p->max.z, 3.0f);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_Select3DBox_WrongParamCount)
{
    auto result = ParseGsEditActionString("select_3d_box [1.0, 2.0]");
    EXPECT_FALSE(result.editAction.has_value());

    result = ParseGsEditActionString("select_3d_box [1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0]");
    EXPECT_FALSE(result.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - transform
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_Transform_IdentityNoSelection)
{
    auto result = ParseGsEditActionString(
        "transform [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, 0]");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::Transform);
    auto* p = static_cast<GS_EDIT_PARAMS::Transform*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->transform.data[0], 1.0f);
    EXPECT_FLOAT_EQ(p->transform.data[5], 1.0f);
    EXPECT_FLOAT_EQ(p->transform.data[10], 1.0f);
    EXPECT_FLOAT_EQ(p->transform.data[15], 1.0f);
    EXPECT_FLOAT_EQ(p->transform.data[1], 0.0f);
    EXPECT_FLOAT_EQ(p->transform.data[4], 0.0f);
    EXPECT_FALSE(p->useSelection);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_Transform_WithSelection)
{
    auto result = ParseGsEditActionString(
        "transform [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, 1]");
    ASSERT_TRUE(result.editAction.has_value());
    auto* p = static_cast<GS_EDIT_PARAMS::Transform*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_TRUE(p->useSelection);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_Transform_NonIdentityMatrix)
{
    auto result = ParseGsEditActionString(
        "transform [2,0,0,0, 0,3,0,0, 0,0,4,0, 1,2,3,1, 0]");
    ASSERT_TRUE(result.editAction.has_value());
    auto* p = static_cast<GS_EDIT_PARAMS::Transform*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->transform.data[0], 2.0f);
    EXPECT_FLOAT_EQ(p->transform.data[5], 3.0f);
    EXPECT_FLOAT_EQ(p->transform.data[10], 4.0f);
    EXPECT_FLOAT_EQ(p->transform.data[12], 1.0f);
    EXPECT_FLOAT_EQ(p->transform.data[13], 2.0f);
    EXPECT_FLOAT_EQ(p->transform.data[14], 3.0f);
    EXPECT_FALSE(p->useSelection);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_Transform_WrongParamCount)
{
    auto result = ParseGsEditActionString("transform [1,0,0,0]");
    EXPECT_FALSE(result.editAction.has_value());
    EXPECT_FALSE(result.isPopAction);

    result = ParseGsEditActionString("transform [1,0,0,0, 0,1,0,0]");
    EXPECT_FALSE(result.editAction.has_value());

    result = ParseGsEditActionString("transform []");
    EXPECT_FALSE(result.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - color_selection
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_ColorSelection_ReplaceMode)
{
    auto result = ParseGsEditActionString("color_selection [1.0, 0.5, 0.25, 1.0, 0]");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::ColorSelection);
    auto* p = static_cast<GS_EDIT_PARAMS::ColorSelection*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_FLOAT_EQ(p->color.x, 1.0f);
    EXPECT_FLOAT_EQ(p->color.y, 0.5f);
    EXPECT_FLOAT_EQ(p->color.z, 0.25f);
    EXPECT_FLOAT_EQ(p->color.w, 1.0f);
    EXPECT_EQ(p->colorMode, 0u);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_ColorSelection_MultiplyMode)
{
    auto result = ParseGsEditActionString("color_selection [1.0, 1.0, 1.0, 1.0, 1]");
    ASSERT_TRUE(result.editAction.has_value());
    auto* p = static_cast<GS_EDIT_PARAMS::ColorSelection*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->colorMode, 1u);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_ColorSelection_WrongParamCount)
{
    auto result = ParseGsEditActionString("color_selection [1.0, 2.0]");
    EXPECT_FALSE(result.editAction.has_value());

    result = ParseGsEditActionString("color_selection [1.0, 2.0, 3.0, 4.0]");
    EXPECT_FALSE(result.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - select_by_index
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_SelectByIndex_Valid)
{
    uint32_t indices[] = {0, 1, 2};
    char addrBuf[32];
    auto addrRes = std::to_chars(
        addrBuf, addrBuf + sizeof(addrBuf), reinterpret_cast<uintptr_t>(indices));
    string addrStr(addrBuf, static_cast<size_t>(addrRes.ptr - addrBuf));
    char lenBuf[32];
    auto lenRes = std::to_chars(lenBuf, lenBuf + sizeof(lenBuf), 3ULL);
    string lenStr(lenBuf, static_cast<size_t>(lenRes.ptr - lenBuf));
    string command = "select_by_index [" + addrStr + ", " + lenStr + "]";

    auto result = ParseGsEditActionString(command);
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::SelectByIndex);
    auto* p = static_cast<GS_EDIT_PARAMS::SelectByIndex*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->indices, indices);
    EXPECT_EQ(p->bufSize, 3ULL);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_SelectByIndex_WrongParamCount)
{
    auto result = ParseGsEditActionString("select_by_index [123]");
    EXPECT_FALSE(result.editAction.has_value());

    result = ParseGsEditActionString("select_by_index [1, 2, 3]");
    EXPECT_FALSE(result.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - select_by_mask
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_SelectByMask_Valid)
{
    uint8_t maskData[] = {0xFF, 0x00};
    char addrBuf[32];
    auto addrRes = std::to_chars(
        addrBuf, addrBuf + sizeof(addrBuf), reinterpret_cast<uintptr_t>(maskData));
    string addrStr(addrBuf, static_cast<size_t>(addrRes.ptr - addrBuf));
    string command = "select_by_mask [" + addrStr + ", 2, 2, 2, 1, 1]";

    auto result = ParseGsEditActionString(command);
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::SelectByMask);
    auto* p = static_cast<GS_EDIT_PARAMS::SelectByMask*>(result.editAction->params.get());
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->width, 2ULL);
    EXPECT_EQ(p->height, 2ULL);
    EXPECT_EQ(p->rowSize, 2ULL);
    EXPECT_EQ(p->formatByteSize, 1ULL);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_SelectByMask_WrongParamCount)
{
    auto result = ParseGsEditActionString("select_by_mask [100, 200]");
    EXPECT_FALSE(result.editAction.has_value());

    result = ParseGsEditActionString("select_by_mask [1, 2, 3, 4]");
    EXPECT_FALSE(result.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - no-param commands
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_NoParamCommands_ClearAndInvert)
{
    auto result = ParseGsEditActionString("clear_selection");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::ClearSelection);
    EXPECT_NE(static_cast<GS_EDIT_PARAMS::NoParams*>(result.editAction->params.get()), nullptr);

    result = ParseGsEditActionString("invert_selection");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::InvertSelection);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_NoParamCommands_RemoveAndIndividual)
{
    auto result = ParseGsEditActionString("remove_selected");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::RemoveSelected);

    result = ParseGsEditActionString("select_individual");
    ASSERT_TRUE(result.editAction.has_value());
    EXPECT_EQ(result.editAction->type, GsEditActionType::SelectIndividual);
}

// ============================================================================
// ParseGsEditActionString - invalid input
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_UnknownAndEmpty)
{
    auto r = ParseGsEditActionString("unknown_command");
    EXPECT_FALSE(r.editAction.has_value());
    EXPECT_FALSE(r.isPopAction);

    r = ParseGsEditActionString("");
    EXPECT_FALSE(r.editAction.has_value());
    EXPECT_FALSE(r.isPopAction);

    r = ParseGsEditActionString("   ");
    EXPECT_FALSE(r.editAction.has_value());
    EXPECT_FALSE(r.isPopAction);
}

TEST(GsEditCommandParsingTest, ParseGsEditActionString_MalformedInput)
{
    // Missing closing bracket
    auto r = ParseGsEditActionString("select_box [0.1, 0.2, 0.8, 0.9");
    EXPECT_FALSE(r.editAction.has_value());

    // Empty bracket content
    r = ParseGsEditActionString("select_box []");
    EXPECT_FALSE(r.editAction.has_value());

    // Invalid float in params
    r = ParseGsEditActionString("select_box [abc, def, ghi, jkl]");
    EXPECT_FALSE(r.editAction.has_value());

    // No space before bracket: parser still matches command
    r = ParseGsEditActionString("select_box[0.0, 0.0, 1.0, 1.0]");
    EXPECT_TRUE(r.editAction.has_value());

    // Extra text after bracket
    r = ParseGsEditActionString("select_box [0.0, 0.0, 1.0, 1.0] extra");
    EXPECT_TRUE(r.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - trimming behavior
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_TrimmingBehavior)
{
    auto r = ParseGsEditActionString("  clear_selection  ");
    ASSERT_TRUE(r.editAction.has_value());
    EXPECT_EQ(r.editAction->type, GsEditActionType::ClearSelection);

    r = ParseGsEditActionString("select_box [0.0, 0.0, 1.0, 1.0]");
    ASSERT_TRUE(r.editAction.has_value());
    EXPECT_EQ(r.editAction->type, GsEditActionType::SelectBox);

    r = ParseGsEditActionString("\tinvert_selection\t");
    ASSERT_TRUE(r.editAction.has_value());
    EXPECT_EQ(r.editAction->type, GsEditActionType::InvertSelection);

    r = ParseGsEditActionString("\n  remove_selected  \n");
    ASSERT_TRUE(r.editAction.has_value());
    EXPECT_EQ(r.editAction->type, GsEditActionType::RemoveSelected);
}

// ============================================================================
// GsEditActionParseResult struct
// ============================================================================
TEST(GsEditCommandParsingTest, GsEditActionParseResult_DefaultState)
{
    GsEditActionParseResult result;
    EXPECT_FALSE(result.editAction.has_value());
    EXPECT_FALSE(result.isPopAction);

    result.isPopAction = true;
    EXPECT_TRUE(result.isPopAction);
    EXPECT_FALSE(result.editAction.has_value());
}

// ============================================================================
// ParseGsEditActionString - each action type returns correct type
// ============================================================================
TEST(GsEditCommandParsingTest, ParseGsEditActionString_AllActionTypesDistinct)
{
    EXPECT_EQ(ParseGsEditActionString("select_box [0,0,1,1]").editAction->type, GsEditActionType::SelectBox);
    EXPECT_EQ(ParseGsEditActionString("select_3d_box [0,0,0,1,1,1]").editAction->type, GsEditActionType::Select3DBox);
    EXPECT_EQ(ParseGsEditActionString("select_individual").editAction->type, GsEditActionType::SelectIndividual);
    EXPECT_EQ(ParseGsEditActionString("clear_selection").editAction->type, GsEditActionType::ClearSelection);
    EXPECT_EQ(ParseGsEditActionString("invert_selection").editAction->type, GsEditActionType::InvertSelection);
    EXPECT_EQ(ParseGsEditActionString("remove_selected").editAction->type, GsEditActionType::RemoveSelected);
    EXPECT_EQ(ParseGsEditActionString("transform [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1,0]").editAction->type,
        GsEditActionType::Transform);
    EXPECT_EQ(ParseGsEditActionString("color_selection [1,1,1,1,0]").editAction->type,
        GsEditActionType::ColorSelection);
}
