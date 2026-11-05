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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cinttypes>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <3d/ecs/components/camera_component.h>
#include <3d/ecs/components/world_matrix_component.h>
#include <3d/ecs/systems/intf_node_system.h>
#include <base/containers/vector.h>
#include <base/math/vector.h>
#include <core/ecs/entity_reference.h>
#include <core/ecs/intf_component_manager.h>
#include <core/ecs/intf_ecs.h>
#include <core/image/intf_image_loader_manager.h>
#include <core/implementation_uids.h>
#include <core/intf_engine.h>
#include <core/io/intf_file.h>
#include <core/io/intf_file_manager.h>
#include <core/log.h>
#include <core/perf/cpu_perf_scope.h>
#include <core/plugin/intf_class_factory.h>
#include <core/threading/intf_thread_pool.h>
#include <core/util/parallel_sort.h>
#include <render/datastore/intf_render_data_store_default_staging.h>
#include <render/datastore/intf_render_data_store_manager.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/implementation_uids.h>
#include <render/intf_render_context.h>
#include <render/render_data_structures.h>
#include <render/resource_handle.h>

#include "data/sog_parsing.h"
#include "data/glb_parsing.h"
#include "plugin_gs/ecs/components/gs_configuration_component.h"
#include "plugin_gs/ecs/components/gs_tiling_component.h"
#include "plugin_gs/ecs/systems/intf_gs_tiling_system.h"
#include "plugin_gs/render/intf_render_data_store_default_gs.h"
#include "render/render_data_store_default_gs.h"
#include "util/gs_loading.h"
#include <plugin_gs/util/gs_limits.h>

using namespace BASE_NS;
using namespace CORE_NS;
using namespace RENDER_NS;
using namespace GS;

namespace {
constexpr string_view RENDER_DATA_STORE_DEFAULT_STAGING { "RenderDataStoreDefaultStaging" };

constexpr size_t kDefaultSogCacheBytesBudget = 256ULL * 1024ULL * 1024ULL;
constexpr uint64_t kDefaultGpuPoolBytesBudget = 704ULL * 1024ULL * 1024ULL;
constexpr uint32_t kDefaultMaxPoolTiles = 2048u;
constexpr uint32_t kInitialSortCapacity = 262144u;
constexpr uint32_t kSortCapacityMarginNum = 9u;
constexpr uint32_t kSortCapacityMarginDen = 8u;

constexpr uint64_t kEvictGracePeriodUs = 250000uLL;

constexpr uint32_t kMinConcurrentDecodes = 16u;
constexpr uint32_t kConcurrentDecodesPerThread = 1u;
constexpr std::chrono::milliseconds kWorkerDrainPollInterval { 50 };

constexpr bool kLogPoolCapacity = false;
constexpr bool kLogSogCacheCapacity = false;
constexpr bool kLogPoolFragmentation = false;

constexpr bool kLogLoadingDebug = false;

inline uint64_t NowUs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline uint32_t NextPow2(uint64_t v)
{
    if (v <= 1u) {
        return 1u;
    }
    if (v >= static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) {
        return std::numeric_limits<uint32_t>::max();
    }

    --v;
    v |= v >> 1;
    v |= v >> 2;
    v |= v >> 4;
    v |= v >> 8;
    v |= v >> 16;
    v |= v >> 32;

    return static_cast<uint32_t>(v + 1u);
}

inline uint32_t AlignSplatCapacity(uint32_t count)
{
    constexpr uint32_t alignment = 32u;
    const uint64_t aligned =
        (static_cast<uint64_t>(Math::max(count, 1u)) + alignment - 1u) & ~(uint64_t(alignment) - 1u);

    return aligned > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                          : static_cast<uint32_t>(aligned);
}

inline uint64_t AlignedSlotSplatContribution(uint32_t splatCount)
{
    constexpr uint64_t kMergedSlotAlignment = 32u;
    return (static_cast<uint64_t>(splatCount) + (kMergedSlotAlignment - 1u)) & ~(kMergedSlotAlignment - 1u);
}

inline size_t EstimateCacheBytes(const GS::CachedSogData& c)
{
    if (c.dataFormat == GS::CachedSogData::DataFormat::GLB_EXPANDED) {
        return c.packedSplats.size() + c.glbAttributeDatasBytes;
    }
    return c.packedSplats.size() + c.packedCentroids.size() + c.packedLabels.size() * sizeof(uint16_t);
}

inline void ReleaseCachedSogData(GS::CachedSogData& cache)
{
    cache.packedSplats = BASE_NS::vector<uint8_t>();
    cache.packedCentroids = BASE_NS::vector<uint8_t>();
    cache.packedLabels = BASE_NS::vector<uint16_t>();

    cache.hasPositions = false;
    cache.hasQuats = false;
    cache.hasScales = false;
    cache.hasSh0 = false;

    cache.glbModel = nullptr;
    cache.glbAttributeDatasBytes = 0;
    cache.dataFormat = GS::CachedSogData::DataFormat::SOG_PACKED;
}

inline bool IsSogUri(const BASE_NS::string_view uri)
{
    constexpr size_t kExtLen = 4u;
    if (uri.size() < kExtLen) {
        return false;
    }

    return uri.compare(uri.size() - kExtLen, kExtLen, ".sog") == 0;
}

inline bool IsGlbUri(const BASE_NS::string_view uri)
{
    constexpr size_t kExtLen = 4u;
    if (uri.size() < kExtLen) {
        return false;
    }

    return uri.compare(uri.size() - kExtLen, kExtLen, ".glb") == 0;
}

class SogCache {
public:
    explicit SogCache(size_t bytesBudget) : bytesBudget_(bytesBudget) {}

    void SetBytesBudget(size_t bytesBudget)
    {
        bytesBudget_ = bytesBudget;
    }

    std::pair<std::shared_ptr<GS::CachedSogData>, bool> GetOrCreate(const std::string& uri)
    {
        auto it = entries_.find(uri);
        if (it != entries_.end()) {
            it->second.lastAccess = ++accessCounter_;
            it->second.accessCount++;
            return { it->second.data, false };
        }

        const auto data = std::make_shared<GS::CachedSogData>();
        Entry entry;
        entry.data = data;
        entry.lastAccess = ++accessCounter_;
        entry.accessCount = 1u;
        entry.bytes = 0u;
        entries_.emplace(uri, std::move(entry));
        return { data, true };
    }

    struct GatedAcquire {
        std::shared_ptr<GS::CachedSogData> data;
        bool isDecoder { false };
        bool accepted { false };
    };

    GatedAcquire GetOrCreateGated(const std::string& uri, uint32_t maxLoading, bool bypassCountLimit)
    {
        auto it = entries_.find(uri);
        if (it != entries_.end()) {
            it->second.lastAccess = ++accessCounter_;
            it->second.accessCount++;
            return { it->second.data, false, true };
        }

        const uint32_t loading = LoadingCount();
        if (!bypassCountLimit && loading >= maxLoading) {
            return { nullptr, false, false };
        }
        if (loading > 0u && totalBytes_ >= bytesBudget_) {
            return { nullptr, false, false };
        }

        const auto data = std::make_shared<GS::CachedSogData>();
        Entry entry;
        entry.data = data;
        entry.lastAccess = ++accessCounter_;
        entry.accessCount = 1u;
        entry.bytes = 0u;
        entries_.emplace(uri, std::move(entry));

        return { data, true, true };
    }

    void Touch(const std::string& uri)
    {
        auto it = entries_.find(uri);
        if (it == entries_.end()) {
            return;
        }

        it->second.lastAccess = ++accessCounter_;
        it->second.accessCount++;
    }

    void Finalize(const std::string& uri, const std::shared_ptr<GS::CachedSogData>& data, const size_t bytes)
    {
        auto it = entries_.find(uri);
        if (it == entries_.end()) {
            return;
        }

        if (it->second.data.get() != data.get()) {
            return;
        }

        if (it->second.bytes <= totalBytes_) {
            totalBytes_ -= it->second.bytes;
        } else {
            totalBytes_ = 0u;
        }

        totalBytes_ += bytes;
        it->second.bytes = bytes;
        it->second.lastAccess = ++accessCounter_;
        EvictUntilUnderBudget(uri);

        if constexpr (kLogSogCacheCapacity) {
            LogStatus();
        }
    }

    void Remove(const std::string& uri, const std::shared_ptr<GS::CachedSogData>& data)
    {
        auto it = entries_.find(uri);
        if (it == entries_.end()) {
            return;
        }

        if (it->second.data.get() != data.get()) {
            return;
        }

        ForgetEntry(it);
    }

    void Clear()
    {
        entries_.clear();
        totalBytes_ = 0u;
    }

    void EvictUnreferenced()
    {
        for (auto it = entries_.begin(); it != entries_.end();) {
            const bool unreferenced = it->second.data.use_count() <= 1;
            const bool loading = IsLoadingEntry(it->second);

            if (unreferenced && loading) {
                it->second.data->state.store(GS::CachedSogData::State::FAILED);
                ReleaseCachedSogData(*it->second.data);

                if (it->second.bytes <= totalBytes_) {
                    totalBytes_ -= it->second.bytes;
                } else {
                    totalBytes_ = 0u;
                }
                it = entries_.erase(it);

                continue;
            }

            it++;
        }

        while (totalBytes_ > bytesBudget_) {
            auto victim = FindEvictable({}, true, true);
            if (victim == entries_.end()) {
                victim = FindEvictable({}, true, false);
            }
            if (victim == entries_.end()) {
                break;
            }
            ForgetEntry(victim);
        }
    }

    uint32_t LoadingCount() const
    {
        uint32_t count = 0u;
        for (const auto& entry : entries_) {
            if (IsLoadingEntry(entry.second)) {
                count++;
            }
        }

        return count;
    }

    void LogStatus(const char* tag = "SogCache") const
    {
        uint32_t ready = 0u;
        uint32_t loading = 0u;
        uint32_t failed = 0u;
        size_t referenced = 0u;

        for (const auto& kv : entries_) {
            const auto& e = kv.second;
            if (e.data) {
                switch (e.data->state.load()) {
                    case GS::CachedSogData::State::READY:
                        ready++;
                        break;
                    case GS::CachedSogData::State::LOADING:
                        loading++;
                        break;
                    case GS::CachedSogData::State::FAILED:
                        failed++;
                        break;
                }

                if (e.data.use_count() > 1) {
                    referenced++;
                }
            }
        }

        const size_t available = totalBytes_ < bytesBudget_ ? bytesBudget_ - totalBytes_ : 0u;

        CORE_LOG_E("%s: files=%zu (ready=%u load=%u fail=%u ref=%zu) "
                   "used=%.1f/%.1f MB available=%.1f MB (%.0f%%)",
            tag, entries_.size(), ready, loading, failed, referenced,
            static_cast<double>(totalBytes_) / (1024.0 * 1024.0), static_cast<double>(bytesBudget_) / (1024.0 * 1024.0),
            static_cast<double>(available) / (1024.0 * 1024.0),
            bytesBudget_ ? 100.0 * static_cast<double>(totalBytes_) / static_cast<double>(bytesBudget_) : 0.0);
    }

private:
    struct Entry {
        std::shared_ptr<GS::CachedSogData> data;
        uint64_t lastAccess { 0u };
        uint64_t accessCount { 0u };
        size_t bytes { 0u };
    };

    using EntryIterator = std::unordered_map<std::string, Entry>::iterator;

    static uint64_t EvictionValue(const Entry& e)
    {
        static constexpr uint64_t kAccessFreqWeight = 16u;
        return e.lastAccess + e.accessCount * kAccessFreqWeight;
    }

    bool IsLoadingEntry(const Entry& entry) const
    {
        return entry.data && entry.data->state.load() == GS::CachedSogData::State::LOADING;
    }

    EntryIterator FindEvictable(const std::string& keepUri, const bool allowKeepUri, const bool requireBytes)
    {
        EntryIterator evictable = entries_.end();
        uint64_t evictableValue = 0u;
        for (auto it = entries_.begin(); it != entries_.end(); it++) {
            if (!allowKeepUri && it->first == keepUri) {
                continue;
            }

            if (IsLoadingEntry(it->second)) {
                continue;
            }

            if (it->second.data.use_count() > 1) {
                continue;
            }

            if (requireBytes && it->second.bytes == 0u) {
                continue;
            }

            const uint64_t value = EvictionValue(it->second);
            if (evictable == entries_.end() || value < evictableValue) {
                evictable = it;
                evictableValue = value;
            }
        }

        return evictable;
    }

    void ForgetEntry(EntryIterator it)
    {
        if (it->second.bytes <= totalBytes_) {
            totalBytes_ -= it->second.bytes;
        } else {
            totalBytes_ = 0u;
        }

        entries_.erase(it);
    }

    void EvictUntilUnderBudget(const std::string& keepUri)
    {
        while (totalBytes_ > bytesBudget_) {
            auto victim = FindEvictable(keepUri, false, true);
            if (victim == entries_.end()) {
                victim = FindEvictable(keepUri, true, true);
            }

            if (victim == entries_.end()) {
                victim = FindEvictable(keepUri, false, false);
            }

            if (victim == entries_.end()) {
                victim = FindEvictable(keepUri, true, false);
            }

            if (victim == entries_.end()) {
                break;
            }

            const std::string victimUri = victim->first;
            ForgetEntry(victim);
        }
    }

    std::unordered_map<std::string, Entry> entries_;
    size_t totalBytes_ { 0u };
    size_t bytesBudget_;
    uint64_t accessCounter_ { 0u };
};
} // namespace

