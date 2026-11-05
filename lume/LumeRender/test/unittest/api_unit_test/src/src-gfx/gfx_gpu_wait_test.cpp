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

#include <chrono>

#include <base/containers/fixed_string.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/intf_render_context.h>
#include <render/intf_renderer.h>
#include <render/nodecontext/intf_render_node_graph_manager.h>
#include <render/util/intf_render_frame_util.h>
#include <render/util/intf_render_util.h>
#if RENDER_HAS_VULKAN_BACKEND
#include <render/vulkan/intf_device_vk.h>
#endif
#if RENDER_HAS_GLES_BACKEND
#include <render/gles/intf_device_gles.h>
#endif

#include "test_framework.h"
#if defined(UNIT_TESTS_USE_HCPPTEST)
#include "test_runner_ohos_system.h"
#else
#include "test_runner.h"
#endif

// Matches the platform guard in util/native_fence_fd.h.
#if defined(__OHOS__) || defined(__ANDROID__) || defined(__linux__) || defined(__unix__)
#define TEST_HAS_NATIVE_FENCE_FD 1
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#else
#define TEST_HAS_NATIVE_FENCE_FD 0
#endif

using namespace BASE_NS;
using namespace RENDER_NS;

namespace {
constexpr Math::UVec2 TEST_DATA_SIZE{2u, 2u};
constexpr uint32_t TEST_IMAGE_COLORS[] = {0xFF0000ff, 0xFF00ff00, 0xFFff0000, 0xFF00ffff};
constexpr string_view INPUT_IMAGE_NAME_0{"InputImage0"};
constexpr string_view OUTPUT_IMAGE_NAME_0{"OutputImage0"};

IRenderFrameUtil& GetFrameUtil(UTest::EngineResources& er)
{
    return er.context->GetRenderUtil().GetRenderFrameUtil();
}

// An id without a reference counter, as clients allocate handles for their own buffers.
RenderHandleReference CounterlessHandle()
{
    constexpr uint64_t id{0x7E57U};
    return RenderHandleReference{{id}, {}};
}

#if RENDER_HAS_VULKAN_BACKEND
// Queried from the driver, independent of the renderer. The renderer enables the extension only on mobile platforms.
bool HasSemaphoreSyncFdFeature(
    [[maybe_unused]] const DevicePlatformDataVk& plat, [[maybe_unused]] const VkExternalSemaphoreFeatureFlags feature)
{
#if defined(__OHOS__) || defined(__ANDROID__)
    bool hasExtension = false;
    for (const auto& extension : plat.physicalDeviceExtensions) {
        hasExtension =
            hasExtension || (string_view(extension.extensionName) == VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    }
    auto vkGetPhysicalDeviceExternalSemaphoreProperties =
        (PFN_vkGetPhysicalDeviceExternalSemaphoreProperties)vkGetInstanceProcAddr(
            plat.instance, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    if ((!hasExtension) || (!vkGetPhysicalDeviceExternalSemaphoreProperties)) {
        return false;
    }
    constexpr VkPhysicalDeviceExternalSemaphoreInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
        nullptr,
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalSemaphoreProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES, nullptr, 0, 0, 0};
    vkGetPhysicalDeviceExternalSemaphoreProperties(plat.physicalDevice, &info, &properties);
    return (properties.externalSemaphoreFeatures & feature) != 0;
#else
    return false;
#endif
}
#endif

// Read from the platform, independent of the renderer, so a regression that disables a capability fails the
// tests instead of turning them into passing no-ops. Export and import are separate driver capabilities.
bool CanExportNativeFence(UTest::EngineResources& er)
{
    const IDevice& device = er.context->GetDevice();
#if RENDER_HAS_VULKAN_BACKEND
    if (device.GetBackendType() == DeviceBackendType::VULKAN) {
        return HasSemaphoreSyncFdFeature(static_cast<const DevicePlatformDataVk&>(device.GetPlatformData()),
            VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT);
    }
#endif
#if RENDER_HAS_GLES_BACKEND
    if (device.GetBackendType() == DeviceBackendType::OPENGLES) {
        return static_cast<const DevicePlatformDataGLES&>(device.GetPlatformData()).hasNativeFenceSyncExt;
    }
#endif
    return false;
}

// When false the wait is skipped by design.
bool CanImportNativeFence(UTest::EngineResources& er)
{
    const IDevice& device = er.context->GetDevice();
#if RENDER_HAS_VULKAN_BACKEND
    if (device.GetBackendType() == DeviceBackendType::VULKAN) {
        return HasSemaphoreSyncFdFeature(static_cast<const DevicePlatformDataVk&>(device.GetPlatformData()),
            VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT);
    }
#endif
#if RENDER_HAS_GLES_BACKEND
    if (device.GetBackendType() == DeviceBackendType::OPENGLES) {
        return static_cast<const DevicePlatformDataGLES&>(device.GetPlatformData()).hasNativeFenceSyncExt;
    }
#endif
    return false;  // desktop GL has no native fence sync
}

// Minimal drawing work. RenderFrame({}) never reaches the backend, so it cannot exercise the native fence paths.
struct FrameWork {
    RenderHandleReference inputImage;
    RenderHandleReference outputImage;
    RenderHandleReference renderNodeGraph;
};

FrameWork CreateFrameWork(UTest::EngineResources& er)
{
    FrameWork work;
    auto& gpuResourceMgr = er.device->GetGpuResourceManager();

    GpuImageDesc imageDesc;
    imageDesc.width = TEST_DATA_SIZE.x;
    imageDesc.height = TEST_DATA_SIZE.y;
    imageDesc.depth = 1;
    imageDesc.engineCreationFlags = CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS;
    imageDesc.format = BASE_FORMAT_R8G8B8A8_UNORM;
    imageDesc.imageTiling = CORE_IMAGE_TILING_OPTIMAL;
    imageDesc.imageType = CORE_IMAGE_TYPE_2D;
    imageDesc.imageViewType = CORE_IMAGE_VIEW_TYPE_2D;
    imageDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    imageDesc.usageFlags = CORE_IMAGE_USAGE_TRANSFER_SRC_BIT | CORE_IMAGE_USAGE_TRANSFER_DST_BIT |
                           CORE_IMAGE_USAGE_SAMPLED_BIT | CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    const array_view<const uint8_t> dataView = {(const uint8_t*)TEST_IMAGE_COLORS, sizeof(TEST_IMAGE_COLORS)};
    work.inputImage = gpuResourceMgr.Create(INPUT_IMAGE_NAME_0, imageDesc, dataView);

    imageDesc.usageFlags =
        CORE_IMAGE_USAGE_TRANSFER_SRC_BIT | CORE_IMAGE_USAGE_SAMPLED_BIT | CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    work.outputImage = gpuResourceMgr.Create(OUTPUT_IMAGE_NAME_0, imageDesc);

    work.renderNodeGraph = er.context->GetRenderNodeGraphManager().LoadAndCreate(
        IRenderNodeGraphManager::RenderNodeGraphUsageType::RENDER_NODE_GRAPH_STATIC,
        "test://renderNodeGraphGfxGpuSignalTest.rng");
    return work;
}

// Renders one frame with a GPU signal for work.outputImage and exports it.
IRenderFrameUtil::NativeFence RenderFrameAndExportFence(UTest::EngineResources& er, FrameWork& work)
{
    IRenderFrameUtil& rfUtil = GetFrameUtil(er);
    rfUtil.AddGpuSignal({work.outputImage});

    er.engine->TickFrame();
    er.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
    return rfUtil.ExportFrameGpuSignal(work.outputImage);
}

#if (TEST_HAS_NATIVE_FENCE_FD == 1)
// Target of /proc/self/fd/N, empty once closed. Unlike an EPIPE write probe it works on a real sync fd, and unlike
// an "is it open" check it survives fd number reuse.
string FdLinkTarget(int32_t fd)
{
    if (fd < 0) {
        return {};
    }
    fixed_string<64u> path = "/proc/self/fd/";
    path += to_string(fd);
    char target[256];
    const ssize_t len = readlink(path.data(), target, sizeof(target) - 1u);
    if (len <= 0) {
        return {};
    }
    return string(target, static_cast<size_t>(len));
}

// Every sync file links to "anon_inode:sync_file", so FdLinkTarget cannot tell it from a new sync file that reuses the
// fd number. O_NONBLOCK marks the open file itself, which a new sync file does not share. Sync files ignore the flag.
bool MarkSyncFile(int32_t fd)
{
    const int flags = fcntl(fd, F_GETFL);
    return (flags >= 0) && (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0);
}

bool IsMarkedSyncFile(int32_t fd, const string& target)
{
    const int flags = fcntl(fd, F_GETFL);
    return (flags >= 0) && ((flags & O_NONBLOCK) != 0) && (FdLinkTarget(fd) == target);
}

// A pipe is not a sync fd, so the renderer polls it on the CPU. With data it polls readable like a signaled fence,
// with the write end closed it only hangs up. Either way the wait ends without the timeout.
class FencePipe {
public:
    FencePipe()
    {
        // otherwise a write after the read end is gone kills the runner
        previousSigPipe_ = signal(SIGPIPE, SIG_IGN);
        int fds[2] = {-1, -1};
        if (pipe(fds) == 0) {
            readFd_ = fds[0];
            writeFd_ = fds[1];
            readFdNumber_ = readFd_;
            readTarget_ = FdLinkTarget(readFd_);
        }
    }

    ~FencePipe()
    {
        if (readFd_ >= 0) {
            close(readFd_);
        }
        if (writeFd_ >= 0) {
            close(writeFd_);
        }
        if ((previousSigPipe_ != SIG_ERR) && (signal(SIGPIPE, previousSigPipe_) == SIG_ERR)) {
            CORE_LOG_E("failed to restore the SIGPIPE handler");
        }
    }

    FencePipe(const FencePipe&) = delete;
    FencePipe& operator=(const FencePipe&) = delete;

    bool IsValid() const
    {
        return (readFd_ >= 0) && (writeFd_ >= 0);
    }

    // Keeps ownership, for calls that may decline the descriptor.
    int32_t PeekReadFd() const
    {
        return readFd_;
    }

    // Ownership goes with the descriptor.
    int32_t ReleaseReadFd()
    {
        const int32_t fd = readFd_;
        readFd_ = -1;
        return fd;
    }

    bool Signal() const
    {
        const uint8_t byte = 0u;
        return write(writeFd_, &byte, 1u) == 1;
    }

    void CloseWriteEnd()
    {
        if (writeFd_ >= 0) {
            close(writeFd_);
            writeFd_ = -1;
        }
    }

    // True once its owner closed the read end. Does not write, so the outcome of a wait stays unchanged.
    bool ReadEndClosed() const
    {
        return FdLinkTarget(readFdNumber_) != readTarget_;
    }

private:
    int32_t readFd_{-1};
    int32_t writeFd_{-1};
    int32_t readFdNumber_{-1};
    string readTarget_;
    void (*previousSigPipe_)(int){SIG_DFL};
};

// A driver may accept a pipe and report WAITED. Otherwise the CPU wait succeeds only for a signaled pipe.
bool IsPipeOutcome(const IRenderFrameUtil::WaitStatus status, const bool signaled)
{
    const auto cpuOutcome =
        signaled ? IRenderFrameUtil::WaitStatus::CPU_WAITED : IRenderFrameUtil::WaitStatus::IMPORT_FAILED;
    return (status == IRenderFrameUtil::WaitStatus::WAITED) || (status == cpuOutcome);
}

// Closes a bare descriptor on any early return, including a failing assertion.
class ScopedFd {
public:
    explicit ScopedFd(int32_t fd) : fd_(fd)
    {}

    ~ScopedFd()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int32_t Get() const
    {
        return fd_;
    }

    // Gives up ownership once the renderer has taken the descriptor.
    void Release()
    {
        fd_ = -1;
    }

private:
    int32_t fd_{-1};
};

#endif  // TEST_HAS_NATIVE_FENCE_FD

#if (TEST_HAS_NATIVE_FENCE_FD == 1)
// below the renderer's CPU wait timeout (NATIVE_FENCE_CPU_WAIT_TIMEOUT_MS), so a wait that ran into it fails the test
constexpr int64_t HANG_UP_WAIT_LIMIT_MS{2000};

// A descriptor that is not a native fence must still be closed exactly once.
void TestGpuWaitNonFenceFdReclaimed(DeviceBackendType backend, bool renderFrames)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);

