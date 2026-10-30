/*
 * render_node_gaussian_splatting.cpp
 *
 * 3D Gaussian Splatting rendering as a Lume RenderNode.
 * Reads gaussian data from liblumeapp.so via dlsym, uploads to GPU SSBO,
 * and renders billboard quads with 2D gaussian alpha blending.
 */
#include "render_node_gaussian_splatting.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cmath>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include <dlfcn.h>

#include <base/containers/array_view.h>
#include <render/device/intf_gpu_resource_manager.h>
#include <render/device/intf_shader_manager.h>
#include <render/device/pipeline_layout_desc.h>
#include <render/device/pipeline_state_desc.h>
#include <render/nodecontext/intf_render_command_list.h>
#include <render/nodecontext/intf_render_node_context_manager.h>
#include <render/nodecontext/intf_node_context_pso_manager.h>
#include <render/nodecontext/intf_node_context_descriptor_set_manager.h>
#include <render/nodecontext/intf_render_node_util.h>
#include <render/render_data_structures.h>
#include <render/namespace.h>

#include "gaussian_splat_data.h"
#include "default_engine_constants.h"
#include "util/log.h"

using BASE_NS::array_view;
using BASE_NS::countof;

RENDER_BEGIN_NAMESPACE()

// Factory
IRenderNode* RenderNodeGaussianSplatting::Create() { return new RenderNodeGaussianSplatting(); }
void RenderNodeGaussianSplatting::Destroy(IRenderNode* instance) {
    delete static_cast<RenderNodeGaussianSplatting*>(instance);
}

// ---- dlsym helper to read shared globals from liblumeapp.so ----
namespace {

struct SharedPtrs {
    const GpuGaussianData** dataPtr = nullptr;
    uint32_t* countPtr = nullptr;
    bool* readyPtr = nullptr;
    GaussianCameraData* cameraPtr = nullptr;  // direct pointer to struct (not pointer-to-pointer)
    bool* cameraReadyPtr = nullptr;
    const float** shPtr = nullptr;
    uint32_t* capacityPtr = nullptr;
    uint32_t* generationPtr = nullptr;
    uint32_t* uploadedGenerationPtr = nullptr;
    uint32_t* uploadStartPtr = nullptr;
    float* backgroundPtr = nullptr;
    bool resolved = false;

    void Resolve() {
        if (resolved) return;
        void* h = dlopen("liblumeapp.so", RTLD_NOLOAD);
        if (!h) h = dlopen("liblumeapp.so", RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            dataPtr = (const GpuGaussianData**)dlsym(h, "g_gaussianSplatData");
            countPtr = (uint32_t*)dlsym(h, "g_gaussianSplatCount");
            readyPtr = (bool*)dlsym(h, "g_gaussianDataReady");
            cameraPtr = (GaussianCameraData*)dlsym(h, "g_gaussianCameraData");
            cameraReadyPtr = (bool*)dlsym(h, "g_gaussianCameraReady");
            shPtr = (const float**)dlsym(h, "g_gaussianShData");
            capacityPtr = (uint32_t*)dlsym(h, "g_gaussianSplatCapacity");
            generationPtr = (uint32_t*)dlsym(h, "g_gaussianSplatGeneration");
            uploadedGenerationPtr = (uint32_t*)dlsym(h, "g_gaussianSplatUploadedGeneration");
            uploadStartPtr = (uint32_t*)dlsym(h, "g_gaussianSplatUploadStart");
            backgroundPtr = (float*)dlsym(h, "g_gaussianBackground");
        }
        resolved = true;
    }
    bool Ready() const { return readyPtr && *readyPtr && dataPtr && *dataPtr && countPtr && *countPtr > 0; }
    const GpuGaussianData* Data() const { return dataPtr ? *dataPtr : nullptr; }
    uint32_t Count() const { return countPtr ? *countPtr : 0; }
    bool CameraReady() const { return cameraReadyPtr && *cameraReadyPtr && cameraPtr; }
    const GaussianCameraData& Camera() const { return *cameraPtr; }
    const float* Sh() const { return shPtr ? *shPtr : nullptr; }
    uint32_t Capacity() const { return capacityPtr ? *capacityPtr : 0u; }
    uint32_t Generation() const { return generationPtr ? *generationPtr : 0u; }
    uint32_t UploadStart() const { return uploadStartPtr ? *uploadStartPtr : 0u; }
    const float* Background() const { return backgroundPtr; }
    void MarkUploaded(uint32_t generation) const
    {
        if (uploadedGenerationPtr) {
            *uploadedGenerationPtr = generation;
        }
    }
};
SharedPtrs g_sp;

// Build a simple look-at view matrix (column-major for GLSL)
void BuildLookAt(float* out, float ex, float ey, float ez, float tx, float ty, float tz) {
    float ux = 0.f, uy = 1.f, uz = 0.f;
    // forward = normalize(eye - target)
    float fx = ex - tx, fy = ey - ty, fz = ez - tz;
    float fl = sqrtf(fx*fx + fy*fy + fz*fz);
    if (fl > 1e-8f) { fx /= fl; fy /= fl; fz /= fl; }
    // right = normalize(up x forward)
    float rx = uy*fz - uz*fy, ry = uz*fx - ux*fz, rz = ux*fy - uy*fx;
    float rl = sqrtf(rx*rx + ry*ry + rz*rz);
    if (rl > 1e-8f) { rx /= rl; ry /= rl; rz /= rl; }
    // true up = forward x right
    float tux = fy*rz - fz*ry, tuy = fz*rx - fx*rz, tuz = fx*ry - fy*rx;
    // Column-major layout
    out[ 0]=rx;  out[ 1]=tux;  out[ 2]=fx;  out[ 3]=0.f;
    out[ 4]=ry;  out[ 5]=tuy;  out[ 6]=fy;  out[ 7]=0.f;
    out[ 8]=rz;  out[ 9]=tuz;  out[10]=fz;  out[11]=0.f;
    out[12]=-(rx*ex+ry*ey+rz*ez);
    out[13]=-(tux*ex+tuy*ey+tuz*ez);
    out[14]=-(fx*ex+fy*ey+fz*ez);
    out[15]=1.f;
}

// Build a perspective projection matrix (column-major, Vulkan clip [0,1] depth)
void BuildPerspective(float* out, float fovYDeg, float aspect, float zn, float zf) {
    float fovRad = fovYDeg * 3.14159265f / 180.f;
    float f = 1.f / tanf(fovRad * 0.5f);
    std::memset(out, 0, 16 * sizeof(float));
    out[0]  = f / aspect;
    out[5]  = -f;  // Vulkan Y-flip
    out[10] = zf / (zn - zf);
    out[11] = -1.f;
    out[14] = (zn * zf) / (zn - zf);
}

// Number of depth buckets for the back-to-front counting sort.
// 65536 buckets over the visible depth range give sub-millimetre slices —
// effectively an exact depth order (bucket-order errors showed up as
// dark-region banding/stripes in deep gaussian stacks).
constexpr uint32_t SORT_BUCKETS = 65536;

// Persistent worker pool for the sort phases. Threads are created once (worker
// creation costs ~50-100 us each, which matters at 60 fps) and the calling
// thread always takes task 0 so a single-task run does not pay any overhead.
class SortPool {
public:
    static SortPool& Instance()
    {
        static SortPool instance;
        return instance;
    }

    uint32_t Tasks() const { return workerCount_ + 1u; }

    void Run(const uint32_t tasks, const std::function<void(uint32_t)>& job)
    {
        if (workerCount_ == 0u || tasks <= 1u) {
            for (uint32_t t = 0; t < tasks; t++) {
                job(t);
            }
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            job_ = &job;
            taskCount_ = tasks;
            done_ = 0u;
            ++generation_;
        }
        cvStart_.notify_all();
        job(0u);
        std::unique_lock<std::mutex> lock(mutex_);
        cvDone_.wait(lock, [this] { return done_ >= workerCount_; });
        job_ = nullptr;
    }

private:
    SortPool()
    {
        const unsigned hw = std::thread::hardware_concurrency();
        workerCount_ = hw > 1u ? std::min(4u, hw - 1u) : 0u;
        for (uint32_t i = 0; i < workerCount_; i++) {
            threads_.emplace_back([this, i] { Loop(i); });
        }
        PLUGIN_LOG_I("GS: sort pool with %u workers (%u hw threads)", workerCount_, hw);
    }

