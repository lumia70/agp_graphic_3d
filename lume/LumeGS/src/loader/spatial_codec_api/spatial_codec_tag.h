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

#ifndef SPATIAL_CODEC_TAG_H
#define SPATIAL_CODEC_TAG_H

#include <string>

namespace Spatial {
// tags for SpatialModelEncoder and SpatialModelDecoder
class SpatialCodecTag {
public:
    /********************************* BEGIN MODEL BASIC INFO NAME *********************************/
    // name used to set the spatial model's basic info: SpatialMeta
    static inline const std::string BASIC_INFO = "basic_info";

    // number of the spatial model(e.g., gs_points_num in the gaussian model): uint32_t
    static inline const std::string GS_POINTS_NUM = "gs_points_num";

    // spherical harmonic order of the gaussian model: uint32_t
    static inline const std::string SH_DEGREE = "SH_degree";

    // Meta Key, vector<float>(3)
    // e.g., [-10, -10, -10]
    // Quantized minimum value of the position attribute
    static inline const std::string POSITION_MIN_VALUE = "position_min_value";

    // Meta Key, vector<float>(3)
    // e.g., [10, 10, 10]
    // Quantized maximum value of the position attribute
    static inline const std::string POSITION_MAX_VALUE = "position_max_value";
    /********************************* END MODEL BASIC INFO NAME ***********************************/

    /********************************* BEGIN ATTRIBUTE NAME ****************************************/
    // total positions of the spatial model: std::vector<float>
    // e.g., [x0, y0, z0, x1, y1, z1, ...] in the gaussian model
    static inline const std::string POSITION = "POSITION";

    // total scales of the spatial model: std::vector<float>
    // e.g., [x0, y0, z0, x1, y1, z1, ...] in the gaussian model
    static inline const std::string SCALE = "SCALE";

    // total rotations of the spatial model: std::vector<float>
    // e.g., [w0, x0, y0, z0, w1, x1, y1, z1, ...] quaternions in the gaussian model
    static inline const std::string ROTATION = "ROTATION";

    // total opacities of the spatial model: std::vector<float>
    // e.g., [alpha0, alpha1, ...] in the gaussian model
    static inline const std::string OPACITY = "OPACITY";

    // the total 0th spherical harmonic coefficients of gaussian model: std::vector<float>
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0 =
        "SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0";

    // the total 1th spherical harmonic coefficients of gaussian model: std::vector<float>
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_1_COEFFICIENT_0 =
        "SPHERICAL_HARMONICS_DEGREE_1_COEFFICIENT_0";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_1_COEFFICIENT_1 =
        "SPHERICAL_HARMONICS_DEGREE_1_COEFFICIENT_1";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_1_COEFFICIENT_2 =
        "SPHERICAL_HARMONICS_DEGREE_1_COEFFICIENT_2";

    // the total 2th spherical harmonic coefficients of gaussian model: std::vector<float>
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_0 =
        "SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_0";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_1 =
        "SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_1";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_2 =
        "SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_2";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_3 =
        "SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_3";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_4 =
        "SPHERICAL_HARMONICS_DEGREE_2_COEFFICIENT_4";

    // the total 3th spherical harmonic coefficients of gaussian model: std::vector<float>
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_0 =
        "SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_0";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_1 =
        "SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_1";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_2 =
        "SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_2";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_3 =
        "SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_3";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_4 =
        "SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_4";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_5 =
        "SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_5";
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_6 =
        "SPHERICAL_HARMONICS_DEGREE_3_COEFFICIENT_6";

    // the total high-order spherical harmonic coefficients of gaussian model: std::vector<float>
    static inline const std::string SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER =
        "SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER";

    // the importance of gs point in the gaussian model: std::vector<float>
    static inline const std::string IMPORTANCE = "Importance";
    /********************************* END ATTRIBUTE NAME ******************************************/

    /****************************** BEGIN ATTRDESC(SpatialMeta)'S KEY *****************************/
    //// Key of attrDesc of SpatialModel.GetAttribute to get output for users
    // These tags are defined by spatial_codec, not the UWA standard
    // uint32_t, 1 is to get uv, default = 0.
    static inline const std::string UV = "uv";
    // uint32_t, 0: pre-reconstructed(raw ASTC only), 1: reconstructed(include all recon); default = 0.
    static inline const std::string RECONSTRUCTED = "reconstructed";
    // uint32_t, only support get attribute by block, must set.
    static inline const std::string BLOCK_INDEX = "block_index";
    // uint32_t, block size for users to get atrribute parallelly, must set.
    static inline const std::string BLOCK_POINTS_NUM = "block_points_num";
    /***************************** END ATTRDESC(SpatialMeta)'S KEY ********************************/

