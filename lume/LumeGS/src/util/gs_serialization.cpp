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

#include "gs_serialization.h"

#include "gs_utils.h"
#include "render/render_data_store_default_gs.h"
#include <plugin_gs/util/gs_limits.h>

namespace {
#include "app/shaders/common/gs_struct_common.h"
} // namespace

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace CORE3D_NS;
using namespace GS;

void EigenvalueDecomposition(const Math::Mat3X3& S, Math::Vec3& outEigenvalues, Math::Mat3X3& outEigenvectors)
{
    constexpr float convergenceTolerance = 1e-8f;
    Math::Mat3X3 Sprime = S;
    Math::Mat3X3 V = Math::Mat3X3(1.0f);

    bool changed = true;
    constexpr size_t maxIterations = 100;
    size_t iters = 0;

    while (changed && iters < maxIterations) {
        iters++;

        changed = false;
        float pivot = 0.0f;
        size_t pivot_i = 0;
        size_t pivot_j = 0;

        // find the largest off-diagonal element
        GetLargestOffDiagonalElement(Sprime, pivot, pivot_i, pivot_j);

        if (pivot < convergenceTolerance) {
            break;
        }

        // Jacobi rotation angle
        const float theta =
            0.5f * std::atan2(2.0f * Sprime[pivot_i][pivot_j], Sprime[pivot_j][pivot_j] - Sprime[pivot_i][pivot_i]);

        const float cc = std::cos(theta);
        const float ss = std::sin(theta);

        // givens rotation matrix
        Math::Mat3X3 G = Math::Mat3X3(1.0f);
        G[pivot_i][pivot_i] = cc;
        G[pivot_j][pivot_j] = cc;
        G[pivot_i][pivot_j] = ss;
        G[pivot_j][pivot_i] = -ss;

        Sprime = Math::Transpose(G) * Sprime * G;
        V = V * G;

        changed = true;
    }

    outEigenvalues = Math::Vec3(Sprime[0][0], Sprime[1][1], Sprime[2][2]);
    outEigenvectors = V;
}

// https://www.euclideanspace.com/maths/geometry/rotations/conversions/matrixToQuaternion/
Math::Vec4 MatrixToQuaternion(const Math::Mat3X3& m)
{
    Math::Vec4 q;
    float trace = m[0][0] + m[1][1] + m[2][2];

    if (trace > 0.0f) {
        float s = sqrtf(trace + 1.0f) * 2.0f; // s = 4*qw
        q.w = 0.25f * s;
        q.x = (m[2][1] - m[1][2]) / s;
        q.y = (m[0][2] - m[2][0]) / s;
        q.z = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = sqrtf(1.0f + m[0][0] - m[1][1] - m[2][2]) * 2.0f; // s = 4*qw
        q.w = (m[2][1] - m[1][2]) / s;
        q.x = 0.25f * s;
        q.y = (m[0][1] + m[1][0]) / s;
        q.z = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = sqrtf(1.0f + m[1][1] - m[0][0] - m[2][2]) * 2.0f; // s = 4*qy
        q.w = (m[0][2] - m[2][0]) / s;
        q.x = (m[0][1] + m[1][0]) / s;
        q.y = 0.25f * s;
        q.z = (m[1][2] + m[2][1]) / s;
    } else {
        float s = sqrtf(1.0f + m[2][2] - m[0][0] - m[1][1]) * 2.0f; // S = 4*qz
        q.w = (m[1][0] - m[0][1]) / s;
        q.x = (m[0][2] + m[2][0]) / s;
        q.y = (m[1][2] + m[2][1]) / s;
        q.z = 0.25f * s;
    }

    return q;
}