    ~SortPool()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
            ++generation_;
        }
        cvStart_.notify_all();
        for (auto& thread : threads_) {
            thread.join();
        }
    }

    void Loop(const uint32_t index)
    {
        uint32_t seen = 0u;
        for (;;) {
            const std::function<void(uint32_t)>* job = nullptr;
            uint32_t tasks = 0u;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cvStart_.wait(lock, [this, &seen] { return stop_ || generation_ != seen; });
                if (stop_) {
                    return;
                }
                seen = generation_;
                job = job_;
                tasks = taskCount_;
            }
            if (job) {
                const uint32_t task = index + 1u;
                if (task < tasks) {
                    (*job)(task);
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                ++done_;
            }
            cvDone_.notify_one();
        }
    }

    std::vector<std::thread> threads_;
    std::mutex mutex_;
    std::condition_variable cvStart_;
    std::condition_variable cvDone_;
    const std::function<void(uint32_t)>* job_ = nullptr;
    uint32_t taskCount_ = 0u;
    uint32_t generation_ = 0u;
    uint32_t done_ = 0u;
    uint32_t workerCount_ = 0u;
    bool stop_ = false;
};

// Per-thread-independent scratch for one sort. The gaussian range is split into
// `tasks` slices; each task culls into its own slice, keeps its own histogram
// and scatters with its own bucket cursors, so no atomics are needed.
struct SortScratch {
    uint32_t tasks = 1u;
    uint32_t sliceSize = 0u;
    std::vector<uint32_t> ids;
    std::vector<float> keys;
    std::vector<uint32_t> taskVisible;
    std::vector<float> taskZMin;
    std::vector<float> taskZMax;
    std::vector<uint32_t> cursors;      // bucket-major: [bucket * tasks + task]
    std::vector<uint32_t> bucketCounts;
    std::vector<uint32_t> bucketStarts;
};

struct SortTiming {
    float totalMs = 0.0f;
    float cullMs = 0.0f;
    float histMs = 0.0f;
    float scatterMs = 0.0f;
};

// Frustum + size cull and back-to-front bucket sort. Returns the visible count
// and writes the order into `out` (which must hold `count` entries).
uint32_t ComputeSort(const float* records, const uint32_t count, SortScratch& s, uint32_t* out,
    const float* vm, const float* pm, const float viewportHeight, const float nearZ, const float farZ,
    SortTiming* timing)
{
    const auto sortBegin = std::chrono::steady_clock::now();
    auto ms = [](std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
        return std::chrono::duration<float, std::milli>(b - a).count();
    };

    SortPool& pool = SortPool::Instance();
    const uint32_t tasks = pool.Tasks();
    if (s.tasks != tasks || s.sliceSize != (count + tasks - 1u) / tasks) {
        s.tasks = tasks;
        s.sliceSize = (count + tasks - 1u) / tasks;
        const size_t sliceTotal = (size_t)s.sliceSize * tasks;
        s.ids.resize(sliceTotal);
        s.keys.resize(sliceTotal);
        s.taskVisible.resize(tasks);
        s.taskZMin.resize(tasks);
        s.taskZMax.resize(tasks);
        s.cursors.resize((size_t)SORT_BUCKETS * tasks);
        s.bucketCounts.resize(SORT_BUCKETS);
        s.bucketStarts.resize(SORT_BUCKETS);
    }

    // View rows (column-major): right, up, forward
    const float r0 = vm[0], r1 = vm[4], r2 = vm[8], t0 = vm[12];
    const float u0 = vm[1], u1 = vm[5], u2 = vm[9], t1 = vm[13];
    const float f0 = vm[2], f1 = vm[6], f2 = vm[10], t2 = vm[14];
    // Frustum half-tangents with generous 2x margin (no false culls)
    const float tanX = 2.0f / fabsf(pm[0]);
    const float tanY = 2.0f / fabsf(pm[5]);
    // Size cull: focal_y in pixels for screen radius estimate
    const float focalY = fabsf(pm[5]) * viewportHeight * 0.5f;

    // Pass 1 (parallel): cull + collect visible + per-task depth range.
    pool.Run(tasks, [&](uint32_t t) {
        const uint32_t begin = t * s.sliceSize;
        const uint32_t finish = std::min(begin + s.sliceSize, count);
        uint32_t* ids = s.ids.data() + begin;
        float* keys = s.keys.data() + begin;
        uint32_t visible = 0;
        float zmin = 1e30f, zmax = -1e30f;
        for (uint32_t i = begin; i < finish; i++) {
            const float* rec = records + (size_t)i * 4u;
            const float px = rec[0], py = rec[1], pz = rec[2];
            const float vz = f0*px + f1*py + f2*pz + t2;
            const float z = -vz;  // positive distance in front
            if (z < nearZ || z > farZ) continue;
            const float ms_ = rec[3];
            const float wr = ms_ * 3.f;
            const float vx = r0*px + r1*py + r2*pz + t0;
            if (fabsf(vx) > z * tanX + wr) continue;
            const float vy = u0*px + u1*py + u2*pz + t1;
            if (fabsf(vy) > z * tanY + wr) continue;
            // Projected 2*sqrt(2)*sigma extent below 2 px (minPixelSize=2.0)
            if (focalY * ms_ * 2.83f / z < 2.0f) continue;
            ids[visible] = i;
            keys[visible] = z;
            visible++;
            if (z < zmin) zmin = z;
            if (z > zmax) zmax = z;
        }
        s.taskVisible[t] = visible;
        s.taskZMin[t] = zmin;
        s.taskZMax[t] = zmax;
    });

    uint32_t visibleCount = 0;
    float zmin = 1e30f, zmax = -1e30f;
    for (uint32_t t = 0; t < tasks; t++) {
        visibleCount += s.taskVisible[t];
        if (s.taskZMin[t] < zmin) zmin = s.taskZMin[t];
        if (s.taskZMax[t] > zmax) zmax = s.taskZMax[t];
    }

    const auto afterCull = std::chrono::steady_clock::now();
    auto afterCount = afterCull;

    if (visibleCount > 0) {
        const float range = zmax - zmin;
        const float invRange = (range > 1e-6f) ? (float)SORT_BUCKETS / range : 0.f;

        // Pass 2 (parallel): per-task histogram (bucket-major layout).
        pool.Run(tasks, [&](uint32_t t) {
            uint32_t* hist = s.cursors.data() + t;
            for (uint32_t b = 0; b < SORT_BUCKETS; b++) {
                hist[(size_t)b * tasks] = 0u;
            }
            const uint32_t n = s.taskVisible[t];
            const float* keys = s.keys.data() + (size_t)t * s.sliceSize;
            for (uint32_t j = 0; j < n; j++) {
                uint32_t b = (uint32_t)((zmax - keys[j]) * invRange);
                if (b >= SORT_BUCKETS) b = SORT_BUCKETS - 1u;
                hist[(size_t)b * tasks]++;
            }
        });

        // Reduce (parallel over buckets), short sequential prefix, then
        // absolute per-task cursors (parallel).
        const uint32_t bucketChunk = (SORT_BUCKETS + tasks - 1u) / tasks;
        pool.Run(tasks, [&](uint32_t t) {
            const uint32_t begin = t * bucketChunk;
            const uint32_t finish = std::min(begin + bucketChunk, SORT_BUCKETS);
            for (uint32_t b = begin; b < finish; b++) {
                const uint32_t* counts = s.cursors.data() + (size_t)b * tasks;
                uint32_t sum = 0;
                for (uint32_t u = 0; u < tasks; u++) {
                    sum += counts[u];
                }
                s.bucketCounts[b] = sum;
            }
        });
        uint32_t running = 0;
        for (uint32_t b = 0; b < SORT_BUCKETS; b++) {
            s.bucketStarts[b] = running;
            running += s.bucketCounts[b];
        }
        pool.Run(tasks, [&](uint32_t t) {
            const uint32_t begin = t * bucketChunk;
            const uint32_t finish = std::min(begin + bucketChunk, SORT_BUCKETS);
            for (uint32_t b = begin; b < finish; b++) {
                uint32_t* counts = s.cursors.data() + (size_t)b * tasks;
                uint32_t cursor = s.bucketStarts[b];
                for (uint32_t u = 0; u < tasks; u++) {
                    const uint32_t c = counts[u];
                    counts[u] = cursor;
                    cursor += c;
                }
            }
        });

        afterCount = std::chrono::steady_clock::now();

        // Pass 3 (parallel): scatter each task's slice into the final order.
        pool.Run(tasks, [&](uint32_t t) {
            const uint32_t n = s.taskVisible[t];
            const uint32_t* ids = s.ids.data() + (size_t)t * s.sliceSize;
            const float* keys = s.keys.data() + (size_t)t * s.sliceSize;
            uint32_t* cursors = s.cursors.data() + t;
            for (uint32_t j = 0; j < n; j++) {
                uint32_t b = (uint32_t)((zmax - keys[j]) * invRange);
                if (b >= SORT_BUCKETS) b = SORT_BUCKETS - 1u;
                out[cursors[(size_t)b * tasks]++] = ids[j];
            }
        });
    }

    // Safety net: if the cull removed everything (camera inside geometry, extreme
    // zoom) fall back to the full unsorted set instead of drawing nothing. The
    // projection still clips what is off-screen, this only costs overdraw.
    if (visibleCount == 0 && count > 0) {
        for (uint32_t i = 0; i < count; i++) {
            out[i] = i;
        }
        visibleCount = count;
    }

    if (timing) {
        const auto sortEnd = std::chrono::steady_clock::now();
        timing->totalMs = ms(sortBegin, sortEnd);
        timing->cullMs = ms(sortBegin, afterCull);
        timing->histMs = ms(afterCull, afterCount);
        timing->scatterMs = ms(afterCount, sortEnd);
    }
    return visibleCount;
}


