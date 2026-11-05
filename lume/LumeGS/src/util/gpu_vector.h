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

#ifndef PLUGIN_GS_GPU_VECTOR_H
#define PLUGIN_GS_GPU_VECTOR_H

#include <base/containers/vector.h>
#include <core/log.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <plugin_gs/util/gs_limits.h>

#include "util/gs_log.h"

namespace GS {
template<typename T>
class GpuVector {
private:
    RENDER_NS::IGpuResourceManager* mgr_;     // GPU resource manager
    RENDER_NS::RenderHandleReference handle_; // GPU buffer handle
    T* mappedPtr_;                            // CPU-mapped pointer
    uint32_t capacity_;                       // allocated size (unit: T)
    uint32_t size_;                           // used size (unit: T)
    bool isMapped_;                           // isMapped

    T* map(RENDER_NS::RenderHandleReference srcHandle)
    {
        if (!mgr_) {
            return nullptr;
        }
        void* ptr = mgr_->MapBufferMemory(srcHandle);
        return ptr ? reinterpret_cast<T*>(ptr) : nullptr;
    }
    void unmap(RENDER_NS::RenderHandleReference srcHandle)
    {
        if (!mgr_) {
            return;
        }
        mgr_->UnmapBuffer(srcHandle);
    }
    bool clone(void* const dst, const size_t dstSize, const void* const src, const size_t srcSize)
    {
        return BASE_NS::CloneData(dst, sizeof(T) * dstSize, src, sizeof(T) * srcSize);
    }
    RENDER_NS::RenderHandleReference alloc(size_t size)
    {
        if (size <= 0 || !mgr_) {
            return {};
        }
        const size_t byteSize = sizeof(T) * size;
        if (byteSize > MAX_GPU_BUFFER_BYTE_SIZE) {
            CORE_LOG_E("Failed to alloc memory (%zu) exceeding UINT32_MAX (%u)", byteSize, UINT32_MAX);
            return {};  // 或 assert/log
        }
        const auto getvalidSize = [](const uint32_t byteSize) -> uint32_t { return BASE_NS::Math::max(byteSize, 16u); };
        return mgr_->Create(GetStagingBufferDesc(getvalidSize(static_cast<uint32_t>(byteSize))));
    }

    bool init(size_t n)
    {
        handle_ = alloc(n);
        if (!RENDER_NS::RenderHandleUtil::IsValid(handle_.GetHandle())) {
            return false;
        }
        mappedPtr_ = map(handle_);
        if (mappedPtr_ == nullptr) {
            CORE_LOG_E("Failed to map GPU buffer");
            return false;
        }
        isMapped_ = true;
        capacity_ = static_cast<uint32_t>(n);
        return true;
    }
    static RENDER_NS::GpuBufferDesc GetStagingBufferDesc(const uint32_t byteSize)
    {
        return RENDER_NS::GpuBufferDesc {
            RENDER_NS::CORE_BUFFER_USAGE_TRANSFER_DST_BIT | RENDER_NS::CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                RENDER_NS::CORE_BUFFER_USAGE_TRANSFER_SRC_BIT,
            RENDER_NS::CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT | RENDER_NS::CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
            RENDER_NS::CORE_ENGINE_BUFFER_CREATION_CREATE_IMMEDIATE |
                RENDER_NS::CORE_ENGINE_BUFFER_CREATION_MAP_OUTSIDE_RENDERER |
                RENDER_NS::CORE_ENGINE_BUFFER_CREATION_ENABLE_MEMORY_OPTIMIZATIONS |
                RENDER_NS::CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS,
            byteSize,
            {},
        };
    }

public:
    GpuVector() : mgr_(nullptr), mappedPtr_(nullptr), capacity_(0), size_(0), isMapped_(false) {}
    GpuVector(RENDER_NS::IGpuResourceManager& gpuResourceMgr, uint32_t n)
        : mgr_(&gpuResourceMgr), mappedPtr_(nullptr), capacity_(n), size_(n), isMapped_(false)
    {
        if (n > 0) {
            init(n);
        }
    }

    ~GpuVector()
    {
        if (isMapped_ && mappedPtr_) {
            unmap(handle_);
        }
        isMapped_ = false;
        mappedPtr_ = nullptr;
        size_ = 0;
        handle_ = {};
        capacity_ = 0;
    }

    void setManager(RENDER_NS::IGpuResourceManager& gpuResourceMgr)
    {
        if (!mgr_) {
            mgr_ = &gpuResourceMgr;
        }
    }

    GpuVector(const GpuVector&) = delete;
    GpuVector& operator=(const GpuVector&) = delete;

    GpuVector(GpuVector&& other) noexcept
        : mgr_(other.mgr_), handle_(other.handle_), mappedPtr_(other.mappedPtr_), capacity_(other.capacity_),
          size_(other.size_), isMapped_(other.isMapped_)
    {
        other.mgr_ = nullptr;
        other.handle_ = {};
        other.mappedPtr_ = nullptr;
        other.capacity_ = 0;
        other.size_ = 0;
        other.isMapped_ = false;
    }

