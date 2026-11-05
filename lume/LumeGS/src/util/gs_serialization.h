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

#ifndef PLUGIN_GS_SERIALIZATION_H
#define PLUGIN_GS_SERIALIZATION_H

#include <atomic>
#include <iostream>
#include <memory>
#include <plugin_gs/ecs/components/gs_component.h>
#include <plugin_gs/render/intf_render_data_store_default_gs.h>
#include <plugin_gs/util/intf_gs_picking.h>
#include <sstream>
#include <thread>

#include <base/math/quaternion_util.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>

namespace GS {

enum class GS_SERIALIZATION_ERRC : uint8_t {
    SUCCESS = 0,
    INVALID_PATH,
    WRITE_FAILED,
    INVALID_PARAM,
};

GS_SERIALIZATION_ERRC WriteGaussianData(CORE_NS::IFileManager& fileManager, const BASE_NS::string& path,
    GsObjectLoadData::GsSplatData* gsSplatData, const uint32_t numSplats);

} // namespace GS

#endif