    const bool canImport = CanImportNativeFence(engine);
    FencePipe fence;
    ASSERT_EQ(fence.IsValid(), true);
    fence.CloseWriteEnd();
    const auto result = GetFrameUtil(engine).AddGpuWait({fence.PeekReadFd()});
    ASSERT_EQ(result == IRenderFrameUtil::WaitResult::ACCEPTED, canImport);
    if (result != IRenderFrameUtil::WaitResult::ACCEPTED) {
        work = {};
        DestroyEngine(engine);
        return;
    }
    (void)fence.ReleaseReadFd();

    if (renderFrames) {
        // a driver that accepts the pipe closes it early
        engine.engine->TickFrame();
        engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
        const auto waits = GetFrameUtil(engine).GetFrameGpuWaitData();
        ASSERT_EQ(waits.size(), 1U);
        ASSERT_EQ(IsPipeOutcome(waits[0].status, false), true);
        if (waits[0].status == IRenderFrameUtil::WaitStatus::IMPORT_FAILED) {
            // declined, and closed in this frame after the CPU wait failed
            ASSERT_EQ(fence.ReadEndClosed(), true);
        }

        engine.engine->TickFrame();
        engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
        ASSERT_EQ(fence.ReadEndClosed(), true);
    } else {
        ASSERT_EQ(fence.ReadEndClosed(), false);
    }