    GpuVector& operator=(GpuVector&& other) noexcept
    {
        if (this != &other) {
            if (isMapped_ && mappedPtr_) {
                unmap(handle_);
            }

            mgr_ = other.mgr_;
            handle_ = other.handle_;
            mappedPtr_ = other.mappedPtr_;
            capacity_ = other.capacity_;
            size_ = other.size_;
            isMapped_ = other.isMapped_;

            other.mgr_ = nullptr;
            other.handle_ = {};
            other.mappedPtr_ = nullptr;
            other.capacity_ = 0;
            other.size_ = 0;
            other.isMapped_ = false;
        }
        return *this;
    }

    T& operator[](size_t index)
    {
        PLUGIN_ASSERT(isMapped_);
        PLUGIN_ASSERT(mappedPtr_);
        PLUGIN_ASSERT(index < size());
        return mappedPtr_[index];
    }

    const T& operator[](size_t index) const
    {
        PLUGIN_ASSERT(isMapped_);
        PLUGIN_ASSERT(mappedPtr_);
        PLUGIN_ASSERT(index < size());
        return mappedPtr_[index];
    }

    T* begin() noexcept
    {
        return mappedPtr_;
    }

    const T* begin() const noexcept
    {
        return mappedPtr_;
    }

    T* end() noexcept
    {
        return mappedPtr_ ? mappedPtr_ + size_ : nullptr;
    }

    const T* end() const noexcept
    {
        return mappedPtr_ ? mappedPtr_ + size_ : nullptr;
    }

    size_t size() const noexcept
    {
        return size_;
    }

    bool empty() const noexcept
    {
        return size_ == 0;
    }

    size_t capacity() const noexcept
    {
        return capacity_;
    }

    void push_back(const T& value)
    {
        if (!mgr_) {
            return;
        }

        if (!isMapped_ || !mappedPtr_) {
            if (!init(1u)) {
                return;
            }
        }
        if (size_ + 1u > capacity_) {
            uint32_t newCapacity = capacity_ == 0 ? 1u : capacity_ * 2;
            reserve(newCapacity);
            if (capacity_ < newCapacity) {
                return;
            }
        }
        mappedPtr_[size_] = value;
        ++size_;
    }

    void resize(size_t newSize)
    {
        if (!mgr_) {
            return;
        }

        if (!isMapped_ || !mappedPtr_) {
            if (!init(newSize)) {
                return;
            }
        }

        if (newSize <= capacity_) {
            size_ = static_cast<uint32_t>(newSize);
            return;
        }

        reserve(newSize);
        if (capacity_ < static_cast<uint32_t>(newSize)) {
            return;
        }
        size_ = static_cast<uint32_t>(newSize);
    }

    void reserve(size_t newCapacity)
    {
        if (!mgr_) {
            return;
        }

        if (!isMapped_ || !mappedPtr_) {
            if (!init(newCapacity)) {
                return;
            }
        }
        if (newCapacity <= capacity_) {
            return;
        }
        RENDER_NS::RenderHandleReference newHandle = alloc(newCapacity);
        if (!RENDER_NS::RenderHandleUtil::IsValid(newHandle.GetHandle())) {
            CORE_LOG_E("Failed to reallocate GPU buffer");
            return;
        }

        T* newMappedPtr = map(newHandle);
        if (!newMappedPtr) {
            CORE_LOG_E("Failed to map new GPU buffer");
            newHandle = {};
            return;
        }
        if (!clone(newMappedPtr, newCapacity, mappedPtr_, size_)) {
            unmap(newHandle);
            return;
        }

        unmap(handle_);
        handle_ = newHandle;
        mappedPtr_ = newMappedPtr;
        capacity_ = static_cast<uint32_t>(newCapacity);
        isMapped_ = true;
    }

    void shrink_to_fit()
    {
        if (!mgr_) {
            return;
        }

        if (size_ == capacity_) {
            return;
        }

        RENDER_NS::RenderHandleReference newHandle = alloc(size_);
        if (!RENDER_NS::RenderHandleUtil::IsValid(newHandle.GetHandle())) {
            CORE_LOG_E("Failed to reallocate GPU buffer");
            return;
        }

        T* newMappedPtr = map(newHandle);
        if (newMappedPtr == nullptr) {
            CORE_LOG_E("Failed to map new GPU buffer");
            newHandle = {};
            return;
        }
        if (!clone(newMappedPtr, size_, mappedPtr_, size_)) {
            unmap(newHandle);
            return;
        }

        unmap(handle_);
        handle_ = newHandle;
        mappedPtr_ = newMappedPtr;
        capacity_ = size_;
        isMapped_ = true;
    }

    // finalize - Finish usage, unmap memory and return GPU buffer handle.
    // No functions that modify this object should be called after this operation.
    RENDER_NS::RenderHandleReference finalize()
    {
        if (isMapped_ && mappedPtr_ && RENDER_NS::RenderHandleUtil::IsValid(handle_.GetHandle())) {
            unmap(handle_);
            isMapped_ = false;
            mappedPtr_ = nullptr;
        }
        mgr_ = nullptr;
        return handle_;
    }

    T* data() noexcept
    {
        return mappedPtr_;
    }

    const T* data() const noexcept
    {
        return mappedPtr_;
    }
};
} // namespace GS

#endif // PLUGIN_GS_GPU_VECTOR_H