// ---------------------------------------------------------------------------
// DESIGN NOTE: why the depth sort runs on the CPU (recorded for later review)
// ---------------------------------------------------------------------------
// 1. Data locality: gaussians are decoded and uploaded from CPU memory; the
//    sort reuses the same arrays with zero copies.
// 2. Cost profile: O(n) counting sort (2 passes over 750K) ~5-15ms, and it
//    only runs on frames where the camera rotated/panned past a threshold —
//    static scenes cost nothing (render-on-demand + axial zoom skips resort).
// 3. GPU alternatives (for future discussion):
//    - Bitonic sort: O(n log^2 n), ~200+ compute dispatches for 750K — too slow.
//    - GPU radix sort: ~8 scatter rounds + prefix sums + ping-pong buffers —
//      significant complexity, plus a fixed per-frame cost: dispatch +
//      compute->vertex barrier must run even when the camera is static
//      (change detection on GPU would need readback, which is worse).
// 4. CPU-side headroom first: multi-thread the bucket sort (Lume task queue)
//    and/or SIMD the key pass before considering a GPU move.
// ---------------------------------------------------------------------------

}  // namespace

// ===================================================================
// Async sort state
// ===================================================================
// The sort runs on a dedicated thread; the render thread keeps drawing with the
// last finished order (PlayCanvas does the same with a web worker). Requests are
// coalesced: only the newest camera state matters.
struct RenderNodeGaussianSplatting::SortState {
    SortScratch scratch;

    std::mutex mutex;
    std::condition_variable cvStart;
    std::thread thread;
    bool stop = false;

    // Pending request (guarded by mutex)
    bool requestPending = false;
    uint64_t requestId = 0;
    uint64_t finishedId = 0;
    const float* records = nullptr;
    uint32_t count = 0;
    float view[16] = {};
    float proj[16] = {};
    float viewportHeight = 0.0f;
    float nearZ = 0.05f;
    float farZ = 2000.0f;

    // Published result (guarded by mutex)
    std::vector<uint32_t> result;
    uint32_t resultCount = 0;
    SortTiming resultTiming;
    bool resultReady = false;

    // Applied order (render thread only)
    std::vector<uint32_t> applied;
    uint32_t appliedCount = 0;
    bool hasApplied = false;
};

RenderNodeGaussianSplatting::~RenderNodeGaussianSplatting()
{
    if (sortState_) {
        {
            std::lock_guard<std::mutex> lock(sortState_->mutex);
            sortState_->stop = true;
        }
        sortState_->cvStart.notify_all();
        if (sortState_->thread.joinable()) {
            sortState_->thread.join();
        }
        delete sortState_;
        sortState_ = nullptr;
    }
}

void RenderNodeGaussianSplatting::ensureSortThread()
{
    if (!sortState_ || sortState_->thread.joinable()) {
        return;
    }
    sortState_->thread = std::thread([this] {
        SortState& state = *sortState_;
        uint64_t seen = 0;
        for (;;) {
            const float* records = nullptr;
            uint32_t count = 0;
            float view[16] = {};
            float proj[16] = {};
            float viewportHeight = 0.0f;
            float nearZ = 0.05f;
            float farZ = 2000.0f;
            uint64_t id = 0;
            {
                std::unique_lock<std::mutex> lock(state.mutex);
                state.cvStart.wait(lock, [&state, &seen] {
                    return state.stop || (state.requestPending && state.requestId != seen);
                });
                if (state.stop) {
                    return;
                }
                seen = state.requestId;
                id = state.requestId;
                records = state.records;
                count = state.count;
                std::memcpy(view, state.view, sizeof(view));
                std::memcpy(proj, state.proj, sizeof(proj));
                viewportHeight = state.viewportHeight;
                nearZ = state.nearZ;
                farZ = state.farZ;
                state.requestPending = false;
            }

            if (!records || count == 0) {
                continue;
            }
            // The result buffer doubles as the scratch output; it is only read by
            // the render thread under the mutex after publication.
            state.result.resize(count);
            SortTiming timing;
            const uint32_t visible = ComputeSort(records, count, state.scratch, state.result.data(), view,
                proj, viewportHeight, nearZ, farZ, &timing);

            {
                std::lock_guard<std::mutex> lock(state.mutex);
                state.resultCount = visible;
                state.resultTiming = timing;
                state.resultReady = true;
                state.finishedId = id;
            }
        }
    });
}

void RenderNodeGaussianSplatting::logSortStats(float totalMs, float cullMs, float histMs, float scatterMs)
{
#if (RENDER_DEV_ENABLED == 1)
    auto accumulate = [](float (&stat)[3], float v) {
        if (v < stat[0]) stat[0] = v;
        if (v > stat[2]) stat[2] = v;
        stat[1] += v;
    };
    accumulate(statTotalMs_, totalMs);
    accumulate(statCullMs_, cullMs);
    accumulate(statHistMs_, histMs);
    accumulate(statScatterMs_, scatterMs);
    lastSortMs_ = totalMs;
    statCount_++;
    if (statCount_ >= 20u) {
        const float inv = 1.0f / (float)statCount_;
        PLUGIN_LOG_I("GS::Sort stats (%u sorts, %u/%u visible): total min/avg/max %.1f/%.1f/%.1f ms | cull %.1f/%.1f/%.1f | hist %.1f/%.1f/%.1f | scatter %.1f/%.1f/%.1f",
            statCount_, visibleCount_, gaussianCount_,
            statTotalMs_[0], statTotalMs_[1] * inv, statTotalMs_[2],
            statCullMs_[0], statCullMs_[1] * inv, statCullMs_[2],
            statHistMs_[0], statHistMs_[1] * inv, statHistMs_[2],
            statScatterMs_[0], statScatterMs_[1] * inv, statScatterMs_[2]);
        for (float* stat : { statTotalMs_, statCullMs_, statHistMs_, statScatterMs_ }) {
            stat[0] = 1e9f;
            stat[1] = 0.0f;
            stat[2] = 0.0f;
        }
        statCount_ = 0;
    }
#else
    (void)totalMs;
    (void)cullMs;
    (void)histMs;
    (void)scatterMs;
#endif
}

