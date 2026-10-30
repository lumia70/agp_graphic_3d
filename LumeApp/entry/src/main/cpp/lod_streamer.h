/*
 * lod_streamer.h - background tile streaming and per-frame assembly for LOD scenes.
 *
 * Render thread: UpdateCamera() publishes a camera snapshot, then Poll() picks up
 * a new assembly when the worker publishes one.
 *
 * Worker thread: culls leaves, picks an LOD per leaf, decodes the tiles it needs
 * (whole-file decode into a count-bounded LRU cache), and fills a double buffer
 * in leaf order. Partial prefixes are published so the scene appears tile by
 * tile instead of after the whole view is decoded.
 *
 * Handshake: after publishing generation G the worker only reuses that buffer
 * once the render node echoes g_gaussianSplatUploadedGeneration == G, so the
 * node never uploads a buffer that is being overwritten.
 */
#pragma once

#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <unordered_set>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gaussian_splat_data.h"
#include "lod_scene.h"
#include "sog_loader.h"

namespace lod {

class Streamer {
public:
    ~Streamer() { Stop(); }

    bool Start(const std::string& sceneDir, uint32_t budget, uint32_t cacheBudget)
    {
        Stop();
        if (!scene_.Load(sceneDir)) {
            return false;
        }
        budget_ = budget ? budget : scene_.maxRenderCount;
        // The two assembly buffers already take 2 * capacity * 64 B; keep the decoded
        // tile cache below the scene's own suggestion to stay around 500 MB RSS.
        cacheBudget_ = cacheBudget ? cacheBudget
                                   : std::min<uint32_t>(scene_.cacheCount ? scene_.cacheCount : 4000000u,
                                         4000000u);
        if (budget_ > 4000000u) {
            budget_ = 4000000u;
        }
        // The buffer keeps stale entries around while moving (append-only), so it
        // has headroom over the per-frame selection budget.
        capacity_ = budget_ + budget_ / 2u;
        for (int i = 0; i < 2; i++) {
            buffers_[i].resize(capacity_);
        }
        publishCount_ = 0;
        publishGeneration_ = 0;
        publishedBuffer_ = -1;
        publishUploadStart_ = 0;
        writeIndex_ = -1;
        entries_.clear();
        entryOf_.clear();
        totalWritten_ = 0;
        dirty_ = false;
        previousLevels_.clear();
        lastBuiltRequest_ = 0;
        basePreloaded_ = false;
        stopped_ = false;
        worker_ = std::thread([this] { Loop(); });
        LOGI("LOD streamer: budget %{public}u, cache %{public}u", budget_, cacheBudget_);
        return true;
    }

    void Stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!worker_.joinable()) {
                return;
            }
            stopped_ = true;
        }
        cv_.notify_all();
        worker_.join();
        caches_.clear();
        cacheCount_ = 0;
    }

    // ---- render thread ----
    void UpdateCamera(const float view[16], const float proj[16], const float camPos[3])
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::memcpy(pending_.view, view, sizeof(pending_.view));
        std::memcpy(pending_.proj, proj, sizeof(pending_.proj));
        std::memcpy(pending_.camPos, camPos, sizeof(pending_.camPos));
        pending_.valid = true;
        ++requestId_;
        cv_.notify_one();
    }

    // Returns true when a new assembly is published for the render thread to use.
    bool Poll(uint32_t& generation, const GpuGaussianData** data, uint32_t& count,
        uint32_t& uploadStart)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (publishedBuffer_ < 0 || publishGeneration_ == consumedGeneration_) {
            return false;
        }
        consumedGeneration_ = publishGeneration_;
        generation = publishGeneration_;
        *data = buffers_[publishedBuffer_].data();
        count = publishCount_;
        uploadStart = publishUploadStart_;
        return count > 0;
    }

    uint32_t Budget() const { return budget_; }
    uint32_t Capacity() const { return capacity_; }
    const Scene& GetScene() const { return scene_; }

