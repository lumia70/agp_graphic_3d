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
#ifndef SPATIAL_BUFFER_H
#define SPATIAL_BUFFER_H

#include <cstdint>
#include <memory>
#include <vector>
#include "spatial_error_code.h"
#include "spatial_meta.h"

namespace Spatial {
class SpatialBuffer {
public:
    virtual ~SpatialBuffer() = default;

    virtual SpatialErrorCode SetMeta(const std::string& name, const std::shared_ptr<SpatialMeta>& meta) = 0;

    virtual SpatialErrorCode GetMeta(const std::string& name, std::shared_ptr<const SpatialMeta>& outMeta) const = 0;

    virtual SpatialErrorCode SetBinaryData(std::vector<uint8_t>&& data) = 0;

    virtual SpatialErrorCode GetBinaryData(std::shared_ptr<const std::vector<uint8_t>>& outData) const = 0;

    virtual size_t Size() const = 0;
};
}

#endif
