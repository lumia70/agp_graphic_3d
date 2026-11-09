# LumeGS 2x 渲染：现状、三个遗留问题分析与下一步（交接）

> 更新时间：2026-11-09
> 基线 commit：`c9c5485c`（fix: lod not render well on 2x gsScaleFactor pipeline）
> 工作区状态：基线 `3dc3764e`（2x 白屏/B 回切/Doll 缓存/异步解码）；当前工作区含 2x jitter 修复（未提交）
> 关联文档：[LumeGS 接入现状](lumegs-integration-status.md)、[tiling host 渐进加载与首帧经验](lumegs-tiling-host-lessons.md)

---

## 1. 一句话现状

| 项 | 状态 |
| --- | --- |
| LumeGS tiling LOD 主链路（渐进加载 / coarse fallback / ring preload / 状态机） | ✅ 已跑通并提交（`111a7895`/`90054941`/`255ed734`） |
| 1x 全分辨率（`gsScaleFactor=1` + `LUMEAPP_GS_TILING_FULL_RES=1`） | ✅ 稳定，出画正常 |
| 2x 默认分辨率（系统侧同款默认值） | ✅ 白屏根因已修复并提交（`c9c5485c`），可出画（截图 mean ≈ 117.5） |
| Doll → City 冷启动白屏 | ✅ 根因已确认并修复（§4.2），设备 8/8 冷启动 + 8/8 回切通过（未提交） |
| 切 Doll 后 5s 内滑动卡顿 | ✅ 已解决：SplatData 单条缓存 + `g_forceRenderFrames` 30→8 + 首次加载 worker 异步解码；切换帧 <100ms（§4.3） |
| 2x 画质 | 🟡 主要问题已修复：jitter 累积被 `needsRender`/force=3 截断；修复后 City edge 10.7→13.16（1x 14.3），残留轻微 softness/亮度差（§4.1） |
| 设备安装版本 | ✅ 已安装当前工作区构建（含 B 修复 + 临时日志） |

三个遗留问题（用户 2026-11-08 反馈）：

1. 2x 默认颜色比 1x 怪，精细度也差一点；
2. 切到小模型（doll）再切回 LOD，能出画但**整个画面偏白**；
3. 切到小模型后**第一次滑动卡约 1s**，之后正常。

---

## 2. 已修复：2x tiling 目标被「缩放两次」

### 2.1 根因

```text
GsSystem::Update()                          lume/LumeGS/src/systems/gs_system.cpp
  ├─ upscaleRatio        = gsScaleFactor * xegUpscaleFactor        // 2 * 1 = 2
  ├─ effectiveRenderSize = renderResolutionFromCC_ / upscaleRatio  // 1280x2598 -> 640x1299
  ├─ previousResolution_ = effectiveRenderSize                     // 存的是"已缩放"尺寸
  └─ tiling-only 分支
       CreateGsRenderTargets(..., previousResolution_)             // ← 640x1299
         └─ CreateGsRenderImages()                                 // 内部再除 gsScaleFactor
              renderSize = resolution / scaleFactor / upscaleFactor // 640/2 -> 320x650
```

render node 仍按 640x1299 绘制，composite 采样 320x650 → 全白。

### 2.2 修复（已提交）

- `lume/LumeGS/src/systems/gs_system.cpp`：tiling 分支传 `renderResolutionFromCC_`，由
  `CreateGsRenderImages` 自己除缩放系数（`gs_system.cpp:892-896`）。
- `lume/LumeGS/src/util/gs_loading.cpp`：`GsRenderStatusUbo renderStatusUbo {};` 零初始化，
  并显式设置 `enableXegUpscale`（`gs_loading.cpp:209-220`）。
  未初始化值非零会让 2x composite 去采样 `uGSTex1`（XEG 输出，本工程未启用）。
- 回到 2x 默认：`render_main.cpp` 删除两处 `gs->gsScaleFactor = 1;`，
  `CMakeLists.txt` 删除 `LUMEAPP_GS_TILING_FULL_RES=1`。

修复后：`GS diag sys: res=1280,2598 prev=640,1299 scale=2`，target 640x1299，
截图 mean ≈ 117.5（正常地形 110–125）。

### 2.3 已排除

- 不是 host/LumeGS 混编问题（旧/新 host × 旧/新 LumeGS 四组矩阵都白屏）。
- 不是 shader/assets 回归（`0c0fc610` 之后未变）。
- 不是 pool/SOG/tile 未加载（诊断日志全部正常）。

---

## 3. 关键运行机制（分析三个问题的前提）

### 3.1 2x composite 的两种路径

`lume/LumeGS/assets/app/shaders/shader/gs3d_gaussian_splatting.frag`：

