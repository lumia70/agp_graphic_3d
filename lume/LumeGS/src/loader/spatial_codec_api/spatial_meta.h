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
#ifndef SPATIAL_META_H
#define SPATIAL_META_H

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include "spatial_error_code.h"

namespace Spatial {
using SpatialMetaType =
    std::variant<uint32_t, float, std::string, std::vector<uint32_t>, std::vector<float>, std::vector<std::string>>;

class SpatialMeta {
public:
    virtual ~SpatialMeta() = default;

    virtual SpatialErrorCode AddMeta(const std::string& name, const SpatialMetaType& data) = 0;

    virtual std::optional<SpatialMetaType> GetMeta(const std::string& name) const = 0;

    // T must be in SpatialMetaType
    template<typename T>
    std::optional<T> GetMetaAs(const std::string& name) const
    {
        auto metaValue = GetMeta(name);
        if (!metaValue.has_value()) {
            return std::nullopt;
        }
        if (auto* value = std::get_if<T>(&metaValue.value())) {
            return *value;
        }
        return std::nullopt;
    }
};
}

#endif
