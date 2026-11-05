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

#include <cmath>
#include <cstdint>
#include <limits>

#include <gtest/gtest.h>

#include <base/containers/allocator.h>
#include <base/math/mathf.h>
#include <base/math/matrix.h>
#include <base/math/vector.h>
#include <base/math/vector_util.h>

#include "util/gs_utils.h"

using namespace GS;
using namespace BASE_NS;

// ============================================================================
// PackHalf2x16 / UnpackHalf2x16
// ============================================================================
TEST(GsUtilsTest, PackHalf2x16_UnpackHalf2x16_ZeroAndOne)
{
    auto packed0 = PackHalf2x16(0.0f, 0.0f);
    auto up0 = UnpackHalf2x16(packed0);
    EXPECT_FLOAT_EQ(up0.x, 0.0f);
    EXPECT_FLOAT_EQ(up0.y, 0.0f);

    auto packed1 = PackHalf2x16(1.0f, 1.0f);
    auto up1 = UnpackHalf2x16(packed1);
    EXPECT_NEAR(up1.x, 1.0f, 0.01f);
    EXPECT_NEAR(up1.y, 1.0f, 0.01f);
}

TEST(GsUtilsTest, PackHalf2x16_UnpackHalf2x16_DistinctAndNegative)
{
    auto packedD = PackHalf2x16(0.5f, -0.5f);
    auto upD = UnpackHalf2x16(packedD);
    EXPECT_NEAR(upD.x, 0.5f, 0.01f);
    EXPECT_NEAR(upD.y, -0.5f, 0.01f);

    auto packedNeg = PackHalf2x16(-1.0f, -2.0f);
    auto upNeg = UnpackHalf2x16(packedNeg);
    EXPECT_NEAR(upNeg.x, -1.0f, 0.05f);
    EXPECT_NEAR(upNeg.y, -2.0f, 0.1f);
}

TEST(GsUtilsTest, PackHalf2x16_UnpackHalf2x16_RoundTrip)
{
    float x = 0.75f;
    float y = -1.5f;
    auto packedRT = PackHalf2x16(x, y);
    auto upRT = UnpackHalf2x16(packedRT);
    EXPECT_NEAR(upRT.x, x, 0.05f);
    EXPECT_NEAR(upRT.y, y, 0.05f);

    float a = 0.125f;
    float b = 3.5f;
    auto packedAB = PackHalf2x16(a, b);
    auto upAB = UnpackHalf2x16(packedAB);
    EXPECT_NEAR(upAB.x, a, 0.01f);
    EXPECT_NEAR(upAB.y, b, 0.05f);
}

TEST(GsUtilsTest, PackHalf2x16_UnpackHalf2x16_SmallValues)
{
    auto packedSmall = PackHalf2x16(0.003f, -0.003f);
    auto upSmall = UnpackHalf2x16(packedSmall);
    EXPECT_NEAR(upSmall.x, 0.003f, 0.001f);
    EXPECT_NEAR(upSmall.y, -0.003f, 0.001f);
}

TEST(GsUtilsTest, PackHalf2x16_XInLowBitsYInHighBits)
{
    // x occupies lower 16 bits, y occupies upper 16 bits
    auto packed = PackHalf2x16(1.0f, 0.0f);
    uint16_t xHalf = static_cast<uint16_t>(packed & 0xFFFF);
    uint16_t yHalf = static_cast<uint16_t>((packed >> 16) & 0xFFFF);
    EXPECT_NE(xHalf, 0u);
    EXPECT_EQ(yHalf, 0u);

    auto packed2 = PackHalf2x16(0.0f, 1.0f);
    uint16_t xHalf2 = static_cast<uint16_t>(packed2 & 0xFFFF);
    uint16_t yHalf2 = static_cast<uint16_t>((packed2 >> 16) & 0xFFFF);
    EXPECT_EQ(xHalf2, 0u);
    EXPECT_NE(yHalf2, 0u);
}