| 条件 | 路径 | 说明 |
| --- | --- | --- |
| `renderScaleFactor <= 1.001` | 直接采样 `uGSTex0` | 1x |
| `enableXegUpscale==1 && renderScaleFactor>1.001` | 采样 `uGSTex1` | XEG，本工程关闭 |
| `jitterFrameRendered != 4` | 对 `uGSTex0` 做 5-tap 模糊 | 预览/运动帧 |
| `jitterFrameRendered == 4` | 4 张 jitter target 按子像素混合 | 静态收敛帧 |

- `UpdateRenderStatusUbo` 每渲染帧用 push constant 覆盖 status UBO
  （`render_node_gs_render.cpp:902-933`），写入 `jitterRendered_ + 1`。
- 渲染目标按 `jitterRendered_` 轮转 0..3（`SetGsColorAttachment`，`render_node_gs_render.cpp:2372-2405`）。
- `useRenderIfDirty_` 是 `static constexpr bool { true }`
  （`render_node_gs_render.h:566`）：**相机静止**时会连续渲染 4 帧 jitter 后停在
  `jitterRendered_==4`，composite 使用 4 帧混合；**相机运动**时走 preview 帧，
  每次把 `jitterRendered_` 置 0，composite 只能走 5-tap 模糊路径。
- `SetGsScaleFactor` 只生效一次（`render_data_store_default_gs.cpp:610-619`），
  同一个 data store 生命周期内无法按模式切换 1x/2x。
- `GsComponent` 默认值（`api/plugin_gs/ecs/components/gs_component.h`）：
  `gsScaleFactor=2`、`gsEnableDepthPass=false`、`convertGsColorSrgb=true`。
  app 在 `render_main.cpp:1077-1079` 覆盖为 `covariance=2.25`、`sharpening=0.75`
  （对齐系统侧 GSPlugin）。

### 3.2 模型切换与 render data 双缓冲

- `LoadGsLodScene()` **先** `DestroyGsLodTiles()`，**后** `DestroyGsRenderPath()`
  （`render_main.cpp:2082-2088`）。
- `DestroyGsLodTiles()` 对 `IGsTilingSystem` 做 `Uninitialize()/Initialize()`
  （`render_main.cpp:1545-1550`），`Uninitialize()` 会销毁 `poolEntity_`
  （`gs_tiling_system.cpp:1277-1319`），重入时 `Initialize()` 创建**新的 pool entity**。
- `GsTilingSystem::InitializePool()` 用 `GetSafeLoadingGsRenderData()` 把当前 active
  render data **整体浅拷贝**到 loading side（`gs_tiling_system.cpp:1911`），
  然后 `RegisterPoolPrimitive()` 立刻 `FlipValidGsRenderData()`
  （`gs_tiling_system.cpp:1880-1887`）。
- `CloneGsRenderData()` 是 `*to = *from` 浅拷贝
  （`render_data_store_default_gs.cpp:1282`），会连同
  `gsMaterial` / `gsMesh` / `gaussianRenderTarget0..3` / `gaussianRenderTargetStatusUbo` /
  `isMaterialValid` / `convertGsColorSrgb` / `gsEnableDepthPass` 一起复制。
- `GS::CreateGsRenderTargets()` 的早退条件只有一个：
  `if (renderData->isMaterialValid && !resolutionRecreate) return;`
  （`gs_loading.cpp:376-380`）。**它不检查 host entity 是否变化、mesh 是否真的还在。**

---

## 4. 三个遗留问题分析

### 4.1 问题 A：2x 颜色偏怪、精细度略差于 1x

**已确认主因：4 帧 jitter 累积被 app 的渲染门控截断，2x composite 停在
5-tap 模糊路径，而不是 4 帧子像素重建。**

#### 设备 A/B（同一默认视角，静止截图）

1x 对照通过临时 `gs->gsScaleFactor=1` 构建：

| 场景 | 指标 | 1x | 2x 修复前 | 2x 修复后 |
| --- | --- | --- | --- | --- |
| City | edge 强度 | 14.3 | **10.7** | **13.16** |
| City | RGB mean | 116.1,121.3,112.6 | 117.5,122.8,113.6 | 117.5,122.7,113.6 |
| City | saturation | 50.9 | 50.2 | 50.8 |
| Doll | 与 1x diff | — | 0.03 | 0.09 |

原因链：

- composite 只有在 `jitterFrameRendered == 4` 时才走 4 帧重建，否则走
  `uGSTex0` 的 5-tap 模糊上采样。
