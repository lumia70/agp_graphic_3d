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

#ifndef GS_AI_EDIT_ERROR_CODE_H
#define GS_AI_EDIT_ERROR_CODE_H

namespace GS {
enum class GSAIEditErrorCode {
    SUCCESS = 0,

    ERROR_INPUT_COORDINATES_NOT_NORMALIZED = 1025200001,
    ERROR_SUBJECT_IS_TOO_FAR_FROM_CAMERA = 1025200002,
    ERROR_CLICK_POINTS_NOT_ON_SUBJECT = 1025200003,
    ERROR_DATA_PARSING_FAILED = 1025200004,
    ERROR_EXTRACT_FAILED = 1025200005,
};
}

#endif