GS_SERIALIZATION_ERRC GS::WriteGaussianData(CORE_NS::IFileManager& fileManager, const BASE_NS::string& path,
    GsObjectLoadData::GsSplatData* gsSplatData, const uint32_t numSplats)
{
    std::ostringstream header;
    header << "ply\n"
           << "format binary_little_endian 1.0\n"
           << "element vertex " << numSplats << "\n"
           << "property float x\n"
           << "property float y\n"
           << "property float z\n"
           << "property float nx\n"
           << "property float ny\n"
           << "property float nz\n"
           << "property float f_dc_0\n"
           << "property float f_dc_1\n"
           << "property float f_dc_2\n"
           << "property float opacity\n"
           << "property float scale_0\n"
           << "property float scale_1\n"
           << "property float scale_2\n"
           << "property float rot_0\n"
           << "property float rot_1\n"
           << "property float rot_2\n"
           << "property float rot_3\n"
           << "end_header\n";

    std::string headerStr = header.str();

    if (fileManager.FileExists(path)) {
        fileManager.DeleteFile(path);
    }

    IFile::Ptr gsFile = fileManager.CreateFile(path);
    if (!gsFile) {
        return GS_SERIALIZATION_ERRC::INVALID_PATH;
    }
    if (gsFile->Write(headerStr.data(), headerStr.size()) != headerStr.size()) {
        return GS_SERIALIZATION_ERRC::WRITE_FAILED;
    }

    constexpr size_t floatsPerSplat = 17; // 3 + 3 + 3 + 1 + 3 + 4
    std::vector<float> buffer;
    buffer.reserve(floatsPerSplat);
    if (numSplats > GS::MAX_SPLATS_LIMIT) {
        return GS_SERIALIZATION_ERRC::INVALID_PARAM;
    }

    const float zeroNormals[3] = { 0, 0, 0 };

    for (uint32_t i = 0; i < numSplats; i++) {
        // position
        buffer.push_back(gsSplatData[i].positionOpacity.x);
        buffer.push_back(gsSplatData[i].positionOpacity.y);
        buffer.push_back(gsSplatData[i].positionOpacity.z);

        // normals (zeros)
        buffer.insert(buffer.end(), zeroNormals, zeroNormals + 3);

        // dc_0, dc_1, dc_2
        Math::Vec3 unpackedCovB;
        UnpackGSColor(gsSplatData[i].covBColorPacked.w, unpackedCovB);

        buffer.push_back((unpackedCovB.x - 0.5f) / SH_C0);
        buffer.push_back((unpackedCovB.y - 0.5f) / SH_C0);
        buffer.push_back((unpackedCovB.z - 0.5f) / SH_C0);

        // opacity
        float opacity = gsSplatData[i].positionOpacity.w;
        buffer.push_back(std::log(opacity / (1.0f - opacity)));

        // scale and rotation
        Math::Mat3X3 covMatrix;
        covMatrix.data[0] = gsSplatData[i].covAScale.x;
        covMatrix.data[1] = gsSplatData[i].covAScale.y;
        covMatrix.data[2] = gsSplatData[i].covAScale.z;
        covMatrix.data[3] = gsSplatData[i].covAScale.y;
        covMatrix.data[4] = gsSplatData[i].covBColorPacked.x;
        covMatrix.data[5] = gsSplatData[i].covBColorPacked.y;
        covMatrix.data[6] = gsSplatData[i].covAScale.z;
        covMatrix.data[7] = gsSplatData[i].covBColorPacked.y;
        covMatrix.data[8] = gsSplatData[i].covBColorPacked.z;

        Math::Mat3X3 eigenvectors;
        Math::Vec3 eigenvalues;
        EigenvalueDecomposition(covMatrix, eigenvalues, eigenvectors);

        // scale
        float scale_0 = Math::sqrt(eigenvalues.x);
        float scale_1 = Math::sqrt(eigenvalues.y);
        float scale_2 = Math::sqrt(eigenvalues.z);

        buffer.push_back(log(scale_0));
        buffer.push_back(log(scale_1));
        buffer.push_back(log(scale_2));

        // rotation
        Math::Vec4 quat = MatrixToQuaternion(eigenvectors);
        buffer.push_back(quat.w);
        buffer.push_back(quat.x);
        buffer.push_back(quat.y);
        buffer.push_back(quat.z);

        if (gsFile->Write(buffer.data(), buffer.size() * sizeof(float)) != buffer.size() * sizeof(float)) {
            return GS_SERIALIZATION_ERRC::WRITE_FAILED;
        }
        buffer.clear();
    }

    gsFile->Close();
    return GS_SERIALIZATION_ERRC::SUCCESS;
}