- 到 4 需要连续 4 个 accum 帧，每帧还可能穿插 tiled precompute。
- `DrawFrame` 原来只用 `viewChanged || g_forceRenderFrames > 0` 决定是否
  `RenderFrame`，**忽略 `TickFrame` 返回的 `needsRender`**；触摸又只给
  `g_forceRenderFrames = 3`。3 帧不足以完成 4 相位（还要 sort/precompute），
  `keepDirty` 的后续请求也被丢掉 → 停在 j=0..2 → 一直模糊。
- LOD 状态机的 sort/precompute 还会把 `jitterRendered_` 重置为 0，进一步拉长
  收敛时间。

#### 修复（未提交）

第一版把 `needsRender` 纳入 `doRender` 并让触摸强制 8 帧，虽然把 City edge 提到
13.16，但引入了两个回归：Doll 每次滑动都渲染 8 帧（~25ms/帧）导致输入延迟；
LOD 在滑动期间连续渲染/选层，出现回弹。最终方案改为**只在手指抬起后 settle**：

- `render_main.cpp`：
  - `g_forceRenderFrames > 0` 时调用 `ecs->RequestRender()`，保证强制帧执行；
  - MOVE：`g_forceRenderFrames = 1`（相机变化本身会出一帧，拖动期间不长时间占用 UI 线程）；
  - UP/CANCEL：LOD 设置 24 帧、Doll 设置 6 帧，用于 sort + precompute + 4 jitter 相位；
  - LOD 状态机 `needRender` 时记录 `g_gsLodSettleNeeded`，并在 streaming 空闲且
    force 归零时补一次 24 帧 settle burst（避免初始加载/选层把 jitter 重置后停在模糊路径）；
  - LOD 状态机的 `force=3` 改为只增不减，避免覆盖 UP 的 settle。
- `doRender` 恢复原门控（`viewChanged || force>0`），不再把任意 `needsRender`
  都当作持续渲染请求。
- 结果：City 2x edge **13.15**（1x 14.3，差约 8%），Doll 1x/2x diff 0.09；
  Doll 拖动 touch gap 回到 33–55ms；City 相机日志中只有触摸期间（age 0–4ms）
  的变化，没有非触摸回弹。

#### 回归记录（第一版引入，已修正）

| 现象 | 原因 | 处理 |
| --- | --- | --- |
| Doll 每次滑动都延迟 | 每个 MOVE 强制 8 帧，每帧 ~25ms，UI 线程持续繁忙 | MOVE 改为 force=1，settle 放到 UP |
| LOD 滑动中多次回弹 | 滑动期间连续 `needsRender` 渲染 + 选层/sort 抖动 | 恢复原门控，只在 UP/streaming 空闲后 settle |

#### 残留（可选）

1. City 2x 仍比 1x 略软、略亮（RGB +1.4，sat -0.1），可能是半分辨率上采样
   的固有差异。
2. combine 帧的 `ApplySharpening` 邻居仍只取 `uGSTex0`
   （`gs3d_gaussian_splatting.frag:93-115`），与 4 帧 center 相位不一致，
   可能造成轻微彩边/过冲；可改成 4 target 一致采样或 combine 帧跳过 sharpening。
3. app 仍是 `sharpening=0.75` / `covariance=2.25`（默认 0.25/2.0），可做最后
   调参 A/B。

#### 关联日志（定位期，已移除）

- `GS jitter: j/preview/accum/cam/sort/pre/scene/resInit/loaded/vp/sv/du`

### 4.2 问题 B：Doll → LOD 回切后整屏偏白（最高优先级）

**结论：已确认——重入后 composite mesh 没有被重新挂到新 pool entity，
画面只剩 LOD 的浅色 clear color（0.62/0.72/0.82，mean ≈ 199.7）。**

#### 已确认（2026-11-08 设备日志）

冷启动默认 Doll → 切 City，可复现（约 1/3 概率），白屏截图 mean = 199.7。
同一次对比中，白屏与正常路径的关键差异：

```text
# 白屏
GS diag uninit: ... matValid=1 rt0=1 ubo=1 prims=1
GS diag host: ent=INVALID ... matValid=0 rt0=0 ubo=0 prims=0
GS diag pool: preFlip loadRd matValid=1 rt0=1 ubo=1 mat=1 mesh=1 tiled=1 poolEnt=29
GS diag pool: postFlip matValid=1 rt0=1 ubo=1 mat=1 ...
GS diag host: adopt ent=29 ... matValid=1 rt0=1 ubo=1 prims=1
GS diag mk: ent=29 matValid=1 rt0=1 ubo=1 mat=1 mesh=1 res=1280,2598 recreate=0
GS diag mk: SKIP ent=29 ...

# 正常
GS diag pool: preFlip loadRd matValid=0 rt0=0 ubo=0 mat=0 mesh=1 tiled=1 poolEnt=29
GS diag host: adopt ent=29 ... matValid=0 rt0=0 ubo=0 prims=1
GS diag mk: ent=29 matValid=0 rt0=0 ubo=0 mat=0 mesh=1 res=1280,2598 recreate=0
GS diag mk: DONE ent=29 matValid=1 rt0=1 ubo=1 mat=1 mesh=1
```

