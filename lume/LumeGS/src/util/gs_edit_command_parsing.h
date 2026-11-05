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

#ifndef PLUGIN_GS_EDIT_COMMAND_PARSING_H
#define PLUGIN_GS_EDIT_COMMAND_PARSING_H

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <climits>
#include <core/log.h>
#include <functional>
#include <memory>
#include <numeric>
#include <optional>
#include <plugin_gs/render/intf_render_data_store_gs_edit.h>
#include <thread>

#include <base/containers/vector.h>

namespace GS_EDIT {
using namespace GS;

/**
 *  Example commands usage:
 *      pop_last
 *      select_box [start_x, start_y, end_x, end_y]
 *      transform [1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1, use_selection]
 *      invert_selection
 */

struct GsEditActionParseResult {
    std::optional<GsEditAction> editAction;
    bool isPopAction = false;
};

namespace {
using ParamParser = std::function<BASE_NS::shared_ptr<IGsEditActionParams>(const uint8_t*, size_t)>;

/**
 * Enum class defining the expected data type during stream parsing.
 */
enum class ParseType : uint8_t {
    NONE = 0,
    FLOAT,
    UINT32,
    UINT64,
};

struct ActionRegistry {
    GsEditActionType type;
    ParamParser parser;
    ParseType parseType = ParseType::NONE;
};

inline bool IsWhitespace(char c)
{
    return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

/**
 * This function safely reads a value of type T from a pointer to
 * raw bytes (`const void*`), using `BASE_NS::CloneData` to ensure
 * memory access is performed in a way that avoids optimization pitfalls
 * introduced by the compiler's TBAA
 */
template <typename T>
T Load(const void* ptr)
{
    static_assert(std::is_trivially_copyable_v<T>, "Must be trivial");
    T res;
    BASE_NS::CloneData(&res, sizeof(T), ptr, sizeof(T));
    return res;
}

// parameter parsers
const auto ParseSelectBox = [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 4u) {
        return nullptr;
    }

    auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::SelectBox>();
    parsed->min.x = Load<float>(data + sizeof(float) * 0u);
    parsed->min.y = Load<float>(data + sizeof(float) * 1u);
    parsed->max.x = Load<float>(data + sizeof(float) * 2u);
    parsed->max.y = Load<float>(data + sizeof(float) * 3u);

    return parsed;
};

const auto ParseTransform = [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 17u) {
        return nullptr;
    }

    const auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::Transform>();
    for (size_t ii = 0; ii < 16u; ii++) {
        parsed->transform.data[ii] = Load<float>(data + sizeof(float) * ii);
    }

    parsed->useSelection = (Load<float>(data + sizeof(float) * 16u) != 0.0f);
    return parsed;
};

const auto ParseColorSelection = [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 5u) {
        return nullptr;
    }

    const auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::ColorSelection>();
    parsed->color.x = Load<float>(data + sizeof(float) * 0u);
    parsed->color.y = Load<float>(data + sizeof(float) * 1u);
    parsed->color.z = Load<float>(data + sizeof(float) * 2u);
    parsed->color.w = Load<float>(data + sizeof(float) * 3u);
    parsed->colorMode = static_cast<uint32_t>(Load<float>(data + sizeof(float) * 4u));

    return parsed;
};

const auto ParseSelect3DBox = [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 6u) {
        return nullptr;
    }

    const auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::Select3DBox>();
    parsed->min.x = Load<float>(data + sizeof(float) * 0u);
    parsed->min.y = Load<float>(data + sizeof(float) * 1u);
    parsed->min.z = Load<float>(data + sizeof(float) * 2u);
    parsed->max.x = Load<float>(data + sizeof(float) * 3u);
    parsed->max.y = Load<float>(data + sizeof(float) * 4u);
    parsed->max.z = Load<float>(data + sizeof(float) * 5u);

    return parsed;
};

const auto ParseSelectByIndex = [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 2u) {
        return nullptr;
    }

    const auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::SelectByIndex>();
    void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(Load<uint64_t>(data + sizeof(uint64_t) * 0u)));
    auto length = Load<uint64_t>(data + sizeof(uint64_t) * 1u);
    parsed->indices = reinterpret_cast<uint32_t*>(addr);
    parsed->bufSize = length;

    return parsed;
};

const auto ParseSelectByMask = [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 6u) {
        return nullptr;
    }

    const auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::SelectByMask>();
    void* addr = reinterpret_cast<void*>(static_cast<uintptr_t>(Load<uint64_t>(data + sizeof(uint64_t) * 0u)));
    parsed->width =  Load<uint64_t>(data + sizeof(uint64_t) * 1u);
    parsed->height = Load<uint64_t>(data + sizeof(uint64_t) * 2u);
    parsed->rowSize = Load<uint64_t>(data + sizeof(uint64_t) * 3u);
    parsed->format = static_cast<BASE_NS::Format>(
        static_cast<uint32_t>(Load<uint64_t>(data + sizeof(uint64_t) * 4u)));
    parsed->formatByteSize = Load<uint64_t>(data + sizeof(uint64_t) * 5u);
    parsed->data = reinterpret_cast<uint8_t*>(addr);

    return parsed;
};