// ===================================================================
// InitNode — called once when the render node graph is created
// ===================================================================
void RenderNodeGaussianSplatting::InitNode(IRenderNodeContextManager& mgr)
{
    renderNodeContextMgr_ = &mgr;
    g_sp.Resolve();
    PLUGIN_LOG_I("GS::InitNode ready=%d count=%u", g_sp.Ready()?1:0, g_sp.Count());

    auto& gpuResMgr = mgr.GetGpuResourceManager();
    auto& shaderMgr = mgr.GetShaderManager();
    const auto& renderNodeUtil = mgr.GetRenderNodeUtil();

    // ---- 1. Create GPU buffers ----

    // Camera UBO (host-visible, updated every frame) — triple buffered
    for (uint32_t s = 0; s < FRAME_SLOTS; s++) {
        GpuBufferDesc uboDesc {};
        uboDesc.usageFlags = CORE_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        uboDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uboDesc.byteSize = sizeof(CameraUboData);
        uboDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        char name[32];
        snprintf(name, sizeof(name), "GS_CamUBO_%u", s);
        cameraUbo_[s] = gpuResMgr.Create(name, uboDesc);

        // SH weights (vec4) — small SSBO per frame slot
        GpuBufferDesc paramDesc {};
        paramDesc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        paramDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        paramDesc.byteSize = 16u;
        paramDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        snprintf(name, sizeof(name), "GS_ShParams_%u", s);
        shParamsBuffer_[s] = gpuResMgr.Create(name, paramDesc);
    }

    // The shader always has binding 3; keep a small dummy when there is no SH
    // data (it is never sampled because both SH weights are zero).
    {
        GpuBufferDesc shDesc {};
        shDesc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        shDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        shDesc.byteSize = 64u;
        shDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        shBuffer_ = gpuResMgr.Create("GS_ShDummy", shDesc);
        hasShData_ = false;
    }

    // Gaussian SSBO (host-visible). LOD scenes re-assemble their visible set, so
    // the buffer is sized for the shared capacity (the frame budget) while only
    // `count` entries are uploaded/drawn.
    if (g_sp.Ready()) {
        const uint32_t cnt = g_sp.Count();
        const uint32_t capacity = g_sp.Capacity() > cnt ? g_sp.Capacity() : cnt;
        GpuBufferDesc desc {};
        desc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        desc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        desc.byteSize = capacity * sizeof(GpuGaussian);
        desc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        gaussianBuffer_ = gpuResMgr.Create("GS_SSBO", desc);
        gaussianBufferAlt_ = gpuResMgr.Create("GS_SSBO_Alt", desc);
        altActive_ = false;
        gaussianCount_ = cnt;
        gaussianCapacity_ = capacity;
        needsUpload_ = true;

        // Sort index buffer (one uint32 per capacity entry) — triple buffered
        for (uint32_t s = 0; s < FRAME_SLOTS; s++) {
            GpuBufferDesc sortDesc {};
            sortDesc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            sortDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            sortDesc.byteSize = capacity * sizeof(uint32_t);
            sortDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
            char name[32];
            snprintf(name, sizeof(name), "GS_SortIdx_%u", s);
            sortIndexBuffer_[s] = gpuResMgr.Create(name, sortDesc);
        }

        // SH coefficient buffer (48 floats per gaussian) when the model has SH.
        if (g_sp.Sh()) {
            GpuBufferDesc shDesc {};
            shDesc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            shDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            shDesc.byteSize = (uint32_t)(capacity * 48ULL * sizeof(float));
            shDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
            shBuffer_ = gpuResMgr.Create("GS_Sh", shDesc);
            hasShData_ = true;
        }

        if (!sortState_) {
            sortState_ = new SortState();
        }

        PLUGIN_LOG_I("GS: SSBO %u/%u gaussians (%u MB), SH=%s", cnt, capacity,
            (uint32_t)(capacity * 64ULL / (1024 * 1024)), hasShData_ ? "yes" : "no");
    }

    // ---- 2. Get shader handle ----
    // The .shader JSON + .spv + .spv.lsb are loaded from "splatshaders://gaussian_render.shader"
    shaderHandle_ = shaderMgr.GetShaderHandle("splatshaders://gaussian_render.shader");
    PLUGIN_LOG_I("GS: shader valid=%d", RenderHandleUtil::IsValid(shaderHandle_) ? 1 : 0);

    if (!RenderHandleUtil::IsValid(shaderHandle_)) {
        PLUGIN_LOG_E("GS: FATAL — shader 'splatshaders://gaussian_render.shader' not found or invalid");
        return;
    }

    // ---- 3. Create pipeline layouts from shader reflection ----
    pipelineLayout_ = renderNodeUtil.CreatePipelineLayout(shaderHandle_);
    PLUGIN_LOG_I("GS: pipelineLayout set count: ds0=%u bindings, ds1=%u bindings",
        (uint32_t)pipelineLayout_.descriptorSetLayouts[0].bindings.size(),
        (uint32_t)pipelineLayout_.descriptorSetLayouts[1].bindings.size());

    // Copy pass layout (offscreen -> sRGB backbuffer)
    copyShaderHandle_ = shaderMgr.GetShaderHandle("splatshaders://gaussian_copy.shader");
    PLUGIN_LOG_I("GS: copy shader valid=%d", RenderHandleUtil::IsValid(copyShaderHandle_) ? 1 : 0);
    if (RenderHandleUtil::IsValid(copyShaderHandle_)) {
        copyPipelineLayout_ = renderNodeUtil.CreatePipelineLayout(copyShaderHandle_);
    }

    // ---- 4. Reserve descriptor sets (ONE combined reserve for both binders) ----
    // ResetAndReserve resets the pool; calling it twice would corrupt the
    // first binder's sets — always reserve all counts in a single call.
    if (RenderHandleUtil::IsValid(copyShaderHandle_)) {
        DescriptorCounts allCounts[] = {
            renderNodeUtil.GetDescriptorCounts(pipelineLayout_),
            renderNodeUtil.GetDescriptorCounts(copyPipelineLayout_),
        };
        mgr.GetDescriptorSetManager().ResetAndReserve({ allCounts, countof(allCounts) });
    } else {
        mgr.GetDescriptorSetManager().ResetAndReserve(renderNodeUtil.GetDescriptorCounts(pipelineLayout_));
    }

    pipelineDescriptorSetBinder_ = renderNodeUtil.CreatePipelineDescriptorSetBinder(pipelineLayout_);

    // Bind our UBO and SSBOs (slot 0 initially; re-bound per frame)
    if (pipelineDescriptorSetBinder_) {
        pipelineDescriptorSetBinder_->BindBuffer(0u, 0u, BindableBuffer { cameraUbo_[0].GetHandle() });
        if (gaussianBuffer_) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 1u,
                BindableBuffer { (altActive_ ? gaussianBufferAlt_ : gaussianBuffer_).GetHandle() });
        }
        if (sortIndexBuffer_[0]) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 2u, BindableBuffer { sortIndexBuffer_[0].GetHandle() });
        }
        if (shBuffer_) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 3u, BindableBuffer { shBuffer_.GetHandle() });
        }
        if (shParamsBuffer_[0]) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 4u, BindableBuffer { shParamsBuffer_[0].GetHandle() });
        }
    }

    // ---- 5. Set up custom graphics state (alpha blending, no depth test) ----
    {
        graphicsState_ = {};
        graphicsState_.inputAssembly.primitiveTopology = PrimitiveTopology::CORE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        graphicsState_.inputAssembly.enablePrimitiveRestart = false;
        graphicsState_.rasterizationState.polygonMode = PolygonMode::CORE_POLYGON_MODE_FILL;
        graphicsState_.rasterizationState.cullModeFlags = CullModeFlagBits::CORE_CULL_MODE_NONE;
        graphicsState_.rasterizationState.frontFace = FrontFace::CORE_FRONT_FACE_COUNTER_CLOCKWISE;
        // No depth test: gaussians are alpha-blended in back-to-front sorted order,
        // correct compositing accumulates color and opacity per pixel.
        graphicsState_.depthStencilState.enableDepthTest = false;
        graphicsState_.depthStencilState.enableDepthWrite = false;

        // Pre-multiplied alpha blending: outColor = src + (1-srcAlpha)*dst
        graphicsState_.colorBlendState.colorAttachmentCount = 1;
        auto& att = graphicsState_.colorBlendState.colorAttachments[0];
        att.enableBlend = true;
        att.srcColorBlendFactor = BlendFactor::CORE_BLEND_FACTOR_ONE;
        att.dstColorBlendFactor = BlendFactor::CORE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp = BlendOp::CORE_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = BlendFactor::CORE_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = BlendFactor::CORE_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.alphaBlendOp = BlendOp::CORE_BLEND_OP_ADD;
        att.colorWriteMask = ColorComponentFlagBits::CORE_COLOR_COMPONENT_R_BIT |
                             ColorComponentFlagBits::CORE_COLOR_COMPONENT_G_BIT |
                             ColorComponentFlagBits::CORE_COLOR_COMPONENT_B_BIT |
                             ColorComponentFlagBits::CORE_COLOR_COMPONENT_A_BIT;
    }

    // ---- 6. Create PSO (using GraphicsState& overload, no need for a handle) ----
    {
        constexpr DynamicStateEnum DYNAMIC_STATES[] = {
            CORE_DYNAMIC_STATE_ENUM_VIEWPORT,
            CORE_DYNAMIC_STATE_ENUM_SCISSOR,
        };
        auto& psoMgr = mgr.GetPsoManager();
        psoHandle_ = psoMgr.GetGraphicsPsoHandle(
            shaderHandle_,
            graphicsState_,
            pipelineLayout_,
            {},  // no vertex input (procedural from gl_VertexIndex)
            {},  // no specialization constants
            { DYNAMIC_STATES, countof(DYNAMIC_STATES) }
        );
        PLUGIN_LOG_I("GS: PSO valid=%d", RenderHandleUtil::IsValid(psoHandle_) ? 1 : 0);
    }

    // ---- 7. Copy pass: offscreen -> sRGB backbuffer (decode cancels hw encode) ----
    {
        if (RenderHandleUtil::IsValid(copyShaderHandle_)) {
            copyBinder_ = renderNodeUtil.CreatePipelineDescriptorSetBinder(copyPipelineLayout_);
            if (copyBinder_) {
                // Bind a default image placeholder until the offscreen exists
                // (re-bound with the real offscreen every frame in ExecuteFrame)
                const RenderHandle defImg =
                    gpuResMgr.GetImageHandle(DefaultEngineGpuResourceConstants::CORE_DEFAULT_GPU_IMAGE);
                const RenderHandle defSamp =
                    gpuResMgr.GetSamplerHandle(DefaultEngineGpuResourceConstants::CORE_DEFAULT_SAMPLER_LINEAR_CLAMP);
                BindableImage bi {};
                bi.handle = defImg;
                bi.imageLayout = ImageLayout::CORE_IMAGE_LAYOUT_UNDEFINED;
                copyBinder_->BindImage(0u, 0u, bi);
                copyBinder_->BindSampler(0u, 1u, BindableSampler { defSamp });
            }
            GraphicsState gs2 {};
            gs2.inputAssembly.primitiveTopology = PrimitiveTopology::CORE_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            gs2.rasterizationState.cullModeFlags = CullModeFlagBits::CORE_CULL_MODE_NONE;
            gs2.depthStencilState.enableDepthTest = false;
            gs2.depthStencilState.enableDepthWrite = false;
            gs2.colorBlendState.colorAttachmentCount = 1;
            auto& att2 = gs2.colorBlendState.colorAttachments[0];
            att2.enableBlend = false;  // replace: the offscreen already contains the composited bg
            constexpr DynamicStateEnum DYNAMIC_STATES2[] = {
                CORE_DYNAMIC_STATE_ENUM_VIEWPORT,
                CORE_DYNAMIC_STATE_ENUM_SCISSOR,
            };
            copyPsoHandle_ = mgr.GetPsoManager().GetGraphicsPsoHandle(
                copyShaderHandle_, gs2, copyPipelineLayout_, {}, {},
                { DYNAMIC_STATES2, countof(DYNAMIC_STATES2) });
            PLUGIN_LOG_I("GS: copy PSO valid=%d", RenderHandleUtil::IsValid(copyPsoHandle_) ? 1 : 0);
        }
    }

    initialized_ = true;
    PLUGIN_LOG_I("GS::InitNode complete — gaussians=%u, shader=%d, pso=%d",
        gaussianCount_,
        RenderHandleUtil::IsValid(shaderHandle_) ? 1 : 0,
        RenderHandleUtil::IsValid(psoHandle_) ? 1 : 0);
}