struct TileSlot {
    BASE_NS::string uri;
    int32_t poolSlot { -1 };
    uint32_t poolOffset { 0u };
    uint32_t poolCapacity { 0u };
    uint32_t splatCount { 0 };
    uint64_t touchedAtUs { 0u };
    bool visible { true };
};

struct TileLoadResult {
    BASE_NS::Math::Vec3 aabbMin {};
    BASE_NS::Math::Vec3 aabbMax {};
    uint32_t count { 0 };
    bool success { false };
    std::atomic_bool cancel { false };
    std::atomic_bool deferred { false };
    std::atomic_bool completed { false };
};

struct PendingLoad {
    CORE_NS::Entity entity;
    BASE_NS::string uri;
    uint32_t offset { 0U };
    uint32_t count { 0U };
    int32_t slot { -1 };
    uint32_t poolOffset { 0u };
    uint32_t poolCapacity { 0u };
    bool cancelled { false };
    bool iAmDecoder { false };
    bool visible { true };
    bool requestVisible { true };
    BASE_NS::Math::Vec3 aabbMin {};
    BASE_NS::Math::Vec3 aabbMax {};
    std::shared_ptr<GS::CachedSogData> cacheEntry;
    BASE_NS::shared_ptr<TileLoadResult> result;
    CORE_NS::IThreadPool::IResult::Ptr taskResult;
    bool awaitingCache { false };
};

namespace {
struct TileLoadTaskCompletion {
    BASE_NS::shared_ptr<TileLoadResult> res;
    std::shared_ptr<GS::CachedSogData> cacheEntry;
    BASE_NS::string uri;
    std::mutex* cacheMtx { nullptr };
    SogCache* cache { nullptr };
    bool iAmDecoder { false };
    void (*notifyFn)(void*) { nullptr };
    void* notifyArg { nullptr };

    ~TileLoadTaskCompletion()
    {
        if (iAmDecoder && cacheEntry) {
            auto expected = GS::CachedSogData::State::LOADING;
            if (cacheEntry->state.compare_exchange_strong(expected, GS::CachedSogData::State::FAILED)) {
                ReleaseCachedSogData(*cacheEntry);
                if (cacheMtx && cache) {
                    const std::string uriKey(uri.data(), uri.size());
                    std::lock_guard<std::mutex> lock(*cacheMtx);
                    cache->Remove(uriKey, cacheEntry);
                }
            }
        }

        if (res) {
            res->completed.store(true, std::memory_order_release);
        }

        if (notifyFn) {
            notifyFn(notifyArg);
        }
    }
};

struct TileLoadTaskContext {
    BASE_NS::shared_ptr<TileLoadResult> res;
    BASE_NS::string uri;
    uint32_t offset { 0u };
    uint32_t count { 0u };
    std::shared_ptr<GS::CachedSogData> cacheEntry;
    bool iAmDecoder { false };
    CORE_NS::IFileManager* fileMgr { nullptr };
    CORE_NS::IImageLoaderManager* imgMgr { nullptr };
    std::mutex* cacheMtx { nullptr };
    SogCache* cache { nullptr };
    uint8_t* dst { nullptr };
    size_t dstAvailable { 0u };
    bool decodeOnly { false };
    bool useTilePack { false };
    void (*notifyFn)(void*) { nullptr };
    void* notifyArg { nullptr };
};

bool DecodeTileCache(const TileLoadTaskContext& ctx)
{
    const std::string uriKey(ctx.uri.data(), ctx.uri.size());
    const auto failCacheEntry = [&ctx, &uriKey]() {
        ReleaseCachedSogData(*ctx.cacheEntry);
        ctx.cacheEntry->state.store(GS::CachedSogData::State::FAILED);

        if (ctx.cacheMtx && ctx.cache) {
            std::lock_guard<std::mutex> lock(*ctx.cacheMtx);
            ctx.cache->Remove(uriKey, ctx.cacheEntry);
        }
    };

    auto file = ctx.fileMgr->OpenFile(ctx.uri);
    if (!file) {
        failCacheEntry();
        return false;
    }

    const uint64_t len = file->GetLength();
    if (len == 0 || len > GS::MAX_SOG_FILE_SIZE) {
        failCacheEntry();
        return false;
    }
    BASE_NS::vector<uint8_t> buf(static_cast<size_t>(len));
    const uint64_t bytesRead = file->Read(buf.data(), static_cast<size_t>(len));
    if (bytesRead != len) {
        failCacheEntry();
        return false;
    }

    if (IsSogUri(ctx.uri)) {
        if (!GS::DecodeSogFile(buf.data(), buf.size(), *ctx.cacheEntry, *ctx.imgMgr, false)) {
            failCacheEntry();
            return false;
        }
    } else if (IsGlbUri(ctx.uri)) {
        if (!GS::DecodeGlbFile(buf.data(), buf.size(), *ctx.cacheEntry, false)) {
            failCacheEntry();
            return false;
        }
    }

    const size_t approxBytes = EstimateCacheBytes(*ctx.cacheEntry);
    {
        std::lock_guard<std::mutex> lock(*ctx.cacheMtx);
        ctx.cacheEntry->state.store(GS::CachedSogData::State::READY);
        ctx.cache->Finalize(uriKey, ctx.cacheEntry, approxBytes);
    }

    if constexpr (kLogLoadingDebug) {
        CORE_LOG_D("GsTilingSystem: decoded SOG '%s' (%.1f MB)", ctx.uri.c_str(),
            static_cast<double>(approxBytes) / (1024.0 * 1024.0));
    }
    return true;
}

bool WaitTileCacheReady(const TileLoadTaskContext& ctx)
{
    const auto state = ctx.cacheEntry->state.load();
    if (state == GS::CachedSogData::State::FAILED) {
        return false;
    }
    if (state == GS::CachedSogData::State::LOADING) {
        ctx.res->deferred.store(true);
        return false;
    }
    return true;
}

bool ExtractTileDirectToPool(const TileLoadTaskContext& ctx, GsPrimitive& tempPrim)
{
    if (ctx.res->cancel.load()) {
        return false;
    }
    if (!ctx.dst) {
        return false;
    }

    const uint64_t writeBytes64 = static_cast<uint64_t>(ctx.count) * (ctx.useTilePack
                                  ? sizeof(GsObjectLoadData::GsSplatDataTilePacked)
                                  : sizeof(GsObjectLoadData::GsSplatData));
    if (writeBytes64 == 0u || writeBytes64 > ctx.dstAvailable) {
        return false;
    }

    tempPrim.gsPath = ctx.uri;

    if (ctx.cacheEntry->dataFormat == GS::CachedSogData::DataFormat::SOG_PACKED) {
        if (!GS::ExtractSplatsToBuffer(
            *ctx.cacheEntry, ctx.offset, ctx.count, ctx.dst, ctx.dstAvailable, tempPrim, ctx.res->cancel, true,
            ctx.useTilePack)) {
            return false;
        }
    } else if (ctx.cacheEntry->dataFormat == GS::CachedSogData::DataFormat::GLB_EXPANDED) {
        if (!GS::ExtractGlbSplatsToBuffer(*ctx.cacheEntry, ctx.res->cancel,
            { ctx.offset, ctx.count, ctx.dst, ctx.dstAvailable, true, ctx.useTilePack }, tempPrim)) {
            return false;
        }
    }

    ctx.res->aabbMin = tempPrim.aabbMin;
    ctx.res->aabbMax = tempPrim.aabbMax;
    ctx.res->count = ctx.count;
    ctx.res->success = true;
    return true;
}

void ExecuteTileLoadTask(TileLoadTaskContext ctx)
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "TileLoadTask", "", 0xFF00FF);

    TileLoadTaskCompletion completion { ctx.res, ctx.cacheEntry, ctx.uri, ctx.cacheMtx, ctx.cache, ctx.iAmDecoder,
        ctx.notifyFn, ctx.notifyArg };

    if (ctx.iAmDecoder) {
        if (!DecodeTileCache(ctx)) {
            return;
        }
    } else {
        if (ctx.res->cancel.load()) {
            return;
        }

        if (!WaitTileCacheReady(ctx)) {
            return;
        }
    }

    if (ctx.decodeOnly || ctx.res->cancel.load()) {
        return;
    }

    GsPrimitive tempPrim;
    if (!ExtractTileDirectToPool(ctx, tempPrim)) {
        return;
    }
}
} // namespace

class GsTilingSystem final : public IGsTilingSystem, private IEcs::ComponentListener {
public:
    explicit GsTilingSystem(IEcs& ecs);
    ~GsTilingSystem() override;

    string_view GetName() const override;
    Uid GetUid() const override;

    IPropertyHandle* GetProperties() override;
    const IPropertyHandle* GetProperties() const override;
    void SetProperties(const IPropertyHandle&) override;

    bool IsActive() const override;
    void SetActive(bool state) override;

    void Initialize() override;
    void Uninitialize() override;
    bool Update(bool frameRenderingQueued, uint64_t time, uint64_t delta) override;

    const IEcs& GetECS() const override;

    void CreateTile(const CORE_NS::Entity entity, const GS::GsTilingComponent& data) override;
    void SetTileVisible(const CORE_NS::Entity entity, const bool visible) override;
    void DestroyTile(const CORE_NS::Entity entity) override;
    bool IsTileResident(const CORE_NS::Entity entity) const override;

    void OnComponentEvent(
        EventType type, const IComponentManager& componentManager, array_view<const Entity> entities) override;

private:
    struct TileEntry;

    const GS::GsTilingComponent* FindTileData(const CORE_NS::Entity entity) const;
    bool ReadTileData(const CORE_NS::Entity entity, GS::GsTilingComponent& out) const;
    bool ReadTileResidency(const CORE_NS::Entity entity) const;
    void SetTileResidency(const CORE_NS::Entity entity, const bool resident, const uint32_t numResidentSplats);

    void StartWorker();
    void StopWorker();
    void NotifyWorker();
    void WorkerLoop();
    bool WorkerUpdate();
    void ProcessWorkerEvents();
    bool HasWorkerEventsLocked() const;
    bool HasOutstandingWork() const;

    struct PreparedSubmission {
        CORE_NS::Entity entity;
        BASE_NS::string uri;
        uint32_t offset { 0u };
        uint32_t count { 0u };
        uint32_t poolOffset { 0u };
        uint32_t poolCapacity { 0u };

        BASE_NS::Math::Vec3 aabbMin {};
        BASE_NS::Math::Vec3 aabbMax {};
        BASE_NS::shared_ptr<TileLoadResult> result;

        bool visible { true };
        int32_t slot { -1 };
    };

    struct LoadCandidate {
        CORE_NS::Entity entity;
        BASE_NS::Math::Vec3 aabbMin;
        BASE_NS::Math::Vec3 aabbMax;
        BASE_NS::string uri;

        uint32_t offset { 0u };
        uint32_t count { 0u };
        bool visible { true };
    };

    void InitializePool();
    void InitializeLoadContext(GsRenderData& rd, GsLoadContext& ctx);
    void ApplyInitialPoolConfig();
    void RegisterPoolPrimitive(RenderDataStoreDefaultGS& internalDs);
    bool PrepareTileSubmission(CORE_NS::Entity entity, const GS::GsTilingComponent& comp, PreparedSubmission& outPrep);
    bool DispatchPreparedSubmission(PreparedSubmission& prep);
    void DispatchTileTask(PendingLoad& pending, bool decodeOnly = false);
    bool TryDispatchAwaitingLoad(PendingLoad& pending);
    void PreparePendingCache(PendingLoad& pending);
    void EvictTile(CORE_NS::Entity entity);
    void ReleaseTileEntry(const TileEntry& entry);
    int32_t ReleaseTileEntryResources(const TileEntry& entry);
    void FreePoolSlotLocked(int32_t slot, GsTilePool& pool, GsPrimitive* poolPrim);
    void EvictForBatch(uint32_t neededSlots, uint64_t neededSplatCapacity);
    bool EnsurePoolCapacity(uint32_t minSlots, uint32_t minSplatCapacity);
    bool UpdateSortCapacityLocked();
    void RecreateSortCapacityBuffers();
    bool ValidateSplatPoolTarget(uint32_t targetSplatCapacity) const;
    void ResizeTilePool(GsTilePool& pool, uint32_t targetSlots, uint32_t targetSplatCapacity);
    bool AllocateSplatRange(uint32_t count, uint32_t& offset, uint32_t& capacity);
    void ReleaseSplatRange(uint32_t offset, uint32_t capacity);
    GsPrimitive* FindPoolPrimitive();

    void UpdateGsConfigCache();

    bool TryEvictNonCriticalTile(uint32_t requiredCapacity = 0u);
    void LogPoolFragmentation(const char* context) const;