    work = {};
    DestroyEngine(engine);
    ASSERT_EQ(fence.ReadEndClosed(), true);
}

// only EXPORTED hands out a descriptor, ALREADY_SIGNALED legally has none
void ExpectExportResult(const IRenderFrameUtil::NativeFence& exported, bool canExport)
{
    using ExportResult = IRenderFrameUtil::ExportResult;
    if (canExport) {
        ASSERT_EQ(
            (exported.result == ExportResult::EXPORTED) || (exported.result == ExportResult::ALREADY_SIGNALED), true);
    } else {
        ASSERT_EQ(static_cast<int>(exported.result), static_cast<int>(ExportResult::NOT_SUPPORTED));
    }
    ASSERT_EQ(exported.nativeFenceFd >= 0, exported.result == ExportResult::EXPORTED);
}

// the outcome must agree with what happened to the descriptor
void ExpectRoundTripWait(UTest::EngineResources& engine, bool canImport)
{
    const auto waits = GetFrameUtil(engine).GetFrameGpuWaitData();
    if (canImport) {
        ASSERT_EQ(waits.size(), 1u);
        ASSERT_EQ(static_cast<int>(waits[0].status), static_cast<int>(IRenderFrameUtil::WaitStatus::WAITED));
    } else {
        ASSERT_EQ(waits.empty(), true);
    }
}

// Exports a frame's GPU signal and waits on it the next frame. Only this feeds the import path a real fence.
void TestGpuWaitFenceRoundTrip(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);

    const bool canExport = CanExportNativeFence(engine);
    const bool canImport = CanImportNativeFence(engine);
    const IRenderFrameUtil::NativeFence exported0 = RenderFrameAndExportFence(engine, work);
    const int32_t fenceFd = exported0.nativeFenceFd;
    ScopedFd fenceFdOwner(fenceFd);
    CORE_LOG_I("native fence round trip: export=%s import=%s result=%d fd=%d",
        canExport ? "yes" : "no",
        canImport ? "yes" : "no",
        static_cast<int>(exported0.result),
        fenceFd);

    ExpectExportResult(exported0, canExport);
    if (exported0.result != IRenderFrameUtil::ExportResult::EXPORTED) {
        work = {};
        DestroyEngine(engine);
        return;
    }
    const string exported = FdLinkTarget(fenceFd);
    ASSERT_EQ(exported.empty(), false);
    ASSERT_EQ(MarkSyncFile(fenceFd), true);

    const auto waitResult = GetFrameUtil(engine).AddGpuWait({fenceFd});
    if (waitResult == IRenderFrameUtil::WaitResult::ACCEPTED) {
        fenceFdOwner.Release();  // the renderer owns it now
    }
    ASSERT_EQ(waitResult == IRenderFrameUtil::WaitResult::ACCEPTED, canImport);
    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});

    ExpectRoundTripWait(engine, canImport);

    // a declined descriptor stays with the caller. An imported one belongs to the driver, which closes it at its own
    // time, but no later than device destruction.
    if (!canImport) {
        ASSERT_EQ(IsMarkedSyncFile(fenceFd, exported), true);
    }

    work = {};
    DestroyEngine(engine);
    ASSERT_EQ(IsMarkedSyncFile(fenceFd, exported), !canImport);
}