// ===================================================================
// PreExecuteFrame — called each frame before ExecuteFrame
// ===================================================================
void RenderNodeGaussianSplatting::PreExecuteFrame()
{
    if (!initialized_) return;

    // Lazy creation of SSBO if data became ready after InitNode
    if (gaussianCount_ == 0 && g_sp.Ready()) {
        uint32_t cnt = g_sp.Count();
        auto& gpuResMgr = renderNodeContextMgr_->GetGpuResourceManager();
        GpuBufferDesc desc {};
        desc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        desc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        desc.byteSize = cnt * sizeof(GpuGaussian);
        desc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
        gaussianBuffer_ = gpuResMgr.Create("GS_SSBO", desc);
        gaussianCount_ = cnt;
        needsUpload_ = true;

        // Sort index buffer
        // Sort index buffer — triple buffered
        for (uint32_t s = 0; s < FRAME_SLOTS; s++) {
            GpuBufferDesc sortDesc {};
            sortDesc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            sortDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            sortDesc.byteSize = cnt * sizeof(uint32_t);
            sortDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
            sortIndexBuffer_[s] = gpuResMgr.Create("GS_SortIdx_Late", sortDesc);
        }

        if (!sortState_) {
            sortState_ = new SortState();
        }

        PLUGIN_LOG_I("GS: late SSBO creation %u gaussians", cnt);

        // Rebind SSBOs
        if (pipelineDescriptorSetBinder_) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 1u,
                BindableBuffer { (altActive_ ? gaussianBufferAlt_ : gaussianBuffer_).GetHandle() });
            pipelineDescriptorSetBinder_->BindBuffer(0u, 2u, BindableBuffer { sortIndexBuffer_[0].GetHandle() });
        }
    }
}