即：`InitializePool()` 通过 `GetSafeLoadingGsRenderData()` 浅拷贝把 Doll 的
`isMaterialValid=1` / targets / material 带进 loading side；flip 后 host 是新 pool
entity 29，但旧 host（Doll entity 16）已销毁，新 host 上没有 `RenderMeshComponent`。
`CreateGsRenderTargets()` 只看 `isMaterialValid` 直接早退（SKIP），composite quad
无人承载，只剩 clear color。

注意：日志里的 `mesh=1` 是 `gsMesh` **资源**引用，不代表 host entity 上的
`RenderMeshComponent` 存在，不能作为早退依据。

#### 修复（`gs_loading.cpp`，本次提交）

```cpp
if (renderData->isMaterialValid && !resolutionRecreate) {
    // 只有 mesh component 真的挂在当前 host 上才允许早退
    if (loadCtx.renderMeshManager_ && loadCtx.renderMeshManager_->HasComponent(targetEntity)) {
        return;
    }
    renderData->isMaterialValid = false; // 重建 material / targets / mesh
}
```

#### 设备验证

- 冷启动 Doll → City：**8/8** 正常，mean=120.2；其中 6 次命中
  `GS diag mk: REBUILD host mesh missing ent=29`，随后 `DONE`。
- City ↔ Doll 连续往返：**8/8** 正常，mean=120.2。
- 修复前同一脚本复现：第 2 次冷启动即白屏 mean=199.7。
- 临时日志：`C:\Users\lambda\AppData\Local\Temp\opencode\lumegs_white.log`、
  `first_fix_1..8.log`（本机临时目录，不入库）。

#### B-1 代码链路

```text
Doll -> LOD:
  LoadGsLodScene()
    DestroyGsLodTiles()
      tileSys.Uninitialize()      // 销毁旧 poolEntity_
      tileSys.Initialize()        // 之后 InitializePool 会创建新 poolEntity_
    ...                          // 此时 Doll 的 GsComponent 还活着
  DestroyGsRenderPath()          // 销毁 Doll 节点 -> GsSystem 异步 RemoveGsObject
```

- `RemoveGsObject()` 在 primitives 清空时销毁 material、清空 targets、把
  `renderTargetHostEntity_` 置空（`gs_system.cpp:1103-1119`）。
- 重入时 `InitializePool()` 先 `CloneGsRenderData()`（浅拷贝，含
  `isMaterialValid=true`、Doll 的 targets/material 引用），再 `FlipValidGsRenderData()`。
- `GsSystem::Update()` 随后把 `renderTargetHostEntity_` 设为新的 pool primitive，
  调用 `CreateGsRenderTargets(..., renderResolutionRecreate=false)`
  （`gs_system.cpp:876-897`）。
- 若此时 `isMaterialValid` 仍为 true，`CreateGsRenderTargets()` **直接早退**
  （`gs_loading.cpp:376-380`），不会在新 host entity 上创建 `RenderMeshComponent`，
  也不会重建 material/mesh。
- 旧 host entity（旧 poolEntity_）已被 `Uninitialize()` 销毁 → composite quad
  没有任何存活实体承载 → 只剩 clear color。

Doll 首次加载正常，是因为它走 `isMaterialValid=false` 的首次创建路径；
首次 LOD 正常，也是因为初始数据是干净的。

#### B-2 为什么值得优先验证

- 现象「整屏偏白」与 LOD clear color（0.62/0.72/0.82）在数值上完全吻合
  （`render_main.cpp:1424-1428`）。
- 之前的 2x 白屏 mean ≈ 194 也是同一个背景色；本次修复只解决了 target 尺寸，
  没有解决「host entity 换了但 mesh 没重建」。
- 该问题与 2x/jitter 无关，1x 回切理论上也会触发。

#### B-3 判别实验

```text
1) 回切后看 GS diag：
   - renderTargetHostEntity_ 是否指向新 pool entity；
   - CreateGsRenderTargets 是否打印早退（加一行 CORE_LOG_E "skip material valid"）；
   - RenderMeshComponent 是否存在于 host entity。
2) 回切后临时把 isMaterialValid 强制置 false（或在 InitializePool 里 reset），
   观察是否立刻恢复正常画面。
3) 1x（gsScaleFactor=1）做同样的 Doll -> LOD 回切：
   - 1x 也白 -> 确认是重入/mesh 生命周期问题；
   - 1x 正常 -> 再查 2x composite/UBO 状态。
```

