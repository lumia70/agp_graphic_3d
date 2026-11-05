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

#ifndef PLUGIN_GS_LOG_H
#define PLUGIN_GS_LOG_H

#include <cassert>

#include <core/implementation_uids.h>
#include <core/intf_logger.h>
#include <core/plugin/intf_class_register.h>

namespace GS {

#ifndef NDEBUG
#define PLUGIN_FILE_INFO __FILE__, __LINE__
#define PLUGIN_ASSERT(expression) \
    assert(!!(expression) || GS::PluginLogAssert(PLUGIN_FILE_INFO, !!(expression), #expression, ""))
#else
#define PLUGIN_ASSERT(...)
#endif

inline CORE_NS::ILogger* GetPluginLogger()
{
    static CORE_NS::ILogger* gPluginGlobalLogger { nullptr };
    if (gPluginGlobalLogger == nullptr) {
        gPluginGlobalLogger = CORE_NS::GetInstance<CORE_NS::ILogger>(CORE_NS::UID_LOGGER);
    }
    return gPluginGlobalLogger;
}

inline FORMAT_FUNC(5, 6) bool PluginLogAssert(const BASE_NS::string_view filename, int lineNumber, bool expression,
    const BASE_NS::string_view expressionString, FORMAT_ATTRIBUTE const char* format, ...)
{
    if (!expression) {
        if (CORE_NS::ILogger* logger = GetPluginLogger(); logger) {
            va_list vl;
            va_start(vl, format);
            logger->VLogAssert(filename, lineNumber, expression, expressionString, format, vl);
            va_end(vl);
        }
    }
    return expression;
}

} // namespace GS

#endif // PLUGIN_GS_LOG_H
