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

#ifndef GS_FLOATER_V2_1_GAUSSIAN_GLSL
#define GS_FLOATER_V2_1_GAUSSIAN_GLSL

#include "common/v2_1_bindings.glsl"

// GetSHCoef unpacks one spherical-harmonic coefficient (RGB vec3) from the
// 24-uint packed half-float array stored in GsHarmonicsData.shCoefficients.
// Copied verbatim from gs_spherical_harmonics_common.h to avoid including the
// full header, which declares layout bindings (camera UBO, harmonicsData at
// binding 2, dynamic data, textures) that conflict with the profile-managed
// bindings in v2_1_bindings.glsl. The function is pure (no global state).
vec3 V21GetSHCoef(const uint coefs[24], const int idx)
{
    const uint readIdx = (uint(idx) * 3u) >> 1u;
    const vec2 coef0 = unpackHalf2x16(coefs[readIdx]);
    const vec2 coef1 = unpackHalf2x16(coefs[readIdx + 1u]);
    return (uint(idx) & 1u) == 0u ? vec3(coef0.xy, coef1.x) : vec3(coef0.y, coef1.xy);
}

// D15: Adapter that lets existing v2.1 shaders call V21ShCoefficient(id, dim)
// with dimension 0..47 while reading from the LumeGS packed harmonics buffer.
// dimension is coefficient-major: coefficient = dim / 3, channel = dim % 3.
float V21ShCoefficient(uint id, uint dimension)
{
    uint coefficient = dimension / 3u;
    uint channel = dimension % 3u;
    vec3 coef = V21GetSHCoef(harmonicsData[id].shCoefficients, int(coefficient));
    return coef[channel];
}

void V21EigenAxes(float cov00, float cov01, float cov11, out vec2 axis0, out vec2 axis1)
{
    float middle = 0.5 * (cov00 + cov11);
    float radius = sqrt(max(0.0, 0.25 * (cov00 - cov11) * (cov00 - cov11) + cov01 * cov01));
    float lambda0 = max(middle + radius, 1.0e-6);
    float lambda1 = max(middle - radius, 1.0e-6);
    vec2 direction = abs(cov01) > 1.0e-8 ? normalize(vec2(cov01, lambda0 - cov00)) :
        (cov00 >= cov11 ? vec2(1.0, 0.0) : vec2(0.0, 1.0));
    axis0 = direction * sqrt(lambda0);
    axis1 = vec2(-direction.y, direction.x) * sqrt(lambda1);
}

// D7: Projection now reads the 3x3 covariance directly from V21Covariance(id)
// instead of computing S*R from scale+rotation. The world covariance from the
// GsSplatData buffer is already the symmetric 3x3 matrix (covA.xyz, covB.xyz).
// D14: Covariance is multiplied by the per-Gaussian scale modifier squared
// (covAScale.w), matching LumeGS ComputeCov2D behavior.
bool V21ProjectGaussian(uint id, out V21ProjectedGaussian outputValue)
{
    vec3 center = V21Position(id);
    vec4 viewPosition4 = v21.view * vec4(center, 1.0);
    vec3 viewPosition = viewPosition4.xyz;
    vec4 clip = v21.viewProjection * vec4(center, 1.0);
    float nearPlane = v21.principalNearFar.z;
    float farPlane = v21.principalNearFar.w;
    // LumeGS uses -Z forward (standard Vulkan). Visible points have negative
    // view-space z. Test against negated near/far to match this convention.
    if (viewPosition.z >= -nearPlane || viewPosition.z <= -farPlane || clip.w <= 1.0e-7) {
        outputValue = V21ProjectedGaussian(vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));
        return false;
    }

    float focalX = v21.intrinsicsViewport.x;
    float focalY = v21.intrinsicsViewport.y;
    float width = v21.intrinsicsViewport.z;
    float height = v21.intrinsicsViewport.w;
    // Jacobian matching LumeGS ComputeCov2D: use view-space z directly (negative).
    float invZ = 1.0 / viewPosition.z;
    float invZ2 = invZ * invZ;
    mat3 jacobian = mat3(
        focalX * invZ,  0.0,            -(focalX * viewPosition.x) * invZ2,
        0.0,           -focalY * invZ,   (focalY * viewPosition.y) * invZ2,
        0.0,            0.0,             0.0);
    mat3 viewRotation = mat3(
        v21.view[0][0], v21.view[1][0], v21.view[2][0],
        v21.view[0][1], v21.view[1][1], v21.view[2][1],
        v21.view[0][2], v21.view[1][2], v21.view[2][2]);
    mat3 transform = viewRotation * jacobian;
    mat3 worldCovariance = V21Covariance(id);
    mat3 covariance = transpose(transform) * transpose(worldCovariance) * transform;

    // D14: Apply per-Gaussian scale modifier squared to the 2D covariance.
    float scale2 = V21ScaleModifier(id) * V21ScaleModifier(id);
    float original00 = max(covariance[0][0] * scale2, 0.0);
    float original01 = covariance[0][1] * scale2;
    float original11 = max(covariance[1][1] * scale2, 0.0);
    float filtered00 = original00 + 0.3;
    float filtered01 = original01;
    float filtered11 = original11 + 0.3;
    float originalDeterminant = max(original00 * original11 - original01 * original01, 1.0e-12);
    float filteredDeterminant = filtered00 * filtered11 - filtered01 * filtered01;
    if (filteredDeterminant <= 1.0e-10) {
        outputValue = V21ProjectedGaussian(vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));
        return false;
    }
    float inverseDeterminant = 1.0 / filteredDeterminant;
    vec3 conic = vec3(filtered11, -filtered01, filtered00) * inverseDeterminant;
    vec2 axis0;
    vec2 axis1;
    V21EigenAxes(filtered00, filtered01, filtered11, axis0, axis1);
    // Cap axis length so 3-sigma extent does not exceed MAX_SCREEN_SPLAT_SIZE,
    // matching gs3d_precompute_projection.comp behavior. Without this cap,
    // large or close gaussians produce screen-filling quads that create
    // visual artifacts in the V21 offscreen render.
    const float V21_MAX_SPLAT_RADIUS = 1024.0 / 3.0;
    float len0 = length(axis0);
    float len1 = length(axis1);
    if (len0 > V21_MAX_SPLAT_RADIUS) {
        axis0 *= V21_MAX_SPLAT_RADIUS / len0;
    }
    if (len1 > V21_MAX_SPLAT_RADIUS) {
        axis1 *= V21_MAX_SPLAT_RADIUS / len1;
    }
    vec2 extent = 3.0 * (abs(axis0) + abs(axis1));
    vec2 ndc = clip.xy / clip.w;
    vec2 centerPixels = vec2((ndc.x * 0.5 + 0.5) * width, (ndc.y * 0.5 + 0.5) * height);
    bool intersects = centerPixels.x + extent.x >= 0.0 && centerPixels.y + extent.y >= 0.0 &&
        centerPixels.x - extent.x < width && centerPixels.y - extent.y < height;
    if (!intersects) {
        outputValue = V21ProjectedGaussian(vec4(0.0), vec4(0.0), vec4(0.0), vec4(0.0));
        return false;
    }
    float aaCompensation = min(1.0, sqrt(originalDeterminant / filteredDeterminant));
    float opacity = clamp(V21Opacity(id) * aaCompensation, 0.0, 0.99);
    float discRadius = sqrt(v21.discAlphaGap.x) * max(length(axis0), length(axis1));
    vec3 scale = V21Scale(id);
    // centerDepth.z stores positive depth (negated view z) for normalized depth.
    outputValue.centerDepth = vec4(centerPixels, -viewPosition.z, 1.0);
    outputValue.conicOpacity = vec4(conic, opacity);
    outputValue.axes = vec4(axis0, axis1);
    outputValue.extentScale = vec4(max(extent, vec2(1.0)), max(scale.x, max(scale.y, scale.z)), discRadius);
    return true;
}