#### B-4 建议修复

最小且稳妥：让 `CreateGsRenderTargets()` 的早退条件也检查 host entity：

```cpp
// 伪代码
if (renderData->isMaterialValid && !resolutionRecreate &&
    renderData->materialHostEntity == targetEntity &&
    renderData->gaussianRenderTarget0 && renderData->gaussianRenderTargetStatusUbo) {
    return;
}
```

`GsRenderData` 增加 `CORE_NS::Entity materialHostEntity {};`，创建成功后记录。
或在 `GsTilingSystem::InitializePool()` / `RegisterPoolPrimitive()` 里显式把
loading side 的 `isMaterialValid=false`（pool entity 已换，mesh 必须重建），
代价是重入时多重建一次 material/target，可接受。

同时建议检查：`Uninitialize()` 是否应通知 GsSystem 清掉
`renderTargetHostEntity_`，避免持有已销毁 entity 的悬空 id。

### 4.3 问题 C：切到 Doll 后 5s 内开始滑动会「不跟手」（机制已定位）

**结论：切换后存在一个约 1–2s 的重负载窗口（切换帧 + 首帧上传/排序 + 强制渲染帧）。
在这个窗口内触摸事件被延迟；等待约 5s 让一次性工作结束后，输入恢复跟手。**

设备实测（2026-11-08，`GS diag frame` / `GS diag touch`）：

| 阶段 | 观测 |
| --- | --- |
| 切换帧（点 Doll 后） | `total ≈ 540–770ms`；其中 legacy `doll.glb` **同步解码 ≈0.5s**，tile teardown/池销毁也在同一帧 |
| `model renderable` 后首帧 | `total ≈ 140ms`（`tick ≈ 80–100ms`，`render ≈ 50–70ms`） |
| 之后若干帧 | 偶发 `render ≈ 200–350ms`（legacy 首次上传/排序/PSO） |
| 强制渲染 | 原 `g_forceRenderFrames = 30`，主线程持续出帧约 0.8–1.5s |
| 触摸延迟 | 切换后 0.8s 注入 drag，首个 MOVE 延迟 **759ms**；等待 6s 后再 drag，最大 gap **95ms** |
| 正常拖拽帧 | 约 20–30ms/帧 |

关键点：

- `LoadSplatModel()` 在 UI/render 线程同步解码 426k gaussians（≈0.5s）。
- `DestroyGsLodTiles()` 的 `Uninitialize()` 会 `DrainInFlightLoaders()` 并销毁池，
  也在切换帧内完成。
- `LoadSplatModel()` 会创建 legacy `RenderNodeGaussianSplatting` RNG；**Doll 目前仍依赖它出画**。
  尝试在 LumeGS 模式关闭 legacy RNG 后，Doll 变黑；即使补了 host mesh 重建，
  LumeGS-only Doll 仍不出画（渲染节点没有可见 composite），需单独排查，不能直接删 legacy。
- `g_forceRenderFrames = 30` 把一次性工作后的 warmup 拉长；期间主线程持续渲染，
  触摸事件被饿死。改为 8 后，干净场景首个 MOVE 延迟从 759ms 降到约 **91–147ms**
  （个别 legacy 200–350ms 帧仍可能到约 230ms）。

已提交（`48175b56`）：

- `g_forceRenderFrames` 30 → 8（模型切换后、`model renderable` 两处）。
- 问题 B 的 host mesh 重建 + GsSystem re-adopt，顺带修复 host entity id 复用导致的
  composite mesh 丢失。
- **单条 SplatData 缓存（移动语义，零拷贝）**：`ReleaseSplatModel()` 把当前 CPU 数据
  移入 `g_cachedSplatData`；再次切回同一文件时 `LoadSplatModel()` 直接移出。

当前工作区（待提交）：**首次未缓存模型的 worker 异步解码**

- `LoadSplatModel()` 只创建 `SplatDecodeTask`；`DecodeSplatFile()` 在 `std::thread`
  中执行，`DrawFrame()` 每帧 `PollSplatDecode()` 应用结果。
- 代际号 + 单 worker 队列处理快速连续切换：过期结果丢弃，最新请求排队。
- `ReleaseSplatModel()` / 缓存命中会提升代际号，取消在途解码；
  `DestroyEngine()` 里 `StopSplatDecode()` join。

实测：

