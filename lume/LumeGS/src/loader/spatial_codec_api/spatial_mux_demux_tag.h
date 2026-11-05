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

#ifndef SPATIAL_MUX_DEMUX_TAG_H
#define SPATIAL_MUX_DEMUX_TAG_H

#include <string>

namespace Spatial {
// tags for SpatialCodecMuxer and SpatialCodecDemuxer
// These tags are used as keys in glTF JSON extensions, glTF extras (meta),
// and data buffers.  Each tag documents its value type and usage scenario.
class SpatialMuxDemuxTag {
public:
    /* =========================== GaussianSplatting Data =========================== */
    // Meta name, SpatialTag::BASIC_INFO
    // used in Muxer&Demuxer Meta name of primitive attribute

    // Meta Key, std::string
    // Used in Muxer
    // available value SpatialTag::UWA_PRIMITIVE_3DGS_COMPRESSION/SpatialTag::UWA_GAUSSIAN_SPLATTING_COMPRESSION_EGSC
    // if data is uncompressed, this tag is empty
    static inline const std::string COMPRESSION_EXTENSION_NAME = "compressionExtensionName";

    // Meta Value
    // compress data with GSCT format decoder/encoder
    static inline const std::string UWA_PRIMITIVE_3DGS_COMPRESSION ="UWA_primitive_3DGS_compression";

    // Meta Value
    // compress data with GSBS format decoder/encoder
    static inline const std::string UWA_GAUSSIAN_SPLATTING_COMPRESSION_EGSC =
        "UWA_gaussian_splatting_compression_EGSC";

    // Meta Key, std::string
    // e.g., SpatialTag::KHR_GAUSSIAN_SPLATTING_AOS/SpatialTag::KHR_GAUSSIAN_SPLATTING_SOA
    static inline const std::string TYPE = "type";

    // Meta Value of TYPE
    // AOS DATA Array of Struct, Each element is one Gaussian Attribute include Position,
    // Rotation, Scale, Opacity, SH0 and SH1&higher(optional)
    // with SpatialTag::BYTEOFFSET(vector<uint32_t>) & SpatialTag::COMPONENT(vector<uint32_t>)
    static inline const std::string KHR_GAUSSIAN_SPLATTING_AOS = "KHRGaussianSplattingAOS";
    
    // Meta Value of TYPE
    // SOA DATA Struct of Array, Each element is an Array of Attribute, like array[position], array[rotation],
    // array[opacity], array[scale], array[sh0], array[sh_n_m]
    // with SpatialTag::BYTEOFFSET(vector<uint32_t>) & SpatialTag::BYTELENGTH(vector<uint32_t>) 
    // & SpatialTag::COMPONENT(vector<uint32_t>)
    static inline const std::string KHR_GAUSSIAN_SPLATTING_SOA = "KHRGaussianSplattingSOA";

    // Meta Key, std::vector<std::string>
    // Used in Demuxer,Muxer
    // e.g., [SpatialTag::POSITION, SpatialTag::ROTATION, SpatialTag::OPACITY, SpatialTag::SCALE,
    // SpatialTag::SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0, SpatialTag::SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER]
    // MIN Size 5(without SH_1_and_higher), MAX Size 6
    static inline const std::string PRIMITIVE_ATTRIBUTES_NAME = "primitiveAttributesName";

    // Meta Key, std::vector<uint32_t>
    // Used in Demuxer for SOA & AOS data
    // e.g, [12,16,...]
    // in SOA: every attributes array size, value = conponent * sizeof(float) * gs_count
    // in AOS: every attribute size in gaussian point, value = component * sizeof(float)
    static inline const std::string BYTELENGTH = "byteLength";

    // Meta Key, std::vector<uint32_t>
    // Used in Demuxer for SOA & AOS Data
    // e.g, [12,24,...]
    // in SOA: every attribute offset in memory
    // in AOS: every attribute offset in struct of gaussian point
    static inline const std::string BYTEOFFSET = "byteOffset";

    // Meta Key, SpatialTag::COMPONENT
    // Used in Demuxer for AOS Data
    // e.g., [1,3,4]
    // attribute type like sclae = 1, vec3 = 3, vec4 = 4
    /* ============================================================================= */

    /* =========================== Camera Initial Params =========================== */
    // Meta name
    static inline const std::string CAMERA_INITIAL_PARAMS = "cameraInitialParams";

    // Meta Key, SpatialTag::Type
    // e.g., 0:perspective; 1:orthographic(disable now)

    // Meta Key, uint32_t
    // e.g, 1920
    // width dpi
    static inline const std::string WIDTH = "width";

    // Meta Key, uint32_t
    // e.g, 1080
    // height dpi
    static inline const std::string HEIGHT = "height";