// ============================================================================
// PackUint2x16 / UnpackUint2x16
// ============================================================================
TEST(GsUtilsTest, PackUint2x16_UnpackUint2x16_ZerosAndMax)
{
    auto packed0 = PackUint2x16(0, 0);
    auto up0 = UnpackUint2x16(packed0);
    EXPECT_EQ(up0.x, 0u);
    EXPECT_EQ(up0.y, 0u);

    auto packedMax = PackUint2x16(0xFFFF, 0xFFFF);
    auto upMax = UnpackUint2x16(packedMax);
    EXPECT_EQ(upMax.x, 0xFFFFu);
    EXPECT_EQ(upMax.y, 0xFFFFu);
}

TEST(GsUtilsTest, PackUint2x16_UnpackUint2x16_DistinctValues)
{
    auto packedD = PackUint2x16(0x1234, 0x5678);
    auto upD = UnpackUint2x16(packedD);
    EXPECT_EQ(upD.x, 0x1234u);
    EXPECT_EQ(upD.y, 0x5678u);
}

TEST(GsUtilsTest, PackUint2x16_TruncatesUpperBits)
{
    auto packedT = PackUint2x16(0x1FFFF, 0x2FFFF);
    auto upT = UnpackUint2x16(packedT);
    EXPECT_EQ(upT.x, 0xFFFFu);
    EXPECT_EQ(upT.y, 0xFFFFu);
}

TEST(GsUtilsTest, PackUint2x16_XInLowBitsYInHighBits)
{
    auto packed = PackUint2x16(0xAAAA, 0x0000);
    auto up = UnpackUint2x16(packed);
    EXPECT_EQ(up.x, 0xAAAAu);
    EXPECT_EQ(up.y, 0x0000u);

    auto packed2 = PackUint2x16(0x0000, 0xBBBB);
    auto up2 = UnpackUint2x16(packed2);
    EXPECT_EQ(up2.x, 0x0000u);
    EXPECT_EQ(up2.y, 0xBBBBu);
}

// ============================================================================
// PackGSColor / UnpackGSColor
// ============================================================================
TEST(GsUtilsTest, PackGSColor_BlackAndWhite)
{
    Math::Vec3 black(0.0f, 0.0f, 0.0f);
    uint32_t packedBlack = PackGSColor(black, 0.0f);
    EXPECT_EQ(packedBlack, 0u);

    Math::Vec3 white(1.0f, 1.0f, 1.0f);
    uint32_t packedWhite = PackGSColor(white, 1.0f);
    EXPECT_EQ(packedWhite, 0xFFFFFFFFu);
}

TEST(GsUtilsTest, PackGSColor_RedChannelLayout)
{
    Math::Vec3 red(1.0f, 0.0f, 0.0f);
    uint32_t packedRed = PackGSColor(red, 1.0f);
    EXPECT_EQ((packedRed >> 24) & 0xFF, 255);
    EXPECT_EQ((packedRed >> 16) & 0xFF, 0);
    EXPECT_EQ((packedRed >> 8) & 0xFF, 0);
    EXPECT_EQ(packedRed & 0xFF, 255);
}

TEST(GsUtilsTest, PackGSColor_GreenAndBlueChannels)
{
    Math::Vec3 green(0.0f, 1.0f, 0.0f);
    uint32_t packedGreen = PackGSColor(green, 1.0f);
    EXPECT_EQ((packedGreen >> 24) & 0xFF, 0);
    EXPECT_EQ((packedGreen >> 16) & 0xFF, 255);
    EXPECT_EQ((packedGreen >> 8) & 0xFF, 0);
    EXPECT_EQ(packedGreen & 0xFF, 255);

    Math::Vec3 blue(0.0f, 0.0f, 1.0f);
    uint32_t packedBlue = PackGSColor(blue, 1.0f);
    EXPECT_EQ((packedBlue >> 24) & 0xFF, 0);
    EXPECT_EQ((packedBlue >> 16) & 0xFF, 0);
    EXPECT_EQ((packedBlue >> 8) & 0xFF, 255);
    EXPECT_EQ(packedBlue & 0xFF, 255);
}

TEST(GsUtilsTest, PackGSColor_ClampsOutOfRange)
{
    Math::Vec3 oor(2.0f, -1.0f, 0.5f);
    uint32_t packedOor = PackGSColor(oor, 1.5f);
    EXPECT_EQ((packedOor >> 24) & 0xFF, 255);
    EXPECT_EQ((packedOor >> 16) & 0xFF, 0);
    EXPECT_EQ((packedOor >> 8) & 0xFF, 127);
    EXPECT_EQ(packedOor & 0xFF, 255);
}

