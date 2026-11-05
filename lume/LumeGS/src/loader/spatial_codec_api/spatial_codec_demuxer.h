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
#ifndef SPATIAL_CODEC_DEMUXER_H
#define SPATIAL_CODEC_DEMUXER_H

#include "spatial_buffer.h"
#include "spatial_error_code.h"
#include "spatial_meta.h"


namespace Spatial {
class SpatialCodecDemuxer {
public:
    virtual ~SpatialCodecDemuxer() = default;

    virtual SpatialErrorCode Initialize(const std::shared_ptr<SpatialBuffer>& inputBuffer,
        const SpatialMeta& config) = 0;

    virtual SpatialErrorCode GetMeta(const std::string& name,
        std::vector<std::shared_ptr<const SpatialMeta>>& outMeta) = 0;

    virtual SpatialErrorCode GetData(const std::string& name, const SpatialMeta& dataDesc,
        std::shared_ptr<const SpatialBuffer>& outBuffer) = 0;
};
}

#endif