| 场景 | 结果 |
| --- | --- |
| City → Doll（缓存） | 解码 0ms，切换帧 <100ms，0.8s 首滑 gap 98–103ms |
| City → Godzilla（1.07M `.splat`，未缓存） | worker 解码约 1.04s；最终画面正常（mean 104.5） |
| City → Bowl SOG（750k，未缓存） | worker 解码约 0.67s；画面正常（mean 125.8） |
| 快速切换（Bowl → 0.1s → Godzilla） | 最终 Godzilla 正常，无崩溃 |
| 连续 City↔Doll 3 轮 | 3/3 Doll 正常（mean 71.9） |

注意：异步解码期间如果用户拖拽，解码完成时 `ApplySplatModelData()` 会重新设置
初始 framing，拖拽结果会被覆盖；如需保留，可在应用时保留当前朝向或显示 loading。

仍待做：

1. LumeGS Doll 回切 composite 黑屏（见下），去掉 legacy 双路径；
2. `model renderable` 后 legacy 首次上传约 149ms；
3. 解码期间的 loading/交互保留（UX）；
4. 首次进入 City 的 0.5–1s 内，streaming/选层/sort 仍会占用主线程，
   左右转动视角有轻微卡顿；后续可考虑预加载、分帧或降低首次选层频率。

#### LumeGS-only Doll 的额外发现

为了去掉 legacy 双路径，尝试在 LumeGS 模式不创建 legacy RNG：

- 冷启动 Doll：LumeGS-only 正常出画（mean 58.0）；
- City → Doll 回切：即使 `GS diag lume` 显示
  `isTiled=0 / pool=0 / loaded=1 / rt0 与 cache 一致 / mat=1 mesh=1`，
  且 mesh 已重建到新 host，画面仍为黑（mean 23.0）。
- 结论：LumeGS Doll 的回切 composite 还有更深的状态问题（疑似材质/描述符绑定或
  数据上传侧），当前不能删除 legacy；本次先保留 legacy 并用缓存消除解码停顿。

#### 关联日志（定位期，已移除）

- `GS diag frame: dt/total/tick/render/doRender/force/view`（`DrawFrame`）
- `GS diag touch: gap/type`（`DispatchTouchEvent`，仅 MOVE/UP 且 gap ≥ 33ms）
- `GS diag rt: re-adopt mesh host A -> B`（GsSystem，host entity 复用）
- `GS diag mk: SKIP/REBUILD/DONE`（`CreateGsRenderTargets`）
- `GS diag lume: state/tiled/isTiled/pool/loaded/rt0/cache/mat/mesh`（状态变化时）
- `GS diag: exec skip` / `GS diag cam:`（render node 早退原因）
- `Splat cache/decode: N gaussians, T ms`（单条 SplatData 缓存命中/解码耗时）

---

## 5. 关键代码锚点

| 位置 | 说明 |
| --- | --- |
| `lume/LumeGS/src/systems/gs_system.cpp:866-897` | 目标重建 + tiling 分支 + `SetGsScaleFactor` |
| `lume/LumeGS/src/systems/gs_system.cpp:1103-1119` | 删除最后一个 primitive 时清 material/targets/host |
| `lume/LumeGS/src/systems/gs_system.cpp:1260-1290` | 配置覆盖（covariance/sharpening/XEG/DRS） |
| `lume/LumeGS/src/systems/gs_tiling_system.cpp:1891-1949` | `InitializePool`（clone + pool + flip） |
| `lume/LumeGS/src/systems/gs_tiling_system.cpp:1860-1889` | `RegisterPoolPrimitive`（pool AABB + flip） |
| `lume/LumeGS/src/systems/gs_tiling_system.cpp:1255-1319` | `Uninitialize` 销毁 pool entity |
| `lume/LumeGS/src/util/gs_loading.cpp:353-391` | `CreateGsRenderTargets`（早退 + mesh/material） |
| `lume/LumeGS/src/util/gs_loading.cpp:166-232` | target 尺寸、status UBO 创建 |
| `lume/LumeGS/src/render/render_node_gs_render.cpp:902-933` | `UpdateRenderStatusUbo` push constant |
| `lume/LumeGS/src/render/render_node_gs_render.cpp:2033-2101` | preview/accum 帧选择与 jitter 递增 |
| `lume/LumeGS/src/render/render_node_gs_render.cpp:2372-2405` | `SetGsColorAttachment` 4 target 轮转 |
| `lume/LumeGS/src/render/render_data_store_default_gs.cpp:610-624` | `SetGsScaleFactor` 一次性 / `GetRenderScaleFactor` |
| `lume/LumeGS/src/render/render_data_store_default_gs.cpp:498-534` | `FlipValidGsRenderData` 双缓冲 |
| `lume/LumeGS/src/render/render_data_store_default_gs.cpp:1253-1318` | `CloneGsRenderData` 浅拷贝 |
| `lume/LumeGS/assets/app/shaders/shader/gs3d_gaussian_splatting.frag:53-115,145-184` | 2x combine / sharpening / sRGB |
| `LumeApp/entry/src/main/cpp/render_main.cpp:1511-1599` | `DestroyGsLodTiles` / `LoadGsLodScene` |
| `LumeApp/entry/src/main/cpp/render_main.cpp:2070-2109` | 模型切换顺序 |
| `LumeApp/entry/src/main/cpp/render_main.cpp:1058-1084` | `EnsureGsConfiguration`（app 调参） |