private:
    struct Camera {
        float view[16] = {};
        float proj[16] = {};
        float camPos[3] = {};
        bool valid = false;
    };

    struct CacheEntry {
        std::vector<GpuGaussianData> data;
        uint64_t lastUse = 0;
    };
    using CacheEntryPtr = std::shared_ptr<CacheEntry>;

    void Loop()
    {
        uint64_t seen = 0;
        for (;;) {
            Camera cam;
            uint64_t snapshot = 0;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(200), [this, &seen] {
                    return stopped_ || requestId_ != seen;
                });
                if (stopped_) {
                    return;
                }
                seen = requestId_;
                snapshot = requestId_;
                cam = pending_;
            }
            // First fill the cache with the coarsest level of the whole scene: it
            // is only a handful of tiles and gives every leaf a fallback, so a
            // rotation always has *something* to show while finer tiles decode.
            // This runs before the first camera arrives so it starts immediately.
            if (!basePreloaded_) {
                const int32_t firstBase = NextBaseTile(-1);
                if (firstBase >= 0) {
                    const int32_t secondBase = NextBaseTile(firstBase);
                    const uint64_t now = ++clock_;
                    if (secondBase >= 0) {
                        auto async = std::async(std::launch::async, [this, secondBase, now] {
                            EnsureTile(secondBase, now);
                        });
                        EnsureTile(firstBase, now);
                        async.wait();
                    } else {
                        EnsureTile(firstBase, now);
                    }
                    if (publishedBuffer_ < 0 && cam.valid) {
                        BuildView(cam, false);
                    }
                    continue;
                }
                if (DecodeNextBaseTile()) {
                    // Show whatever the base layer already covers instead of
                    // waiting for the whole preload (progressive first view).
                    if (publishedBuffer_ < 0 && cam.valid) {
                        BuildView(cam, false);
                    }
                    continue;
                }
                basePreloaded_ = true;
                continue;
            }

            if (!cam.valid) {
                continue;
            }

            // Decode work runs while the camera keeps moving: each iteration
            // prepares at most one missing tile for the *current* selection, then
            // re-reads the camera. This is what removes the multi-second wait
            // after a large rotation (tiles are already cached when the camera
            // stops).
            const int32_t missing = NextMissingTile(cam);
            if (missing >= 0) {
                // Decode two tiles at once (the second one on a helper thread) to
                // halve the time until a new area is usable.
                const uint64_t now = ++clock_;
                int32_t second = -1;
                {
                    const int32_t probe = NextMissingTileExcluding(cam, missing);
                    second = probe;
                }
                if (second >= 0) {
                    auto async = std::async(std::launch::async, [this, second, now] {
                        EnsureTile(second, now);
                    });
                    EnsureTile(missing, now);
                    async.wait();
                } else {
                    EnsureTile(missing, now);
                }
                continue;
            }

            // Everything needed is cached. Assemble only once the camera has been
            // still for a while: a rebuild swaps buffers, and doing that while a
            // frame is in flight shows up as a one-frame flash.
            bool settled = false;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                const uint64_t target = requestId_;
                settled = !cv_.wait_for(lock, std::chrono::milliseconds(450), [this, target] {
                    return stopped_ || requestId_ != target;
                });
            }
            if (!settled) {
                // While the camera keeps moving, append the tiles that just became
                // available (tail upload, no buffer swap -> no flash). This is what
                // makes a newly visible area show up without lifting the finger.
                if (dirty_) {
                    BuildView(cam, false);
                    dirty_ = false;
                    lastBuiltRequest_ = snapshot;
                }
                seen = requestId_;
                continue;
            }

            const bool cameraChanged = (snapshot != lastBuiltRequest_);
            if (!dirty_ && !cameraChanged) {
                continue;
            }
            // Settled -> full rebuild into the alternate buffer (compacts stale
            // entries, no tearing because nothing is moving). While moving ->
            // append only the newly available tiles, which uploads a tail and
            // never swaps buffers, so it cannot flash.
            BuildView(cam, settled);
            lastBuiltRequest_ = snapshot;
            seen = snapshot;
            dirty_ = false;
        }
    }

    int32_t NextMissingTileExcluding(const Camera& cam, int32_t exclude)
    {
        Selection sel;
        if (previousLevels_.size() != scene_.leaves.size()) {
            previousLevels_.assign(scene_.leaves.size(), (int8_t)-1);
        }
        SelectVisible(scene_, cam.view, cam.proj, cam.camPos, budget_, previousLevels_.data(), sel);
        for (const auto& it : sel.items) {
            const Slice& slice = scene_.leaves[it.leaf].lods[it.level];
            if (slice.file < 0 || slice.file == exclude) {
                continue;
            }
            if (!TileReady(slice.file)) {
                return slice.file;
            }
        }
        return -1;
    }

    // Nearest not-yet-decoded coarsest tile (exclude one file), or -1.
    int32_t NextBaseTile(int32_t exclude)
    {
        const int baseLevel = scene_.levelCount - 1;
        float bestDist = 0.0f;
        int32_t bestFile = -1;
        for (const auto& leaf : scene_.leaves) {
            if (baseLevel >= leaf.levels) {
                continue;
            }
            const int32_t file = leaf.lods[baseLevel].file;
            if (file < 0 || file == exclude || TileReady(file)) {
                continue;
            }
            const float dist = DistanceToAabb(scene_.camPos, leaf);
            if (bestFile < 0 || dist < bestDist) {
                bestFile = file;
                bestDist = dist;
            }
        }
        return bestFile;
    }

    bool DecodeNextBaseTile()
    {
        // Nearest-to-the-initial-camera first: the first frames then show the part
        // of the coarse layer that is actually in view instead of empty sky.
        const int baseLevel = scene_.levelCount - 1;
        float bestDist = 0.0f;
        int32_t bestFile = -1;
        for (const auto& leaf : scene_.leaves) {
            if (baseLevel >= leaf.levels) {
                continue;
            }
            const int32_t file = leaf.lods[baseLevel].file;
            if (file < 0 || TileReady(file)) {
                continue;
            }
            const float dist = DistanceToAabb(scene_.camPos, leaf);
            if (bestFile < 0 || dist < bestDist) {
                bestFile = file;
                bestDist = dist;
            }
        }
        if (bestFile < 0) {
            return false;
        }
        LOGI("LOD: preloading base tile %{public}d", bestFile);
        return EnsureTile(bestFile, ++clock_);
    }

    // First tile of the current selection that is not decoded yet (priority
    // order: nearest leaf first).
    int32_t NextMissingTile(const Camera& cam)
    {
        if (previousLevels_.size() != scene_.leaves.size()) {
            previousLevels_.assign(scene_.leaves.size(), (int8_t)-1);
        }
        Selection sel;
        SelectVisible(scene_, cam.view, cam.proj, cam.camPos, budget_, previousLevels_.data(), sel);
        for (const auto& it : sel.items) {
            const Slice& slice = scene_.leaves[it.leaf].lods[it.level];
            if (slice.file < 0) {
                continue;
            }
            if (!TileReady(slice.file)) {
                return slice.file;
            }
        }
        return -1;
    }

    // Decodes a tile into the cache. Thread-safe: the (slow) decode runs outside
    // the lock and the entry is published afterwards, so several tiles can be
    // decoded concurrently.
    bool EnsureTile(int32_t file, uint64_t now)
    {
        {
            std::lock_guard<std::mutex> lock(cacheMutex_);
            auto it = caches_.find(file);
            if (it != caches_.end()) {
                it->second->lastUse = now;
                return true;
            }
            if (decoding_.count(file) != 0) {
                return false;  // another thread is already decoding it
            }
            if (file < 0 || (size_t)file >= scene_.files.size()) {
                return false;
            }
            decoding_.insert(file);
        }

        const std::string path = scene_.dir + "/" + scene_.files[file];
        SplatData tile;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = sog::LoadSogFile(path.c_str(), tile, 1.0f, false);

        auto entry = std::make_shared<CacheEntry>();
        if (ok) {
            entry->data.resize(tile.gaussians.size());
            if (!tile.gaussians.empty()) {
                std::memcpy(entry->data.data(), tile.gaussians.data(),
                    tile.gaussians.size() * sizeof(GpuGaussianData));
            }
            entry->lastUse = now;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

        std::lock_guard<std::mutex> lock(cacheMutex_);
        decoding_.erase(file);
        if (!ok) {
            LOGE("LOD: failed to decode '%{public}s'", path.c_str());
            return false;
        }
        caches_[file] = entry;
        cacheCount_ += (uint32_t)entry->data.size();
        dirty_ = true;
        LOGI("LOD: decoded tile %{public}d (%{public}zu gaussians, %.0f ms)", file, entry->data.size(), ms);
        EvictLocked();
        return true;
    }

    void EvictLocked()
    {
        while (cacheCount_ > cacheBudget_ && caches_.size() > 1) {
            auto oldest = caches_.end();
            for (auto it = caches_.begin(); it != caches_.end(); ++it) {
                if (oldest == caches_.end() || it->second->lastUse < oldest->second->lastUse) {
                    oldest = it;
                }
            }
            if (oldest == caches_.end()) {
                return;
            }
            cacheCount_ -= (uint32_t)oldest->second->data.size();
            caches_.erase(oldest);
        }
    }

    // True when the tile is decoded (or being decoded by another thread).
    bool TileReady(int32_t file)
    {
        std::lock_guard<std::mutex> lock(cacheMutex_);
        return caches_.find(file) != caches_.end();
    }

    // Fill the write buffer in leaf order (nearest first) and publish prefixes.
    // Assembles the current selection.
    //   settled == false : append-only update (uploads only the new tail, the
    //                      buffer that is being drawn is never swapped);
    //   settled == true  : full rebuild into the other buffer, then swap.
    void BuildView(const Camera& cam, bool settled)
    {
        if (previousLevels_.size() != scene_.leaves.size()) {
            previousLevels_.assign(scene_.leaves.size(), (int8_t)-1);
        }
        Selection sel;
        SelectVisible(scene_, cam.view, cam.proj, cam.camPos, budget_, previousLevels_.data(), sel);

        auto resolveLevel = [&](uint32_t leafIndex, int desired) {
            const Leaf& leaf = scene_.leaves[leafIndex];
            for (int level = desired; level < leaf.levels; level++) {
                const int32_t file = leaf.lods[level].file;
                if (file >= 0 && TileReady(file)) {
                    return level;
                }
            }
            return -1;
        };

        // New (leaf, level) entries that the current selection needs.
        uint32_t appended = 0;
        for (const auto& it : sel.items) {
            const int level = resolveLevel(it.leaf, it.level);
            if (level < 0) {
                continue;
            }
            const uint32_t key = (it.leaf * 16u) + (uint32_t)level;
            if (entryOf_.find(key) == entryOf_.end()) {
                appended += scene_.leaves[it.leaf].lods[level].count;
            }
        }

        const bool first = (publishedBuffer_ < 0);
        // Full rewrite when nothing exists yet, when the camera is at rest, or
        // when appending would overflow the buffer.
        const bool full = first || settled || (totalWritten_ + appended > capacity_);
        if (full) {
            entries_.clear();
            entryOf_.clear();
            totalWritten_ = 0;
            writeIndex_ = first ? 0 : (1 - publishedBuffer_);
        } else if (writeIndex_ < 0) {
            writeIndex_ = 0;
        }

        const uint32_t uploadStart = full ? 0u : totalWritten_;
        const uint32_t before = totalWritten_;
        const bool progressive = first;
        const uint32_t publishStep = progressive ? std::max<uint32_t>(budget_ / 8u, 150000u)
                                                 : (budget_ + 1u);
        const uint64_t now = ++clock_;
        uint32_t lastPublished = before;
        uint32_t skippedNoTile = 0;

        for (const auto& it : sel.items) {
            const int level = resolveLevel(it.leaf, it.level);
            if (level < 0) {
                skippedNoTile++;
                continue;
            }
            const uint32_t key = (it.leaf * 16u) + (uint32_t)level;
            if (entryOf_.find(key) != entryOf_.end()) {
                continue;
            }
            const Slice& slice = scene_.leaves[it.leaf].lods[level];
            if (slice.file < 0 || !EnsureTile(slice.file, now)) {
                continue;
            }
            CacheEntryPtr entry;
            {
                std::lock_guard<std::mutex> lock(cacheMutex_);
                auto it2 = caches_.find(slice.file);
                if (it2 == caches_.end()) {
                    continue;
                }
                entry = it2->second;
            }
            const auto& src = entry->data;
            if (slice.offset + slice.count > src.size()) {
                LOGE("LOD: slice out of range (file %{public}d offset %{public}u count %{public}u)",
                    slice.file, slice.offset, slice.count);
                continue;
            }
            if (totalWritten_ + slice.count > capacity_) {
                break;
            }
            std::memcpy(&buffers_[writeIndex_][totalWritten_], &src[slice.offset],
                (size_t)slice.count * sizeof(GpuGaussianData));
            Entry entryInfo;
            entryInfo.count = slice.count;
            entryInfo.dst = totalWritten_;
            entries_.push_back(entryInfo);
            entryOf_[key] = (uint32_t)(entries_.size() - 1);
            totalWritten_ += slice.count;

            if (totalWritten_ - lastPublished >= publishStep) {
                Publish(writeIndex_, totalWritten_, uploadStart);
                lastPublished = totalWritten_;
            }
        }

        if (totalWritten_ > lastPublished) {
            Publish(writeIndex_, totalWritten_, uploadStart);
        } else if (full && totalWritten_ == before && before > 0) {
            // Nothing changed and the set is not empty: keep the published buffer.
            LOGI("LOD: rebuild produced no change (%u gaussians)", totalWritten_);
        }
        for (const auto& it : sel.items) {
            previousLevels_[it.leaf] = (int8_t)it.level;
        }
        LOGI("LOD: %{public}s %{public}u gaussians (+%{public}u, no-tile %{public}u, gen 0x%{public}x)",
            full ? "rebuild" : "append", totalWritten_, totalWritten_ - before, skippedNoTile,
            publishGeneration_);
    }

    void Publish(int bufferIndex, uint32_t count, uint32_t uploadStart)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        publishedBuffer_ = bufferIndex;
        publishCount_ = count;
        publishUploadStart_ = uploadStart;
        ++publishGeneration_;
    }

    Scene scene_;
    uint32_t budget_ = 1000000;
    uint32_t capacity_ = 1500000;
    uint32_t cacheBudget_ = 3000000;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopped_ = false;
    Camera pending_;
    uint64_t requestId_ = 0;

    std::vector<GpuGaussianData> buffers_[2];
    struct Entry {
        uint32_t count = 0;
        uint32_t dst = 0;
    };

    uint32_t publishUploadStart_ = 0;
    int writeIndex_ = -1;
    std::vector<Entry> entries_;
    std::unordered_map<uint32_t, uint32_t> entryOf_;
    uint32_t totalWritten_ = 0;
    bool dirty_ = false;
    int publishedBuffer_ = -1;
    uint32_t publishCount_ = 0;
    uint32_t publishGeneration_ = 0;
    uint32_t consumedGeneration_ = 0;

    std::vector<int8_t> previousLevels_;
    std::vector<int8_t> lastSelection_;
    std::mutex cacheMutex_;
    std::unordered_map<int32_t, CacheEntryPtr> caches_;
    std::unordered_set<int32_t> decoding_;
    uint32_t cacheCount_ = 0;
    uint64_t clock_ = 0;
    uint64_t lastBuiltRequest_ = 0;
    bool publishedForRequest_ = false;
    bool basePreloaded_ = false;
};

} // namespace lod