TEST(GsUtilsTest, PackGSColor_UnpackGSColor_RoundTrip)
{
    Math::Vec3 color(0.5f, 0.25f, 0.75f);
    uint32_t packedRT = PackGSColor(color, 1.0f);
    Math::Vec3 unpackedRT;
    UnpackGSColor(BitCast<float>(packedRT), unpackedRT);
    EXPECT_NEAR(unpackedRT.x, 0.5f, 1.0f / 255.0f);
    EXPECT_NEAR(unpackedRT.y, 0.25f, 1.0f / 255.0f);
    EXPECT_NEAR(unpackedRT.z, 0.75f, 1.0f / 255.0f);
}

TEST(GsUtilsTest, PackGSColor_UnpackGSColor_WhiteRoundTrip)
{
    Math::Vec3 white(1.0f, 1.0f, 1.0f);
    uint32_t packed = PackGSColor(white, 1.0f);
    Math::Vec3 unpacked;
    UnpackGSColor(BitCast<float>(packed), unpacked);
    EXPECT_NEAR(unpacked.x, 1.0f, 1.0f / 255.0f);
    EXPECT_NEAR(unpacked.y, 1.0f, 1.0f / 255.0f);
    EXPECT_NEAR(unpacked.z, 1.0f, 1.0f / 255.0f);
}

TEST(GsUtilsTest, PackGSColor_MidGrayValues)
{
    Math::Vec3 gray(0.5f, 0.5f, 0.5f);
    uint32_t packed = PackGSColor(gray, 0.5f);
    EXPECT_NEAR((packed >> 24) & 0xFF, 127, 1);
    EXPECT_NEAR((packed >> 16) & 0xFF, 127, 1);
    EXPECT_NEAR((packed >> 8) & 0xFF, 127, 1);
    EXPECT_NEAR(packed & 0xFF, 127, 1);
}

// ============================================================================
// PackGSDcWithMinMax
// ============================================================================
TEST(GsUtilsTest, PackGSDcWithMinMax_MidRange)
{
    Math::Vec3 mid(0.5f, 0.5f, 0.5f);
    uint32_t packedMid = PackGSDcWithMinMax(mid, 0.0f, 1.0f);
    EXPECT_NEAR((packedMid >> 24) & 0xFF, 127, 2);
    EXPECT_NEAR((packedMid >> 16) & 0xFF, 127, 2);
    EXPECT_NEAR((packedMid >> 8) & 0xFF, 127, 2);
}

TEST(GsUtilsTest, PackGSDcWithMinMax_MinValue)
{
    Math::Vec3 zero(0.0f, 0.0f, 0.0f);
    uint32_t packedZero = PackGSDcWithMinMax(zero, 0.0f, 1.0f);
    EXPECT_EQ((packedZero >> 24) & 0xFF, 0);
    EXPECT_EQ((packedZero >> 16) & 0xFF, 0);
    EXPECT_EQ((packedZero >> 8) & 0xFF, 0);
}

TEST(GsUtilsTest, PackGSDcWithMinMax_MaxValue)
{
    // epsilon in denominator: 1.0/(1.0+1e-6)*255 ≈ 254.999, truncates to 254
    Math::Vec3 one(1.0f, 1.0f, 1.0f);
    uint32_t packedOne = PackGSDcWithMinMax(one, 0.0f, 1.0f);
    EXPECT_EQ((packedOne >> 24) & 0xFF, 254);
    EXPECT_EQ((packedOne >> 16) & 0xFF, 254);
    EXPECT_EQ((packedOne >> 8) & 0xFF, 254);
}

TEST(GsUtilsTest, PackGSDcWithMinMax_SwapsMinMax)
{
    Math::Vec3 mid2(0.5f, 0.5f, 0.5f);
    uint32_t packedSwap = PackGSDcWithMinMax(mid2, 1.0f, 0.0f);
    EXPECT_NEAR((packedSwap >> 24) & 0xFF, 127, 2);
    EXPECT_NEAR((packedSwap >> 16) & 0xFF, 127, 2);
    EXPECT_NEAR((packedSwap >> 8) & 0xFF, 127, 2);
}