// Hands the descriptor over only when the renderer took it.
bool AddOwnedGpuWait(IRenderFrameUtil& rfUtil, ScopedFd& fd, const RenderHandleReference& owner)
{
    const auto result = rfUtil.AddGpuWait({fd.Get(), IRenderFrameUtil::WaitStatus::PENDING, owner});
    if (result != IRenderFrameUtil::WaitResult::ACCEPTED) {
        return false;
    }
    fd.Release();
    return true;
}

void ExpectDefaultWaitData(const IRenderFrameUtil::WaitData& data)
{
    ASSERT_EQ(data.nativeFenceFd, -1);
    ASSERT_EQ(static_cast<int>(data.status), static_cast<int>(IRenderFrameUtil::WaitStatus::PENDING));
    ASSERT_EQ(static_cast<bool>(data.handle), false);
}

// The owner's own entry, without a descriptor the renderer may still close.
void ExpectWaitFound(const IRenderFrameUtil::WaitData& found, const IRenderFrameUtil::WaitData& queued)
{
    ASSERT_EQ(found.handle.GetHandle() == queued.handle.GetHandle(), true);
    ASSERT_EQ(static_cast<int>(found.status), static_cast<int>(queued.status));
    ASSERT_EQ(found.nativeFenceFd, -1);
    // the backend ran, so it must have decided the outcome
    ASSERT_NE(static_cast<int>(found.status), static_cast<int>(IRenderFrameUtil::WaitStatus::PENDING));
}

// Waits queued for work.inputImage, work.outputImage and no handle. The last must not match an empty handle.
void ExpectWaitsFoundByHandle(IRenderFrameUtil& rfUtil, const FrameWork& work, bool canImport, bool haveFence)
{
    const auto waits = rfUtil.GetFrameGpuWaitData();
    CORE_LOG_I("wait lookup by handle: import=%s fence=%s waits=%u",
        canImport ? "yes" : "no",
        haveFence ? "yes" : "no",
        static_cast<uint32_t>(waits.size()));
    if (canImport) {
        // the list keeps insertion order, and the lookups must agree with it
        ASSERT_EQ(waits.size(), 3u);
        ASSERT_EQ(waits[0].handle.GetHandle() == work.inputImage.GetHandle(), true);
        ASSERT_EQ(waits[1].handle.GetHandle() == work.outputImage.GetHandle(), true);
        ASSERT_EQ(static_cast<bool>(waits[waits.size() - 1U].handle), false);
        ExpectWaitFound(rfUtil.GetFrameGpuWaitData(work.inputImage), waits[0]);
        ExpectWaitFound(rfUtil.GetFrameGpuWaitData(work.outputImage), waits[1]);
        ASSERT_EQ(IsPipeOutcome(waits[0].status, true), true);
        if (haveFence) {
            ASSERT_EQ(static_cast<int>(waits[1].status), static_cast<int>(IRenderFrameUtil::WaitStatus::WAITED));
        } else {
            ASSERT_EQ(IsPipeOutcome(waits[1].status, false), true);
        }
    } else {
        // nothing was queued, so no owner may be handed an outcome
        ASSERT_EQ(waits.empty(), true);
        ExpectDefaultWaitData(rfUtil.GetFrameGpuWaitData(work.inputImage));
        ExpectDefaultWaitData(rfUtil.GetFrameGpuWaitData(work.outputImage));
    }
    ExpectDefaultWaitData(rfUtil.GetFrameGpuWaitData(RenderHandleReference{}));
    // a valid handle that queued nothing
    ExpectDefaultWaitData(rfUtil.GetFrameGpuWaitData(work.renderNodeGraph));
}

// Several owners queue a wait each for the same frame, and each must find its own outcome by handle.
void TestGpuWaitLookupByHandle(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);
    IRenderFrameUtil& rfUtil = GetFrameUtil(engine);
    const bool canImport = CanImportNativeFence(engine);
    FencePipe pipe0;
    FencePipe pipe1;
    FencePipe pipe2;
    ASSERT_EQ(pipe0.IsValid() && pipe1.IsValid() && pipe2.IsValid(), true);
    // signaled and hung up, so the outcomes differ also under the CPU wait
    ASSERT_EQ(pipe0.Signal(), true);
    pipe1.CloseWriteEnd();
    pipe2.CloseWriteEnd();

    const IRenderFrameUtil::NativeFence exported = RenderFrameAndExportFence(engine, work);
    const bool haveFence = (exported.result == IRenderFrameUtil::ExportResult::EXPORTED);
    ScopedFd fd0(pipe0.ReleaseReadFd());
    ScopedFd fd1(haveFence ? exported.nativeFenceFd : pipe1.ReleaseReadFd());
    ScopedFd fd2(pipe2.ReleaseReadFd());
    // resource handles stand in for the owners
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, fd0, work.inputImage), canImport);
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, fd1, work.outputImage), canImport);
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, fd2, RenderHandleReference{}), canImport);

    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
    ExpectWaitsFoundByHandle(rfUtil, work, canImport, haveFence);

    work = {};
    DestroyEngine(engine);
}

// A frame with no GPU work.
void RenderIdleFrame(UTest::EngineResources& er)
{
    er.engine->TickFrame();
    er.context->GetRenderer().RenderFrame({});
}