    void ProcessPendingLoads();
    void UpdatePendingVisibility(PendingLoad& pending);
    bool HandleCompletedPendingLoad(
        PendingLoad& pending, BASE_NS::vector<PendingLoad>& finalizeBatch, BASE_NS::vector<PendingLoad>& releaseBatch);
    BASE_NS::vector<PendingLoad> CollectCompletedPendingLoads(BASE_NS::vector<PendingLoad>& releaseBatch);
    bool AcquirePoolResidency(PendingLoad& pending);
    void ReleasePendingTileResources(BASE_NS::vector<PendingLoad>& releaseBatch);
    void DrainInFlightLoaders();
    uint8_t* EnsureSplatPoolMapped();
    void UnmapSplatPool();
    void MarkPoolTileUnloaded(int32_t slot, GsPrimitive* poolPrim);
    void ReturnReleasedPendingSlots(
        const BASE_NS::vector<PendingLoad>& releaseBatch, GsTilePool& pool, GsPrimitive* poolPrim);
    void UpdatePoolTileMetadata(PendingLoad& pending, GsTileMetadata& tile, bool isVisible);
    void AddTileEntry(PendingLoad& pending, bool isVisible);
    void CommitFinalizedPendingLoads(
        BASE_NS::vector<PendingLoad>& finalizeBatch, GsTilePool& pool, GsPrimitive* poolPrim);
    void CommitPendingResults(
        BASE_NS::vector<PendingLoad>& finalizeBatch, const BASE_NS::vector<PendingLoad>& releaseBatch);
    void RemoveDeadTilesAndSyncVisibility();
    void EnqueueTileRequest(CORE_NS::Entity entity);
    void QueueExistingTileRequests();
    bool BuildLoadCandidate(CORE_NS::Entity entity, LoadCandidate& candidate);
    BASE_NS::vector<LoadCandidate> BuildLoadCandidates();
    BASE_NS::vector<PreparedSubmission> PrepareSubmissions(const BASE_NS::vector<LoadCandidate>& candidates);
    void DispatchPreparedLoads(BASE_NS::vector<PreparedSubmission>& prepared);
    void SubmitNewTileLoads();

    std::atomic_bool active_ { true };
    IEcs& ecs_;

    enum class TileEventType {
        CREATE_OR_UPDATE,
        SET_VISIBLE,
        DESTROY,
    };

    struct TileEvent {
        TileEventType type { TileEventType::CREATE_OR_UPDATE };
        CORE_NS::Entity entity;
        GS::GsTilingComponent data;
        bool visible { true };
    };

    std::thread workerThread_;
    std::mutex workerMutex_;
    std::condition_variable workerCv_;
    BASE_NS::vector<TileEvent> workerTileEvents_;
    BASE_NS::vector<CORE_NS::Entity> workerRequestEvents_;
    BASE_NS::vector<CORE_NS::Entity> workerDestroyEvents_;
    CORE_NS::Entity workerCreatedConfigEntity_ {};
    bool workerHasCreatedConfigEntity_ { false };
    bool workerConfigDestroyed_ { false };
    uint32_t workerWakeCount_ { 0u };
    bool workerStopRequested_ { false };

    RENDER_NS::IRenderContext* renderContext_ { nullptr };
    RENDER_NS::IRenderDataStoreDefaultStaging* dsStaging_ { nullptr };
    IGsConfigurationComponentManager* gsConfigManager_ { nullptr };
    BASE_NS::refcnt_ptr<IRenderDataStoreDefaultGs> dataStoreGs_;

    CORE_NS::Entity poolEntity_ {};
    CORE_NS::Entity gsConfigEntity_ {};
    bool poolEntityRegistered_ { false };
    bool poolInitialized_ { false };

    uint32_t maxPoolTiles_ { 0u };
    uint32_t splatsPerTile_ { 1u };
    uint32_t splatPoolCapacity_ { 0u };
    uint32_t sortPoolCapacity_ { 0u };
    uint32_t nextSplatOffset_ { 0u };

    struct FreeSplatRange {
        uint32_t offset { 0u };
        uint32_t count { 0u };
    };
    BASE_NS::vector<FreeSplatRange> freeSplatRanges_;

    std::shared_ptr<uint32_t> multiGsBufferIndexCounter_ { nullptr };

    struct TileEntry {
        CORE_NS::Entity entity;
        TileSlot slot;
    };

    BASE_NS::vector<TileEntry> tileSlots_;
    uint64_t visibleSplatSum_ { 0u };
    CORE_NS::IThreadPool::Ptr threadPool_;
    BASE_NS::vector<PendingLoad> pendingLoads_;
    BASE_NS::vector<PendingLoad> readyToCommit_;
    std::unordered_set<uint64_t> pendingEntities_;
    std::unordered_set<uint64_t> pendingVisibleSnapshot_;
    BASE_NS::vector<CORE_NS::Entity> requestQueue_;
    std::unordered_set<uint64_t> queuedEntities_;
    size_t requestQueueHead_ { 0u };
    bool initialTileRequestsQueued_ { false };
    std::chrono::steady_clock::time_point lastReenqueueTime_ {};

    std::mutex sogCacheMutex_;
    SogCache sogCache_ { kDefaultSogCacheBytesBudget };
    std::atomic<uint64_t> gpuPoolBytesBudget_ { kDefaultGpuPoolBytesBudget };
    uint32_t lastAppliedQualityLevel_ { 0xFFFFFFFFu };

    uint32_t maxConcurrentDecodes_ { kMinConcurrentDecodes };

    RENDER_NS::RenderHandleReference splatPoolMappedHandle_;
    uint8_t* splatPoolMappedPtr_ { nullptr };

    mutable std::mutex gsConfigCacheMutex_;
    BASE_NS::shared_ptr<GS::GsConfigurationComponent> cachedGsConfig_;
    std::atomic_bool wantsRender_ { false };

    std::unordered_map<uint64_t, GS::GsTilingComponent> tileData_;

    mutable std::mutex residencyMutex_;
    std::unordered_set<uint64_t> residentEntities_;
};

GsTilingSystem::GsTilingSystem(IEcs& ecs) : ecs_(ecs)
{
    IClassRegister* classRegister = ecs.GetClassFactory().GetInterface<IClassRegister>();
    if (classRegister) {
        renderContext_ = GetInstance<IRenderContext>(*classRegister, UID_RENDER_CONTEXT);
    }
    if (renderContext_) {
        const IRenderDataStoreManager& dsm = renderContext_->GetRenderDataStoreManager();
        dsStaging_ = static_cast<IRenderDataStoreDefaultStaging*>(
            dsm.GetRenderDataStore(RENDER_DATA_STORE_DEFAULT_STAGING).get());
    }
    gsConfigManager_ = GetManager<IGsConfigurationComponentManager>(ecs);
    multiGsBufferIndexCounter_ = std::make_shared<uint32_t>(0u);
    threadPool_ = ecs.GetThreadPool();
    uint32_t threadCount = threadPool_ ? static_cast<uint32_t>(threadPool_->GetNumberOfThreads()) : 1u;
    threadCount = Math::max(threadCount, 1u);
    maxConcurrentDecodes_ = Math::max(kMinConcurrentDecodes, threadCount * kConcurrentDecodesPerThread);
}

GsTilingSystem::~GsTilingSystem()
{
    StopWorker();

    for (auto& pending : pendingLoads_) {
        if (pending.result) {
            pending.result->cancel.store(true);
        }

        if (pending.taskResult) {
            pending.taskResult->Wait();
        }
    }
}

string_view GsTilingSystem::GetName() const
{
    return GS::GetName(static_cast<const IGsTilingSystem*>(this));
}

Uid GsTilingSystem::GetUid() const
{
    return IGsTilingSystem::UID;
}

IPropertyHandle* GsTilingSystem::GetProperties()
{
    return nullptr;
}

const IPropertyHandle* GsTilingSystem::GetProperties() const
{
    return nullptr;
}

void GsTilingSystem::SetProperties(const IPropertyHandle&) {}

bool GsTilingSystem::IsActive() const
{
    return active_.load(std::memory_order_acquire);
}

void GsTilingSystem::SetActive(bool state)
{
    active_.store(state, std::memory_order_release);
    NotifyWorker();
}

bool GsTilingSystem::HasWorkerEventsLocked() const
{
    return workerWakeCount_ > 0u || !workerTileEvents_.empty() || !workerRequestEvents_.empty() ||
           !workerDestroyEvents_.empty() || workerHasCreatedConfigEntity_ || workerConfigDestroyed_;
}

void GsTilingSystem::StartWorker()
{
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (workerThread_.joinable()) {
            return;
        }

        workerStopRequested_ = false;
        workerWakeCount_ = 0u;
    }

    workerThread_ = std::thread([this]() { WorkerLoop(); });
}

void GsTilingSystem::StopWorker()
{
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        workerStopRequested_ = true;
        workerWakeCount_ = 0u;
    }
    workerCv_.notify_one();

    if (workerThread_.joinable()) {
        workerThread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(workerMutex_);

        workerTileEvents_.clear();
        workerRequestEvents_.clear();
        workerDestroyEvents_.clear();
        workerCreatedConfigEntity_ = {};
        workerHasCreatedConfigEntity_ = false;
        workerConfigDestroyed_ = false;
        workerWakeCount_ = 0u;
    }
}

void GsTilingSystem::NotifyWorker()
{
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (workerStopRequested_) {
            return;
        }

        workerWakeCount_ = Math::min(workerWakeCount_ + 1u, 1024u);
    }

    workerCv_.notify_one();
}

void GsTilingSystem::WorkerLoop()
{
    bool workOutstanding = false;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(workerMutex_);
            const auto ready = [this]() { return workerStopRequested_ || HasWorkerEventsLocked(); };
            if (workOutstanding) {
                workerCv_.wait_for(lock, kWorkerDrainPollInterval, ready);
            } else {
                workerCv_.wait(lock, ready);
            }
            if (workerStopRequested_) {
                return;
            }

            if (workerWakeCount_ > 0u) {
                workerWakeCount_--;
            }
        }

        workOutstanding = WorkerUpdate();
    }
}

void GsTilingSystem::ProcessWorkerEvents()
{
    BASE_NS::vector<TileEvent> tileEvents;
    BASE_NS::vector<CORE_NS::Entity> requestEvents;
    BASE_NS::vector<CORE_NS::Entity> destroyEvents;
    CORE_NS::Entity createdConfigEntity {};
    bool hasCreatedConfigEntity = false;
    bool configDestroyed = false;

    {
        std::lock_guard<std::mutex> lock(workerMutex_);

        tileEvents.swap(workerTileEvents_);
        requestEvents.swap(workerRequestEvents_);
        destroyEvents.swap(workerDestroyEvents_);
        createdConfigEntity = workerCreatedConfigEntity_;
        hasCreatedConfigEntity = workerHasCreatedConfigEntity_;
        configDestroyed = workerConfigDestroyed_;
        workerCreatedConfigEntity_ = {};
        workerHasCreatedConfigEntity_ = false;
        workerConfigDestroyed_ = false;
    }

    if (hasCreatedConfigEntity) {
        if (EntityUtil::IsValid(gsConfigEntity_)) {
            CORE_LOG_E("GsTilingSystem::OnComponentEvent: GsConfigurationComponent already set! Only one can exist!");
        } else {
            gsConfigEntity_ = createdConfigEntity;
        }
    }

    if (configDestroyed) {
        gsConfigEntity_ = {};
    }

    BASE_NS::vector<CORE_NS::Entity> tileRequests;
    std::unordered_set<uint64_t> tileRequestIds;
    tileRequests.reserve(tileEvents.size());
    tileRequestIds.reserve(tileEvents.size());

    const auto addTileRequest = [&tileRequests, &tileRequestIds](const CORE_NS::Entity entity) {
        if (tileRequestIds.insert(entity.id).second) {
            tileRequests.push_back(entity);
        }
    };

    for (const auto& event : tileEvents) {
        switch (event.type) {
            case TileEventType::CREATE_OR_UPDATE:
                tileData_[event.entity.id] = event.data;
                addTileRequest(event.entity);

                break;
            case TileEventType::SET_VISIBLE: {
                auto it = tileData_.find(event.entity.id);
                if (it != tileData_.end() && it->second.visible != event.visible) {
                    it->second.visible = event.visible;
                    addTileRequest(event.entity);
                }

                break;
            }
            case TileEventType::DESTROY:
                tileData_.erase(event.entity.id);
                tileRequestIds.erase(event.entity.id);

                {
                    std::lock_guard<std::mutex> lock(residencyMutex_);
                    residentEntities_.erase(event.entity.id);
                }

                queuedEntities_.erase(event.entity.id);
                EvictTile(event.entity);

                break;
        }
    }

    for (const auto& entity : tileRequests) {
        if (tileRequestIds.find(entity.id) != tileRequestIds.end() && FindTileData(entity)) {
            EnqueueTileRequest(entity);
        }
    }

    for (const auto& entity : destroyEvents) {
        tileData_.erase(entity.id);
        {
            std::lock_guard<std::mutex> lock(residencyMutex_);
            residentEntities_.erase(entity.id);
        }

        queuedEntities_.erase(entity.id);
        EvictTile(entity);
    }

    for (const auto& entity : requestEvents) {
        EnqueueTileRequest(entity);
    }
}

bool GsTilingSystem::HasOutstandingWork() const
{
    return !pendingLoads_.empty() || !readyToCommit_.empty() || requestQueueHead_ < requestQueue_.size();
}

bool GsTilingSystem::WorkerUpdate()
{
    if (!active_.load(std::memory_order_acquire)) {
        ProcessWorkerEvents();
        return false;
    }

    ProcessWorkerEvents();

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (!dataStoreGs_) {
            return false;
        }
    }

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsTilingSystem::WorkerUpdate", "", 0x00FF88);

    uint64_t versionBefore = 0u;
    if (GsTilePool* pool = dataStoreGs_->GetTilePool()) {
        versionBefore = pool->resourceVersion;
    }

    QueueExistingTileRequests();
    RemoveDeadTilesAndSyncVisibility();
    ProcessPendingLoads();
    SubmitNewTileLoads();
    {
        std::lock_guard<std::mutex> lock(sogCacheMutex_);
        sogCache_.EvictUnreferenced();
    }

    uint64_t versionAfter = versionBefore;
    if (GsTilePool* pool = dataStoreGs_->GetTilePool()) {
        versionAfter = pool->resourceVersion;
    }

    if (versionAfter != versionBefore) {
        wantsRender_.store(true, std::memory_order_release);
    }

    return HasOutstandingWork();
}