const auto ParseNoParams = [](const uint8_t*, size_t) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    return BASE_NS::make_shared<GS_EDIT_PARAMS::NoParams>();
};

// Parse camera intrinsics: 4 floats (fx, fy, cx, cy) + 2 uint32 (width, height).
// All 6 values are parsed as float (ParseType::FLOAT), width/height cast to uint32.
const auto ParseCameraIntrinsics = [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 6u) {
        return nullptr;
    }

    const auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::CameraIntrinsics>();
    parsed->fx = Load<float>(data + sizeof(float) * 0u);
    parsed->fy = Load<float>(data + sizeof(float) * 1u);
    parsed->cx = Load<float>(data + sizeof(float) * 2u);
    parsed->cy = Load<float>(data + sizeof(float) * 3u);
    parsed->width = static_cast<uint32_t>(Load<float>(data + sizeof(float) * 4u));
    parsed->height = static_cast<uint32_t>(Load<float>(data + sizeof(float) * 5u));

    return parsed;
};

// Parse floater removal options: 2 floats (mode as uint32, taskId as uint64).
// Both parsed as float (ParseType::FLOAT), then cast to their integer types.
const auto ParseFloaterRemovalOptions =
    [](const uint8_t* data, size_t size) -> BASE_NS::shared_ptr<IGsEditActionParams> {
    if (!data || size != 2u) {
        return nullptr;
    }
    const auto parsed = BASE_NS::make_shared<GS_EDIT_PARAMS::FloaterRemovalOptions>();
    parsed->mode = static_cast<uint32_t>(Load<float>(data));
    parsed->taskId = static_cast<uint64_t>(Load<float>(data + sizeof(float)));
    return parsed;
};

// command registry
static const BASE_NS::unordered_map<BASE_NS::string, ActionRegistry> GS_EDIT_PARSE_COMMAND_REGISTRY = {
    { "select_box",        { GsEditActionType::SelectBox, ParseSelectBox,                  ParseType::FLOAT  }},
    { "select_3d_box",     { GsEditActionType::Select3DBox, ParseSelect3DBox,              ParseType::FLOAT  }},
    { "select_by_index",   { GsEditActionType::SelectByIndex, ParseSelectByIndex,          ParseType::UINT64 }},
    { "select_by_mask",    { GsEditActionType::SelectByMask, ParseSelectByMask,            ParseType::UINT64 }},
    { "clear_selection",   { GsEditActionType::ClearSelection, ParseNoParams,                                }},
    { "select_individual", { GsEditActionType::SelectIndividual, ParseNoParams,                              }},
    { "invert_selection",  { GsEditActionType::InvertSelection, ParseNoParams,                               }},
    { "remove_selected",   { GsEditActionType::RemoveSelected, ParseNoParams,                                }},
    { "remove_floaters",   { GsEditActionType::FloaterRemoval, ParseFloaterRemovalOptions, ParseType::FLOAT  }},
    { "transform",         { GsEditActionType::Transform, ParseTransform,                  ParseType::FLOAT  }},
    { "color_selection",   { GsEditActionType::ColorSelection, ParseColorSelection,        ParseType::FLOAT  }},
};

BASE_NS::vector<uint8_t> ParseFloatParameters(const BASE_NS::string& paramsStr)
{
    BASE_NS::vector<uint8_t> values;
    size_t pos = 0;

    while (pos < paramsStr.length()) {
        while (pos < paramsStr.length() && IsWhitespace(paramsStr[pos])) {
            pos++;
        }
        if (pos >= paramsStr.length()) {
            break;
        }

        size_t end = pos;
        while (end < paramsStr.length() && paramsStr[end] != ',') {
            end++;
        }

        BASE_NS::string numStr = BASE_NS::string(paramsStr.substr(pos, end - pos));
        while (!numStr.empty() && IsWhitespace(numStr[numStr.length() - 1])) {
            numStr.erase(numStr.length() - 1, 1);
        }

        if (!numStr.empty()) {
            char* endPtr = nullptr;
            float value = strtof(numStr.c_str(), &endPtr);
            if (endPtr == numStr.c_str() || (*endPtr != '\0' && !IsWhitespace(*endPtr))) {
                // parse error
                return {};
            }
            constexpr auto BUF_SIZE = sizeof(float);
            uint8_t data[BUF_SIZE] {};
            BASE_NS::CloneData(data, BUF_SIZE, &value, BUF_SIZE);
            for (size_t i = 0; i < BUF_SIZE; ++i) {
                values.push_back(data[i]);
            }
        }
        pos = (end < paramsStr.length()) ? end + 1 : paramsStr.length();
    }

    return values;
}