int WaitStatusOf(IRenderFrameUtil& rfUtil, const RenderHandleReference& handle)
{
    return static_cast<int>(rfUtil.GetFrameGpuWaitData(handle).status);
}

// Waits queued for work.inputImage (a pipe) and, with a fence, work.outputImage must be held over an idle frame.
void ExpectWaitsHeldOverIdleFrame(IRenderFrameUtil& rfUtil, const FrameWork& work, bool canImport, bool haveFence)
{
    const auto waits = rfUtil.GetFrameGpuWaitData();
    if (!canImport) {
        // nothing was taken, so there is nothing to hold
        ASSERT_EQ(waits.empty(), true);
        ExpectDefaultWaitData(rfUtil.GetFrameGpuWaitData(work.inputImage));
        return;
    }
    ASSERT_EQ(waits.size(), haveFence ? 2U : 1U);
    constexpr auto notSubmitted = static_cast<int>(IRenderFrameUtil::WaitStatus::NOT_SUBMITTED);
    ASSERT_EQ(WaitStatusOf(rfUtil, work.inputImage), notSubmitted);
    if (haveFence) {
        ASSERT_EQ(WaitStatusOf(rfUtil, work.outputImage), notSubmitted);
    }
}

// The frame with work must decide every held wait, ahead of the one queued for it (work.renderNodeGraph).
void ExpectHeldWaitsDecided(IRenderFrameUtil& rfUtil, const FrameWork& work, bool canImport, bool haveFence)
{
    const auto waits = rfUtil.GetFrameGpuWaitData();
    if (!canImport) {
        ASSERT_EQ(waits.empty(), true);
        return;
    }
    ASSERT_EQ(waits.size(), haveFence ? 3U : 2U);
    ASSERT_EQ(waits[0].handle.GetHandle() == work.inputImage.GetHandle(), true);
    ASSERT_EQ(waits[waits.size() - 1U].handle.GetHandle() == work.renderNodeGraph.GetHandle(), true);
    // the held pipe was signaled and the late one hung up
    ASSERT_EQ(IsPipeOutcome(rfUtil.GetFrameGpuWaitData(work.inputImage).status, true), true);
    ASSERT_EQ(IsPipeOutcome(rfUtil.GetFrameGpuWaitData(work.renderNodeGraph).status, false), true);
    if (haveFence) {
        ASSERT_EQ(waits[1].handle.GetHandle() == work.outputImage.GetHandle(), true);
        ASSERT_EQ(WaitStatusOf(rfUtil, work.outputImage), static_cast<int>(IRenderFrameUtil::WaitStatus::WAITED));
    }
}

// A wait queued before frames with no GPU work is held and applied to the next frame with work.
void TestGpuWaitHeldOverIdleFrames(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);
    IRenderFrameUtil& rfUtil = GetFrameUtil(engine);
    const bool canImport = CanImportNativeFence(engine);
    FencePipe pipe;
    FencePipe latePipe;
    ASSERT_EQ(pipe.IsValid() && latePipe.IsValid(), true);
    ASSERT_EQ(pipe.Signal(), true);
    latePipe.CloseWriteEnd();

    // a real fence, where exportable, covers a successful wait
    const IRenderFrameUtil::NativeFence exported = RenderFrameAndExportFence(engine, work);
    const bool haveFence = (exported.result == IRenderFrameUtil::ExportResult::EXPORTED);
    ScopedFd fenceFd(exported.nativeFenceFd);
    ScopedFd pipeFd(pipe.ReleaseReadFd());
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, pipeFd, work.inputImage), canImport);
    if (haveFence) {
        ASSERT_EQ(AddOwnedGpuWait(rfUtil, fenceFd, work.outputImage), canImport);
    }
    CORE_LOG_I("wait held over idle frames: import=%s fence=%s", canImport ? "yes" : "no", haveFence ? "yes" : "no");

    // the waits must survive every idle frame
    for (uint32_t idx = 0U; idx < 2U; ++idx) {
        RenderIdleFrame(engine);
        ExpectWaitsHeldOverIdleFrame(rfUtil, work, canImport, haveFence);
        ASSERT_EQ(pipe.ReadEndClosed(), false);
    }

    ScopedFd lateFd(latePipe.ReleaseReadFd());
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, lateFd, work.renderNodeGraph), canImport);
    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
    ExpectHeldWaitsDecided(rfUtil, work, canImport, haveFence);
    if (canImport &&
        (WaitStatusOf(rfUtil, work.inputImage) != static_cast<int>(IRenderFrameUtil::WaitStatus::WAITED))) {
        // the GPU could not take it, so it was waited on and closed in this frame
        ASSERT_EQ(pipe.ReadEndClosed(), true);
    }

    // decided waits are not held again, and every descriptor the renderer took is closed by now
    RenderIdleFrame(engine);
    ASSERT_EQ(rfUtil.GetFrameGpuWaitData().empty(), true);
    ASSERT_EQ(pipe.ReadEndClosed(), canImport);
    ASSERT_EQ(latePipe.ReadEndClosed(), canImport);

    work = {};
    DestroyEngine(engine);
}