void GsTilingSystem::Initialize()
{
    if (gsConfigManager_) {
        ecs_.AddListener(*gsConfigManager_, *this);
    }

    if (auto nodeSsytem = GetSystem<CORE3D_NS::INodeSystem>(ecs_)) {
        if (auto node = nodeSsytem->CreateNode()) {
            poolEntity_ = node->GetEntity();
        }
    }
    if (!CORE_NS::EntityUtil::IsValid(poolEntity_)) {
        poolEntity_ = ecs_.GetEntityManager().Create();
    }

    StartWorker();
}

void GsTilingSystem::Uninitialize()
{
    StopWorker();

    for (auto& p : pendingLoads_) {
        p.result->cancel.store(true);
        if (p.taskResult) {
            p.taskResult->Wait();
        }
    }
    pendingLoads_.clear();
    pendingEntities_.clear();
    requestQueue_.clear();
    queuedEntities_.clear();
    requestQueueHead_ = 0u;
    initialTileRequestsQueued_ = false;

    {
        std::lock_guard<std::mutex> lock(sogCacheMutex_);
        sogCache_.Clear();
    }

    if (EntityUtil::IsValid(poolEntity_) && poolEntityRegistered_ && dataStoreGs_) {
        auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());

        if (internalDs) {
            internalDs->DisableSortFlag();
            internalDs->RemoveGsPrimitiveIfPresent(poolEntity_);
            internalDs->ResetTiledRenderData();
            internalDs->EnableSortFlag();
        }
    }

    tileSlots_.clear();
    visibleSplatSum_ = 0u;
    if (!readyToCommit_.empty()) {
        ReleasePendingTileResources(readyToCommit_);
        readyToCommit_.clear();
    }

    DrainInFlightLoaders();
    UnmapSplatPool();

    pendingLoads_.clear();
    pendingEntities_.clear();
    requestQueue_.clear();
    queuedEntities_.clear();
    requestQueueHead_ = 0u;
    initialTileRequestsQueued_ = false;
    freeSplatRanges_.clear();
    poolInitialized_ = false;
    poolEntityRegistered_ = false;
    nextSplatOffset_ = 0u;
    splatPoolCapacity_ = 0u;
    maxPoolTiles_ = 0u;

    tileData_.clear();

    {
        std::lock_guard<std::mutex> lock(residencyMutex_);
        residentEntities_.clear();
    }

    if (EntityUtil::IsValid(poolEntity_)) {
        ecs_.GetEntityManager().Destroy(poolEntity_);
    }

    if (gsConfigManager_) {
        ecs_.RemoveListener(*gsConfigManager_, *this);
    }
    gsConfigEntity_ = {};

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        workerTileEvents_.clear();
        workerRequestEvents_.clear();
        workerDestroyEvents_.clear();
        workerCreatedConfigEntity_ = {};
        workerHasCreatedConfigEntity_ = false;
        workerConfigDestroyed_ = false;
        workerWakeCount_ = 0u;
    }
}

const IEcs& GsTilingSystem::GetECS() const
{
    return ecs_;
}

void GsTilingSystem::CreateTile(const CORE_NS::Entity entity, const GS::GsTilingComponent& data)
{
    if (!CORE_NS::EntityUtil::IsValid(entity)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(residencyMutex_);
        residentEntities_.erase(entity.id);
    }

    TileEvent event;
    event.type = TileEventType::CREATE_OR_UPDATE;
    event.entity = entity;
    event.data = data;

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (workerStopRequested_) {
            return;
        }

        workerTileEvents_.push_back(BASE_NS::move(event));
    }

    workerCv_.notify_one();
}

void GsTilingSystem::SetTileVisible(const CORE_NS::Entity entity, const bool visible)
{
    if (!CORE_NS::EntityUtil::IsValid(entity)) {
        return;
    }

    TileEvent event;
    event.type = TileEventType::SET_VISIBLE;
    event.entity = entity;
    event.visible = visible;

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (workerStopRequested_) {
            return;
        }

        workerTileEvents_.push_back(BASE_NS::move(event));
    }

    workerCv_.notify_one();
}

void GsTilingSystem::DestroyTile(const CORE_NS::Entity entity)
{
    if (!CORE_NS::EntityUtil::IsValid(entity)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(residencyMutex_);
        residentEntities_.erase(entity.id);
    }

    TileEvent event;
    event.type = TileEventType::DESTROY;
    event.entity = entity;

    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        if (workerStopRequested_) {
            return;
        }

        workerTileEvents_.push_back(BASE_NS::move(event));
    }

    workerCv_.notify_one();
}

bool GsTilingSystem::IsTileResident(const CORE_NS::Entity entity) const
{
    return ReadTileResidency(entity);
}

const GS::GsTilingComponent* GsTilingSystem::FindTileData(const CORE_NS::Entity entity) const
{
    const auto it = tileData_.find(entity.id);
    if (it == tileData_.end()) {
        return nullptr;
    }

    return &it->second;
}

bool GsTilingSystem::ReadTileData(CORE_NS::Entity entity, GS::GsTilingComponent& out) const
{
    const auto* data = FindTileData(entity);
    if (!data) {
        return false;
    }

    out = *data;
    return true;
}

bool GsTilingSystem::ReadTileResidency(const CORE_NS::Entity entity) const
{
    std::lock_guard<std::mutex> lock(residencyMutex_);
    return residentEntities_.find(entity.id) != residentEntities_.end();
}

void GsTilingSystem::SetTileResidency(
    const CORE_NS::Entity entity, const bool resident, const uint32_t numResidentSplats)
{
    bool haveData = false;
    auto it = tileData_.find(entity.id);
    if (it != tileData_.end()) {
        it->second.isResident = resident;
        it->second.numResidentSplats = numResidentSplats;
        haveData = true;
    }

    std::lock_guard<std::mutex> lock(residencyMutex_);
    if (resident && haveData) {
        residentEntities_.insert(entity.id);
    } else {
        residentEntities_.erase(entity.id);
    }
}

void GsTilingSystem::OnComponentEvent(
    EventType type, const IComponentManager& componentManager, array_view<const Entity> entities)
{
    if (!gsConfigManager_ || componentManager.GetName() != gsConfigManager_->GetName()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(workerMutex_);

        switch (type) {
            case EventType::CREATED:
                if (!entities.empty()) {
                    workerCreatedConfigEntity_ = entities.at(0);
                    workerHasCreatedConfigEntity_ = true;
                }
                break;
            case EventType::DESTROYED:
                workerConfigDestroyed_ = true;
                break;
            case EventType::MODIFIED:
            case EventType::MOVED:
                break;
        }
    }

    workerCv_.notify_one();
}

GsPrimitive* GsTilingSystem::FindPoolPrimitive()
{
    if (!dataStoreGs_) {
        return nullptr;
    }
    for (auto& prim : dataStoreGs_->GetGsPrimitives()) {
        if (prim.splatEntity == poolEntity_) {
            return &prim;
        }
    }
    return nullptr;
}

void GsTilingSystem::UpdateGsConfigCache()
{
    if (!gsConfigManager_) {
        std::lock_guard<std::mutex> lock(gsConfigCacheMutex_);
        cachedGsConfig_.reset();
        return;
    }

    if (gsConfigManager_->GetComponentCount() == 0u) {
        std::lock_guard<std::mutex> lock(gsConfigCacheMutex_);
        cachedGsConfig_.reset();
        return;
    }

    const auto entity = gsConfigManager_->GetEntity(0u);
    if (!CORE_NS::EntityUtil::IsValid(entity)) {
        return;
    }

    const auto handle = gsConfigManager_->Read(entity);
    if (!handle) {
        return;
    }

    auto snapshot = BASE_NS::make_shared<GS::GsConfigurationComponent>(*handle);

    if (snapshot->lsgsLevel != lastAppliedQualityLevel_) {
        lastAppliedQualityLevel_ = snapshot->lsgsLevel;
        const auto preset = GS::GetLsgsLevelPreset(snapshot->lsgsLevel);
        gpuPoolBytesBudget_.store(preset.gpuPoolBytes, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> sogLock(sogCacheMutex_);
            sogCache_.SetBytesBudget(preset.sogCacheBytes);
        }
        CORE_LOG_I("LSGS: GsTilingSystem: quality level %u applied, sog cache budget %llu MB, "
                    "gpu pool budget %llu MB",
            snapshot->lsgsLevel,
            static_cast<unsigned long long>(preset.sogCacheBytes / (1024u * 1024u)),
            static_cast<unsigned long long>(preset.gpuPoolBytes / (1024u * 1024u)));
    }

    std::lock_guard<std::mutex> lock(gsConfigCacheMutex_);
    cachedGsConfig_ = BASE_NS::move(snapshot);
}

bool GsTilingSystem::TryEvictNonCriticalTile(uint32_t requiredCapacity)
{
    if (tileSlots_.empty()) {
        return false;
    }

    const uint64_t now = NowUs();

    std::unordered_map<uint32_t, uint32_t> freeRangeStart;
    std::unordered_map<uint32_t, uint32_t> freeRangeEnd;
    if (requiredCapacity > 0u) {
        freeRangeStart.reserve(freeSplatRanges_.size() + 1u);
        freeRangeEnd.reserve(freeSplatRanges_.size() + 1u);
        for (const auto& r : freeSplatRanges_) {
            const uint32_t rangeEnd = r.offset + r.count;
            freeRangeStart[r.offset] = rangeEnd;
            freeRangeEnd[rangeEnd] = r.offset;
        }
        if (nextSplatOffset_ < splatPoolCapacity_) {
            freeRangeStart[nextSplatOffset_] = splatPoolCapacity_;
            freeRangeEnd[splatPoolCapacity_] = nextSplatOffset_;
        }
    }

    const auto computePotential = [&freeRangeStart, &freeRangeEnd](const TileEntry& entry) -> uint32_t {
        const uint32_t tileEnd = entry.slot.poolOffset + entry.slot.poolCapacity;
        uint32_t spanStart = entry.slot.poolOffset;
        uint32_t spanEnd = tileEnd;
        if (const auto it = freeRangeEnd.find(entry.slot.poolOffset); it != freeRangeEnd.end()) {
            spanStart = it->second;
        }
        if (const auto it = freeRangeStart.find(tileEnd); it != freeRangeStart.end()) {
            spanEnd = it->second;
        }
        return spanEnd - spanStart;
    };

    const auto findVictim = [this, requiredCapacity, now, &computePotential](bool respectGrace) -> size_t {
        size_t bestSatisfying = tileSlots_.size();
        uint64_t bestSatisfyingTouched = std::numeric_limits<uint64_t>::max();
        size_t bestFallback = tileSlots_.size();
        uint32_t bestFallbackPotential = 0u;
        uint64_t bestFallbackTouched = std::numeric_limits<uint64_t>::max();

        for (size_t ii = 0; ii < tileSlots_.size(); ii++) {
            const TileEntry& entry = tileSlots_[ii];

            if (entry.slot.poolSlot < 0) {
                continue;
            }
            if (entry.slot.visible) {
                continue;
            }
            if (pendingVisibleSnapshot_.find(entry.entity.id) != pendingVisibleSnapshot_.end()) {
                continue;
            }
            const auto* comp = FindTileData(entry.entity);
            if (comp && (comp->visible || comp->preload)) {
                continue;
            }
            if (respectGrace && now < entry.slot.touchedAtUs + kEvictGracePeriodUs) {
                continue;
            }

            if (requiredCapacity > 0u) {
                const uint32_t potential = computePotential(entry);
                if (potential >= requiredCapacity) {
                    if (entry.slot.touchedAtUs < bestSatisfyingTouched) {
                        bestSatisfyingTouched = entry.slot.touchedAtUs;
                        bestSatisfying = ii;
                    }

                    continue;
                }

                if (potential > bestFallbackPotential ||
                    (potential == bestFallbackPotential && entry.slot.touchedAtUs < bestFallbackTouched)) {
                    bestFallbackPotential = potential;
                    bestFallbackTouched = entry.slot.touchedAtUs;
                    bestFallback = ii;
                }
            } else if (entry.slot.touchedAtUs < bestFallbackTouched) {
                bestFallbackTouched = entry.slot.touchedAtUs;
                bestFallback = ii;
            }
        }

        return bestSatisfying < tileSlots_.size() ? bestSatisfying : bestFallback;
    };

    size_t victim = findVictim(true);
    if (victim >= tileSlots_.size()) {
        victim = findVictim(false);
    }
    if (victim >= tileSlots_.size()) {
        return false;
    }

    const TileEntry& entry = tileSlots_[victim];

    if constexpr (kLogLoadingDebug) {
        CORE_LOG_D("GsTilingSystem: evicting invisible tile '%s' (slot %d) to free pool space", entry.slot.uri.c_str(),
            entry.slot.poolSlot);
    }

    ReleaseTileEntry(entry);

    if (victim + 1 != tileSlots_.size()) {
        tileSlots_[victim] = BASE_NS::move(tileSlots_.back());
    }
    tileSlots_.pop_back();
    return true;
}

