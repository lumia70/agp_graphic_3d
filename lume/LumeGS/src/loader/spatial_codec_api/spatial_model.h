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
#ifndef SPATIAL_MODEL_H
#define SPATIAL_MODEL_H

#include <memory>
#include <utility>
#include "spatial_dma_buffer.h"
#include "spatial_error_code.h"
#include "spatial_meta.h"

namespace Spatial {
enum class DataType {
    UINT8,     // unsigned 8-bit integer
    UINT16,    // unsigned 16-bit integer
    UINT32,    // unsigned 32-bit integer
    FLOAT,     // 32-bit float
};

struct ModelAttribute {
    const std::vector<uint8_t> data;
    const std::shared_ptr<SpatialDmaBuffer> buf{nullptr};
    const uint32_t componentCount{1};          // the count of components for a single attribute
    const DataType dataType{DataType::FLOAT};  // the type of component

    ModelAttribute() = delete;
    ModelAttribute(std::vector<uint8_t>&& d, uint32_t cc = 1, DataType dt = DataType::FLOAT)
        : data(std::move(d)), componentCount(cc), dataType(dt) {}
    ModelAttribute(int32_t fd, size_t size, uint32_t cc = 1, DataType dt = DataType::FLOAT);

    ModelAttribute(const ModelAttribute&) = default;
    ModelAttribute& operator=(const ModelAttribute&) = delete;
    ModelAttribute(ModelAttribute&&) noexcept = default;
    ModelAttribute& operator=(ModelAttribute&&) = delete;

    // Returns raw data pointer and size from either vector or DMA buffer
    inline std::pair<const void*, size_t> GetData() const
    {
        if (buf != nullptr && buf->GetFd() > -1) {
            return { buf->GetVirAddr(), buf->GetCapacity() };
        }
        return { data.data(), data.size() };
    }

    // T need be compatible with DataType; otherwise, the behavior may be undefined
    template<typename T>
    std::pair<const T*, size_t> GetAs() const
    {
        static_assert(sizeof(T) > 0 && alignof(T) > 0, "T must be a complete type");
        auto [dataPtr, dataSize] = GetData();
        if (dataPtr == nullptr || dataSize == 0 || (dataSize % sizeof(T)) != 0) {
            return { nullptr, 0 };
        }
        const uintptr_t addr = reinterpret_cast<uintptr_t>(dataPtr);
        if ((addr % alignof(T)) != 0) {
            return { nullptr, 0 };
        }
        return { reinterpret_cast<const T*>(dataPtr), dataSize / sizeof(T) };
    }
};

class SpatialModel {
public:
    virtual ~SpatialModel() = default;

    // set the meta for model
    virtual SpatialErrorCode SetMeta(const std::string& name, const std::shared_ptr<const SpatialMeta>& meta) = 0;

    virtual SpatialErrorCode GetMeta(const std::string& name, std::shared_ptr<const SpatialMeta>& outMeta) const = 0;

    // add attribute one by one, eg: positions, rotations, ...
    virtual SpatialErrorCode AddAttribute(const std::string& name,
        const std::shared_ptr<const ModelAttribute>& attribute) = 0;

    virtual SpatialErrorCode GetAttribute(const std::string& name, const SpatialMeta& attrDesc,
        std::shared_ptr<const ModelAttribute>& outAttr) const = 0;

    virtual size_t Size() const = 0;
};
}

#endif
