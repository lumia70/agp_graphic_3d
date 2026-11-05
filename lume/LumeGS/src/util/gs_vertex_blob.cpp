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

#include "gs_vertex_blob.h"

#include <algorithm>

#include "gs_log.h"
#include "gs_utils.h"

namespace GS {
inline void PackGsData(const GsObjectLoadData::GsSplatData& data, GsObjectLoadData::GsSplatDataPacked& packData)
{
    packData.positionOpacityPacked = BASE_NS::Math::UVec2(PackHalf2x16(data.positionOpacity.x, data.positionOpacity.y),
        PackHalf2x16(data.positionOpacity.z, data.positionOpacity.w));

    float packedColor = data.covBColorPacked.w;
    packData.covColorPacked =
        BASE_NS::Math::UVec4(PackHalf2x16(SqrtWithSign(data.covAScale.x), SqrtWithSign(data.covAScale.y)),
            PackHalf2x16(SqrtWithSign(data.covAScale.z), SqrtWithSign(data.covBColorPacked.x)),
            PackHalf2x16(SqrtWithSign(data.covBColorPacked.y), SqrtWithSign(data.covBColorPacked.z)),
            BitCast<uint32_t>(packedColor));
    if (data.astcOffsetColor.w > 0) {
        packData.astcOffsetColor =
            BASE_NS::Math::UVec2(PackUint2x16(data.astcOffsetColor.x * 4u + data.astcOffsetColor.w / 4u,
                                     data.astcOffsetColor.y * 4u + data.astcOffsetColor.w % 4u),
                data.astcOffsetColor.z);
    } else {
        packData.astcOffsetColor =
            BASE_NS::Math::UVec2(PackUint2x16(data.astcOffsetColor.x, data.astcOffsetColor.y), data.astcOffsetColor.z);
    }
}

inline void UnpackGsData(const GsObjectLoadData::GsSplatDataPacked& packData, GsObjectLoadData::GsSplatData& data)
{
    auto tmp1 = UnpackHalf2x16(packData.positionOpacityPacked.y);
    data.positionOpacity = BASE_NS::Math::Vec4(UnpackHalf2x16(packData.positionOpacityPacked.x), tmp1.x, tmp1.y);

    tmp1 = UnpackHalf2x16(packData.covColorPacked.x);
    auto tmp2 = UnpackHalf2x16(packData.covColorPacked.y);
    data.covAScale = BASE_NS::Math::Vec4(SqWithSign(tmp1.x), SqWithSign(tmp1.y), SqWithSign(tmp2.x), 1.0f);

    tmp1 = UnpackHalf2x16(packData.covColorPacked.z);
    const float color = BitCast<float>(packData.covColorPacked.w);
    data.covBColorPacked = BASE_NS::Math::Vec4(SqWithSign(tmp2.y), SqWithSign(tmp1.x), SqWithSign(tmp1.y), color);
    auto tmp3 = UnpackUint2x16(packData.astcOffsetColor.x);
    data.astcOffsetColor = BASE_NS::Math::UVec4(tmp3.x, tmp3.y, packData.astcOffsetColor.y, 0);
}

void GsVertexBlob::SetPackingEnabled(bool enable)
{
    if (initialized_ || data_.capacity() > 0 || dataPacked_.capacity() > 0) {
        // already alloc memory or set allocator, cannot change pack mode anymore
        return;
    }
    isPacked_ = enable;
}

void GsVertexBlob::SetGPUAllocator(RENDER_NS::IGpuResourceManager& gpuMgr)
{
    if (isPacked_) {
        dataPacked_.setManager(gpuMgr);
    } else {
        data_.setManager(gpuMgr);
    }
    initialized_ = true;
}

GsVertexBlob::GsVertexBlob(GsVertexBlob&& other) noexcept
    : data_(BASE_NS::move(other.data_)), dataPacked_(BASE_NS::move(other.dataPacked_)),
      initialized_(other.initialized_), isPacked_(other.isPacked_)
{
    other.isPacked_ = false;
    other.initialized_ = false;
}

GsVertexBlob& GsVertexBlob::operator=(GsVertexBlob&& other) noexcept
{
    if (this != &other) {
        data_ = BASE_NS::move(other.data_);
        dataPacked_ = BASE_NS::move(other.dataPacked_);
        initialized_ = other.initialized_;
        isPacked_ = other.isPacked_;
        other.initialized_ = false;
        other.isPacked_ = false;
    }
    return *this;
}

// use setter and getter instead of operator
void GsVertexBlob::Set(size_t index, const GsObjectLoadData::GsSplatData& data)
{
    PLUGIN_ASSERT(index < Size());
    if (isPacked_) {
        GsObjectLoadData::GsSplatDataPacked packed;
        PackGsData(data, packed);
        dataPacked_[index] = packed;
        return;
    }
    data_[index] = data;
}

void GsVertexBlob::Set(size_t index, GsObjectLoadData::GsSplatData&& data)
{
    PLUGIN_ASSERT(index < Size());
    if (isPacked_) {
        GsObjectLoadData::GsSplatDataPacked packed;
        PackGsData(data, packed);
        dataPacked_[index] = packed;
        return;
    }
    data_[index] = BASE_NS::move(data);
}

GsObjectLoadData::GsSplatData GsVertexBlob::Get(size_t index) const
{
    PLUGIN_ASSERT(index < Size());
    if (isPacked_) {
        GsObjectLoadData::GsSplatData data;
        UnpackGsData(dataPacked_[index], data);
        return data;
    }
    return data_[index];
}

size_t GsVertexBlob::Size() const noexcept
{
    return isPacked_ ? dataPacked_.size() : data_.size();
}

bool GsVertexBlob::Empty() const noexcept
{
    return isPacked_ ? dataPacked_.empty() : data_.empty();
}

size_t GsVertexBlob::Capacity() const noexcept
{
    return isPacked_ ? dataPacked_.capacity() : data_.capacity();
}

const void* GsVertexBlob::Data() const noexcept
{
    if (isPacked_) {
        return static_cast<const void*>(dataPacked_.data());
    }
    return static_cast<const void*>(data_.data());
}

void* GsVertexBlob::Data() noexcept
{
    if (isPacked_) {
        return static_cast<void*>(dataPacked_.data());
    }
    return static_cast<void*>(data_.data());
}

void GsVertexBlob::PushBack(const GsObjectLoadData::GsSplatData& value)
{
    if (!isPacked_) {
        data_.push_back(value);
        return;
    }
    GsObjectLoadData::GsSplatDataPacked d;
    PackGsData(value, d);
    dataPacked_.push_back(d);
}

void GsVertexBlob::Resize(size_t count)
{
    if (isPacked_) {
        dataPacked_.resize(count);
    } else {
        data_.resize(count);
    }
}

void GsVertexBlob::Reserve(size_t count)
{
    if (isPacked_) {
        dataPacked_.reserve(count);
    } else {
        data_.reserve(count);
    }
}

void GsVertexBlob::ShrinkToFit()
{
    if (isPacked_) {
        dataPacked_.shrink_to_fit();
    } else {
        data_.shrink_to_fit();
    }
}

BASE_NS::vector<GsObjectLoadData::GsSplatData> GsVertexBlob::GetOriginVector() const
{
    if (!isPacked_) {
        // assign directly
        return BASE_NS::vector<GsObjectLoadData::GsSplatData>(data_.begin(), data_.end());
    }

    BASE_NS::vector<GsObjectLoadData::GsSplatData> result;
    result.reserve(Size());
    // for packed data, need to unpack first (Get() will unpack it internally)
    for (size_t i = 0; i < Size(); ++i) {
        result.push_back(Get(i));
    }
    return result;
}

RENDER_NS::RenderHandleReference GsVertexBlob::FinishGPUAlloc() noexcept
{
    return isPacked_ ? dataPacked_.finalize() : data_.finalize();
}

} // namespace GS