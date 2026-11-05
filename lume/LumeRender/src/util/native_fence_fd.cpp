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

#include "util/native_fence_fd.h"

#if RENDER_HAS_NATIVE_FENCE_FD
#include <cerrno>
#include <poll.h>
#include <unistd.h>
#endif

#include "util/log.h"

RENDER_BEGIN_NAMESPACE()
namespace {
#if defined(__OHOS__)
// fdsan owner tag of native fence fd held by the renderer: high 32 bits are the domain id,
// low 32 bits identify the native fence use case
constexpr uint64_t fdsanTag{ (0xD001402ULL << 32) | 0xD001402ULL };
#endif

#if RENDER_HAS_NATIVE_FENCE_FD
// a signaled sync file polls readable, a hang-up or an error without POLLIN never signals
bool PollNativeFenceFd(const int32_t fd)
{
    pollfd pfd{fd, POLLIN, 0};
    int ret = -1;
    do {
        ret = poll(&pfd, 1U, static_cast<int>(NATIVE_FENCE_CPU_WAIT_TIMEOUT_MS));
    } while ((ret < 0) && (errno == EINTR));
    if (ret == 0) {
        PLUGIN_LOG_W("native fence did not signal within %u ms, the frame is not synchronized against it",
            NATIVE_FENCE_CPU_WAIT_TIMEOUT_MS);
        return false;
    }
    if ((ret < 0) || ((static_cast<uint32_t>(pfd.revents) & POLLIN) == 0U)) {
        PLUGIN_LOG_W("native fence wait failed (revents 0x%x), the frame is not synchronized against it",
            static_cast<uint32_t>(pfd.revents));
        return false;
    }
    return true;
}
#endif
}  // namespace

// Exchanges the fdsan ownership of a native fence fd: acquiring takes over a fd handed over by a client (tag 0 ->
// renderer), releasing hands it to e.g. a driver importing the descriptor (renderer -> 0). No-op on platforms without
// fdsan.
void ExchangeNativeFenceFdOwnership(const int32_t fd, const bool acquireOwnership)
{
#if defined(__OHOS__)
    if (fd >= 0) {
        fdsan_exchange_owner_tag(fd, acquireOwnership ? 0 : fdsanTag, acquireOwnership ? fdsanTag : 0);
    }
#else
    PLUGIN_UNUSED(fd);
    PLUGIN_UNUSED(acquireOwnership);
#endif
}

void CloseNativeFenceFd(const int32_t fd)
{
#if RENDER_HAS_NATIVE_FENCE_FD
    if (fd >= 0) {
#if defined(__OHOS__)
        // close the fd with the renderer tag so fdsan detects an owner mismatch (e.g. a double close)
        fdsan_close_with_tag(fd, fdsanTag);
#else
        close(fd);
#endif
    }
#else
    PLUGIN_UNUSED(fd);
#endif
}

void CpuWaitNativeFence(IRenderFrameUtil::WaitData& waitData)
{
    if (waitData.nativeFenceFd < 0) {
        return;
    }
#if RENDER_HAS_NATIVE_FENCE_FD
    const bool signaled = PollNativeFenceFd(waitData.nativeFenceFd);
#else
    const bool signaled = false;
#endif
    waitData.status = signaled ? IRenderFrameUtil::WaitStatus::CPU_WAITED : IRenderFrameUtil::WaitStatus::IMPORT_FAILED;
    CloseNativeFenceFd(waitData.nativeFenceFd);
    waitData.nativeFenceFd = -1;
}
RENDER_END_NAMESPACE()