void GsTilingSystem::EvictForBatch(uint32_t neededSlots, uint64_t neededSplatCapacity)
{
    GsTilePool* pool = dataStoreGs_ ? dataStoreGs_->GetTilePool() : nullptr;
    if (!pool) {
        return;
    }

    uint32_t availSlots = static_cast<uint32_t>(pool->freeSlots.size());
    uint64_t availSplatCapacity =
        (splatPoolCapacity_ > nextSplatOffset_) ? (splatPoolCapacity_ - nextSplatOffset_) : 0u;
    for (const auto& range : freeSplatRanges_) {
        availSplatCapacity += range.count;
    }

    if (availSlots >= neededSlots && availSplatCapacity >= neededSplatCapacity) {
        return;
    }

    const uint64_t now = NowUs();

    BASE_NS::vector<size_t> evictable;
    evictable.reserve(tileSlots_.size());

    for (size_t ii = 0u; ii < tileSlots_.size(); ii++) {
        const TileEntry& entry = tileSlots_[ii];
        if (entry.slot.poolSlot < 0 || entry.slot.visible) {
            continue;
        }

        if (pendingVisibleSnapshot_.find(entry.entity.id) != pendingVisibleSnapshot_.end()) {
            continue;
        }

        const auto* comp = FindTileData(entry.entity);
        if (comp && (comp->visible || comp->preload)) {
            continue;
        }

        evictable.push_back(ii);
    }

    std::sort(evictable.begin(), evictable.end(), [this, now](const size_t a, const size_t b) {
        const bool graceA = now < tileSlots_[a].slot.touchedAtUs + kEvictGracePeriodUs;
        const bool graceB = now < tileSlots_[b].slot.touchedAtUs + kEvictGracePeriodUs;

        if (graceA != graceB) {
            return !graceA;
        }

        if (tileSlots_[a].slot.splatCount != tileSlots_[b].slot.splatCount) {
            return tileSlots_[a].slot.splatCount > tileSlots_[b].slot.splatCount;
        }

        return tileSlots_[a].slot.touchedAtUs < tileSlots_[b].slot.touchedAtUs;
    });

    std::vector<uint8_t> removeMask(tileSlots_.size(), 0u);
    BASE_NS::vector<int32_t> freedSlots;
    freedSlots.reserve(evictable.size());

    for (const size_t idx : evictable) {
        if (availSlots >= neededSlots && availSplatCapacity >= neededSplatCapacity) {
            break;
        }

        const uint32_t freedCapacity = tileSlots_[idx].slot.poolCapacity;
        freedSlots.push_back(ReleaseTileEntryResources(tileSlots_[idx]));
        removeMask[idx] = 1u;
        availSlots += 1u;
        availSplatCapacity += freedCapacity;
    }

    if (freedSlots.empty()) {
        return;
    }

    if (dataStoreGs_) {
        auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
        auto lg = internalDs->GetDataLockGuard();
        if (GsTilePool* lockedPool = dataStoreGs_->GetTilePool()) {
            GsPrimitive* poolPrim = FindPoolPrimitive();

            for (const int32_t slot : freedSlots) {
                FreePoolSlotLocked(slot, *lockedPool, poolPrim);
            }
        }
    }

    size_t writeIdx = 0u;
    for (size_t readIdx = 0u; readIdx < tileSlots_.size(); readIdx++) {
        if (removeMask[readIdx]) {
            continue;
        }

        if (writeIdx != readIdx) {
            tileSlots_[writeIdx] = BASE_NS::move(tileSlots_[readIdx]);
        }
        writeIdx++;
    }
    tileSlots_.resize(writeIdx);
}

void GsTilingSystem::LogPoolFragmentation(const char* context) const
{
    if constexpr (!kLogPoolFragmentation) {
        return;
    }

    uint64_t freeListBytes = 0u;
    uint32_t largestFreeRange = 0u;
    for (const auto& range : freeSplatRanges_) {
        freeListBytes += range.count;
        if (range.count > largestFreeRange) {
            largestFreeRange = range.count;
        }
    }

    const uint32_t tailFree = (splatPoolCapacity_ > nextSplatOffset_) ? (splatPoolCapacity_ - nextSplatOffset_) : 0u;
    const uint32_t largestContiguous = Math::max(largestFreeRange, tailFree);
    const uint64_t totalFree = freeListBytes + tailFree;
    const uint64_t allocated =
        (static_cast<uint64_t>(nextSplatOffset_) > freeListBytes) ? (nextSplatOffset_ - freeListBytes) : 0u;
    const double fragRatio =
        (totalFree > 0u) ? (1.0 - static_cast<double>(largestContiguous) / static_cast<double>(totalFree)) : 0.0;
    const double utilization =
        (splatPoolCapacity_ > 0u) ? (static_cast<double>(allocated) / static_cast<double>(splatPoolCapacity_)) : 0.0;

    CORE_LOG_E("GsTilingSystem pool[%s]: cap=%u alloc=%llu (%.1f%%) free=%llu (list=%llu/%zu ranges, tail=%u) "
               "largestContig=%u frag=%.2f",
        context, splatPoolCapacity_, static_cast<unsigned long long>(allocated), utilization * 100.0,
        static_cast<unsigned long long>(totalFree), static_cast<unsigned long long>(freeListBytes),
        freeSplatRanges_.size(), tailFree, largestContiguous, fragRatio);
}

void GsTilingSystem::InitializeLoadContext(GsRenderData& rd, GsLoadContext& ctx)
{
    ctx.renderContext_ = renderContext_;
    ctx.dsStaging_ = dsStaging_;
    ctx.dataStoreGs_ = dataStoreGs_;
    ctx.multiGsBufferIndexCounter = multiGsBufferIndexCounter_;
    ctx.uniqueName = "GsTiling";
    ctx.tempRenderData = &rd;

    BASE_NS::shared_ptr<GsConfigurationComponent> cfg;
    {
        std::lock_guard<std::mutex> lock(gsConfigCacheMutex_);
        cfg = cachedGsConfig_;
    }

    if (cfg && dataStoreGs_) {
        ctx.gsConfigComponent_ = cfg;
        rd.convertGsColorSrgb = cfg->convertGsColorSrgb;

        static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())
            ->SetRenderCovarianceStddev(cfg->renderCovarianceStddev);

        static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get())
            ->SetRenderSharpeningCoefficient(cfg->renderSharpeningCoefficient);
    }
}

void GsTilingSystem::ApplyInitialPoolConfig()
{
    maxPoolTiles_ = kDefaultMaxPoolTiles;
    splatsPerTile_ = 1u;

    const uint64_t bytesPerSplat = sizeof(GsObjectLoadData::GsSplatData) + 2u * (sizeof(uint32_t) * 2u) +
                                   sizeof(Math::UVec4) + sizeof(uint32_t) + sizeof(float);
    const uint64_t splatCapacity64 = gpuPoolBytesBudget_.load(std::memory_order_relaxed) / bytesPerSplat;
    CORE_LOG_I("LSGS: ApplyInitialPoolConfig, lastAppliedQualityLevel_ %u, gpuPoolBytesBudget_ %" PRIu64 ", "
                "splatCapacity64 %" PRIu64 ", bytesPerSplat %" PRIu64,
            lastAppliedQualityLevel_,
            gpuPoolBytesBudget_.load(std::memory_order_relaxed),
            splatCapacity64,
            bytesPerSplat);
    splatPoolCapacity_ = splatCapacity64 > std::numeric_limits<uint32_t>::max()
                             ? std::numeric_limits<uint32_t>::max()
                             : static_cast<uint32_t>(splatCapacity64);
    splatPoolCapacity_ = Math::max(splatPoolCapacity_, 1024u);
    sortPoolCapacity_ = Math::min(kInitialSortCapacity, splatPoolCapacity_);
    nextSplatOffset_ = 0u;
    freeSplatRanges_.clear();

    CORE_LOG_I("GsTilingSystem: pool initialized to %u slots, %u splats.", maxPoolTiles_, splatPoolCapacity_);

    dataStoreGs_->SetTilePoolConfig(maxPoolTiles_, splatsPerTile_);
    if (GsTilePool* pool = dataStoreGs_->GetTilePool()) {
        pool->splatCapacity = splatPoolCapacity_;
        pool->sortCapacity = sortPoolCapacity_;
    }
}

void GsTilingSystem::RegisterPoolPrimitive(RenderDataStoreDefaultGS& internalDs)
{
    dataStoreGs_->DisableSortFlag();
    GsPrimitive poolPrim;
    poolPrim.splatEntity = poolEntity_;
    poolPrim.numGs = 0u;
    poolPrim.gsBufferSize = 0u;
    poolPrim.isValid = true;
    poolPrim.active = true;
    poolPrim.codecVersion = 1u;
    poolPrim.cropBoxProperties.cropBoxChanged = false;
    dataStoreGs_->AddGsPrimitive(BASE_NS::move(poolPrim));
    poolEntityRegistered_ = true;

    internalDs.FlipValidGsRenderData();
    dataStoreGs_->EnableSortFlag();
}

void GsTilingSystem::InitializePool()
{
    CORE_LOG_I("LSGS: PrepareSubmissions, InitializePool %u", lastAppliedQualityLevel_);
    if (poolInitialized_ || !dataStoreGs_ || !renderContext_ || !dsStaging_) {
        return;
    }

    ApplyInitialPoolConfig();

    auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
    GsRenderData* loadRd = internalDs->GetSafeLoadingGsRenderData();
    if (!loadRd) {
        return;
    }
    loadRd->isTiled = true;

    GsLoadContext ctx;
    InitializeLoadContext(*loadRd, ctx);
    CreateGsRenderDataBuffers(loadRd, 0u, 0u, 0u, ctx);
    RegisterPoolPrimitive(*internalDs);
    EnsureSplatPoolMapped();

    poolInitialized_ = true;
}

