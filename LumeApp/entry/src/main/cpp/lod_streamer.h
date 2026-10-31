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

#include <atomic>
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
                                   : std::min<uint32_t>(scene_.cacheCount ? scene_.cacheCount : 5000000u,
                                         5000000u);
        if (budget_ > 4000000u) {
            budget_ = 4000000u;
        }
        // The buffer keeps stale entries around while moving (append-only), so it
        // has headroom over the per-frame selection budget.
        capacity_ = budget_ * 2u;
        // Appends add duplicates (a leaf at a new level), so the buffer grows over
        // a session. Past this we stop appending instead of compacting: a
        // compaction shrinks the drawn set, which reads as a "coarse flash".
        // Every file that holds the coarsest level is pinned in the cache.
        {
            const int baseLevel = scene_.levelCount - 1;
            for (const auto& leaf : scene_.leaves) {
                if (baseLevel < leaf.levels && leaf.lods[baseLevel].file >= 0) {
                    baseFiles_.insert(leaf.lods[baseLevel].file);
                }
            }
        }
        for (int i = 0; i < 2; i++) {
            buffers_[i].resize(capacity_);
        }
        publishCount_ = 0;
        publishGeneration_ = 0;
        publishedBuffer_ = -1;
        publishUploadStart_ = 0;
        writeIndex_ = 0;
        leafEntry_.clear();
        freeBlocks_.clear();
        poolTop_ = 0;
        dirtyRanges_.clear();
        liveRanges_.clear();
        liveCount_ = 0;
        dirty_ = false;
        publishPoolSize_ = 0;
        publishLiveCount_ = 0;
        publishLiveHash_ = 0;
        lastLiveHash_ = 0;
        decodes_.clear();
        lastBuiltHash_ = 0;
        lastBuildMs_ = 0;
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
    struct Result {
        uint32_t generation = 0;
        const GpuGaussianData* data = nullptr;
        uint32_t count = 0;
        uint32_t poolSize = 0;
        const uint32_t* liveRanges = nullptr;
        uint32_t liveRangeCount = 0;
        const uint32_t* dirtyRanges = nullptr;
        uint32_t dirtyRangeCount = 0;
    };

    bool Poll(Result& result)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (publishedBuffer_ < 0 || publishGeneration_ == consumedGeneration_) {
            return false;
        }
        consumedGeneration_ = publishGeneration_;
        result.generation = publishGeneration_;
        result.data = buffers_[publishedBuffer_].data();
        result.count = publishCount_;
        result.poolSize = publishPoolSize_;
        result.liveRanges = liveRanges_.empty() ? nullptr : liveRanges_.data();
        result.liveRangeCount = (uint32_t)(liveRanges_.size() / 2);
        result.dirtyRanges = dirtyRanges_.empty() ? nullptr : dirtyRanges_.data();
        result.dirtyRangeCount = (uint32_t)(dirtyRanges_.size() / 2);
        return result.count > 0;
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

            // 1) Collect finished background decodes (never blocks the loop).
            for (size_t d = 0; d < decodes_.size();) {
                if (decodes_[d].second.wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::ready) {
                    decodes_[d].second.get();
                    decodes_.erase(decodes_.begin() + (long)d);
                    dirty_ = true;
                } else {
                    d++;
                }
            }

            // 2) Publish immediately when the camera moved or new data landed. This
            //    is cheap (block table only) and is what makes newly visible leaves
            //    show up *while* the finger is still moving.
            const bool cameraChanged = (snapshot != lastBuiltRequest_);
            if (cameraChanged || dirty_) {
                BuildView(cam, false);
                dirty_ = false;
                lastBuiltRequest_ = snapshot;
            }
            seen = snapshot;

            // 3) Keep a few decodes in flight: one tile takes 0.3-0.7 s and a sharp
            //    centre needs several of them.
            constexpr size_t kMaxDecodesInFlight = 2;
            while (decodes_.size() < kMaxDecodesInFlight) {
                int32_t exclude = decodes_.empty() ? -1 : decodes_.front().first;
                const int32_t missing = NextMissingTileExcluding(cam, exclude);
                if (missing < 0) {
                    break;
                }
                const uint64_t now = ++clock_;
                decodes_.emplace_back(missing, std::async(std::launch::async, [this, missing, now] {
                    return EnsureTile(missing, now);
                }));
            }
        }
    }

    int32_t NextMissingTileExcluding(const Camera& cam, int32_t exclude)
    {
        // Screen first, then the wider prefetch band.
        int32_t file = PickTile(cam, false, exclude);
        if (file < 0) {
            file = PickTile(cam, true, exclude);
        }
        return file;
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

    // First tile of the current selection that is not decoded yet.
    // Priority: tiles closest to the view axis first (that is where the user is
    // looking, and where an empty hole is most obvious), then nearest.
    int32_t NextMissingTile(const Camera& cam)
    {
        return NextMissingTileExcluding(cam, -1);
    }

    // Wider-than-screen prefetch frustum: tiles just outside the view are decoded
    // before they become visible.
    void WideProj(const Camera& cam, float* out) const
    {
        for (int i = 0; i < 16; i++) {
            out[i] = cam.proj[i];
        }
        constexpr float kMargin = 0.65f;  // 1/0.65 ~ 1.54x wider
        out[0] *= kMargin;
        out[5] *= kMargin;
    }

    int32_t PickTile(const Camera& cam, bool wide, int32_t exclude)
    {
        if (previousLevels_.size() != scene_.leaves.size()) {
            previousLevels_.assign(scene_.leaves.size(), (int8_t)-1);
        }
        Selection sel;
        if (wide) {
            float proj[16];
            WideProj(cam, proj);
            SelectVisible(scene_, cam.view, proj, cam.camPos, budget_, previousLevels_.data(), sel);
        } else {
            SelectVisible(scene_, cam.view, cam.proj, cam.camPos, budget_, previousLevels_.data(), sel);
        }

        // View direction (camera looks along -back = -(row 2 of the view matrix)).
        float dx = -cam.view[2], dy = -cam.view[6], dz = -cam.view[10];
        const float dlen = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dlen > 1e-6f) {
            dx /= dlen;
            dy /= dlen;
            dz /= dlen;
        }

        int32_t bestFile = -1;
        float bestScore = 0.0f;
        for (const auto& it : sel.items) {
            const Slice& slice = scene_.leaves[it.leaf].lods[it.level];
            if (slice.file < 0 || slice.file == exclude || TileReady(slice.file)) {
                continue;
            }
            const Leaf& leaf = scene_.leaves[it.leaf];
            const float cx = (leaf.bmin[0] + leaf.bmax[0]) * 0.5f;
            const float cy = (leaf.bmin[1] + leaf.bmax[1]) * 0.5f;
            const float cz = (leaf.bmin[2] + leaf.bmax[2]) * 0.5f;
            float vx = cx - cam.camPos[0], vy = cy - cam.camPos[1], vz = cz - cam.camPos[2];
            const float vlen = std::sqrt(vx * vx + vy * vy + vz * vz);
            if (vlen < 1e-3f) {
                return slice.file;
            }
            vx /= vlen;
            vy /= vlen;
            vz /= vlen;
            const float along = vx * dx + vy * dy + vz * dz;  // 1 on the view axis
            // Prefer centred tiles, break ties with distance.
            const float score = along - 0.001f * vlen;
            if (bestFile < 0 || score > bestScore) {
                bestFile = slice.file;
                bestScore = score;
            }
        }
        return bestFile;
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
        if (baseFiles_.count(file) != 0) {
            pinned_.insert(file);
        }
        dirty_ = true;
        LOGI("LOD: decoded tile %{public}d (%{public}zu gaussians, %{public}.0f ms)", file, entry->data.size(), ms);
        EvictLocked();
        return true;
    }

    void EvictLocked()
    {
        while (cacheCount_ > cacheBudget_ && caches_.size() > 1) {
            auto oldest = caches_.end();
            for (auto it = caches_.begin(); it != caches_.end(); ++it) {
                if (pinned_.count(it->first) != 0) {
                    continue;  // pinned (coarse base) tiles stay
                }
                // Tiles the current selection depends on must not be evicted:
                // re-decoding them on the next frame is what made the picture
                // flicker between coarse and fine while the camera was still.
                if (selectionFiles_.count(it->first) != 0) {
                    continue;
                }
                if (oldest == caches_.end() || it->second->lastUse < oldest->second->lastUse) {
                    oldest = it;
                }
            }
            if (oldest == caches_.end()) {
                return;  // everything is pinned
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
    // Rebuilds the drawn set from the current selection. Blocks are owned per leaf
    // in a shared pool: when a leaf's level changes, only its block is rewritten
    // (allocated / reused) and only those slots are uploaded. The rest of the pool
    // is untouched, so nothing "shrinks" and nothing is rebuilt globally.
    void BuildView(const Camera& cam, bool /*force*/)
    {
        const auto buildBegin = std::chrono::steady_clock::now();
        if (previousLevels_.size() != scene_.leaves.size()) {
            previousLevels_.assign(scene_.leaves.size(), (int8_t)-1);
        }
        Selection sel;
        const auto tSel0 = std::chrono::steady_clock::now();
        SelectVisible(scene_, cam.view, cam.proj, cam.camPos, budget_, previousLevels_.data(), sel);
        const auto tSel1 = std::chrono::steady_clock::now();

        // If the selection did not change and no tile arrived, there is nothing to
        // do. Rebuilding the live list (~1M entries) and republishing on every
        // camera frame starves the decoder threads.
        {
            uint64_t h = 1469598103934665603ull ^ (uint64_t)sel.items.size();
            for (const auto& it : sel.items) {
                h = (h ^ ((uint64_t)it.leaf * 16u + (uint64_t)it.level)) * 1099511628211ull;
            }
            const uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            const bool changed = (h != lastBuiltHash_);
            const bool throttled = !dirty_ && (nowMs - lastBuildMs_ < 150u);
            // Never skip while a tile just landed: the *selection* (which leaves
            // and levels) does not change when a tile finishes decoding, so a hash
            // check alone would decode tiles and never use them.
            if (!changed && !dirty_) {
                return;
            }
            if (throttled && !dirty_) {
                return;
            }
            lastBuiltHash_ = h;
            lastBuildMs_ = nowMs;
        }

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

        if (writeIndex_ < 0) {
            writeIndex_ = 0;
        }
        ++buildId_;
        selectedLeaves_.clear();
        selectionFiles_.clear();
        for (const auto& item : sel.items) {
            selectedLeaves_.insert(item.leaf);
            const Leaf& leaf = scene_.leaves[item.leaf];
            for (int lvl = item.level; lvl < leaf.levels; lvl++) {
                if (leaf.lods[lvl].file >= 0) {
                    selectionFiles_.insert(leaf.lods[lvl].file);
                }
            }
        }
        dirtyRanges_.clear();
        uint32_t skippedNoTile = 0;
        uint32_t replaced = 0;

        for (const auto& it : sel.items) {
            const int level = resolveLevel(it.leaf, it.level);
            if (level < 0) {
                skippedNoTile++;
                continue;
            }
            const Slice& slice = scene_.leaves[it.leaf].lods[level];
            if (slice.file < 0 || slice.count == 0) {
                continue;
            }
            auto found = leafEntry_.find(it.leaf);
            Entry entry = (found != leafEntry_.end()) ? found->second : Entry {};
            if (entry.level == level && entry.count == slice.count && entry.start + entry.count <= poolTop_) {
                entry.lastUse = buildId_;
                leafEntry_[it.leaf] = entry;
                continue;
            }
            if (!EnsureTile(slice.file, ++clock_)) {
                continue;
            }
            CacheEntryPtr tile;
            {
                std::lock_guard<std::mutex> lock(cacheMutex_);
                auto cached = caches_.find(slice.file);
                if (cached == caches_.end()) {
                    continue;
                }
                tile = cached->second;
            }
            if (slice.offset + slice.count > tile->data.size()) {
                continue;
            }

            // Reuse the existing block when it is big enough, otherwise allocate.
            uint32_t start = 0;
            bool reused = false;
            if (entry.count >= slice.count && entry.start + entry.count <= poolTop_) {
                start = entry.start;
                reused = true;
                if (entry.count > slice.count) {
                    FreeBlock(entry.start + slice.count, entry.count - slice.count);
                }
            } else {
                // Growing: release the old block first so the pool only needs the
                // larger of the two sizes at any time.
                if (entry.count > 0 && entry.start + entry.count <= poolTop_) {
                    FreeBlock(entry.start, entry.count);
                    entry = Entry {};
                }
                if (!AllocBlock(slice.count, start)) {
                    continue;  // pool exhausted and nothing evictable
                }
            }

            std::memcpy(&buffers_[writeIndex_][start], &tile->data[slice.offset],
                (size_t)slice.count * sizeof(GpuGaussianData));
            dirtyRanges_.push_back(start);
            dirtyRanges_.push_back(slice.count);

            entry.level = (int8_t)level;
            entry.start = start;
            entry.count = slice.count;
            entry.lastUse = buildId_;
            leafEntry_[it.leaf] = entry;
            if (!reused) {
                replaced++;
            }
        }

        const auto tBlocks = std::chrono::steady_clock::now();

        // Live list = the gaussians of the selected leaves only (no duplicates).
        // Live set as ranges (one pair per selected leaf): publishing a per-gaussian
        // index list cost tens of milliseconds per rebuild plus a 4 MB copy per
        // publish, which starved everything else.
        liveRanges_.clear();
        liveRanges_.reserve(sel.items.size() * 2u);
        uint32_t liveTotal = 0;
        for (const auto& it : sel.items) {
            auto found = leafEntry_.find(it.leaf);
            if (found == leafEntry_.end()) {
                continue;
            }
            const Entry& e = found->second;
            if (e.count == 0 || e.start + e.count > poolTop_) {
                continue;
            }
            liveRanges_.push_back(e.start);
            liveRanges_.push_back(e.count);
            liveTotal += e.count;
        }
        liveCount_ = liveTotal;

        for (const auto& it : sel.items) {
            previousLevels_[it.leaf] = (int8_t)it.level;
        }

        const auto tLive = std::chrono::steady_clock::now();

        // The live *content* can change without changing the count (leaves enter /
        // leave the frustum), so compare a hash of the selection as well.
        uint64_t hash = 1469598103934665603ull ^ (uint64_t)liveCount_;
        for (const auto& it : sel.items) {
            hash = (hash ^ ((uint64_t)it.leaf * 16u + (uint64_t)it.level)) * 1099511628211ull;
        }
        lastLiveHash_ = hash;
        const bool liveChanged = liveCount_ != publishLiveCount_ || hash != publishLiveHash_;
        if (dirtyRanges_.empty() && !liveChanged) {
            return;
        }
        Publish();
        const double buildMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - buildBegin).count();
        auto msBetween = [](std::chrono::steady_clock::time_point a,
                             std::chrono::steady_clock::time_point b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        LOGI("LOD: set %{public}zu live gaussians (pool %{public}u, %{public}u dirty ranges, "
             "%{public}u new blocks, no-tile %{public}u, %{public}.0f ms = sel %{public}.0f + "
             "blocks %{public}.0f + live %{public}.0f) gen 0x%{public}x",
            (size_t)liveCount_, poolTop_, (unsigned)(dirtyRanges_.size() / 2), replaced, skippedNoTile,
            buildMs, msBetween(tSel0, tSel1), msBetween(tSel1, tBlocks), msBetween(tBlocks, tLive),
            publishGeneration_);
    }

    // First-fit block allocation from the free list, then from the bump pointer.
    bool AllocBlock(uint32_t count, uint32_t& start)
    {
        if (TryAlloc(count, start)) {
            return true;
        }
        // Pool exhausted: release blocks that are not part of this selection,
        // least recently used first. Membership is a hash set (building it inside
        // the victim search made every allocation O(entries * selection) and
        // effectively froze the streamer once the pool filled up).
        while (true) {
            auto victim = leafEntry_.end();
            for (auto it = leafEntry_.begin(); it != leafEntry_.end(); ++it) {
                if (it->second.count == 0 || selectedLeaves_.count(it->first) != 0) {
                    continue;
                }
                if (victim == leafEntry_.end() || it->second.lastUse < victim->second.lastUse) {
                    victim = it;
                }
            }
            if (victim == leafEntry_.end()) {
                return false;
            }
            FreeBlock(victim->second.start, victim->second.count);
            victim->second = Entry {};
            if (TryAlloc(count, start)) {
                return true;
            }
        }
    }

    bool TryAlloc(uint32_t count, uint32_t& start)
    {
        for (size_t i = 0; i < freeBlocks_.size(); i++) {
            if (freeBlocks_[i].size < count) {
                continue;
            }
            start = freeBlocks_[i].start;
            freeBlocks_[i].start += count;
            freeBlocks_[i].size -= count;
            if (freeBlocks_[i].size == 0) {
                freeBlocks_.erase(freeBlocks_.begin() + (long)i);
            }
            return true;
        }
        if (poolTop_ + count > capacity_) {
            return false;
        }
        start = poolTop_;
        poolTop_ += count;
        return true;
    }

    void FreeBlock(uint32_t start, uint32_t count)
    {
        if (count == 0) {
            return;
        }
        size_t i = 0;
        while (i < freeBlocks_.size() && freeBlocks_[i].start < start) {
            i++;
        }
        freeBlocks_.insert(freeBlocks_.begin() + (long)i, Block { start, count });
        // Coalesce with the next and previous block.
        for (size_t k = 0; k + 1 < freeBlocks_.size();) {
            if (freeBlocks_[k].start + freeBlocks_[k].size == freeBlocks_[k + 1].start) {
                freeBlocks_[k].size += freeBlocks_[k + 1].size;
                freeBlocks_.erase(freeBlocks_.begin() + (long)(k + 1));
            } else {
                k++;
            }
        }
    }

    void Publish()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        writeIndex_ = (writeIndex_ < 0) ? 0 : writeIndex_;
        publishedBuffer_ = writeIndex_;
        publishCount_ = liveCount_;
        publishPoolSize_ = poolTop_;
        publishLiveCount_ = liveCount_;
        publishLiveHash_ = lastLiveHash_;
        publishUploadStart_ = 0;
        ++publishGeneration_;
    }

    Scene scene_;
    uint32_t budget_ = 1000000;
    uint32_t capacity_ = 2000000;
    uint32_t cacheBudget_ = 3000000;

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopped_ = false;
    Camera pending_;
    uint64_t requestId_ = 0;

    std::vector<GpuGaussianData> buffers_[2];
    struct Entry {
        int8_t level = -1;
        uint32_t start = 0;
        uint32_t count = 0;
        uint64_t lastUse = 0;
    };

    struct Block {
        uint32_t start = 0;
        uint32_t size = 0;
    };

    uint32_t publishUploadStart_ = 0;
    uint32_t publishPoolSize_ = 0;
    uint32_t publishLiveCount_ = 0;
    uint64_t publishLiveHash_ = 0;
    uint64_t lastLiveHash_ = 0;
    std::vector<std::pair<int32_t, std::future<bool>>> decodes_;
    int writeIndex_ = -1;
    std::unordered_map<uint32_t, Entry> leafEntry_;  // leaf -> block
    std::vector<Block> freeBlocks_;
    uint32_t poolTop_ = 0;
    std::vector<uint32_t> dirtyRanges_;  // (start, count) pairs
    std::vector<uint32_t> liveRanges_;  // (start, count) pairs of the drawn set
    uint32_t liveCount_ = 0;
    std::unordered_set<uint32_t> selectedLeaves_;
    std::unordered_set<int32_t> selectionFiles_;
    uint64_t buildId_ = 0;
    uint64_t lastBuiltHash_ = 0;
    uint64_t lastBuildMs_ = 0;
    std::atomic<bool> dirty_ { false };
    int publishedBuffer_ = -1;
    uint32_t publishCount_ = 0;
    uint32_t publishGeneration_ = 0;
    uint32_t consumedGeneration_ = 0;

    std::vector<int8_t> previousLevels_;
    std::vector<int8_t> lastSelection_;
    std::mutex cacheMutex_;
    std::unordered_map<int32_t, CacheEntryPtr> caches_;
    std::unordered_set<int32_t> decoding_;
    std::unordered_set<int32_t> baseFiles_;
    std::unordered_set<int32_t> pinned_;
    uint32_t cacheCount_ = 0;
    uint64_t clock_ = 0;
    uint64_t lastBuiltRequest_ = 0;
    bool publishedForRequest_ = false;
    bool basePreloaded_ = false;
};

} // namespace lod