BASE_NS::vector<uint8_t> ParseUint32Parameters(const BASE_NS::string& paramsStr)
{
    BASE_NS::vector<uint8_t> values;
    size_t pos = 0;

    while (pos < paramsStr.length()) {
        while (pos < paramsStr.length() && IsWhitespace(paramsStr[pos])) {
            pos++;
        }
        if (pos >= paramsStr.length()) {
            break;
        }

        size_t end = pos;
        while (end < paramsStr.length() && paramsStr[end] != ',') {
            end++;
        }

        BASE_NS::string numStr = BASE_NS::string(paramsStr.substr(pos, end - pos));
        while (!numStr.empty() && IsWhitespace(numStr[numStr.length() - 1])) {
            numStr.erase(numStr.length() - 1, 1);
        }

        if (!numStr.empty()) {
            char* endPtr = nullptr;
            uint32_t value = static_cast<uint32_t>(strtoul(numStr.c_str(), &endPtr, 10));
            if (endPtr == numStr.c_str() || (*endPtr != '\0' && !IsWhitespace(*endPtr))) {
                // parse error
                return {};
            }
            constexpr auto BUF_SIZE = sizeof(uint32_t);
            uint8_t data[BUF_SIZE] {};
            BASE_NS::CloneData(data, BUF_SIZE, &value, BUF_SIZE);
            for (size_t i = 0; i < BUF_SIZE; ++i) {
                values.push_back(data[i]);
            }
        }
        pos = (end < paramsStr.length()) ? end + 1 : paramsStr.length();
    }

    return values;
}

BASE_NS::vector<uint8_t> ParseUint64Parameters(const BASE_NS::string& paramsStr)
{
    BASE_NS::vector<uint8_t> values;
    size_t pos = 0;

    while (pos < paramsStr.length()) {
        while (pos < paramsStr.length() && IsWhitespace(paramsStr[pos])) {
            pos++;
        }
        if (pos >= paramsStr.length()) {
            break;
        }

        size_t end = pos;
        while (end < paramsStr.length() && paramsStr[end] != ',') {
            end++;
        }

        BASE_NS::string numStr = BASE_NS::string(paramsStr.substr(pos, end - pos));
        while (!numStr.empty() && IsWhitespace(numStr[numStr.length() - 1])) {
            numStr.erase(numStr.length() - 1, 1);
        }

        if (!numStr.empty()) {
            uint64_t value = 0;
            auto [ptr, ec] = std::from_chars(numStr.c_str(), numStr.c_str() + numStr.length(), value);
            if (ec == std::errc::result_out_of_range || ptr == numStr.c_str()) {
                return {};
            }
            constexpr auto BUF_SIZE = sizeof(uint64_t);
            uint8_t data[BUF_SIZE] {};
            BASE_NS::CloneData(data, BUF_SIZE, &value, BUF_SIZE);
            for (size_t i = 0; i < BUF_SIZE; ++i) {
                values.push_back(data[i]);
            }
        }
        pos = (end < paramsStr.length()) ? end + 1 : paramsStr.length();
    }

    return values;
}
} // namespace

GsEditActionParseResult ParseGsEditActionString(const BASE_NS::string& command)
{
    GsEditActionParseResult result;

    // trim whitespace
    auto trimmed = command;
    while (!trimmed.empty() && IsWhitespace(trimmed.front())) {
        trimmed.erase(0, 1);
    }
    while (!trimmed.empty() && IsWhitespace(trimmed[trimmed.length() - 1])) {
        trimmed.erase(trimmed.length() - 1, 1);
    }

    // handle pop_last (undo)
    if (trimmed == "pop_last") {
        result.isPopAction = true;
        return result;
    }

    // extract command name
    const size_t bracketPos = trimmed.find('[');
    BASE_NS::string cmdName =
        BASE_NS::string((bracketPos != BASE_NS::string::npos) ? trimmed.substr(0, bracketPos) : trimmed);
    while (!cmdName.empty() && IsWhitespace(cmdName[cmdName.length() - 1])) {
        cmdName.erase(cmdName.length() - 1, 1);
    }

    // find command in registry
    const auto it = GS_EDIT_PARSE_COMMAND_REGISTRY.find(cmdName);
    if (it == GS_EDIT_PARSE_COMMAND_REGISTRY.end()) {
        // unknown command
        return result;
    }

    // parse parameters
    BASE_NS::vector<uint8_t> values;
    size_t size {};
    if (bracketPos != BASE_NS::string::npos) {
        const size_t closePos = trimmed.find(']', bracketPos);
        if (closePos == BASE_NS::string::npos) {
            // invalid
            return result;
        }

        const BASE_NS::string paramsStr = BASE_NS::string(trimmed.substr(bracketPos + 1, closePos - bracketPos - 1));
        if (it->second.parseType == ParseType::FLOAT) {
            values = ParseFloatParameters(paramsStr);
            size = values.size() / sizeof(float);
        } else if (it->second.parseType == ParseType::UINT32) {
            values = ParseUint32Parameters(paramsStr);
            size = values.size() / sizeof(uint32_t);
        } else if (it->second.parseType == ParseType::UINT64) {
            values = ParseUint64Parameters(paramsStr);
            size = values.size() / sizeof(uint64_t);
        }
        if (paramsStr.length() > 0 && values.empty()) {
            // parse error
            return result;
        }
    }

    // parse specific command
    const auto params = it->second.parser(values.data(), size);
    if (!params) {
        return result;
    }

    result.editAction = GsEditAction { it->second.type, params };
    return result;
}

} // namespace GS_EDIT

#endif