bool GsTilingSystem::ValidateSplatPoolTarget(uint32_t targetSplatCapacity) const
{
    const uint64_t splatBytes = static_cast<uint64_t>(targetSplatCapacity) * sizeof(GsObjectLoadData::GsSplatData);
    const uint64_t sortBytes = static_cast<uint64_t>(targetSplatCapacity) * (2u * (sizeof(uint32_t) * 2u));
    const uint64_t cacheBytes = static_cast<uint64_t>(targetSplatCapacity) * sizeof(Math::UVec4);
    const uint64_t indexBytes = static_cast<uint64_t>(targetSplatCapacity) * sizeof(uint32_t);
    const uint64_t viewZBytes = static_cast<uint64_t>(targetSplatCapacity) * sizeof(float);
    if (splatBytes > std::numeric_limits<uint32_t>::max() || sortBytes > std::numeric_limits<uint32_t>::max() ||
        cacheBytes > std::numeric_limits<uint32_t>::max() || indexBytes > std::numeric_limits<uint32_t>::max() ||
        viewZBytes > std::numeric_limits<uint32_t>::max()) {
        CORE_LOG_E("GsTilingSystem: tile pool cannot grow to %u splats, GPU alloc limit reached", targetSplatCapacity);
        return false;
    }
    if (targetSplatCapacity > splatPoolCapacity_ &&
        splatBytes + sortBytes + cacheBytes + indexBytes + viewZBytes > gpuPoolBytesBudget_.load(
                                                                       std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

void GsTilingSystem::ResizeTilePool(GsTilePool& pool, uint32_t targetSlots, uint32_t targetSplatCapacity)
{
    auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
    auto lg = internalDs->GetDataLockGuard();
    if (targetSlots > pool.maxTiles) {
        pool.freeSlots.reserve(targetSlots);
        for (uint32_t slot = pool.maxTiles; slot < targetSlots; slot++) {
            pool.freeSlots.push_back(static_cast<int32_t>(slot));
        }
        pool.maxTiles = targetSlots;
    }

    pool.splatCapacity = targetSplatCapacity;
    pool.sortCapacity = Math::min(pool.sortCapacity, targetSplatCapacity);
    pool.resourceVersion++;
}

bool GsTilingSystem::EnsurePoolCapacity(uint32_t minSlots, uint32_t minSplatCapacity)
{
    if (!dataStoreGs_ || !renderContext_ || !dsStaging_) {
        return false;
    }

    GsTilePool* pool = dataStoreGs_->GetTilePool();
    if (!pool) {
        return false;
    }

    const uint32_t targetSlots = minSlots > maxPoolTiles_ ? NextPow2(minSlots) : maxPoolTiles_;
    const uint32_t targetSplatCapacity =
        minSplatCapacity > splatPoolCapacity_ ? NextPow2(minSplatCapacity) : splatPoolCapacity_;

    if (targetSlots == maxPoolTiles_ && targetSplatCapacity == splatPoolCapacity_) {
        return true;
    }

    if (!ValidateSplatPoolTarget(targetSplatCapacity)) {
        return false;
    }

    const bool needBufferRecreate = targetSplatCapacity > splatPoolCapacity_;
    if (needBufferRecreate) {
        DrainInFlightLoaders();
        UnmapSplatPool();
    }

    ResizeTilePool(*pool, targetSlots, targetSplatCapacity);

    if (!needBufferRecreate) {
        maxPoolTiles_ = targetSlots;
        return true;
    }

    GsRenderData* rd = dataStoreGs_->GetGsRenderData();
    if (!rd) {
        return false;
    }
    rd->isTiled = true;

    GsLoadContext ctx;
    InitializeLoadContext(*rd, ctx);
    CreateGsRenderDataBuffers(rd, 0u, 0u, 0u, ctx);

    maxPoolTiles_ = targetSlots;
    splatPoolCapacity_ = targetSplatCapacity;
    return EnsureSplatPoolMapped() != nullptr;
}

bool GsTilingSystem::UpdateSortCapacityLocked()
{
    if (!dataStoreGs_) {
        return false;
    }

    GsTilePool* pool = dataStoreGs_->GetTilePool();
    if (!pool) {
        return false;
    }

    const uint64_t visibleSplats = visibleSplatSum_;
    if (visibleSplats == 0u) {
        return false;
    }

    const uint64_t target64 = (visibleSplats * kSortCapacityMarginNum) / kSortCapacityMarginDen;
    uint32_t target = NextPow2(target64);
    target = Math::max(target, kInitialSortCapacity);
    target = Math::min(target, splatPoolCapacity_);

    if (target <= sortPoolCapacity_ && target <= pool->allocatedSortCapacity) {
        return false;
    }

    pool->sortCapacity = target;
    pool->resourceVersion++;
    sortPoolCapacity_ = target;

    return true;
}

void GsTilingSystem::RecreateSortCapacityBuffers()
{
    if (!dataStoreGs_ || !renderContext_ || !dsStaging_) {
        return;
    }

    GsRenderData* rd = dataStoreGs_->GetGsRenderData();
    if (!rd) {
        return;
    }
    rd->isTiled = true;

    GsLoadContext ctx;
    InitializeLoadContext(*rd, ctx);
    CreateGsRenderDataBuffers(rd, 0u, 0u, 0u, ctx);
}

bool GsTilingSystem::AllocateSplatRange(uint32_t count, uint32_t& offset, uint32_t& capacity)
{
    capacity = AlignSplatCapacity(count);

    const auto tryFreeList = [this, capacity, &offset]() {
        auto best = freeSplatRanges_.end();
        for (auto it = freeSplatRanges_.begin(); it != freeSplatRanges_.end(); ++it) {
            if (it->count < capacity) {
                continue;
            }
            if (best == freeSplatRanges_.end() || it->count < best->count) {
                best = it;
            }
        }
        if (best == freeSplatRanges_.end()) {
            return false;
        }
        offset = best->offset;
        best->offset += capacity;
        best->count -= capacity;
        if (best->count == 0u) {
            freeSplatRanges_.erase(best);
        }
        return true;
    };

    const auto tryTail = [this, capacity, &offset]() {
        const uint64_t requiredEnd = static_cast<uint64_t>(nextSplatOffset_) + capacity;
        if (requiredEnd > std::numeric_limits<uint32_t>::max()) {
            return false;
        }
        if (requiredEnd > splatPoolCapacity_ &&
            !EnsurePoolCapacity(maxPoolTiles_, static_cast<uint32_t>(requiredEnd))) {
            return false;
        }
        offset = nextSplatOffset_;
        nextSplatOffset_ += capacity;
        return true;
    };

    if (tryFreeList()) {
        return true;
    }
    if (tryTail()) {
        return true;
    }

    return false;
}

void GsTilingSystem::ReleaseSplatRange(uint32_t offset, uint32_t capacity)
{
    if (capacity == 0u) {
        return;
    }

    FreeSplatRange released { offset, capacity };
    auto it = std::lower_bound(freeSplatRanges_.begin(), freeSplatRanges_.end(), released,
        [](const FreeSplatRange& a, const FreeSplatRange& b) { return a.offset < b.offset; });

    if (it != freeSplatRanges_.begin()) {
        auto prev = it - 1;
        const uint64_t prevEnd = static_cast<uint64_t>(prev->offset) + prev->count;
        if (prevEnd >= released.offset) {
            const uint64_t releasedEnd = static_cast<uint64_t>(released.offset) + released.count;
            prev->count = static_cast<uint32_t>(Math::max(prevEnd, releasedEnd) - prev->offset);
            it = prev;
        } else {
            it = freeSplatRanges_.insert(it, released);
        }
    } else {
        it = freeSplatRanges_.insert(it, released);
    }

    while ((it + 1) != freeSplatRanges_.end()) {
        auto next = it + 1;
        const uint64_t end = static_cast<uint64_t>(it->offset) + it->count;
        if (end < next->offset) {
            break;
        }

        const uint64_t nextEnd = static_cast<uint64_t>(next->offset) + next->count;
        it->count = static_cast<uint32_t>(Math::max(end, nextEnd) - it->offset);
        freeSplatRanges_.erase(next);
    }

    if (!freeSplatRanges_.empty()) {
        auto& last = freeSplatRanges_.back();
        const uint64_t lastEnd = static_cast<uint64_t>(last.offset) + last.count;
        if (lastEnd >= nextSplatOffset_) {
            nextSplatOffset_ = last.offset;
            freeSplatRanges_.pop_back();
        }
    }
}

uint8_t* GsTilingSystem::EnsureSplatPoolMapped()
{
    if (!dataStoreGs_ || !renderContext_) {
        return nullptr;
    }

    GsTilePool* pool = dataStoreGs_->GetTilePool();
    if (!pool || !RENDER_NS::RenderHandleUtil::IsValid(pool->splatPoolBuffer.GetHandle())) {
        return nullptr;
    }

    if (splatPoolMappedPtr_ && splatPoolMappedHandle_.GetHandle() == pool->splatPoolBuffer.GetHandle()) {
        return splatPoolMappedPtr_;
    }

    UnmapSplatPool();

    auto& gpuMgr = renderContext_->GetDevice().GetGpuResourceManager();
    void* mapped = gpuMgr.MapBufferMemory(pool->splatPoolBuffer);
    if (!mapped) {
        return nullptr;
    }

    splatPoolMappedHandle_ = pool->splatPoolBuffer;
    splatPoolMappedPtr_ = static_cast<uint8_t*>(mapped);

    return splatPoolMappedPtr_;
}

void GsTilingSystem::UnmapSplatPool()
{
    if (!splatPoolMappedPtr_) {
        splatPoolMappedHandle_ = {};
        return;
    }

    if (renderContext_ && RENDER_NS::RenderHandleUtil::IsValid(splatPoolMappedHandle_.GetHandle())) {
        renderContext_->GetDevice().GetGpuResourceManager().UnmapBuffer(splatPoolMappedHandle_);
    }

    splatPoolMappedPtr_ = nullptr;
    splatPoolMappedHandle_ = {};
}

void GsTilingSystem::DrainInFlightLoaders()
{
    for (auto& pending : pendingLoads_) {
        if (pending.taskResult && !pending.taskResult->IsDone()) {
            pending.taskResult->Wait();
        }
    }
}

bool GsTilingSystem::PrepareTileSubmission(
    CORE_NS::Entity entity, const GS::GsTilingComponent& comp, PreparedSubmission& outPrep)
{
    if (!poolInitialized_ || !threadPool_) {
        return false;
    }

    if (!dataStoreGs_->GetTilePool()) {
        return false;
    }

    outPrep.entity = entity;
    outPrep.uri = comp.uri;
    outPrep.offset = comp.offset;
    outPrep.count = comp.count;
    outPrep.poolOffset = 0u;
    outPrep.poolCapacity = 0u;
    outPrep.aabbMin = comp.aabbMin;
    outPrep.aabbMax = comp.aabbMax;
    outPrep.result = BASE_NS::make_shared<TileLoadResult>();
    outPrep.visible = comp.visible;
    outPrep.slot = -1;
    return true;
}

void GsTilingSystem::PreparePendingCache(PendingLoad& pending)
{
    const std::string uriKey(pending.uri.data(), pending.uri.size());
    std::lock_guard<std::mutex> lock(sogCacheMutex_);
    if (!pending.cacheEntry) {
        auto r = sogCache_.GetOrCreate(uriKey);
        pending.cacheEntry = r.first;
        pending.iAmDecoder = r.second;
        return;
    }

    sogCache_.Touch(uriKey);
}

void GsTilingSystem::DispatchTileTask(PendingLoad& pending, bool decodeOnly)
{
    auto res = pending.result;
    res->completed.store(false, std::memory_order_relaxed);
    res->deferred.store(false);
    res->success = false;

    TileLoadTaskContext ctx;
    ctx.res = res;
    ctx.uri = pending.uri;
    ctx.offset = pending.offset;
    ctx.count = pending.count;
    ctx.cacheEntry = pending.cacheEntry;
    ctx.iAmDecoder = pending.iAmDecoder;
    ctx.fileMgr = &renderContext_->GetEngine().GetFileManager();
    ctx.imgMgr = &renderContext_->GetEngine().GetImageLoaderManager();
    ctx.cacheMtx = &sogCacheMutex_;
    ctx.cache = &sogCache_;
    ctx.decodeOnly = decodeOnly;

    if (!decodeOnly && pending.slot >= 0 && splatPoolMappedPtr_) {
        const bool useTilePack = dataStoreGs_ && dataStoreGs_->GetGsRenderData() &&
                                 dataStoreGs_->GetGsRenderData()->CanUseTilePackedData();
        const size_t stride = useTilePack
                                  ? sizeof(GsObjectLoadData::GsSplatDataTilePacked)
                                  : sizeof(GsObjectLoadData::GsSplatData);
        const uint64_t dstOffset = static_cast<uint64_t>(pending.poolOffset) * stride;
        const uint64_t poolBytes = static_cast<uint64_t>(splatPoolCapacity_) * stride;
        if (dstOffset < poolBytes) {
            ctx.dst = splatPoolMappedPtr_ + dstOffset;
            ctx.dstAvailable = static_cast<size_t>(poolBytes - dstOffset);
            ctx.useTilePack = useTilePack;
        }
    }

    ctx.notifyFn = [](void* arg) { static_cast<GsTilingSystem*>(arg)->NotifyWorker(); };
    ctx.notifyArg = this;

    auto task = CreateFunctionTask([ctx = BASE_NS::move(ctx)]() mutable { ExecuteTileLoadTask(BASE_NS::move(ctx)); });
    pending.taskResult = threadPool_->Push(BASE_NS::move(task));
}

bool GsTilingSystem::TryDispatchAwaitingLoad(PendingLoad& pending)
{
    if (pending.cancelled) {
        return false;
    }

    GS::CachedSogData::State state =
        pending.cacheEntry ? pending.cacheEntry->state.load() : GS::CachedSogData::State::FAILED;

    if (state == GS::CachedSogData::State::LOADING) {
        return false;
    }

    if (state == GS::CachedSogData::State::FAILED) {
        if (pending.taskResult && !pending.taskResult->IsDone()) {
            return false;
        }

        pending.cacheEntry.reset();
        pending.iAmDecoder = false;
        PreparePendingCache(pending);

        GS::CachedSogData::State recheck =
            pending.cacheEntry ? pending.cacheEntry->state.load() : GS::CachedSogData::State::FAILED;
        if (pending.iAmDecoder) {
            pending.taskResult.reset();
            DispatchTileTask(pending, true);
            return true;
        }

        if (recheck != GS::CachedSogData::State::READY) {
            if (recheck == GS::CachedSogData::State::FAILED) {
                pending.cancelled = true;
                pending.result->cancel.store(true);
            }

            return false;
        }
    }

    if (pending.taskResult && !pending.taskResult->IsDone()) {
        return false;
    }

    pending.taskResult.reset();
    pending.iAmDecoder = false;
    if (!AcquirePoolResidency(pending)) {
        return false;
    }

    pending.awaitingCache = false;
    DispatchTileTask(pending, false);

    return true;
}

bool GsTilingSystem::AcquirePoolResidency(PendingLoad& pending)
{
    if (!dataStoreGs_) {
        return false;
    }

    auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());

    GsTilePool* pool = dataStoreGs_->GetTilePool();
    if (!pool || pending.count == 0u) {
        return false;
    }

    const uint32_t requiredCapacity = AlignSplatCapacity(pending.count);
    if (requiredCapacity > splatPoolCapacity_ && !EnsurePoolCapacity(maxPoolTiles_, requiredCapacity)) {
        return false;
    }

    pool = dataStoreGs_->GetTilePool();
    if (!pool) {
        return false;
    }

    while (pending.slot < 0) {
        if (!pool->freeSlots.empty()) {
            pending.slot = pool->freeSlots.back();
            pool->freeSlots.pop_back();
            break;
        }

        if (EnsurePoolCapacity(maxPoolTiles_ + 1u, splatPoolCapacity_)) {
            pool = dataStoreGs_->GetTilePool();
            if (!pool) {
                return false;
            }

            continue;
        }

        if (!TryEvictNonCriticalTile()) {
            return false;
        }
    }

    if (pending.poolCapacity == 0u) {
        uint32_t newOffset = 0u;
        uint32_t newCapacity = 0u;
        bool loggedFragmentation = false;
        while (!AllocateSplatRange(pending.count, newOffset, newCapacity)) {
            if (!loggedFragmentation) {
                LogPoolFragmentation("alloc-fail-pre-evict");
                loggedFragmentation = true;
            }

            if (!TryEvictNonCriticalTile(AlignSplatCapacity(pending.count))) {
                LogPoolFragmentation("alloc-fail-no-victim");
                if (pending.slot >= 0) {
                    pool = dataStoreGs_->GetTilePool();
                    if (pool) {
                        pool->freeSlots.push_back(pending.slot);
                    }
                    pending.slot = -1;
                }
                return false;
            }
        }

        if (loggedFragmentation) {
            LogPoolFragmentation("alloc-ok-post-evict");
        }

        pending.poolOffset = newOffset;
        pending.poolCapacity = newCapacity;
    }

    if (!EnsureSplatPoolMapped()) {
        if (pending.poolCapacity > 0u) {
            ReleaseSplatRange(pending.poolOffset, pending.poolCapacity);
            pending.poolOffset = 0u;
            pending.poolCapacity = 0u;
        }

        if (pending.slot >= 0) {
            pool = dataStoreGs_->GetTilePool();
            if (pool) {
                pool->freeSlots.push_back(pending.slot);
            }
            pending.slot = -1;
        }

        return false;
    }

    return true;
}

bool GsTilingSystem::DispatchPreparedSubmission(PreparedSubmission& prep)
{
    PendingLoad pending;
    pending.entity = prep.entity;
    pending.uri = prep.uri;
    pending.slot = prep.slot;
    pending.poolOffset = prep.poolOffset;
    pending.poolCapacity = prep.poolCapacity;
    pending.result = prep.result;
    pending.offset = prep.offset;
    pending.count = prep.count;
    pending.visible = prep.visible;
    pending.requestVisible = prep.visible;
    pending.aabbMin = prep.aabbMin;
    pending.aabbMax = prep.aabbMax;

    {
        const std::string uriKey(pending.uri.data(), pending.uri.size());
        std::lock_guard<std::mutex> lock(sogCacheMutex_);
        auto acquired = sogCache_.GetOrCreateGated(uriKey, maxConcurrentDecodes_, pending.visible);
        if (!acquired.accepted) {
            return false;
        }

        pending.cacheEntry = acquired.data;
        pending.iAmDecoder = acquired.isDecoder;
    }

    const auto cacheState = pending.cacheEntry ? pending.cacheEntry->state.load() : GS::CachedSogData::State::FAILED;
    if (pending.iAmDecoder) {
        pending.awaitingCache = true;
        DispatchTileTask(pending, true);
    } else if (cacheState == GS::CachedSogData::State::READY) {
        if (!AcquirePoolResidency(pending)) {
            return false;
        }

        pending.awaitingCache = false;
        DispatchTileTask(pending, false);
    } else {
        pending.awaitingCache = true;
    }

    pendingEntities_.insert(prep.entity.id);
    pendingLoads_.push_back(BASE_NS::move(pending));

    if constexpr (kLogLoadingDebug) {
        CORE_LOG_D("GsTilingSystem: submitted async load for tile '%s'", prep.uri.c_str());
    }
    return true;
}

void GsTilingSystem::EvictTile(CORE_NS::Entity entity)
{
    bool anyPendingForEntity = false;
    for (auto& p : pendingLoads_) {
        if (p.entity == entity && !p.cancelled) {
            p.cancelled = true;
            p.result->cancel.store(true);
            anyPendingForEntity = true;
        }
    }

    const auto it = std::find_if(
        tileSlots_.begin(), tileSlots_.end(), [&entity](const TileEntry& e) { return e.entity == entity; });
    if (it == tileSlots_.end()) {
        if constexpr (kLogLoadingDebug) {
            if (!anyPendingForEntity) {
                CORE_LOG_D("GsTilingSystem: EvictTile: entity not tracked, nothing to do");
            }
        }
        return;
    }

    if constexpr (kLogLoadingDebug) {
        CORE_LOG_D("GsTilingSystem: evicting tile '%s' from pool slot %d", it->slot.uri.c_str(), it->slot.poolSlot);
    }

    ReleaseTileEntry(*it);

    if (it->slot.visible) {
        const uint64_t c = AlignedSlotSplatContribution(it->slot.splatCount);
        visibleSplatSum_ = (visibleSplatSum_ >= c) ? visibleSplatSum_ - c : 0u;
    }

    if ((it + 1) != tileSlots_.end()) {
        *it = BASE_NS::move(tileSlots_.back());
    }
    tileSlots_.pop_back();
}

int32_t GsTilingSystem::ReleaseTileEntryResources(const TileEntry& entry)
{
    ReleaseSplatRange(entry.slot.poolOffset, entry.slot.poolCapacity);

    SetTileResidency(entry.entity, false, 0u);

    if (FindTileData(entry.entity)) {
        EnqueueTileRequest(entry.entity);
    }

    return entry.slot.poolSlot;
}

void GsTilingSystem::FreePoolSlotLocked(int32_t slot, GsTilePool& pool, GsPrimitive* poolPrim)
{
    if (slot < 0) {
        return;
    }

    pool.freeSlots.push_back(slot);
    pool.resourceVersion++;

    MarkPoolTileUnloaded(slot, poolPrim);
}

void GsTilingSystem::ReleaseTileEntry(const TileEntry& entry)
{
    const int32_t slot = ReleaseTileEntryResources(entry);

    if (!dataStoreGs_) {
        return;
    }

    auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
    auto lg = internalDs->GetDataLockGuard();

    if (GsTilePool* pool = dataStoreGs_->GetTilePool(); pool && slot >= 0) {
        FreePoolSlotLocked(slot, *pool, FindPoolPrimitive());
    }
}

void GsTilingSystem::UpdatePendingVisibility(PendingLoad& pending)
{
    if (pending.cancelled) {
        return;
    }

    const auto* comp = FindTileData(pending.entity);
    if (!comp) {
        pending.cancelled = true;
        pending.result->cancel.store(true);
        return;
    }

    pending.visible = comp->visible;
}

bool GsTilingSystem::HandleCompletedPendingLoad(
    PendingLoad& pending, BASE_NS::vector<PendingLoad>& finalizeBatch, BASE_NS::vector<PendingLoad>& releaseBatch)
{
    if (pending.awaitingCache) {
        if (pending.cancelled) {
            pending.cacheEntry.reset();
            pendingEntities_.erase(pending.entity.id);
            releaseBatch.push_back(BASE_NS::move(pending));
            return false;
        }

        TryDispatchAwaitingLoad(pending);
        return true;
    }

    if (!pending.taskResult->IsDone()) {
        return true;
    }

    if (!pending.result->completed.load(std::memory_order_acquire)) {
        return true;
    }

    if (pending.result->deferred.load() && !pending.cancelled) {
        const auto state = pending.cacheEntry ? pending.cacheEntry->state.load() : GS::CachedSogData::State::FAILED;
        if (state == GS::CachedSogData::State::READY) {
            DispatchTileTask(pending);
            return true;
        }

        if (state != GS::CachedSogData::State::FAILED) {
            return true;
        }

    } else if (pending.result->success && !pending.cancelled) {
        pending.cacheEntry.reset();
        pending.taskResult.reset();
        pendingEntities_.erase(pending.entity.id);
        finalizeBatch.push_back(BASE_NS::move(pending));
        return false;
    } else if (!pending.result->success && !pending.cancelled) {
        CORE_LOG_E("GsTilingSystem: async load failed for tile '%s'", pending.uri.c_str());
    }

    pending.cacheEntry.reset();
    pending.taskResult.reset();
    pendingEntities_.erase(pending.entity.id);
    releaseBatch.push_back(BASE_NS::move(pending));

    return false;
}

BASE_NS::vector<PendingLoad> GsTilingSystem::CollectCompletedPendingLoads(BASE_NS::vector<PendingLoad>& releaseBatch)
{
    BASE_NS::vector<PendingLoad> finalizeBatch;
    finalizeBatch.reserve(pendingLoads_.size());

    size_t writeIdx = 0u;

    for (size_t readIdx = 0u; readIdx < pendingLoads_.size(); readIdx++) {
        PendingLoad& pending = pendingLoads_[readIdx];
        UpdatePendingVisibility(pending);
        const bool keep = HandleCompletedPendingLoad(pending, finalizeBatch, releaseBatch);
        if (keep) {
            if (writeIdx != readIdx) {
                pendingLoads_[writeIdx] = BASE_NS::move(pending);
            }
            writeIdx++;
        }
    }

    pendingLoads_.resize(writeIdx);
    return finalizeBatch;
}

void GsTilingSystem::ReleasePendingTileResources(BASE_NS::vector<PendingLoad>& releaseBatch)
{
    if (releaseBatch.empty()) {
        return;
    }

    for (auto& pending : releaseBatch) {
        ReleaseSplatRange(pending.poolOffset, pending.poolCapacity);
        pending.poolOffset = 0u;
        pending.poolCapacity = 0u;
    }

    auto* internalDs = dataStoreGs_ ? static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get()) : nullptr;
    GsTilePool* pool = dataStoreGs_ ? dataStoreGs_->GetTilePool() : nullptr;
    if (!internalDs || !pool) {
        for (auto& pending : releaseBatch) {
            pending.slot = -1;
        }
        return;
    }

    auto lg = internalDs->GetDataLockGuard();
    GsPrimitive* poolPrim = FindPoolPrimitive();
    for (auto& pending : releaseBatch) {
        if (pending.slot < 0) {
            continue;
        }
        pool->freeSlots.push_back(pending.slot);
        MarkPoolTileUnloaded(pending.slot, poolPrim);
        pending.slot = -1;
    }
}

void GsTilingSystem::MarkPoolTileUnloaded(int32_t slot, GsPrimitive* poolPrim)
{
    if (!poolPrim || slot < 0 || static_cast<size_t>(slot) >= poolPrim->tileGrid.tiles.size()) {
        return;
    }

    GsTileMetadata& tile = poolPrim->tileGrid.tiles[static_cast<size_t>(slot)];
    tile.state = GsTileState::UNLOADED;
    tile.gpuPoolSlot = -1;
    tile.gpuPoolOffset = 0u;
    tile.gpuPoolCapacity = 0u;
    tile.sortOutputValid = false;
    tile.splatCount = 0u;
}

void GsTilingSystem::ReturnReleasedPendingSlots(
    const BASE_NS::vector<PendingLoad>& releaseBatch, GsTilePool& pool, GsPrimitive* poolPrim)
{
    for (const auto& pending : releaseBatch) {
        if (pending.slot < 0) {
            continue;
        }

        pool.freeSlots.push_back(pending.slot);
        MarkPoolTileUnloaded(pending.slot, poolPrim);
    }
}

void GsTilingSystem::UpdatePoolTileMetadata(PendingLoad& pending, GsTileMetadata& tile, bool isVisible)
{
    const bool haveRequestAabb = (pending.aabbMin.x < pending.aabbMax.x) || (pending.aabbMin.y < pending.aabbMax.y) ||
                                 (pending.aabbMin.z < pending.aabbMax.z);
    const TileLoadResult& result = *pending.result;

    tile.currentRadixPass = 0u;
    tile.sortOutputValid = false;
    tile.splatCount = result.count;
    tile.gpuPoolSlot = pending.slot;
    tile.gpuPoolOffset = pending.poolOffset;
    tile.gpuPoolCapacity = pending.poolCapacity;
    tile.state = isVisible ? GsTileState::RESIDENT : GsTileState::EVICTING;
    tile.aabbMin = haveRequestAabb ? pending.aabbMin : result.aabbMin;
    tile.aabbMax = haveRequestAabb ? pending.aabbMax : result.aabbMax;
}

void GsTilingSystem::AddTileEntry(PendingLoad& pending, bool isVisible)
{
    TileEntry entry;
    entry.entity = pending.entity;
    entry.slot.uri = pending.uri;
    entry.slot.poolSlot = pending.slot;
    entry.slot.poolOffset = pending.poolOffset;
    entry.slot.poolCapacity = pending.poolCapacity;
    entry.slot.splatCount = pending.result->count;
    entry.slot.visible = isVisible;
    entry.slot.touchedAtUs = NowUs();
    if (isVisible) {
        visibleSplatSum_ += AlignedSlotSplatContribution(entry.slot.splatCount);
    }
    tileSlots_.push_back(BASE_NS::move(entry));

    if constexpr (kLogPoolCapacity) {
        // prevent log pollution
        static uint32_t counter = 0;
        if (((counter++) % 50) == 0) {
            const GsTilePool* pool = dataStoreGs_ ? dataStoreGs_->GetTilePool() : nullptr;
            const uint32_t totalSlots = pool ? pool->maxTiles : maxPoolTiles_;
            const uint32_t availableSlots = pool ? static_cast<uint32_t>(pool->freeSlots.size()) : 0u;
            const uint32_t usedSlots = totalSlots > availableSlots ? totalSlots - availableSlots : 0u;

            const uint64_t totalSplatCapacity = pool ? pool->splatCapacity : splatPoolCapacity_;
            uint64_t availableSplatCapacity =
                totalSplatCapacity > nextSplatOffset_ ? totalSplatCapacity - nextSplatOffset_ : 0u;
            for (const auto& range : freeSplatRanges_) {
                availableSplatCapacity += range.count;
            }
            if (availableSplatCapacity > totalSplatCapacity) {
                availableSplatCapacity = totalSplatCapacity;
            }
            const uint64_t usedSplatCapacity = totalSplatCapacity - availableSplatCapacity;

            constexpr uint64_t kBytesPerSplat = sizeof(GsObjectLoadData::GsSplatData) + 2u * (sizeof(uint32_t) * 2u) +
                                                sizeof(Math::UVec4) + sizeof(uint32_t) + sizeof(float);
            const double usedMB = static_cast<double>(usedSplatCapacity * kBytesPerSplat) / (1024.0 * 1024.0);
            const double totalMB =
                static_cast<double>(gpuPoolBytesBudget_.load(std::memory_order_relaxed)) / (1024.0 * 1024.0);

            CORE_LOG_E("GsTilingSystem: pool slots %u/%u, splats %llu/%llu, memory %.1f/%.1f MB", usedSlots,
                availableSlots, static_cast<unsigned long long>(usedSplatCapacity),
                static_cast<unsigned long long>(availableSplatCapacity), usedMB, totalMB);
        }
    }
}

void GsTilingSystem::CommitFinalizedPendingLoads(
    BASE_NS::vector<PendingLoad>& finalizeBatch, GsTilePool& pool, GsPrimitive* poolPrim)
{
    for (auto& pending : finalizeBatch) {
        if (pending.slot < 0) {
            ReleaseSplatRange(pending.poolOffset, pending.poolCapacity);
            continue;
        }

        bool isVisible = true;
        const auto* comp = FindTileData(pending.entity);
        if (comp) {
            isVisible = comp->visible;
            SetTileResidency(pending.entity, true, pending.result->count);
        } else {
            pool.freeSlots.push_back(pending.slot);
            MarkPoolTileUnloaded(pending.slot, poolPrim);
            ReleaseSplatRange(pending.poolOffset, pending.poolCapacity);
            continue;
        }

        if (poolPrim) {
            if (static_cast<size_t>(pending.slot + 1) > poolPrim->tileGrid.tiles.size()) {
                poolPrim->tileGrid.tiles.resize(static_cast<size_t>(pending.slot + 1));
            }
            UpdatePoolTileMetadata(pending, poolPrim->tileGrid.tiles[static_cast<size_t>(pending.slot)], isVisible);
        }
        AddTileEntry(pending, isVisible);
        if (isVisible) {
            pool.resourceVersion++;
        }
    }
}

void GsTilingSystem::CommitPendingResults(
    BASE_NS::vector<PendingLoad>& finalizeBatch, const BASE_NS::vector<PendingLoad>& releaseBatch)
{
    auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
    if (!internalDs) {
        ReleasePendingTileResources(finalizeBatch);
        return;
    }
    GsTilePool* pool = dataStoreGs_ ? dataStoreGs_->GetTilePool() : nullptr;
    if (!pool) {
        ReleasePendingTileResources(finalizeBatch);
        return;
    }

    bool sortBuffersNeedRecreate = false;
    {
        GsPrimitive* poolPrim = FindPoolPrimitive();
        ReturnReleasedPendingSlots(releaseBatch, *pool, poolPrim);
        CommitFinalizedPendingLoads(finalizeBatch, *pool, poolPrim);
        sortBuffersNeedRecreate = UpdateSortCapacityLocked();
    }

    if (sortBuffersNeedRecreate) {
        RecreateSortCapacityBuffers();
    }
}

void GsTilingSystem::ProcessPendingLoads()
{
    if (pendingLoads_.empty() && readyToCommit_.empty()) {
        return;
    }

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsTilingSystem::Update::ProcessPendingLoads", "", 0x00FF88);

    pendingVisibleSnapshot_.clear();
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        for (const auto& ev : workerTileEvents_) {
            if (ev.type == TileEventType::SET_VISIBLE && ev.visible) {
                pendingVisibleSnapshot_.insert(ev.entity.id);
            }
        }
    }

    BASE_NS::vector<PendingLoad> releaseBatch;
    releaseBatch.reserve(pendingLoads_.size());
    BASE_NS::vector<PendingLoad> finalizeBatch = CollectCompletedPendingLoads(releaseBatch);

    if (!finalizeBatch.empty()) {
        readyToCommit_.reserve(readyToCommit_.size() + finalizeBatch.size());
        for (auto& p : finalizeBatch) {
            readyToCommit_.push_back(BASE_NS::move(p));
        }
    }

    ReleasePendingTileResources(releaseBatch);

    if (!readyToCommit_.empty()) {
        BASE_NS::vector<PendingLoad> emptyRelease;
        CommitPendingResults(readyToCommit_, emptyRelease);
        readyToCommit_.clear();
    }
}

