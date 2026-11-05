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
#ifndef SPATIAL_CODEC_FACTORY_H
#define SPATIAL_CODEC_FACTORY_H

#include "spatial_buffer.h"
#include "spatial_codec_demuxer.h"
#include "spatial_meta.h"
#include "spatial_model_decoder.h"

namespace Spatial {
class SpatialCodecFactory {
public:
    SpatialCodecFactory() = delete;
    ~SpatialCodecFactory() = delete;

    static bool IsLoaded();

    static std::shared_ptr<SpatialMeta> CreateMeta(const std::string& format);

    static std::shared_ptr<SpatialBuffer> CreateBuffer(const std::string& format);

    static std::shared_ptr<SpatialModelDecoder> CreateDecoder(const std::string& format);

    static std::shared_ptr<SpatialCodecDemuxer> CreateDemuxer(const std::string& format);
};
}

#endif
