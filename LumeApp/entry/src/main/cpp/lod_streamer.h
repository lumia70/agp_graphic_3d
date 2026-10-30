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
        previousLevels_.clear();
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
                EnsureTile(missing, ++clock_);
                continue;
            }

            // Everything needed is cached. Assemble once the camera is still.
            bool settled = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                settled = (requestId_ == snapshot);
            }
            if (!settled) {
                continue;
            }

            const bool compact = totalWritten_ > (budget_ * 11u) / 10u;
            // Nothing to do unless the camera moved since the last assembly or a
            // compaction is due (otherwise this loop would spin every 200 ms).
            if (!compact && snapshot == lastBuiltRequest_) {
                continue;
            }
            BuildView(cam, compact);
            lastBuiltRequest_ = snapshot;
            seen = snapshot;
        }
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
            if (caches_.find(slice.file) == caches_.end()) {
                return slice.file;
            }
        }
        return -1;
    }

    bool EnsureTile(int32_t file, uint64_t now)
    {
        auto it = caches_.find(file);
        if (it != caches_.end()) {
            it->second.lastUse = now;
            return true;
        }
        if (file < 0 || (size_t)file >= scene_.files.size()) {
            return false;
        }
        const std::string path = scene_.dir + "/" + scene_.files[file];
        SplatData tile;
        const auto t0 = std::chrono::steady_clock::now();
        if (!sog::LoadSogFile(path.c_str(), tile, 1.0f, false)) {
            LOGE("LOD: failed to decode '%{public}s'", path.c_str());
            return false;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        auto& entry = caches_[file];
        entry.data.resize(tile.gaussians.size());
        if (!tile.gaussians.empty()) {
            std::memcpy(entry.data.data(), tile.gaussians.data(),
                tile.gaussians.size() * sizeof(GpuGaussianData));
        }
        entry.lastUse = now;
        cacheCount_ += (uint32_t)entry.data.size();
        LOGI("LOD: decoded tile %{public}d (%{public}zu gaussians, %.0f ms)", file, entry.data.size(), ms);
        Evict();
        return true;
    }

    void Evict()
    {
        while (cacheCount_ > cacheBudget_ && caches_.size() > 1) {
            auto oldest = caches_.end();
            for (auto it = caches_.begin(); it != caches_.end(); ++it) {
                if (oldest == caches_.end() || it->second.lastUse < oldest->second.lastUse) {
                    oldest = it;
                }
            }
            if (oldest == caches_.end()) {
                return;
            }
            cacheCount_ -= (uint32_t)oldest->second.data.size();
            caches_.erase(oldest);
        }
    }

    // Fill the write buffer in leaf order (nearest first) and publish prefixes.
    void BuildView(const Camera& cam, bool forceCompact = false)
    {
        // Skip near-identical rebuilds (tiny camera drift): re-assembling means a
        // full re-upload, which is visible as a flash.
        if (publishedBuffer_ >= 0 && lastSelection_.size() == 0) {
            lastSelection_.resize(scene_.leaves.size(), (int8_t)-1);
        }
        if (previousLevels_.size() != scene_.leaves.size()) {
            // Sentinel -1 = "no previous level" so the first build uses the
            // plain distance thresholds.
            previousLevels_.assign(scene_.leaves.size(), (int8_t)-1);
        }
        Selection sel;
        SelectVisible(scene_, cam.view, cam.proj, cam.camPos, budget_, previousLevels_.data(), sel);
        LOGI("LOD: %{public}zu leaves visible (%{public}u culled), %{public}u gaussians "
             "[L0 %u/%u L1 %u/%u L2 %u/%u L3 %u/%u]",
            sel.items.size(), sel.culled, sel.total,
            sel.perLevelLeaves[0], sel.perLevelGaussians[0], sel.perLevelLeaves[1],
            sel.perLevelGaussians[1], sel.perLevelLeaves[2], sel.perLevelGaussians[2],
            sel.perLevelLeaves[3], sel.perLevelGaussians[3]);

        // Grow-only tile table: (leaf, level) entries already resident stay in the
        // buffer, and a rebuild only appends the newly selected ones. This keeps
        // motion smooth (no whole-set swap, no geometry disappearing) and lets the
        // node upload just the appended tail.
        uint32_t appended = 0;
        for (const auto& it : sel.items) {
            const uint32_t key = (it.leaf * 16u) + (uint32_t)it.level;
            if (entryOf_.find(key) == entryOf_.end()) {
                appended += scene_.leaves[it.leaf].lods[it.level].count;
            }
        }
        // Full rewrites alternate to the other SSBO, which may still be read by a
        // frame that is already in flight -> one corrupted frame (seen as a black
        // flash while rotating). So only rebuild when the camera is settled (the
        // compaction pass) or for the very first build; while moving we simply
        // stop appending once the buffer is full and keep drawing the current set.
        const bool overflowing = (totalWritten_ + appended > capacity_);
        bool fullRebuild = forceCompact || (publishedBuffer_ < 0);
        if (fullRebuild) {
            entries_.clear();
            entryOf_.clear();
            totalWritten_ = 0;
            // A full rebuild rewrites everything: write into the other buffer and
            // let the node switch SSBOs once the upload completes.
            writeIndex_ = (publishedBuffer_ < 0) ? 0 : (1 - publishedBuffer_);
        } else if (writeIndex_ < 0) {
            writeIndex_ = 0;
        }

        const uint32_t uploadStart = fullRebuild ? 0u : totalWritten_;
        const uint32_t before = totalWritten_;
        const uint64_t now = ++clock_;
        // The first build publishes prefixes so the scene appears tile by tile;
        // later appends publish once so there is no mid-motion swap.
        const bool progressive = (publishedBuffer_ < 0);
        const uint32_t publishStep = progressive ? std::max<uint32_t>(budget_ / 8u, 150000u)
                                                 : (budget_ + 1u);
        uint32_t lastPublished = before;

        for (const auto& it : sel.items) {
            const uint32_t key = (it.leaf * 16u) + (uint32_t)it.level;
            if (entryOf_.find(key) != entryOf_.end()) {
                continue;
            }
            const Leaf& leaf = scene_.leaves[it.leaf];
            const Slice& slice = leaf.lods[it.level];
            if (slice.file < 0) {
                continue;
            }
            if (!EnsureTile(slice.file, now)) {
                continue;
            }
            const auto& src = caches_[slice.file].data;
            if (slice.offset + slice.count > src.size()) {
                LOGE("LOD: slice out of range (file %{public}d offset %{public}u count %{public}u size %{public}zu)",
                    slice.file, slice.offset, slice.count, src.size());
                continue;
            }
            if (totalWritten_ + slice.count > capacity_) {
                break;
            }
            std::memcpy(&buffers_[writeIndex_][totalWritten_], &src[slice.offset],
                (size_t)slice.count * sizeof(GpuGaussianData));
            Entry entry;
            entry.key = key;
            entry.count = slice.count;
            entry.dst = totalWritten_;
            entries_.push_back(entry);
            entryOf_[key] = (uint32_t)(entries_.size() - 1);
            totalWritten_ += slice.count;

            if (totalWritten_ - lastPublished >= publishStep) {
                Publish(writeIndex_, totalWritten_, uploadStart);
                lastPublished = totalWritten_;
            }
        }
        if (totalWritten_ > lastPublished) {
            Publish(writeIndex_, totalWritten_, uploadStart);
        } else if (fullRebuild) {
            // Nothing decoded yet: still expose the (empty) rebuild.
            Publish(writeIndex_, totalWritten_, uploadStart);
        }
        for (const auto& it : sel.items) {
            previousLevels_[it.leaf] = (int8_t)it.level;
        }
        LOGI("LOD: assembly %{public}u gaussians (+%{public}u, uploadStart %{public}u, %s%s)",
            totalWritten_, totalWritten_ - before, uploadStart, fullRebuild ? "full" : "append",
            overflowing ? ", buffer full" : "");
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

    struct Entry {
        uint32_t key = 0;
        uint32_t count = 0;
        uint32_t dst = 0;
    };

    std::vector<GpuGaussianData> buffers_[2];
    int writeIndex_ = -1;
    std::vector<Entry> entries_;
    std::unordered_map<uint32_t, uint32_t> entryOf_;
    uint32_t totalWritten_ = 0;
    uint32_t publishUploadStart_ = 0;
    int publishedBuffer_ = -1;
    uint32_t publishCount_ = 0;
    uint32_t publishGeneration_ = 0;
    uint32_t consumedGeneration_ = 0;

    std::vector<int8_t> previousLevels_;
    std::vector<int8_t> lastSelection_;
    std::unordered_map<int32_t, CacheEntry> caches_;
    uint32_t cacheCount_ = 0;
    uint64_t clock_ = 0;
    uint64_t lastBuiltRequest_ = 0;
};

} // namespace lod