void GsTilingSystem::RemoveDeadTilesAndSyncVisibility()
{
    CORE_CPU_PERF_SCOPE("LUMEGS", "GsTilingSystem::Update::RemoveEntities", "", 0x00FF88);

    bool visibilityChanged = false;

    for (size_t ii = 0u; ii < tileSlots_.size();) {
        const auto* comp = FindTileData(tileSlots_[ii].entity);
        if (!comp) {
            if constexpr (kLogLoadingDebug) {
                CORE_LOG_D("GsTilingSystem: removing dead tile '%s' from pool slot %d", tileSlots_[ii].slot.uri.c_str(),
                    tileSlots_[ii].slot.poolSlot);
            }
            if (tileSlots_[ii].slot.visible) {
                const uint64_t c = AlignedSlotSplatContribution(tileSlots_[ii].slot.splatCount);
                visibleSplatSum_ = (visibleSplatSum_ >= c) ? visibleSplatSum_ - c : 0u;
            }
            ReleaseTileEntry(tileSlots_[ii]);

            if (ii + 1u != tileSlots_.size()) {
                tileSlots_[ii] = BASE_NS::move(tileSlots_.back());
            }

            tileSlots_.pop_back();
            continue;
        }

        if (tileSlots_[ii].slot.visible != comp->visible) {
            const uint64_t c = AlignedSlotSplatContribution(tileSlots_[ii].slot.splatCount);
            if (comp->visible) {
                visibleSplatSum_ += c;
            } else {
                visibleSplatSum_ = (visibleSplatSum_ >= c) ? visibleSplatSum_ - c : 0u;
            }
            tileSlots_[ii].slot.visible = comp->visible;
            tileSlots_[ii].slot.touchedAtUs = NowUs();
            visibilityChanged = true;
        }

        ii++;
    }

    if (!visibilityChanged || !dataStoreGs_) {
        return;
    }

    auto* internalDs = static_cast<RenderDataStoreDefaultGS*>(dataStoreGs_.get());
    if (!internalDs) {
        return;
    }

    bool sortBuffersNeedRecreate = false;
    {
        auto lg = internalDs->GetDataLockGuard();
        GsPrimitive* poolPrim = FindPoolPrimitive();
        if (!poolPrim) {
            return;
        }

        bool poolChanged = false;
        for (const auto& entry : tileSlots_) {
            const int32_t slot = entry.slot.poolSlot;
            if (slot < 0 || static_cast<size_t>(slot) >= poolPrim->tileGrid.tiles.size()) {
                continue;
            }

            GsTileMetadata& tile = poolPrim->tileGrid.tiles[static_cast<size_t>(slot)];
            const GsTileState newState = entry.slot.visible ? GsTileState::RESIDENT : GsTileState::EVICTING;
            if (tile.state != newState) {
                tile.state = newState;
                poolChanged = true;
            }
            tile.sortOutputValid = false;
        }

        if (!poolChanged) {
            return;
        }

        sortBuffersNeedRecreate = UpdateSortCapacityLocked();

        if (GsTilePool* pool = dataStoreGs_->GetTilePool()) {
            pool->resourceVersion++;
        }
    }

    if (sortBuffersNeedRecreate) {
        RecreateSortCapacityBuffers();
    }
}