    /***************************** BEGIN ATTRIBUTE META NAME **************************************/
    //// Key of SpatialMeta(of atttribute)
    // uint32_t, the component count of the attribute
    static inline const std::string COMPONENT = "component";
    /************************ BEGIN ATTRIBUTE'S RECONSTRUCTION INDORMATION ************************/
    //// Attribute meta fields - texture meta
    // texture_decode_information
    // uint32_t, texture decoder type used by the packing map. 0: raw; 1: astc
    static inline const std::string PACKING_MAP_TEXTURE_CODEC_ID = "packing_map_texture_codec_id";
    // texture_packing_information
    // uint32_t, width of the packing map, in pixels.
    static inline const std::string PACKING_MAP_WIDTH = "packing_map_width";
    // uint32_t, height of the packing map, in pixels.
    static inline const std::string PACKING_MAP_HEIGHT = "packing_map_height";
    // uint32_t, region width, in pixels.
    static inline const std::string REGION_WIDTH = "region_width";
    // uint32_t, region height, in pixels.
    static inline const std::string REGION_HEIGHT = "region_height";
    // uint32_t, scanning method of the region, 0:by row; 2:by column
    static inline const std::string PACKING_SCANNING_TYPE = "packing_scanning_type";
    // uint32_t, block scanning size
    static inline const std::string PACKING_SCANNING_BLOCK_SIZE = "packing_scanning_block_size";
    // uint32_t, add 1 is the region count
    static inline const std::string PACKING_REGION_COUNT_MINUS_1 = "packing_region_count_minus1";
    // uint32_t, horizontal coordinate of the upper left corner of the region in the packing map
    static inline const std::string REGION_TOP_LEFT_X = "region_top_left_x";
    // uint32_t, vertical coordinate of the upper left corner of the region in the packing map
    static inline const std::string REGION_TOP_LEFT_Y = "region_top_left_y";
    // uint32_t, the channels of the texture
    static inline const std::string TEXTURE_CHANNEL_NUM = "texture_channel_num";
    // uint32_t, the number of rightward bit shifts for the quantized value of
    // the texture encoding attribute or for the prediction.
    // When there is no shift, the value is 0. When there is a shift,
    // the value needs to be shifted leftward by the corresponding number of bits during decoding to be restored.
    static inline const std::string BYTESHIFT = "byteshift";

    //// Attribute meta fields - prediction information, include PREDICTION_TYPE/BYTESHIFT/BLOCK_SIZE
    // uint32_t, 0: None, 1: Block-based differential prediction
    static inline const std::string PREDICTION_TYPE = "prediction_type";
    // uint32_t, block size for attribute prediction.
    static inline const std::string BLOCK_SIZE = "blocksize";
    // BYTESHIFT : The number of rightward bit shifts for the quantized value during attribute prediction.
    // When there is a shift, the decoding needs to perform a corresponding leftward shift to restore the value.
    
    //// Attribute meta fields - quantization information
    // uint32_t, 0:None; 1:Uniform scalar quantization;
    // 2:Uniform quantization based on the minimum and maximum values;
    // 3:Grouped uniform scalar quantization.
    static inline const std::string QUANTIZATION_TYPE = "quantization_type";
    // uint32_t, quantization bit depth of attribute data.
    static inline const std::string QUANTIZATION_BITDEPTH = "quantization_bitdepth";
    // std::vector<float>, the min quantization value per component (per group, if has)
    static inline const std::string QUANTIZATION_MIN_VALUE = "quantization_min_value";
    // std::vector<float>, the max quantization value per component (per group, if has)
    static inline const std::string QUANTIZATION_MAX_VALUE = "quantization_max_value";
    // std::vector<uint32_t>, pointNum per group of the group quantization
    static inline const std::string PATCH_SIZE = "patch_size";
    
    //// Attribute meta fields - transformation information
    // uint32_t, 0:None; 1:Conversion from Euler angles to quaternions; 2:Transformation based on importance
    static inline const std::string TRANSFORMATION_TYPE = "transformation_type";
    /************************ END ATTRIBUTE'S RECONSTRUCTION INDORMATION ************************/
    /***************************** END ATTRIBUTE META NAME **************************************/
};
}

#endif