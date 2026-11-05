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
#ifndef SPATIAL_DMA_BUFFER_H
#define SPATIAL_DMA_BUFFER_H

#include <cstdint>
#include <memory>
#include <vector>

namespace Spatial {
class SpatialDmaBuffer {
public:
    static std::shared_ptr<SpatialDmaBuffer> CreateDmaBuffer(int32_t fd, uint32_t capacity);
    static std::shared_ptr<SpatialDmaBuffer> CreateDmaBuffer(const std::vector<uint8_t>& data, bool isCached);
    virtual ~SpatialDmaBuffer() = default;
    virtual int32_t GetFd() const = 0;
    virtual uint32_t GetCapacity() const = 0;
    virtual const void* GetVirAddr() = 0;

protected:
    SpatialDmaBuffer() = default;
    SpatialDmaBuffer(const SpatialDmaBuffer&) = delete;
    SpatialDmaBuffer& operator=(const SpatialDmaBuffer&) = delete;
};
} // namespace Spatial

#endif // SPATIAL_DMA_BUFFER_H