// Checks waits on a signaled pipe for work.inputImage and a hung-up one for work.outputImage.
void ExpectCpuWaitOutcomes(
    IRenderFrameUtil& rfUtil, const FrameWork& work, bool canImport, const FencePipe& signaled, const FencePipe& hungUp)
{
    const auto signaledStatus = rfUtil.GetFrameGpuWaitData(work.inputImage).status;
    const auto hungUpStatus = rfUtil.GetFrameGpuWaitData(work.outputImage).status;
    CORE_LOG_I("cpu wait fallback: import=%s signaled=%d hung up=%d",
        canImport ? "yes" : "no",
        static_cast<int>(signaledStatus),
        static_cast<int>(hungUpStatus));
    if (!canImport) {
        // the caller kept both, and the renderer must not have touched them
        ASSERT_EQ(rfUtil.GetFrameGpuWaitData().empty(), true);
        ASSERT_EQ(signaled.ReadEndClosed(), false);
        ASSERT_EQ(hungUp.ReadEndClosed(), false);
        return;
    }
    ASSERT_EQ(IsPipeOutcome(signaledStatus, true), true);
    ASSERT_EQ(IsPipeOutcome(hungUpStatus, false), true);
    // the renderer closes what it waited on in the same frame, not at the next frame boundary
    if (signaledStatus != IRenderFrameUtil::WaitStatus::WAITED) {
        ASSERT_EQ(signaled.ReadEndClosed(), true);
    }
    if (hungUpStatus != IRenderFrameUtil::WaitStatus::WAITED) {
        ASSERT_EQ(hungUp.ReadEndClosed(), true);
    }
}

// The render thread waits for a descriptor the GPU cannot take. A signaled one synchronizes the frame, a hung-up
// one fails at once and is reported.
void TestGpuWaitCpuFallback(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);
    IRenderFrameUtil& rfUtil = GetFrameUtil(engine);
    const bool canImport = CanImportNativeFence(engine);
    FencePipe signaled;
    FencePipe hungUp;
    ASSERT_EQ(signaled.IsValid() && hungUp.IsValid(), true);
    ASSERT_EQ(signaled.Signal(), true);
    hungUp.CloseWriteEnd();
    ScopedFd signaledFd(signaled.ReleaseReadFd());
    ScopedFd hungUpFd(hungUp.ReleaseReadFd());
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, signaledFd, work.inputImage), canImport);
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, hungUpFd, work.outputImage), canImport);

    const auto start = std::chrono::steady_clock::now();
    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ExpectCpuWaitOutcomes(rfUtil, work, canImport, signaled, hungUp);
    // the hang-up must fail the wait at once, not after the timeout
    ASSERT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), HANG_UP_WAIT_LIMIT_MS);

    // decided waits are not carried to the next frame
    RenderIdleFrame(engine);
    ASSERT_EQ(rfUtil.GetFrameGpuWaitData().empty(), true);
    ASSERT_EQ(signaled.ReadEndClosed(), canImport);
    ASSERT_EQ(hungUp.ReadEndClosed(), canImport);

    work = {};
    DestroyEngine(engine);
}

// The wait lookup compares only the id, so a handle without a reference counter must find its wait.
void TestGpuWaitCounterlessHandle(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);
    IRenderFrameUtil& rfUtil = GetFrameUtil(engine);
    const bool canImport = CanImportNativeFence(engine);
    const RenderHandleReference owner = CounterlessHandle();
    ASSERT_EQ(static_cast<bool>(owner), false);
    FencePipe pipe;
    ASSERT_EQ(pipe.IsValid() && pipe.Signal(), true);
    ScopedFd fd(pipe.ReleaseReadFd());
    ASSERT_EQ(AddOwnedGpuWait(rfUtil, fd, owner), canImport);

    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
    const IRenderFrameUtil::WaitData found = rfUtil.GetFrameGpuWaitData(owner);
    CORE_LOG_I(
        "counterless wait handle: import=%s status=%d", canImport ? "yes" : "no", static_cast<int>(found.status));
    if (canImport) {
        ASSERT_EQ(found.handle.GetHandle() == owner.GetHandle(), true);
        ASSERT_EQ(IsPipeOutcome(found.status, true), true);
    } else {
        ExpectDefaultWaitData(found);
    }

    work = {};
    DestroyEngine(engine);
}
#endif  // TEST_HAS_NATIVE_FENCE_FD

void TestGpuWaitInvalidFd(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);

    // a negative descriptor must be rejected without reaching the backend
    ASSERT_EQ(static_cast<int>(GetFrameUtil(engine).AddGpuWait({})),
        static_cast<int>(IRenderFrameUtil::WaitResult::INVALID_FD));
    ASSERT_EQ(static_cast<int>(GetFrameUtil(engine).AddGpuWait({-1})),
        static_cast<int>(IRenderFrameUtil::WaitResult::INVALID_FD));

    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});

    work = {};
    DestroyEngine(engine);
}

// The caller owns an export's descriptor. Only EXPORTED has one.
void CloseExportedFence(const IRenderFrameUtil::NativeFence& fence)
{
#if (TEST_HAS_NATIVE_FENCE_FD == 1)
    if (fence.nativeFenceFd >= 0) {
        close(fence.nativeFenceFd);
    }
#endif
}

// Exporting consumes the signal, so a second export must fail.
void TestGpuSignalExportOnce(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);

    const bool canExport = CanExportNativeFence(engine);
    const IRenderFrameUtil::NativeFence first = RenderFrameAndExportFence(engine, work);
    const IRenderFrameUtil::NativeFence second = GetFrameUtil(engine).ExportFrameGpuSignal(work.outputImage);
    CloseExportedFence(first);
    CloseExportedFence(second);
    CORE_LOG_I("export once: export=%s first=%d second=%d",
        canExport ? "yes" : "no",
        static_cast<int>(first.result),
        static_cast<int>(second.result));

    using ExportResult = IRenderFrameUtil::ExportResult;
    ASSERT_EQ(second.nativeFenceFd, -1);
    if (canExport) {
        // the frame had work, so the first call attempted the export whatever its outcome
        ASSERT_NE(static_cast<int>(first.result), static_cast<int>(ExportResult::NOT_SIGNALED));
        ASSERT_EQ(static_cast<int>(second.result), static_cast<int>(ExportResult::FAILED));
    } else {
        // nothing was attempted, so nothing was consumed
        ASSERT_EQ(static_cast<int>(first.result), static_cast<int>(ExportResult::NOT_SUPPORTED));
        ASSERT_EQ(static_cast<int>(second.result), static_cast<int>(ExportResult::NOT_SUPPORTED));
    }

    work = {};
    DestroyEngine(engine);
}