TEST(GsUtilsTest, PackGSDcWithMinMax_CustomRange)
{
    // dcMin=10, dcMax=20, dc=15 → normalized = 0.5
    Math::Vec3 dc(15.0f, 15.0f, 15.0f);
    uint32_t packed = PackGSDcWithMinMax(dc, 10.0f, 20.0f);
    EXPECT_NEAR((packed >> 24) & 0xFF, 127, 2);
    EXPECT_NEAR((packed >> 16) & 0xFF, 127, 2);
    EXPECT_NEAR((packed >> 8) & 0xFF, 127, 2);
}

TEST(GsUtilsTest, PackGSDcWithMinMax_DcAtMin)
{
    Math::Vec3 dc(10.0f, 10.0f, 10.0f);
    uint32_t packed = PackGSDcWithMinMax(dc, 10.0f, 20.0f);
    EXPECT_EQ((packed >> 24) & 0xFF, 0);
    EXPECT_EQ((packed >> 16) & 0xFF, 0);
    EXPECT_EQ((packed >> 8) & 0xFF, 0);
}

TEST(GsUtilsTest, PackGSDcWithMinMax_DcAtMax)
{
    Math::Vec3 dc(20.0f, 20.0f, 20.0f);
    uint32_t packed = PackGSDcWithMinMax(dc, 10.0f, 20.0f);
    EXPECT_NEAR((packed >> 24) & 0xFF, 254, 1);
    EXPECT_NEAR((packed >> 16) & 0xFF, 254, 1);
    EXPECT_NEAR((packed >> 8) & 0xFF, 254, 1);
}

