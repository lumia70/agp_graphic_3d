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

#ifndef ADDON_GS_BUILTIN_OBJECTS_H
#define ADDON_GS_BUILTIN_OBJECTS_H

#include <meta/base/types.h>
#include <gaussian_splatting/base/namespace.h>
#include <scene/base/namespace.h>

GS_ADDON_BEGIN_NAMESPACE()

// clang-format off
META_REGISTER_CLASS(GSNode, "a6e808b2-0e84-426c-9f09-2323002e20d7", META_NS::ObjectCategoryBits::NO_CATEGORY)

// clang-format on

GS_ADDON_END_NAMESPACE()

#endif