// A frame with no GPU work never signals, so there is nothing to export.
void TestGpuSignalExportIdleFrame(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);
    IRenderFrameUtil& rfUtil = GetFrameUtil(engine);
    const bool canExport = CanExportNativeFence(engine);

    // the first frame uploads the images, so only the second one is idle
    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({&work.renderNodeGraph, 1u});
    rfUtil.AddGpuSignal({work.outputImage});
    engine.engine->TickFrame();
    engine.context->GetRenderer().RenderFrame({});
    ASSERT_EQ(rfUtil.GetFrameGpuSignalData().size(), 1U);
    ASSERT_EQ(rfUtil.GetFrameGpuSignalData(work.outputImage).signaled, false);

    const IRenderFrameUtil::NativeFence fence = rfUtil.ExportFrameGpuSignal(work.outputImage);
    CloseExportedFence(fence);
    CORE_LOG_I(
        "export after idle frame: export=%s result=%d", canExport ? "yes" : "no", static_cast<int>(fence.result));
    // the capability is checked before the signal
    using ExportResult = IRenderFrameUtil::ExportResult;
    const ExportResult expected = canExport ? ExportResult::NOT_SIGNALED : ExportResult::NOT_SUPPORTED;
    ASSERT_EQ(static_cast<int>(fence.result), static_cast<int>(expected));
    ASSERT_EQ(fence.nativeFenceFd, -1);

    work = {};
    DestroyEngine(engine);
}

// Only the owner's handle finds a signal. An empty handle must not match a signal added without one.
void TestGpuSignalExportUnknownHandle(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);
    IRenderFrameUtil& rfUtil = GetFrameUtil(engine);

    rfUtil.AddGpuSignal({});
    const IRenderFrameUtil::NativeFence owner = RenderFrameAndExportFence(engine, work);
    CloseExportedFence(owner);
    ASSERT_EQ(rfUtil.GetFrameGpuSignalData().size(), 2U);
    const IRenderFrameUtil::NativeFence empty = rfUtil.ExportFrameGpuSignal(RenderHandleReference{});
    // a valid handle that added no signal
    const IRenderFrameUtil::NativeFence unknown = rfUtil.ExportFrameGpuSignal(work.inputImage);
    CloseExportedFence(empty);
    CloseExportedFence(unknown);

    using ExportResult = IRenderFrameUtil::ExportResult;
    // the owner's signal was found, a lookup miss would report NOT_SIGNALED
    ASSERT_NE(static_cast<int>(owner.result), static_cast<int>(ExportResult::NOT_SIGNALED));
    ASSERT_EQ(static_cast<int>(empty.result), static_cast<int>(ExportResult::NOT_SIGNALED));
    ASSERT_EQ(empty.nativeFenceFd, -1);
    ASSERT_EQ(static_cast<int>(unknown.result), static_cast<int>(ExportResult::NOT_SIGNALED));
    ASSERT_EQ(unknown.nativeFenceFd, -1);

    work = {};
    DestroyEngine(engine);
}

// The signal lookups compare only the id, so a handle without a reference counter must find its signal.
void TestGpuSignalCounterlessHandle(DeviceBackendType backend)
{
    UTest::EngineResources engine;
    engine.backend = backend;
    if (backend != DeviceBackendType::VULKAN) {
        engine.createWindow = true;
    }
    CreateEngineSetup(engine);
    FrameWork work = CreateFrameWork(engine);
    IRenderFrameUtil& rfUtil = GetFrameUtil(engine);
    const RenderHandleReference owner = CounterlessHandle();
    ASSERT_EQ(static_cast<bool>(owner), false);

    rfUtil.AddGpuSignal({owner});
    CloseExportedFence(RenderFrameAndExportFence(engine, work));
    const auto signals = rfUtil.GetFrameGpuSignalData();
    ASSERT_EQ(signals.size(), 2U);
    const IRenderFrameUtil::SignalData found = rfUtil.GetFrameGpuSignalData(owner);
    ASSERT_EQ(found.handle.GetHandle() == owner.GetHandle(), true);
    ASSERT_EQ(found.signaled, signals[0].signaled);

    // the frame had work, so a found signal is never reported as not signaled
    const IRenderFrameUtil::NativeFence fence = rfUtil.ExportFrameGpuSignal(owner);
    CloseExportedFence(fence);
    ASSERT_NE(static_cast<int>(fence.result), static_cast<int>(IRenderFrameUtil::ExportResult::NOT_SIGNALED));

    work = {};
    DestroyEngine(engine);
}
}  // namespace