void V21FillShBasis(vec3 direction, out float basis[16])
{
    const float c0 = 0.28209479177387814;
    const float c1 = 0.4886025119029199;
    const float c20 = 1.0925484305920792;
    const float c21 = -1.0925484305920792;
    const float c22 = 0.31539156525252005;
    const float c23 = -1.0925484305920792;
    const float c24 = 0.5462742152960396;
    const float c30 = -0.5900435899266435;
    const float c31 = 2.890611442640554;
    const float c32 = -0.4570457994644658;
    const float c33 = 0.3731763325901154;
    const float c34 = -0.4570457994644658;
    const float c35 = 1.445305721320277;
    const float c36 = -0.5900435899266435;
    float x = direction.x;
    float y = direction.y;
    float z = direction.z;
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    basis[0] = c0;
    basis[1] = -c1 * y;
    basis[2] = c1 * z;
    basis[3] = -c1 * x;
    basis[4] = c20 * x * y;
    basis[5] = c21 * y * z;
    basis[6] = c22 * (2.0 * zz - xx - yy);
    basis[7] = c23 * x * z;
    basis[8] = c24 * (xx - yy);
    basis[9] = c30 * y * (3.0 * xx - yy);
    basis[10] = c31 * x * y * z;
    basis[11] = c32 * y * (4.0 * zz - xx - yy);
    basis[12] = c33 * z * (2.0 * zz - 3.0 * xx - 3.0 * yy);
    basis[13] = c34 * x * (4.0 * zz - xx - yy);
    basis[14] = c35 * z * (xx - yy);
    basis[15] = c36 * x * (xx - 3.0 * yy);
}

vec3 V21ShColor(uint id)
{
    vec3 direction = normalize(V21Position(id) - v21.cameraPosition.xyz);
    float basis[16];
    V21FillShBasis(direction, basis);
    uint coefficientCount = min(16u, (v21.counts.w + 1u) * (v21.counts.w + 1u));
    vec3 color = vec3(0.0);
    for (uint coefficient = 0u; coefficient < coefficientCount; ++coefficient) {
        uint offset = coefficient * 3u;
        color += basis[coefficient] * vec3(
            V21ShCoefficient(id, offset + 0u),
            V21ShCoefficient(id, offset + 1u),
            V21ShCoefficient(id, offset + 2u));
    }
    return max(color + vec3(0.5), vec3(0.0));
}

vec2 V21QuadCorner(uint vertexIndex)
{
    const vec2 corners[4] = vec2[4](
        vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0));
    return corners[vertexIndex & 3u];
}

#endif