---

## 6. 构建 / 部署 / 验证

```powershell
# 改过 GS GLSL / ROFS 时先从仓库根重编 shader（本轮只改 C++ 可跳过）
python LumeApp/tools/gs_assets/compile_gs_shaders.py
python LumeApp/tools/gs_assets/pack_gs_rofs.py

# 构建（workdir 必须是 LumeApp）
$env:PATH="D:\Coding\DevEcoStudio\jbr\bin;D:\Coding\DevEcoStudio\tools\hvigor\bin;"+$env:PATH
$env:JAVA_HOME="D:\Coding\DevEcoStudio\jbr"
Remove-Item -Force entry/libs/arm64-v8a/*.so -ErrorAction SilentlyContinue
hvigorw assembleHap --mode module -p product=default --no-daemon

# 部署（绝对不要 uninstall，会清空 files/assets）
hdc install -r entry\build\default\outputs\default\entry-default-signed.hap
hdc shell "aa force-stop com.example.lumeapp"; hdc shell "hilog -r"
hdc shell "aa start -a EntryAbility -b com.example.lumeapp"

# 观测
hdc shell "hilog -x | grep -E 'GS|LOD' | tail -40"
hdc shell "snapshot_display -f /data/local/tmp/a.jpeg"
hdc file recv /data/local/tmp/a.jpeg C:\Users\lambda\AppData\Local\Temp\a.jpeg
python -c "from PIL import Image,ImageStat; im=Image.open(r'C:\...\a.jpeg').convert('L'); s=ImageStat.Stat(im); print('mean',round(s.mean[0],1))"
```

判读基线：正常地形 ≈ 110–125；纯背景/白屏 ≈ 190+（LOD clear color 0.62/0.72/0.82）。

---

## 7. 不要重踩的坑

1. `CreateGsRenderImages` **内部自己会除 `gsScaleFactor`**；调用方必须传原始输出分辨率。
2. `renderStatusUbo` 必须零初始化再逐字段赋值（未初始化字段会改变 shader 分支）。
3. `CreateGsRenderTargets` 的 `isMaterialValid` 早退不校验 host entity / mesh 是否存在
   ——重入场景务必检查。
4. `CloneGsRenderData` 是浅拷贝，会复制 material/targets/isMaterialValid，不要假设重入是干净的。
5. 诊断日志一律 `CORE_LOG_E` + 简单 `%d`，长格式会被 HiLog 丢掉。
6. 临时 `Gs diag` 日志验证完要清掉，勿提交。

---

## 8. 下一步建议顺序

1. ~~**问题 B**：验证「回切后 composite mesh 是否挂在新的 pool entity 上」~~ ✅
   已确认并修复（`HasComponent` 校验），设备 8/8 + 8/8 通过。
2. ~~**问题 C**：复现「切 Doll 后 5s 内滑动卡顿」~~ ✅ 已解决：
   `g_forceRenderFrames` 30→8 + SplatData 零拷贝缓存 + 首次加载 worker 异步解码；
   City→Doll 切换帧 <100ms，0.8s 首滑 gap 98–103ms。剩余仅解码期间的 loading/交互保留
   与 legacy 首次上传 ~149ms。
3. ~~**问题 A**：先关 sharpening（0 或 0.25）做 A/B；再修 combine 帧邻居纹理不一致。~~
   ✅ 主因已修复（jitter 累积被 `needsRender`/force=3 截断）；City 2x edge
   10.7→13.16（1x 14.3）。残留约 8% 边缘差与轻微亮度差，可选修 combine
   sharpening 邻居或调参。
4. 三项都验证后，删除 `TEMP-DIAG` 日志，补 `lumegs-tiling-host-lessons.md` 的结论与未完成项。

---

## 9. 定位期临时诊断日志（已在提交前移除）

> 以下日志名称仅作复现参考；当前代码已移除 `TEMP-DIAG` 输出，
> 需要再次定位时可按名称重新加回。基础日志 `GS diag sys` / `GS diag: draw` /
> `GS diag: handles INVALID` 仍然保留（定位前已存在）。

### 9.1 重入 / material / mesh（问题 B）