    // Meta Key, float
    // camera yfov
    static inline const std::string YFOV = "yfov";

    // Meta Key, float
    // camera znear
    static inline const std::string ZNEAR = "znear";

    // Meta Key, float
    // camera zfar
    static inline const std::string ZFAR = "zfar";

    // Meta Key, float
    // camera width / height
    static inline const std::string ASPECT_RATIO = "aspectRatio";

    // Meta Key, std::vector<float>(3)
    // e.g., [x,y,z]
    // camera rotation in world coordinate system
    static inline const std::string TRANSLATION = "translation";

    // Meta Key, std::string
    // camera node name
    static inline const std::string NAME = "name";

    // Meta Key, string
    // e.g., SpatialTag::ALLOCENTRIC_SIXDOF, SpatialTag::EGOCENTRIC_SIXDOF , SpatialTag::EGOCENTRIC_THREEDOF
    static inline const std::string VIEW_PARAM_TYPE = "viewParamType";

    // Meta Value, string
    // Value of SpatialTag::VIEW_PARAM_TYPE
    // centred in object with six dof
    static inline const std::string ALLOCENTRIC_SIXDOF = "allocentricSixDof";
    
    // Meta Value, string
    // Value of SpatialTag::VIEW_PARAM_TYPE
    // centred in camera with six dof
    static inline const std::string EGOCENTRIC_SIXDOF = "egocentricSixDof";

    // Meta Value, string
    // Value of SpatialTag::VIEW_PARAM_TYPE
    // centred in camera with thre dof
    static inline const std::string EGOCENTRIC_THREEDOF = "egocentricThreeDof";

    // Meta Key, std::vector<float>(2)
    // e.g., [-Π, Π]
    // Used in allocentricSixDof/allocentric
    static inline const std::string AZIMUTH_RANGE = "azimuthRange";

    // Meta Key, std::vector<float>(2)
    // e.g., [0, Π]
    // Used in allocentricSixDof/allocentric
    static inline const std::string POLAR_RANGE = "polarRange";

    // Meta Key, std::vector<float>(2)
    // e.g., [0, inf]
    // Used in allocentricSixDof/allocentric
    static inline const std::string DISTANCE_RANGE = "distanceRange";

    // Meta Key, std::vector<float>(3)
    // e.g., [0, 0, 0]
    // Used in allocentricSixDof/allocentric
    static inline const std::string TARGET = "target";

    // Meta Key, std::vector<float>(9)
    // e.g., [1, 0, 0, 0, 1, 0, 0, 0, 1]
    // Used in allocentric
    static inline const std::string GRAVITY_COORDINATE_SYSTEM = "gravityCoordinateSystem";

    // Meta Key, std::vector<float>(2)
    // e.g., [-Π, Π]
    // Used in egocentricThreeDof
    static inline const std::string PITCH_RANGE = "pitchRange";

    // Meta Key, std::vector<float>(2)
    // e.g., [-Π, Π]
    // Used in egocentricThreeDof
    static inline const std::string YAW_RANGE = "yawRange";

    // Meta Key, std::vector<float>(2)
    // e.g., [-Π, Π]
    // Used in egocentricThreeDof
    static inline const std::string ROLL_RANGE = "rollRange";

    // Meta Key, std::vector<float>(6)
    // e.g., [center.x, center.y, center.z, size.x, size.y, size.z]
    // Used in allocentricSixDof/allocentric
    static inline const std::string TARGET_BOUNDING_BOX = "targetBoundingBox";

    // Meta Key, std::vector<float>(6)
    // e.g., [center.x, center.y, center.z, size.x, size.y, size.z]
    // Used in egocentricSixDof
    static inline const std::string CAMERA_BOUNDING_BOX = "cameraBoundingBox";
    /* ============================================================================= */

    /* =========================== Camera Motion Params =========================== */
    // Meta name
    static inline const std::string CAMERA_MOTION_PARAMS = "cameraMotionParams";

    // Meta Key, std::vector<float>
    // e.g., [t1, t2, t3, ... , tn]
    static inline const std::string TIMESTAMPS = "timestamps";

    // Meta Key, std::string
    // e.g., "/cameras/0/perspective/yfov"
    static inline const std::string POINTER = "pointer";

    // Meta Key, SpatialTag::TRANSLATION
    // e.g., [x1, y1, z1, x2, y2, z2, ... , xn, yn, zn]

    // Meta Key, SpatialTag::ROTATION
    // e.g., [x1, y1, z1, w1, x2, y2, z2, w2, ... , xn, yn, zn, wn]

    // Meta Key, SpatialTag::YFOV
    // e.g., [fov1, fov2, fov3, ... , fovn]
    /* ============================================================================= */
};
}
#endif