#if RENDER_HAS_VULKAN_BACKEND
/**
 * @tc.name: GfxGpuWaitInvalidFdTestVulkan
 * @tc.desc: Tests that IRenderFrameUtil::AddGpuWait ignores an invalid native fence descriptor.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitInvalidFdTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitInvalidFd(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuSignalExportOnceTestVulkan
 * @tc.desc: Tests that IRenderFrameUtil::ExportFrameGpuSignal fails a second export of the same signal.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalExportOnceTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuSignalExportOnce(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuSignalExportIdleFrameTestVulkan
 * @tc.desc: Tests that the signal of a frame without GPU work is reported as not signaled.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalExportIdleFrameTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuSignalExportIdleFrame(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuSignalExportUnknownHandleTestVulkan
 * @tc.desc: Tests that an empty or unknown handle exports nothing.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalExportUnknownHandleTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuSignalExportUnknownHandle(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuSignalCounterlessHandleTestVulkan
 * @tc.desc: Tests that a signal is found and exported by a handle without a reference counter.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalCounterlessHandleTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuSignalCounterlessHandle(DeviceBackendType::VULKAN);
}

#if (TEST_HAS_NATIVE_FENCE_FD == 1)
/**
 * @tc.name: GfxGpuWaitNonFenceFdTestVulkan
 * @tc.desc: Tests that a descriptor the backend cannot import is still closed exactly once.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitNonFenceFdTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitNonFenceFdReclaimed(DeviceBackendType::VULKAN, true);
}

/**
 * @tc.name: GfxGpuWaitFdDestroyTestVulkan
 * @tc.desc: Tests that a native fence descriptor is closed on teardown when no frame ever consumed it.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitFdDestroyTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitNonFenceFdReclaimed(DeviceBackendType::VULKAN, false);
}

/**
 * @tc.name: GfxGpuWaitRoundTripTestVulkan
 * @tc.desc: Exports a frame's GPU signal as a native fence fd and waits on it the following frame.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitRoundTripTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitFenceRoundTrip(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuWaitLookupByHandleTestVulkan
 * @tc.desc: Tests that waits queued for the same frame are each found by their own handle.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitLookupByHandleTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitLookupByHandle(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuWaitHeldOverIdleFramesTestVulkan
 * @tc.desc: Tests that a wait survives frames with no GPU work and applies to the next frame with work.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitHeldOverIdleFramesTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitHeldOverIdleFrames(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuWaitCpuFallbackTestVulkan
 * @tc.desc: Tests that a descriptor the GPU cannot wait on is waited on the CPU and closed within the frame.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitCpuFallbackTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitCpuFallback(DeviceBackendType::VULKAN);
}

/**
 * @tc.name: GfxGpuWaitCounterlessHandleTestVulkan
 * @tc.desc: Tests that a wait is found by a handle without a reference counter.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitCounterlessHandleTestVulkan, testing::ext::TestSize.Level1)
{
    TestGpuWaitCounterlessHandle(DeviceBackendType::VULKAN);
}
#endif  // TEST_HAS_NATIVE_FENCE_FD
#endif  // RENDER_HAS_VULKAN_BACKEND

#if RENDER_HAS_GL_BACKEND || RENDER_HAS_GLES_BACKEND
/**
 * @tc.name: GfxGpuWaitInvalidFdTestOpenGL
 * @tc.desc: Tests that IRenderFrameUtil::AddGpuWait ignores an invalid native fence descriptor.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitInvalidFdTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuWaitInvalidFd(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuSignalExportOnceTestOpenGL
 * @tc.desc: Tests that IRenderFrameUtil::ExportFrameGpuSignal fails a second export of the same signal.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalExportOnceTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuSignalExportOnce(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuSignalExportIdleFrameTestOpenGL
 * @tc.desc: Tests that the signal of a frame without GPU work is reported as not signaled.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalExportIdleFrameTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuSignalExportIdleFrame(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuSignalExportUnknownHandleTestOpenGL
 * @tc.desc: Tests that an empty or unknown handle exports nothing.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalExportUnknownHandleTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuSignalExportUnknownHandle(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuSignalCounterlessHandleTestOpenGL
 * @tc.desc: Tests that a signal is found and exported by a handle without a reference counter.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuSignal, GfxGpuSignalCounterlessHandleTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuSignalCounterlessHandle(UTest::GetOpenGLBackend());
}

#if (TEST_HAS_NATIVE_FENCE_FD == 1)
/**
 * @tc.name: GfxGpuWaitNonFenceFdTestOpenGL
 * @tc.desc: Tests that a descriptor the backend cannot import is still closed exactly once.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitNonFenceFdTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuWaitNonFenceFdReclaimed(UTest::GetOpenGLBackend(), true);
}

/**
 * @tc.name: GfxGpuWaitRoundTripTestOpenGL
 * @tc.desc: Exports a frame's GPU signal as a native fence fd and waits on it the following frame. Covers
 * EGL native fence sync objects.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitRoundTripTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuWaitFenceRoundTrip(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuWaitLookupByHandleTestOpenGL
 * @tc.desc: Tests that waits queued for the same frame are each found by their own handle.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitLookupByHandleTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuWaitLookupByHandle(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuWaitHeldOverIdleFramesTestOpenGL
 * @tc.desc: Tests that a wait survives frames with no GPU work and applies to the next frame with work.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitHeldOverIdleFramesTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuWaitHeldOverIdleFrames(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuWaitCpuFallbackTestOpenGL
 * @tc.desc: Tests that a descriptor the GPU cannot wait on is waited on the CPU and closed within the frame.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitCpuFallbackTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuWaitCpuFallback(UTest::GetOpenGLBackend());
}

/**
 * @tc.name: GfxGpuWaitCounterlessHandleTestOpenGL
 * @tc.desc: Tests that a wait is found by a handle without a reference counter.
 * @tc.type: FUNC
 */
UNIT_TEST(API_GfxGpuWait, GfxGpuWaitCounterlessHandleTestOpenGL, testing::ext::TestSize.Level1)
{
    TestGpuWaitCounterlessHandle(UTest::GetOpenGLBackend());
}
#endif  // TEST_HAS_NATIVE_FENCE_FD
#endif  // RENDER_HAS_GL_BACKEND || RENDER_HAS_GLES_BACKEND