void GsTilingSystem::EnqueueTileRequest(CORE_NS::Entity entity)
{
    if (!CORE_NS::EntityUtil::IsValid(entity) || entity == poolEntity_) {
        return;
    }

    if (pendingEntities_.find(entity.id) != pendingEntities_.end()) {
        return;
    }

    if (queuedEntities_.insert(entity.id).second) {
        requestQueue_.push_back(entity);
    }
}

void GsTilingSystem::QueueExistingTileRequests()
{
    if (initialTileRequestsQueued_) {
        return;
    }

    for (const auto& kv : tileData_) {
        EnqueueTileRequest(CORE_NS::Entity { kv.first });
    }
    initialTileRequestsQueued_ = true;
}

bool GsTilingSystem::BuildLoadCandidate(CORE_NS::Entity entity, LoadCandidate& candidate)
{
    if (entity == poolEntity_ || pendingEntities_.find(entity.id) != pendingEntities_.end()) {
        return false;
    }

    const auto* comp = FindTileData(entity);
    if (!comp || comp->isResident || comp->uri.empty() ||
        (!IsSogUri(comp->uri) && !IsGlbUri(comp->uri)) || comp->count == 0u) {
        return false;
    }

    candidate.entity = entity;
    candidate.aabbMin = comp->aabbMin;
    candidate.aabbMax = comp->aabbMax;
    candidate.uri = comp->uri;
    candidate.offset = comp->offset;
    candidate.count = comp->count;
    candidate.visible = comp->visible;

    return true;
}

BASE_NS::vector<GsTilingSystem::LoadCandidate> GsTilingSystem::BuildLoadCandidates()
{
    BASE_NS::vector<LoadCandidate> candidates;
    candidates.reserve(requestQueue_.size() - requestQueueHead_);

    while (requestQueueHead_ < requestQueue_.size()) {
        const CORE_NS::Entity entity = requestQueue_[requestQueueHead_++];
        queuedEntities_.erase(entity.id);

        LoadCandidate candidate;
        if (!BuildLoadCandidate(entity, candidate)) {
            continue;
        }

        candidates.push_back(BASE_NS::move(candidate));
    }

    if (requestQueueHead_ > 0u &&
        (requestQueueHead_ == requestQueue_.size() || requestQueueHead_ > requestQueue_.size() / 2u)) {
        requestQueue_.erase(requestQueue_.begin(), requestQueue_.begin() + static_cast<int64_t>(requestQueueHead_));
        requestQueueHead_ = 0u;
    }

    std::stable_partition(candidates.begin(), candidates.end(), [](const LoadCandidate& c) { return c.visible; });

    return candidates;
}

BASE_NS::vector<GsTilingSystem::PreparedSubmission> GsTilingSystem::PrepareSubmissions(
    const BASE_NS::vector<LoadCandidate>& candidates)
{
    BASE_NS::vector<PreparedSubmission> prepared;
    prepared.reserve(candidates.size());

    for (const auto& cand : candidates) {
        if (!poolInitialized_) {
            InitializePool();
            if (!poolInitialized_) {
                EnqueueTileRequest(cand.entity);
                continue;
            }
        }

        GS::GsTilingComponent comp;
        comp.uri = cand.uri;
        comp.offset = cand.offset;
        comp.count = cand.count;
        comp.aabbMin = cand.aabbMin;
        comp.aabbMax = cand.aabbMax;
        comp.visible = cand.visible;

        PreparedSubmission prep;
        if (!PrepareTileSubmission(cand.entity, comp, prep)) {
            EnqueueTileRequest(cand.entity);
            continue;
        }
        prepared.push_back(BASE_NS::move(prep));
    }

    return prepared;
}

void GsTilingSystem::DispatchPreparedLoads(BASE_NS::vector<PreparedSubmission>& prepared)
{
    uint32_t neededSlots = 0u;
    uint64_t neededSplatCapacity = 0u;
    for (const auto& prep : prepared) {
        if (!prep.visible) {
            continue;
        }
        neededSlots++;
        neededSplatCapacity += AlignSplatCapacity(prep.count);
    }
    EvictForBatch(neededSlots, neededSplatCapacity);

    for (auto& prep : prepared) {
        if (!DispatchPreparedSubmission(prep)) {
            EnqueueTileRequest(prep.entity);
        }
    }
}

void GsTilingSystem::SubmitNewTileLoads()
{
    if (requestQueueHead_ >= requestQueue_.size()) {
        return;
    }

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsTilingSystem::Update::SubmitLoads", "", 0x00FF88);

    BASE_NS::vector<LoadCandidate> candidates = BuildLoadCandidates();
    if (candidates.empty()) {
        return;
    }

    BASE_NS::vector<PreparedSubmission> prepared = PrepareSubmissions(candidates);
    if (prepared.empty()) {
        return;
    }

    DispatchPreparedLoads(prepared);
}

bool GsTilingSystem::Update(bool frameRenderingQueued, uint64_t time, uint64_t delta)
{
    if (!active_.load(std::memory_order_acquire)) {
        return false;
    }

    CORE_CPU_PERF_SCOPE("LUMEGS", "GsTilingSystem::Update::Orchestrate", "", 0x00FF88);

    bool hasDataStore = false;
    {
        std::lock_guard<std::mutex> lock(workerMutex_);
        hasDataStore = static_cast<bool>(dataStoreGs_);
    }

    if (!hasDataStore && renderContext_) {
        if (auto render = GetSystem<CORE3D_NS::IRenderPreprocessorSystem>(ecs_)) {
            if (auto propertyHandle =
                    ScopedHandle<const CORE3D_NS::IRenderPreprocessorSystem::Properties>(render->GetProperties())) {
                const auto dataStoreName = propertyHandle->dataStorePrefix + DEFAULT_GS_RENDER_DATA_STORE_NAME_POST_FIX;
                auto* store = renderContext_->GetRenderDataStoreManager().GetRenderDataStore(dataStoreName).get();
                auto dataStore = refcnt_ptr<IRenderDataStoreDefaultGs>(static_cast<IRenderDataStoreDefaultGs*>(store));

                if (dataStore) {
                    std::lock_guard<std::mutex> lock(workerMutex_);

                    if (!dataStoreGs_) {
                        dataStoreGs_ = BASE_NS::move(dataStore);
                    }

                    hasDataStore = static_cast<bool>(dataStoreGs_);
                }
            }
        }
    }

    if (!hasDataStore) {
        return false;
    }

    UpdateGsConfigCache();
    if (wantsRender_.exchange(false, std::memory_order_acq_rel)) {
        // modify the actual lsgs node, this triggers a render in rendersystem.
        auto t = GetManager<CORE3D_NS::INodeComponentManager>(ecs_);
        {
            t->Write(poolEntity_);
        }
        ecs_.RequestRender();
    }

    NotifyWorker();

    return false;
}

namespace GS {
ISystem* IGsTilingSystemInstance(IEcs& ecs)
{
    return new GsTilingSystem(ecs);
}

void IGsTilingSystemDestroy(ISystem* system)
{
    delete static_cast<GsTilingSystem*>(system);
}

} // namespace GS
