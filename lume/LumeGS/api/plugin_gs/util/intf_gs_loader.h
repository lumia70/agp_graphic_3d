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

#ifndef API_GS_UTIL_IGSLOADER_H
#define API_GS_UTIL_IGSLOADER_H

#include <3d/loaders/intf_scene_loader.h>

CORE3D_BEGIN_NAMESPACE()
class ISceneNode;
CORE3D_END_NAMESPACE()

namespace GS {

class IGsLoader : public CORE_NS::IInterface {
public:
    static constexpr auto UID = BASE_NS::Uid { "b17bbc60-20c2-4a2d-9446-c77274a15872" };

    virtual CORE3D_NS::ISceneLoader::Result Load(BASE_NS::string_view uri, int64_t offset) = 0;
    virtual CORE3D_NS::ISceneLoader::Result Load(BASE_NS::string_view uri) = 0;
};

} // namespace GS

#endif // API_GS_UTIL_IGSLOADER_H