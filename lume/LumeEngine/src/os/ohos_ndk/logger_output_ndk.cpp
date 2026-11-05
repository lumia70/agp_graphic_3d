/*
 * Copyright (c) 2022 Huawei Device Co., Ltd.
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

/*
 * NDK-compatible logger output using OH_LOG_Print (public NDK API).
 * Replaces the internal-API version that uses HILOG_IMPL/LOG_CORE/LOG_LEVEL_MIN.
 */

#include "log/logger_output.h"

#include <cstdarg>
#include <sstream>
#include <string_view>

#include <hilog/log.h>

#include <core/namespace.h>

#include "log/logger.h"

#undef LOG_DOMAIN
#define LOG_DOMAIN 0xD001402
#undef LOG_TAG
#define LOG_TAG "ohos_lume"

CORE_BEGIN_NAMESPACE()
using BASE_NS::string_view;

namespace {
BASE_NS::string_view GetFilename(BASE_NS::string_view path)
{
    if (auto const pos = path.find_last_of("\\/"); pos != BASE_NS::string_view::npos) {
        return path.substr(pos + 1);
    }
    return path;
}
} // namespace

class LogcatOutput final : public Logger::IOutput {
public:
    void Write(
        ILogger::LogLevel logLevel, const string_view filename, int linenumber, const string_view message) override
    {
        LogLevel logPriority;
        switch (logLevel) {
            case ILogger::LogLevel::LOG_VERBOSE:
                logPriority = LOG_DEBUG;
                break;
            case ILogger::LogLevel::LOG_DEBUG:
                logPriority = LOG_DEBUG;
                break;
            case ILogger::LogLevel::LOG_INFO:
                logPriority = LOG_INFO;
                break;
            case ILogger::LogLevel::LOG_WARNING:
                logPriority = LOG_WARN;
                break;
            case ILogger::LogLevel::LOG_ERROR:
                logPriority = LOG_ERROR;
                break;
            case ILogger::LogLevel::LOG_FATAL:
                logPriority = LOG_FATAL;
                break;
            default:
                logPriority = LOG_DEBUG;
                break;
        }

        if (!filename.empty()) {
            std::stringstream outputStream;
            auto const filenameView = GetFilename(filename);
            outputStream << '(' << std::string_view(filenameView.data(), filenameView.size()) << ':' << linenumber
                         << "): ";
            outputStream << std::string_view(message.data(), message.size());
            OH_LOG_Print(LOG_APP, logPriority, LOG_DOMAIN, LOG_TAG, "%{public}s", outputStream.str().c_str());
        } else {
            OH_LOG_Print(LOG_APP, logPriority, LOG_DOMAIN, LOG_TAG, "%{public}s", message.data());
        }
    }

protected:
    void Destroy() override
    {
        delete this;
    }
};

ILogger::IOutput::Ptr CreateLoggerConsoleOutput()
{
    return ILogger::IOutput::Ptr{new LogcatOutput};
}

ILogger::IOutput::Ptr CreateLoggerDebugOutput()
{
    return {};
}
CORE_END_NAMESPACE()
