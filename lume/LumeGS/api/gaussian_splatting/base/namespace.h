/*
 * Copyright (c) 2024 Huawei Device Co., Ltd.
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

#ifndef META_GS_NAMESPACE_H
#define META_GS_NAMESPACE_H

#include <base/util/uid.h>

#define GS_ADDON_NS GsAddon
#define GS_ADDON_BEGIN_NAMESPACE() namespace GS_ADDON_NS {
#define GS_ADDON_END_NAMESPACE() }

GS_ADDON_BEGIN_NAMESPACE()
static constexpr BASE_NS::Uid UID_GS_ADDON_PLUGIN{
    "447c4937-a9f4-4fd9-922a-f1bf43ba727e"};
GS_ADDON_END_NAMESPACE()

#endif
