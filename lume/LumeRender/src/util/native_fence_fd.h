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

#ifndef RENDER_UTIL_NATIVE_FENCE_FD_H
#define RENDER_UTIL_NATIVE_FENCE_FD_H

#include <cstdint>

#include <render/namespace.h>
#include <render/util/intf_render_frame_util.h>

// Native fence fds only exist on POSIX-like platforms. Elsewhere the functions below still compile.
#if defined(__OHOS__) || defined(__ANDROID__) || defined(__linux__) || defined(__unix__)
#define RENDER_HAS_NATIVE_FENCE_FD 1
#else
#define RENDER_HAS_NATIVE_FENCE_FD 0
#endif

RENDER_BEGIN_NAMESPACE()
constexpr uint32_t NATIVE_FENCE_CPU_WAIT_TIMEOUT_MS{3000U};

// Exchanges the fdsan ownership of a native fence fd: acquiring takes over a fd handed over by a client (tag 0 ->
// renderer), releasing hands it to e.g. a driver importing the descriptor (renderer -> 0). No-op on platforms without
// fdsan.
void ExchangeNativeFenceFdOwnership(int32_t fd, bool acquireOwnership);

// negative fds are ignored
void CloseNativeFenceFd(int32_t fd);

// For a fence the GPU could not take. Must run before the frame submits any work. Closes the fd, sets it to -1
// and the status to CPU_WAITED, or to IMPORT_FAILED on failure or timeout.
void CpuWaitNativeFence(IRenderFrameUtil::WaitData& waitData);
RENDER_END_NAMESPACE()

#endif  // RENDER_UTIL_NATIVE_FENCE_FD_H
