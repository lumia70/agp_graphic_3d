/*
 * render_node_gaussian_splatting.h
 *
 * Custom RenderNode for 3D Gaussian Splatting rendering.
 */
#pragma once

#include <render/nodecontext/intf_render_node.h>
#include <render/nodecontext/intf_pipeline_descriptor_set_binder.h>
#include <render/device/pipeline_layout_desc.h>
#include <render/namespace.h>
#include <render/resource_handle.h>
#include <base/containers/string.h>
#include <base/containers/unique_ptr.h>
#include <base/containers/vector.h>
#include <base/util/uid.h>

#include <chrono>

RENDER_BEGIN_NAMESPACE()

class IRenderNodeContextManager;

class RenderNodeGaussianSplatting final : public IRenderNode {
public:
    RenderNodeGaussianSplatting() = default;
    ~RenderNodeGaussianSplatting() override;

    // IRenderNode interface
    void InitNode(IRenderNodeContextManager& renderNodeContextMgr) override;
    void PreExecuteFrame() override;
    void ExecuteFrame(IRenderCommandList& cmdList) override;
    ExecuteFlags GetExecuteFlags() const override { return 0U; }

    // for plugin / factory interface
    static constexpr BASE_NS::Uid UID { "a1b2c3d4-e5f6-7890-abcd-ef0123456789" };
    static constexpr const char* TYPE_NAME = "RenderNodeGaussianSplatting";
    static constexpr IRenderNode::BackendFlags BACKEND_FLAGS = IRenderNode::BackendFlagBits::BACKEND_FLAG_BITS_DEFAULT;
    static constexpr IRenderNode::ClassType CLASS_TYPE = IRenderNode::ClassType::CLASS_TYPE_NODE;
    static IRenderNode* Create();
    static void Destroy(IRenderNode* instance);

    struct GpuGaussian {
        float px, py, pz, pad0;
        float sx, sy, sz, opacity;
        float qw, qx, qy, qz;
        float cr, cg, cb, ca;
    };

    // Camera UBO layout (must match shader)
    struct CameraUboData {
        float view[16];
        float proj[16];
        float viewport[4];   // x, y, w, h
        float cameraPos[4];  // xyz + pad
    };

private:
    void ensureSortThread();
    void logSortStats(float totalMs, float cullMs, float histMs, float scatterMs);

    IRenderNodeContextManager* renderNodeContextMgr_ = nullptr;

    // GPU resources
    // Two alternating gaussian SSBOs: LOD re-assembly uploads into the inactive
    // one and only then switches, so the GPU is never handed a buffer that is
    // being overwritten mid-frame (that showed up as flicker while moving).
    RenderHandleReference gaussianBuffer_;
    RenderHandleReference gaussianBufferAlt_;
    bool altActive_ = false;
    // Triple-buffered per-frame written buffers: while the GPU executes up to
    // 2 in-flight frames, the CPU writes the next slot — no torn reads.
    static constexpr uint32_t FRAME_SLOTS = 3;
    RenderHandleReference cameraUbo_[FRAME_SLOTS];
    RenderHandleReference sortIndexBuffer_[FRAME_SLOTS];
    uint32_t currentSlot_ = 0;
    RenderHandleReference shBuffer_;
    RenderHandleReference shParamsBuffer_[FRAME_SLOTS];  // SH weights per frame (binding 4)
    // UNORM offscreen target: raw (code-space) splat compositing, exactly
    // like the web canvas; copied to the sRGB backbuffer by a decode pass.
    RenderHandleReference offscreenImage_;
    uint32_t offscreenWidth_ = 0;
    uint32_t offscreenHeight_ = 0;
    // Copy pass resources
    RenderHandle copyShaderHandle_;
    PipelineLayout copyPipelineLayout_;
    RenderHandle copyPsoHandle_;
    IPipelineDescriptorSetBinder::Ptr copyBinder_;

    // Sort scratch + async sort worker + currently applied order. Defined in
    // the .cpp so the thread/mutex/slot machinery stays out of this header.
    struct SortState;
    SortState* sortState_ = nullptr;
    // Compact cull input (px, py, pz, maxScale = 16 B) built once per upload so
    // the sort's cull pass streams 16 B/gaussian instead of the full 64 B record.
    BASE_NS::vector<float> cullRecords_;
    uint32_t visibleCount_ = 0;
    // Coarse view-change tracking (re-sort threshold)
    float lastDirX_ = 0.f, lastDirY_ = 0.f, lastDirZ_ = -1.f;
    float lastEyeX_ = 0.f, lastEyeY_ = 0.f, lastEyeZ_ = 0.f;
    bool sortBufferUploaded_ = false;
    // Baseline instrumentation: cull+sort cost in milliseconds (logged every
    // kSortLogInterval re-sorts so the numbers stay readable in hilog).
    float lastSortMs_ = 0.0f;
    uint32_t sortCount_ = 0;
    // Aggregated over the last kSortLogInterval sorts (min/avg/max per phase).
    float statTotalMs_[3] = { 1e9f, 0.0f, 0.0f };
    float statCullMs_[3] = { 1e9f, 0.0f, 0.0f };
    float statHistMs_[3] = { 1e9f, 0.0f, 0.0f };
    float statScatterMs_[3] = { 1e9f, 0.0f, 0.0f };
    uint32_t statCount_ = 0;
    // Frame pacing: average interval between ExecuteFrame calls (render thread
    // health; a blocking sort shows up here immediately).
    float execSumMs_ = 0.0f;
    float execMaxMs_ = 0.0f;
    uint32_t execCount_ = 0;

    // Shader + PSO
    RenderHandle shaderHandle_;
    GraphicsState graphicsState_;
    PipelineLayout pipelineLayout_;
    RenderHandle psoHandle_;

    // Descriptor set management
    IPipelineDescriptorSetBinder::Ptr pipelineDescriptorSetBinder_;

    uint32_t gaussianCount_ = 0;
    uint32_t emptyFrameCount_ = 0;
    uint32_t gaussianCapacity_ = 0;
    bool hasShData_ = false;
    // Data generation already uploaded to the GPU (LOD re-assembly).
    uint32_t uploadedGeneration_ = 0;
    bool needsUpload_ = true;
    bool initialized_ = false;

    uint32_t frameCount_ = 0;
};

RENDER_END_NAMESPACE()