// ===================================================================
// ExecuteFrame — called each frame, issues GPU commands
// ===================================================================
void RenderNodeGaussianSplatting::ExecuteFrame(IRenderCommandList& cmdList)
{
#if (RENDER_DEV_ENABLED == 1)
    const auto execBegin = std::chrono::steady_clock::now();
#endif
    if (!initialized_) return;

    // Data generation changed (model switch / LOD re-assembly): pick up the new
    // count, grow the buffers if the capacity changed, and re-upload.
    const uint32_t generation = g_sp.Generation();
    if (generation != uploadedGeneration_ && g_sp.Ready()) {
        const uint32_t newCount = g_sp.Count();
        const uint32_t newCapacity = g_sp.Capacity() > newCount ? g_sp.Capacity() : newCount;
        if (newCapacity != gaussianCapacity_ || !gaussianBuffer_) {
            // Recreate the gaussian + sort buffers for the new capacity.
            auto& gpuResMgr = renderNodeContextMgr_->GetGpuResourceManager();
            GpuBufferDesc desc {};
            desc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            desc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            desc.byteSize = newCapacity * sizeof(GpuGaussian);
            desc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
            gaussianBuffer_ = gpuResMgr.Create("GS_SSBO_Dyn", desc);
            gaussianBufferAlt_ = gpuResMgr.Create("GS_SSBO_DynAlt", desc);
            altActive_ = false;
            for (uint32_t s = 0; s < FRAME_SLOTS; s++) {
                GpuBufferDesc sortDesc {};
                sortDesc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                sortDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
                sortDesc.byteSize = newCapacity * sizeof(uint32_t);
                sortDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
                char name[32];
                snprintf(name, sizeof(name), "GS_SortIdx_Dyn_%u", s);
                sortIndexBuffer_[s] = gpuResMgr.Create(name, sortDesc);
            }
            gaussianCapacity_ = newCapacity;
            if (sortState_) {
                sortState_->hasApplied = false;
                sortState_->scratch = SortScratch {};
            }
            sortBufferUploaded_ = false;
            visibleCount_ = 0;
            PLUGIN_LOG_I("GS: recreated buffers for capacity %u", newCapacity);
        }
        const uint32_t uploadStart = g_sp.UploadStart();
        const bool appendOnly = (uploadStart > 0u) && (uploadStart <= newCount) &&
                                (newCapacity == gaussianCapacity_) && (bool)gaussianBuffer_;
        const bool needShBuffer = g_sp.Sh() != nullptr && !hasShData_;
        if (needShBuffer) {
            auto& gpuResMgr = renderNodeContextMgr_->GetGpuResourceManager();
            GpuBufferDesc shDesc {};
            shDesc.usageFlags = CORE_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            shDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_HOST_VISIBLE_BIT | CORE_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            shDesc.byteSize = (uint32_t)(newCapacity * 48ULL * sizeof(float));
            shDesc.engineCreationFlags = CORE_ENGINE_BUFFER_CREATION_DYNAMIC_BARRIERS;
            shBuffer_ = gpuResMgr.Create("GS_ShDyn", shDesc);
            hasShData_ = true;
        }
        const bool countChanged = newCount != gaussianCount_;
        gaussianCount_ = newCount;
        hasShData_ = g_sp.Sh() != nullptr;
        needsUpload_ = true;
        if (appendOnly) {
            // The existing prefix is unchanged, so the applied order still covers
            // it: keep drawing it and let the sort thread extend to the new tail.
            sortBufferUploaded_ = false;
        } else {
            if (sortState_) {
                sortState_->hasApplied = false;
            }
            sortBufferUploaded_ = false;
            // A rewritten set invalidates the previous order.
            visibleCount_ = 0;
        }
        uploadedGeneration_ = generation;
        g_sp.MarkUploaded(generation);
    }
    if (gaussianCount_ == 0) return;
    if (!RenderHandleUtil::IsValid(psoHandle_)) {
        if (frameCount_ < 3) {
            PLUGIN_LOG_E("GS::ExecuteFrame: PSO invalid, skipping");
        }
        frameCount_++;
        return;
    }

    auto& gpuResMgr = renderNodeContextMgr_->GetGpuResourceManager();

    // ---- 1. Upload gaussian data ----
    if (needsUpload_ && g_sp.Ready() && gaussianBuffer_) {
        const uint32_t start = g_sp.UploadStart();
        const bool appendOnly = (start > 0u) && (start < gaussianCount_);
        // Appends write into the buffer that is currently being drawn (past the
        // data the GPU reads); full rebuilds go into the alternate buffer and are
        // switched in only after the copy completes.
        RenderHandleReference* target = appendOnly ? (altActive_ ? &gaussianBufferAlt_ : &gaussianBuffer_)
                                                   : (altActive_ ? &gaussianBuffer_ : &gaussianBufferAlt_);
        if (!*target) {
            *target = gaussianBuffer_;
        }
        void* mapped = gpuResMgr.MapBuffer(target->GetHandle());
        if (mapped) {
            const uint32_t first = appendOnly ? start : 0u;
            const size_t bytes = (size_t)(gaussianCount_ - first) * sizeof(GpuGaussian);
            std::memcpy(static_cast<uint8_t*>(mapped) + (size_t)first * sizeof(GpuGaussian),
                static_cast<const uint8_t*>(static_cast<const void*>(g_sp.Data())) +
                    (size_t)first * sizeof(GpuGaussian),
                bytes);
            gpuResMgr.UnmapBuffer(target->GetHandle());
            if (!appendOnly) {
                altActive_ = (*target).GetHandle().id == gaussianBufferAlt_.GetHandle().id;
            }
            PLUGIN_LOG_I("GS: uploaded gaussians [%u,%u) to GPU (%s)", first, gaussianCount_,
                appendOnly ? "append" : (altActive_ ? "alt" : "primary"));
        }
        // Build the compact cull records (position + largest scale).
        {
            const GpuGaussianData* src = g_sp.Data();
            cullRecords_.resize((size_t)gaussianCount_ * 4u);
            float* dst = cullRecords_.data();
            for (uint32_t i = 0; i < gaussianCount_; i++, dst += 4) {
                const float msx = src[i].sx, msy = src[i].sy, msz = src[i].sz;
                dst[0] = src[i].px;
                dst[1] = src[i].py;
                dst[2] = src[i].pz;
                dst[3] = msx > msy ? (msx > msz ? msx : msz) : (msy > msz ? msy : msz);
            }
            PLUGIN_LOG_I("GS: built %u cull records (%u MB)", gaussianCount_,
                (uint32_t)(cullRecords_.size() * sizeof(float) / (1024 * 1024)));
        }

        // Upload SH coefficients (skipped for DC-only models: the shader does not
        // sample the SH buffer when both weights are zero)
        if (shBuffer_ && hasShData_) {
            const size_t shBytes = (size_t)gaussianCount_ * 48ULL * sizeof(float);
            void* shMapped = gpuResMgr.MapBuffer(shBuffer_.GetHandle());
            if (shMapped) {
                if (g_sp.Sh()) {
                    std::memcpy(shMapped, g_sp.Sh(), shBytes);
                } else {
                    std::memset(shMapped, 0, shBytes);
                }
                gpuResMgr.UnmapBuffer(shBuffer_.GetHandle());
                PLUGIN_LOG_I("GS: uploaded SH coefficients (%s)",
                    g_sp.Sh() ? "present" : "absent — zero filled");
            }
        }
        needsUpload_ = false;
    }

    // ---- 2. Get backbuffer and its dimensions ----
    const RenderHandle backbufferHandle = gpuResMgr.GetImageHandle("CORE_DEFAULT_BACKBUFFER");
    if (!RenderHandleUtil::IsValid(backbufferHandle)) {
        if (frameCount_ < 3) {
            PLUGIN_LOG_E("GS: CORE_DEFAULT_BACKBUFFER not found");
        }
        frameCount_++;
        return;
    }
    const GpuImageDesc bbDesc = gpuResMgr.GetImageDescriptor(backbufferHandle);
    const uint32_t width = bbDesc.width;
    const uint32_t height = bbDesc.height;

    // ---- 2b. Create (or recreate) UNORM offscreen target matching backbuffer size ----
    // Raw code-space compositing target: splats blend here exactly like the
    // web canvas (no sRGB decode/encode), then a copy pass transfers it to
    // the sRGB backbuffer with a decode that cancels the hardware encode.
    if (!offscreenImage_ || offscreenWidth_ != width || offscreenHeight_ != height) {
        GpuImageDesc offDesc {};
        offDesc.format = BASE_NS::Format::BASE_FORMAT_R8G8B8A8_UNORM;
        offDesc.width = width;
        offDesc.height = height;
        offDesc.depth = 1;
        offDesc.mipCount = 1;
        offDesc.layerCount = 1;
        offDesc.sampleCountFlags = CORE_SAMPLE_COUNT_1_BIT;
        offDesc.imageTiling = CORE_IMAGE_TILING_OPTIMAL;
        offDesc.usageFlags = CORE_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | CORE_IMAGE_USAGE_SAMPLED_BIT;
        offDesc.memoryPropertyFlags = CORE_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        offDesc.engineCreationFlags = CORE_ENGINE_IMAGE_CREATION_DYNAMIC_BARRIERS;
        offscreenImage_ = gpuResMgr.Create("GS_Offscreen", offDesc);
        offscreenWidth_ = width;
        offscreenHeight_ = height;
        PLUGIN_LOG_I("GS: UNORM offscreen created %ux%u", width, height);
        // Re-bind the offscreen to the copy binder (handle changed)
        if (copyBinder_) {
            BindableImage bi {};
            bi.handle = offscreenImage_.GetHandle();
            bi.imageLayout = ImageLayout::CORE_IMAGE_LAYOUT_UNDEFINED;
            copyBinder_->BindImage(0u, 0u, bi);
        }
    }

    // ---- 3. Update camera UBO ----
    // Pick this frame's buffer slot (triple buffered against in-flight frames)
    const uint32_t slot = currentSlot_;
    currentSlot_ = (currentSlot_ + 1) % FRAME_SLOTS;
    {
        CameraUboData camData {};
        if (g_sp.CameraReady()) {
            // Use shared camera data from app's orbit camera
            const auto& cam = g_sp.Camera();
            std::memcpy(camData.view, cam.view, sizeof(camData.view));
            std::memcpy(camData.proj, cam.proj, sizeof(camData.proj));
            std::memcpy(camData.viewport, cam.viewport, sizeof(camData.viewport));
            std::memcpy(camData.cameraPos, cam.cameraPos, sizeof(camData.cameraPos));
        } else {
            // Fallback: default camera
            const float camX = 0.f, camY = 10.f, camZ = 150.f;
            BuildLookAt(camData.view, camX, camY, camZ,  0.f, 0.f, 0.f);
            BuildPerspective(camData.proj, 60.f, (float)width / (float)height, 1.f, 500.f);
            camData.viewport[0] = 0.f;
            camData.viewport[1] = 0.f;
            camData.viewport[2] = (float)width;
            camData.viewport[3] = (float)height;
            camData.cameraPos[0] = camX;
            camData.cameraPos[1] = camY;
            camData.cameraPos[2] = camZ;
            camData.cameraPos[3] = 1.f;
        }

        void* mapped = gpuResMgr.MapBuffer(cameraUbo_[slot].GetHandle());
        if (mapped) {
            std::memcpy(mapped, &camData, sizeof(camData));
            gpuResMgr.UnmapBuffer(cameraUbo_[slot].GetHandle());
        }

        // SH weights (per-model configuration) go through a small SSBO: adding a
        // field to the camera UBO was not readable through Lume's UBO path.
        if (shParamsBuffer_[slot]) {
            float params[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            if (g_sp.CameraReady()) {
                const auto& cam = g_sp.Camera();
                params[0] = cam.shScale[0];
                params[1] = cam.shScale[1];
            }
            void* pMapped = gpuResMgr.MapBuffer(shParamsBuffer_[slot].GetHandle());
            if (pMapped) {
                std::memcpy(pMapped, params, sizeof(params));
                gpuResMgr.UnmapBuffer(shParamsBuffer_[slot].GetHandle());
            }
        }

        // Depth range for the cull comes from the app camera (big LOD scenes need a
    // much larger far plane than the small demo models).
    float clipNear = 0.05f;
    float clipFar = 2000.0f;
    if (g_sp.CameraReady()) {
        const auto& appCam = g_sp.Camera();
        if (appCam.clip[0] > 0.0f) {
            clipNear = appCam.clip[0];
        }
        if (appCam.clip[1] > clipNear) {
            clipFar = appCam.clip[1];
        }
    }

    // ---- 3.5 Frustum + size cull, back-to-front bucket sort ----
        // Re-sorted only when the view rotated/zoomed past a small threshold;
        // the cull pass reduces both GPU vertex load and sort work.
        if (sortIndexBuffer_[slot] && gaussianCount_ > 0 && g_sp.Data()) {
            const float* vm = camData.view;
            const float* pm = camData.proj;

            // Coarse change detection: re-sort on rotation, or on ANY camera
            // movement large enough to change the cull set. Axial zoom must
            // also trigger a refresh: gaussians culled as sub-pixel at a far
            // distance become large and visible when zooming in, and a frozen
            // cull list would never bring them back.
            float dirX = -vm[2], dirY = -vm[6], dirZ = -vm[10];
            const float dlen = sqrtf(dirX*dirX + dirY*dirY + dirZ*dirZ);
            if (dlen > 1e-8f) { dirX /= dlen; dirY /= dlen; dirZ /= dlen; }
            const float dirDot = dirX*lastDirX_ + dirY*lastDirY_ + dirZ*lastDirZ_;
            const float ex = camData.cameraPos[0], ey = camData.cameraPos[1], ez = camData.cameraPos[2];
            const float dex = ex - lastEyeX_, dey = ey - lastEyeY_, dez = ez - lastEyeZ_;
            const float axial = dex*dirX + dey*dirY + dez*dirZ;
            const float latx = dex - axial*dirX, laty = dey - axial*dirY, latz = dez - axial*dirZ;
            const float latT = sqrtf(latx*latx + laty*laty + latz*latz);
            const float eyeLen = sqrtf(ex*ex + ey*ey + ez*ez);
            const float moveScale = 0.001f * (eyeLen > 1.f ? eyeLen : 1.f);
            const bool viewChanged = !sortBufferUploaded_ ||
                (dirDot < 0.99999f) ||              // rotated ~0.25 degrees
                (latT > moveScale) ||               // lateral (pan / orbit) movement
                (fabsf(axial) > 20.f * moveScale);  // zoom: refresh cull set (2%)

            if (viewChanged && sortState_) {
                lastDirX_ = dirX; lastDirY_ = dirY; lastDirZ_ = dirZ;
                lastEyeX_ = ex; lastEyeY_ = ey; lastEyeZ_ = ez;

                if (!sortState_->hasApplied) {
                    // First sort for this model: run it inline so the very first
                    // frame is correctly ordered.
                    sortState_->applied.resize(gaussianCount_);
                    SortTiming timing;
                    const float nearZ = clipNear;
                    const float farZ = clipFar;
                    sortState_->appliedCount = ComputeSort(cullRecords_.data(), gaussianCount_,
                        sortState_->scratch, sortState_->applied.data(), vm, pm, (float)height, nearZ, farZ,
                        &timing);
                    sortState_->hasApplied = true;
                    visibleCount_ = sortState_->appliedCount;
                    sortBufferUploaded_ = true;
                    logSortStats(timing.totalMs, timing.cullMs, timing.histMs, timing.scatterMs);
                } else {
                    // Hand the newest camera state to the sort thread. Requests
                    // are coalesced: if one is still pending the snapshot is
                    // replaced, and the worker always uses the latest.
                    {
                        std::lock_guard<std::mutex> lock(sortState_->mutex);
                        std::memcpy(sortState_->view, vm, sizeof(sortState_->view));
                        std::memcpy(sortState_->proj, pm, sizeof(sortState_->proj));
                        sortState_->viewportHeight = (float)height;
                        sortState_->nearZ = clipNear;
                        sortState_->farZ = clipFar;
                        sortState_->records = cullRecords_.data();
                        sortState_->count = gaussianCount_;
                        sortState_->requestPending = true;
                        ++sortState_->requestId;
                    }
                    sortState_->cvStart.notify_one();
                    ensureSortThread();
                }
            }

            // Adopt a newly finished order (never blocks on the worker: the
            // worker only holds the mutex while publishing).
            if (sortState_) {
                SortTiming timing;
                bool adopted = false;
                {
                    std::lock_guard<std::mutex> lock(sortState_->mutex);
                    if (sortState_->resultReady) {
                        sortState_->applied.assign(sortState_->result.begin(),
                            sortState_->result.begin() + sortState_->resultCount);
                        sortState_->appliedCount = sortState_->resultCount;
                        sortState_->resultReady = false;
                        adopted = true;
                        timing = sortState_->resultTiming;
                    }
                }
                if (adopted) {
                    sortState_->hasApplied = true;
                    visibleCount_ = sortState_->appliedCount;
                    sortBufferUploaded_ = true;
                    logSortStats(timing.totalMs, timing.cullMs, timing.histMs, timing.scatterMs);
                }
            }

            // Upload the master sort order to THIS frame's slot every frame
            // (slots rotate; even a skipped re-sort must refresh the slot).
            // Tail padded with 0xFFFFFFFF sentinel — shader skips those entries.
            void* sortMapped = gpuResMgr.MapBuffer(sortIndexBuffer_[slot].GetHandle());
            if (sortMapped) {
                if (visibleCount_ > gaussianCount_) {
                    visibleCount_ = 0;
                }
                if (visibleCount_ > 0 && sortState_ && sortState_->hasApplied) {
                    std::memcpy(sortMapped, sortState_->applied.data(), visibleCount_ * sizeof(uint32_t));
                }
                std::memset((uint8_t*)sortMapped + visibleCount_ * sizeof(uint32_t), 0xFF,
                    (size_t)(gaussianCount_ - visibleCount_) * sizeof(uint32_t));
                gpuResMgr.UnmapBuffer(sortIndexBuffer_[slot].GetHandle());
            }
        }
    }

    // ---- 3.6 Early out when nothing is visible ----
    if (visibleCount_ == 0) {
        if (frameCount_ < 3) {
            PLUGIN_LOG_I("GS::ExecuteFrame: no visible gaussians, skipping render pass");
        }
        frameCount_++;
        return;
    }

    // ---- 4. Update descriptor sets ----
    if (pipelineDescriptorSetBinder_) {
        // Bind this frame's slot buffers
        pipelineDescriptorSetBinder_->BindBuffer(0u, 0u, BindableBuffer { cameraUbo_[slot].GetHandle() });
        if (gaussianBuffer_) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 1u,
                BindableBuffer { (altActive_ ? gaussianBufferAlt_ : gaussianBuffer_).GetHandle() });
        }
        if (sortIndexBuffer_[slot]) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 2u, BindableBuffer { sortIndexBuffer_[slot].GetHandle() });
        }
        if (shBuffer_) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 3u, BindableBuffer { shBuffer_.GetHandle() });
        }
        if (shParamsBuffer_[slot]) {
            pipelineDescriptorSetBinder_->BindBuffer(0u, 4u, BindableBuffer { shParamsBuffer_[slot].GetHandle() });
        }

        for (auto refIndex : pipelineDescriptorSetBinder_->GetSetIndices()) {
            const auto descHandle = pipelineDescriptorSetBinder_->GetDescriptorSetHandle(refIndex);
            const auto bindings = pipelineDescriptorSetBinder_->GetDescriptorSetLayoutBindingResources(refIndex);
            cmdList.UpdateDescriptorSet(descHandle, bindings);
        }
    }

    // ---- 5. PASS 1: splats -> UNORM offscreen (raw code-space compositing) ----
    // Cleared to the web background color (raw display values) — the offscreen
    // holds the fully composited splat layer, exactly like the web canvas.
    ViewportDesc vp {};
    vp.x = 0.f;
    vp.y = 0.f;
    vp.width = (float)width;
    vp.height = (float)height;
    vp.minDepth = 0.f;
    vp.maxDepth = 1.f;

    ScissorDesc sc {};
    sc.offsetX = 0;
    sc.offsetY = 0;
    sc.extentWidth = width;
    sc.extentHeight = height;

    if (visibleCount_ > 0) {
        RenderPassDesc rpDesc {};
        rpDesc.attachmentCount = 1;
        rpDesc.attachmentHandles[0] = offscreenImage_.GetHandle();
        rpDesc.attachments[0].loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
        rpDesc.attachments[0].storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
        // Raw (gamma-space) background; the app switches it to a sky colour for
        // large scenes (defaults to the web capture's (18,12,6)).
        const float* bg = g_sp.Background();
        rpDesc.attachments[0].clearValue = bg ? ClearValue(bg[0], bg[1], bg[2], 1.0f)
                                              : ClearValue(0.071f, 0.047f, 0.024f, 1.0f);
        rpDesc.renderArea.offsetX = 0;
        rpDesc.renderArea.offsetY = 0;
        rpDesc.renderArea.extentWidth = width;
        rpDesc.renderArea.extentHeight = height;
        rpDesc.subpassCount = 1;
        rpDesc.subpassContents = SubpassContents::CORE_SUBPASS_CONTENTS_INLINE;

        RenderPassSubpassDesc subpassDesc {};
        subpassDesc.colorAttachmentCount = 1;
        subpassDesc.colorAttachmentIndices[0] = 0;

        cmdList.BeginRenderPass(rpDesc, 0u, subpassDesc);
        cmdList.BindPipeline(psoHandle_);
        if (pipelineDescriptorSetBinder_) {
            const uint32_t firstSetIndex = pipelineDescriptorSetBinder_->GetFirstSet();
            cmdList.BindDescriptorSets(firstSetIndex, pipelineDescriptorSetBinder_->GetDescriptorSetHandles());
        }
        cmdList.SetDynamicStateViewport(vp);
        cmdList.SetDynamicStateScissor(sc);

        // Visible gaussians only (CPU frustum + size culled, sorted back-to-front)
        const uint32_t vertexCount = visibleCount_ * 6u;
        cmdList.Draw(vertexCount, 1u, 0u, 0u);
        cmdList.EndRenderPass();
    } else {
        // Nothing visible — clear the offscreen to bg directly (no splat draw)
        RenderPassDesc rpDesc {};
        rpDesc.attachmentCount = 1;
        rpDesc.attachmentHandles[0] = offscreenImage_.GetHandle();
        rpDesc.attachments[0].loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_CLEAR;
        rpDesc.attachments[0].storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
        rpDesc.attachments[0].clearValue = ClearValue(0.071f, 0.047f, 0.024f, 1.0f);
        rpDesc.renderArea.offsetX = 0;
        rpDesc.renderArea.offsetY = 0;
        rpDesc.renderArea.extentWidth = width;
        rpDesc.renderArea.extentHeight = height;
        rpDesc.subpassCount = 1;
        rpDesc.subpassContents = SubpassContents::CORE_SUBPASS_CONTENTS_INLINE;
        RenderPassSubpassDesc subpassDesc {};
        subpassDesc.colorAttachmentCount = 1;
        subpassDesc.colorAttachmentIndices[0] = 0;
        cmdList.BeginRenderPass(rpDesc, 0u, subpassDesc);
        cmdList.SetDynamicStateViewport(vp);
        cmdList.SetDynamicStateScissor(sc);
        cmdList.EndRenderPass();
    }

    // ---- 6. PASS 2: fullscreen copy offscreen -> sRGB backbuffer ----
    // The copy shader decodes the raw values; the sRGB hardware encode on
    // write cancels it — stored code == offscreen raw value == web output.
    if (RenderHandleUtil::IsValid(copyPsoHandle_) && copyBinder_) {
        // Re-bind the offscreen (may have been recreated on resize)
        BindableImage bi {};
        bi.handle = offscreenImage_.GetHandle();
        bi.imageLayout = ImageLayout::CORE_IMAGE_LAYOUT_UNDEFINED;
        copyBinder_->BindImage(0u, 0u, bi);
        for (auto refIndex : copyBinder_->GetSetIndices()) {
            const auto descHandle = copyBinder_->GetDescriptorSetHandle(refIndex);
            const auto bindings = copyBinder_->GetDescriptorSetLayoutBindingResources(refIndex);
            cmdList.UpdateDescriptorSet(descHandle, bindings);
        }

        RenderPassDesc rpDesc2 {};
        rpDesc2.attachmentCount = 1;
        rpDesc2.attachmentHandles[0] = backbufferHandle;
        rpDesc2.attachments[0].loadOp = AttachmentLoadOp::CORE_ATTACHMENT_LOAD_OP_DONT_CARE;
        rpDesc2.attachments[0].storeOp = AttachmentStoreOp::CORE_ATTACHMENT_STORE_OP_STORE;
        rpDesc2.renderArea.offsetX = 0;
        rpDesc2.renderArea.offsetY = 0;
        rpDesc2.renderArea.extentWidth = width;
        rpDesc2.renderArea.extentHeight = height;
        rpDesc2.subpassCount = 1;
        rpDesc2.subpassContents = SubpassContents::CORE_SUBPASS_CONTENTS_INLINE;

        RenderPassSubpassDesc subpassDesc2 {};
        subpassDesc2.colorAttachmentCount = 1;
        subpassDesc2.colorAttachmentIndices[0] = 0;

        cmdList.BeginRenderPass(rpDesc2, 0u, subpassDesc2);
        cmdList.BindPipeline(copyPsoHandle_);
        const uint32_t firstSetIndex2 = copyBinder_->GetFirstSet();
        cmdList.BindDescriptorSets(firstSetIndex2, copyBinder_->GetDescriptorSetHandles());
        cmdList.SetDynamicStateViewport(vp);
        cmdList.SetDynamicStateScissor(sc);
        cmdList.Draw(3u, 1u, 0u, 0u);  // fullscreen triangle
        cmdList.EndRenderPass();
    }

#if (RENDER_DEV_ENABLED == 1)
    {
        const float execMs = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - execBegin).count();
        execSumMs_ += execMs;
        if (execMs > execMaxMs_) {
            execMaxMs_ = execMs;
        }
        if (++execCount_ >= 60u) {
            const float avg = execSumMs_ / (float)execCount_;
            PLUGIN_LOG_I("GS::ExecuteFrame cost: avg %.2f ms, max %.2f ms over %u frames (sort avg %.1f ms)",
                avg, execMaxMs_, execCount_, lastSortMs_);
            execSumMs_ = 0.0f;
            execMaxMs_ = 0.0f;
            execCount_ = 0;
        }
    }
#endif

    if (frameCount_ < 3 || (frameCount_ % 300 == 0)) {
        PLUGIN_LOG_I("GS::ExecuteFrame #%u: %u/%u visible, %ux%u",
            frameCount_, visibleCount_, gaussianCount_, width, height);
    }
    frameCount_++;
}

RENDER_END_NAMESPACE()