| 日志 | 位置 | 判读 |
| --- | --- | --- |
| `GS diag mk: ent=... matValid=... rt0=... ubo=... mat=... mesh=... res=... recreate=...` | `gs_loading.cpp:CreateGsRenderTargets` | 每次创建入口状态 |
| `GS diag mk: SKIP ent=...` | 同上 | **出现即命中假设**：`isMaterialValid` 早退，未给新 host 建 mesh |
| `GS diag mk: DONE ent=...` | 同上 | mesh 已挂到该 host entity |
| `GS diag host: ent=... matValid=...` / `adopt ent=...` | `gs_system.cpp:Update` | host entity 切换与当前 material/target 状态 |
| `GS diag rt: rebuild ...` | 同上 | 目标重建原因（尺寸/空 target） |
| `GS diag pool: preFlip/postFlip ...` | `gs_tiling_system.cpp:InitializePool` | loading side 从 Doll 继承了什么、flip 后 active 是什么 |
| `GS diag uninit: poolEnt=... matValid=... rt0=...` | `gs_tiling_system.cpp:Uninitialize` | 拆 pool 前的状态 |
| `GS slow CreateGraphicsState / targets/UBO` | `gs_loading.cpp:SetupGsMaterial` | 切模型时的一次性 GPU 资源成本 |

判读：回切 LOD 时若出现 `GS diag mk: SKIP ent=<new>`，且之前的 `DONE ent=<old>` 是 Doll 的
host entity，则确认「composite mesh 没挂到新 pool entity」。

### 9.2 首次滑动 1s（问题 C）

| 日志 | 位置 | 判读 |
| --- | --- | --- |
| `GS slow GetPsoData: N ms shader=... flags=...` | `render_node_gs_render.cpp:GetPsoData` | PSO/变体编译 |
| `GS slow PreExecuteFrame: N ms` | `PreExecuteFrame` | 帧前 CPU 总成本 |
| `GS slow UpdateCameraUBO: N ms` | 同上 | camera UBO 映射 |
| `GS slow RecreateDescriptorSets: N ms` | 同上 | descriptor 重建 |
| `GS slow tiled sort/precompute: N ms active=... jitter=...` | `ExecuteFrame` tiled 块 | tile sort/precompute 首次成本 |
| `GS slow GaussianRenderPass: N ms jitter=... tiled=... preview=...` | `ExecuteFrame` | GS 渲染 pass（含首次 BindPipeline） |
| `GS slow UpdateRenderStatusUbo / ExecuteFrame: N ms` | 同上 | 其余成本 |

判读：1s 落在 `GetPsoData` / `CreateGraphicsState` / `GaussianRenderPass` → PSO 编译；
落在 `PreExecuteFrame` → descriptor/UBO；落在 `tiled sort/precompute` → 首次排序/预计算。
阈值：GetPsoData/资源 20ms，UBO/status 10ms，整帧 20ms。

### 9.3 复现步骤（设备）

```text
启动 → 等 LOD 稳定（静止 4 帧，jitter=4）→ 切 Doll → 再切回 LOD（观察偏白）
→ 切到 Doll 后第一次拖动（观察 1s 卡顿）
每次操作后：
hdc shell "hilog -x" > C:\Users\lambda\AppData\Local\Temp\opencode\lumegs.log
rg -n 'GS diag|GS slow|GS LOD|GS path' C:\Users\lambda\AppData\Local\Temp\opencode\lumegs.log
```

本机自动化复现脚本（临时目录，不入库）：

| 脚本 | 作用 |
| --- | --- |
| `C:\Users\lambda\AppData\Local\Temp\opencode\repro_first_city.ps1` | 循环冷启动 → 默认 Doll → 切 City，截图 mean > 175 判定白屏 |
| `...\repro_first_city_fixcheck.ps1` | 同上，并保存每次 `first_fix_N.log` |
| `...\repro_city_white.ps1` | City ↔ Doll 连续往返 |
| `...\image_mean.py` / `...\parse_layout.py` | 截图均值 / uitest layout 解析 |

---

## 10. 给新 session 的开场提示

```text
读 docs/knowledge/lumegs-2x-composite-status.md 和 docs/knowledge/lumegs-tiling-host-lessons.md。
基线 commit 是 c9c5485c，工作区应为干净。
继续处理 LumeGS 2x 的三个遗留问题，优先级：
  1) Doll -> LOD 回切后整屏偏白（怀疑 isMaterialValid 早退导致 composite mesh 未挂到新 pool entity）；
  2) 切到 Doll 后第一次滑动卡 1s（怀疑 PSO/首帧初始化被延迟）；
  3) 2x 颜色偏怪/细节略差（先验证 combine 帧 sharpening 邻居取错纹理 + 调参）。
```