// ============================================================================
// SqrtWithSign
// ============================================================================
TEST(GsUtilsTest, SqrtWithSign_PositiveValues)
{
    EXPECT_FLOAT_EQ(SqrtWithSign(4.0f), 2.0f);
    EXPECT_FLOAT_EQ(SqrtWithSign(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(SqrtWithSign(0.25f), 0.5f);
    EXPECT_NEAR(SqrtWithSign(0.01f), 0.1f, 1e-5f);
}

TEST(GsUtilsTest, SqrtWithSign_ZeroAndNegative)
{
    EXPECT_FLOAT_EQ(SqrtWithSign(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(SqrtWithSign(-4.0f), -2.0f);
    EXPECT_FLOAT_EQ(SqrtWithSign(-1.0f), -1.0f);
    EXPECT_FLOAT_EQ(SqrtWithSign(-0.25f), -0.5f);
}

// ============================================================================
// SqWithSign
// ============================================================================
TEST(GsUtilsTest, SqWithSign_PositiveValues)
{
    EXPECT_FLOAT_EQ(SqWithSign(3.0f), 9.0f);
    EXPECT_FLOAT_EQ(SqWithSign(1.0f), 1.0f);
    EXPECT_FLOAT_EQ(SqWithSign(0.5f), 0.25f);
    EXPECT_FLOAT_EQ(SqWithSign(10.0f), 100.0f);
    EXPECT_FLOAT_EQ(SqWithSign(0.1f), 0.01f);
}

TEST(GsUtilsTest, SqWithSign_ZeroAndNegative)
{
    EXPECT_FLOAT_EQ(SqWithSign(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(SqWithSign(-3.0f), -9.0f);
    EXPECT_FLOAT_EQ(SqWithSign(-1.0f), -1.0f);
    EXPECT_FLOAT_EQ(SqWithSign(-0.5f), -0.25f);
}

TEST(GsUtilsTest, SqWithSign_InverseOfSqrtWithSign)
{
    EXPECT_FLOAT_EQ(SqWithSign(SqrtWithSign(2.5f)), 2.5f);
    EXPECT_FLOAT_EQ(SqWithSign(SqrtWithSign(-2.5f)), -2.5f);
    EXPECT_FLOAT_EQ(SqWithSign(SqrtWithSign(100.0f)), 100.0f);
    EXPECT_FLOAT_EQ(SqWithSign(SqrtWithSign(-100.0f)), -100.0f);
    EXPECT_FLOAT_EQ(SqWithSign(SqrtWithSign(0.0f)), 0.0f);
}

// ============================================================================
// BitCast
// ============================================================================
TEST(GsUtilsTest, BitCast_FloatToUint32)
{
    float f = 1.0f;
    auto u = BitCast<uint32_t>(f);
    uint32_t expected = 0u;
    (void)CloneData(&expected, sizeof(float), &f, sizeof(float));
    EXPECT_EQ(u, expected);
}

TEST(GsUtilsTest, BitCast_Uint32ToFloat)
{
    uint32_t u = 0x3F800000u;
    auto f = BitCast<float>(u);
    EXPECT_FLOAT_EQ(f, 1.0f);

    uint32_t halfBits = 0x3F000000u;
    auto halfFloat = BitCast<float>(halfBits);
    EXPECT_FLOAT_EQ(halfFloat, 0.5f);
}

TEST(GsUtilsTest, BitCast_RoundTrip)
{
    float original = -3.14f;
    auto intermediate = BitCast<uint32_t>(original);
    auto restored = BitCast<float>(intermediate);
    EXPECT_FLOAT_EQ(restored, original);
}

TEST(GsUtilsTest, BitCast_PreservesSignBit)
{
    float negZero = -0.0f;
    auto bits = BitCast<uint32_t>(negZero);
    EXPECT_NE(bits, 0u);
    auto restored = BitCast<float>(bits);
    EXPECT_FLOAT_EQ(restored, negZero);
}

TEST(GsUtilsTest, BitCast_SpecialFloatValues)
{
    float posInf = std::numeric_limits<float>::infinity();
    auto infBits = BitCast<uint32_t>(posInf);
    EXPECT_EQ(infBits, 0x7F800000u);
    auto restoredInf = BitCast<float>(infBits);
    EXPECT_TRUE(std::isinf(restoredInf));

    float negInf = -std::numeric_limits<float>::infinity();
    auto negInfBits = BitCast<uint32_t>(negInf);
    EXPECT_EQ(negInfBits, 0xFF800000u);
    auto restoredNegInf = BitCast<float>(negInfBits);
    EXPECT_TRUE(std::isinf(restoredNegInf));
}

TEST(GsUtilsTest, BitCast_KnownBitPatterns)
{
    // 0.5f = 0x3F000000
    uint32_t halfBits = 0x3F000000u;
    auto halfFloat = BitCast<float>(halfBits);
    EXPECT_FLOAT_EQ(halfFloat, 0.5f);

    // -2.0f = 0xC0000000
    uint32_t negTwoBits = 0xC0000000u;
    auto negTwoFloat = BitCast<float>(negTwoBits);
    EXPECT_FLOAT_EQ(negTwoFloat, -2.0f);

    // Smallest positive normalized float
    uint32_t minNormBits = 0x00800000u;
    auto minNormFloat = BitCast<float>(minNormBits);
    EXPECT_FLOAT_EQ(minNormFloat, std::numeric_limits<float>::min());
}

TEST(GsUtilsTest, BitCast_Uint64RoundTrip)
{
    uint64_t original = 0xDEADBEEF12345678ULL;
    auto asFloat = BitCast<double>(original);
    auto restored = BitCast<uint64_t>(asFloat);
    EXPECT_EQ(restored, original);
}

// ============================================================================
// GetLargestOffDiagonalElement
// ============================================================================
TEST(GsUtilsTest, GetLargestOffDiagonalElement_Identity)
{
    Math::Mat3X3 identity(1.0f);
    float pivot = 0.0f;
    size_t pi = 0u;
    size_t pj = 0u;
    GetLargestOffDiagonalElement(identity, pivot, pi, pj);
    EXPECT_FLOAT_EQ(pivot, 0.0f);
}

TEST(GsUtilsTest, GetLargestOffDiagonalElement_Symmetric)
{
    float elems[] = {1.0f, 5.0f, 2.0f, 5.0f, 3.0f, 1.0f, 2.0f, 1.0f, 4.0f};
    Math::Mat3X3 sym(elems);
    float pivot = 0.0f;
    size_t pi = 0u;
    size_t pj = 0u;
    GetLargestOffDiagonalElement(sym, pivot, pi, pj);
    EXPECT_FLOAT_EQ(pivot, 5.0f);
    EXPECT_EQ(pi, 0u);
    EXPECT_EQ(pj, 1u);
}

TEST(GsUtilsTest, GetLargestOffDiagonalElement_NegativeValues)
{
    float negElems[] = {1.0f, -7.0f, 3.0f, -7.0f, 2.0f, 0.5f, 3.0f, 0.5f, 1.0f};
    Math::Mat3X3 negM(negElems);
    float pivot = 0.0f;
    size_t pi = 0u;
    size_t pj = 0u;
    GetLargestOffDiagonalElement(negM, pivot, pi, pj);
    EXPECT_FLOAT_EQ(pivot, 7.0f);
    EXPECT_EQ(pi, 0u);
    EXPECT_EQ(pj, 1u);
}

TEST(GsUtilsTest, GetLargestOffDiagonalElement_UpperTriangleOnly)
{
    // Only (0,2) = 10, all other off-diagonals smaller
    float elems[] = {1.0f, 0.0f, 10.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 3.0f};
    Math::Mat3X3 m(elems);
    float pivot = 0.0f;
    size_t pi = 0u;
    size_t pj = 0u;
    GetLargestOffDiagonalElement(m, pivot, pi, pj);
    EXPECT_FLOAT_EQ(pivot, 10.0f);
    EXPECT_EQ(pi, 0u);
    EXPECT_EQ(pj, 2u);
}

TEST(GsUtilsTest, GetLargestOffDiagonalElement_NonZeroPivotInitial)
{
    float elems[] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
    Math::Mat3X3 m(elems);
    float pivot = 100.0f; // already larger than any off-diagonal
    size_t pi = 5u;
    size_t pj = 5u;
    GetLargestOffDiagonalElement(m, pivot, pi, pj);
    // pivot stays 100.0 since no off-diagonal > 100
    EXPECT_FLOAT_EQ(pivot, 100.0f);
    EXPECT_EQ(pi, 5u);
    EXPECT_EQ(pj, 5u);
}

// ============================================================================
// ExtractCameraPositionFromViewMatrix
// ============================================================================
TEST(GsUtilsTest, ExtractCameraPositionFromViewMatrix_Identity)
{
    Math::Mat4X4 identity = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f);
    auto pos = ExtractCameraPositionFromViewMatrix(identity);
    EXPECT_FLOAT_EQ(pos.x, 0.0f);
    EXPECT_FLOAT_EQ(pos.y, 0.0f);
    EXPECT_FLOAT_EQ(pos.z, 0.0f);
}

TEST(GsUtilsTest, ExtractCameraPositionFromViewMatrix_Translated)
{
    // Camera at (5, 10, -3): view matrix has translation = -R * P = -P (identity rotation)
    Math::Mat4X4 viewMatrix = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        -5.0f, -10.0f, 3.0f, 1.0f);
    auto pos = ExtractCameraPositionFromViewMatrix(viewMatrix);
    EXPECT_FLOAT_EQ(pos.x, 5.0f);
    EXPECT_FLOAT_EQ(pos.y, 10.0f);
    EXPECT_FLOAT_EQ(pos.z, -3.0f);
}

TEST(GsUtilsTest, ExtractCameraPositionFromViewMatrix_RotatedAndTranslated)
{
    // 90-degree rotation around Z: R^T = [[0,1,0],[-1,0,0],[0,0,1]]
    // Camera at (1,2,3): t = -P*R^T columns = (-P*r0, -P*r1, -P*r2) = (2,-1,-3)
    Math::Mat4X4 viewMatrix = Math::Mat4X4(0.0f, 1.0f, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        2.0f, -1.0f, -3.0f, 1.0f);
    auto pos = ExtractCameraPositionFromViewMatrix(viewMatrix);
    EXPECT_NEAR(pos.x, 1.0f, 1e-5f);
    EXPECT_NEAR(pos.y, 2.0f, 1e-5f);
    EXPECT_NEAR(pos.z, 3.0f, 1e-5f);
}

TEST(GsUtilsTest, ExtractCameraPositionFromViewMatrix_LargeTranslation)
{
    Math::Mat4X4 viewMatrix = Math::Mat4X4(1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f,
        -1000.0f, -2000.0f, 3000.0f, 1.0f);
    auto pos = ExtractCameraPositionFromViewMatrix(viewMatrix);
    EXPECT_FLOAT_EQ(pos.x, 1000.0f);
    EXPECT_FLOAT_EQ(pos.y, 2000.0f);
    EXPECT_FLOAT_EQ(pos.z, -3000.0f);
}
