# LumeGS tiling host 渐进加载与首帧经验

关联文档：[LumeGS 接入现状与计划](lumegs-integration-status.md)、[3DGS LOD 现状](3dgs-lod-status.md)。

## 结论

- City LOD 首帧从 **~20s 浅色天空** 降到 **约 1s**。
- 首帧慢不是 SOG/WebP 解码（日志里 `SOG DecodeSogFile` 约 0.1s 完成），主要是：
  1. host 一次性创建 940 个 tile / ~1M splats；
  2. tiling pool 的首次 433 MB 分配 + sort capacity 262k→2M；
  3. **pool primitive 的 AABB 默认是 0**，`RenderNodeGsRender` 先按 pool primitive AABB 做整体视锥剔除，相机在场景远处时整个 tiled pool 被剔除，tile resident 也无法显示。
- 选级/可见窗口/ring preload/分批创建属于 **render_main host**；pool primitive AABB、tile pool 双缓冲属于 **LumeGS tiling**。系统侧 GSPlugin 用同一套 LumeGS 但体验快，通常是它已经有正确的 pool AABB/预热和 host 侧渐进窗口。

## 最终 host 流程（`render_main.cpp`）

`UpdateGsLodTiles()` 现在按以下顺序工作：

1. `CullVisible` + `ChooseScale` + `AssignLevels` 得到严格可见的 `sel.items`（默认 1M 预算）；
2. 投影放宽 1.3×，再 `CullVisible` 一次；视锥外邻近 leaf 作为 `ring`，最多 256 个；
3. tile 创建优先级：
   - 最近 64 个可见 leaf 的 **coarsest** tile；
   - 周围 ring 的 **coarsest** tile（`preload=true`、visible=false）；
   - 其余可见 leaf 的 coarsest；
   - 可见 leaf 的 fine level；
4. 每帧最多创建 **32** 个 tile（`kCreatePerUpdate`），多余留在 pending 队列；
5. 可见 leaf 先显示 coarsest fallback；fine tile resident 后隐藏 coarse；
6. ring tile resident 后，快速转视角时可直接显示粗层，再等 fine 补齐；
7. 用 `GS LOD select: vis/cull/scale/total/sel/ring/fineRes/coarseRes/pending` 观察窗口状态。

关键代码锚点：

- `LumeApp/entry/src/main/cpp/render_main.cpp`：`LoadGsLodScene`、`DestroyGsLodTiles`、`UpdateGsLodTiles`、`EnsureGsConfiguration`、`ApplyGsLodCamera`。
- `GsTilingComponent` 的 `visible` / `preload` 字段已足够表达可见与预加载，不需要新增 API。
- 可见 leaf 的 fallback 状态用 `g_gsLodFineLevel` / `g_gsLodFineShownLevel` / `g_gsLodCoarseShown` 跟踪；pending 用 `g_gsLodPending` + `g_gsLodTilePending` 去重。

## LumeGS 侧必须修的两处

### 1. pool primitive AABB

位置：`lume/LumeGS/src/systems/gs_tiling_system.cpp` `RegisterPoolPrimitive()`。

`RenderNodeGsRender::ProcessPrimitiveTiles()` 会先检查 primitive 的 AABB：

- `GsPrimitive poolPrim` 默认 `aabbMin/aabbMax` 为 0；
- 相机在 (1952, 488, 1439) 这类场景远点时，整个 pool 被 `IsPrimitiveAabbFullyOutsideFrustum()` 剔除；
- 即使 tile 已经 commit、`SetTileResidency(true)`、`tile.state=RESIDENT`，屏幕仍只有天空。

修复：给 pool primitive 设置覆盖全场景的 AABB（`±1e30`），每个 tile 仍按自身 AABB 剔除。

### 2. tile pool 双缓冲

位置：`GsTilingSystem::InitializePool()`。

`SetTilePoolConfig` 只配置当前 render data，`GetSafeLoadingGsRenderData()`/`FlipValidGsRenderData()` 后另一侧 `GetTilePool()` 可能为空，出现 `prep fail tile pool missing`、无 SOG decode。现在：

- 在即将生效的 `loadRd` 上配置 `tilePool`；
- `currentRd->tilePool` 指向同一个 shared pool；
- pool/sort capacity 由 tiling system 直接写入，保证 flip 后仍然有效。

另外，`LUMEAPP_GS_TILING_FULL_RES=1` 让 LumeApp 的 tiling 使用全分辨率（`gsScaleFactor=1`），产品 GN 构建不定义该宏，保持默认 2。`GsConfigurationComponent` 对齐系统侧：

```text
useGpuSort = true
use24BitGpuSort = true
convertGsColorSrgb = true
renderCovarianceStddev = 2.25
renderSharpeningCoefficient = 0.75
```

## 系统侧 GSPlugin 对照

系统侧 host 的 `loadTiledGSNode(jsonUri, sceneRoot)` → `LoadScene` → `UpdateTiles` 模式的核心是：

- `RebuildTileSelection`；
- `ProcessPendingTileCreates`（有限创建）；
- `ProcessVisibleTiles`；
- `ProcessPreloadTiles`；
- `cameraChanged/tileStateChanged/residencyChanged/hasUnresolvedVisibleTiles/HasUnresolvedPreloadTiles` → `ecs_->RequestRender()`。

本仓库当前实现对应关系：

| 系统侧 | 当前 render_main |
| --- | --- |
| `LoadScene` 预热 | `LoadGsLodScene` 在切换时执行；可继续移到启动/场景加载 prewarm |
| `RebuildTileSelection` | `CullVisible/ChooseScale/AssignLevels` |
| `ProcessPendingTileCreates` | `g_gsLodPending` + 32/frame |
| `ProcessVisibleTiles` | fine/coarse fallback + `SetTileVisible` |
| `ProcessPreloadTiles` | ring 的 coarsest `preload=true` |
| `RequestRender` | 当前 `g_gsLodForceUntil` 30s 连续渲染窗口 |

## 验证

构建：`hvigorw assembleHap --mode module -p product=default --no-daemon` 成功。

设备结果（1M 预算，空中机位）：

- 首帧约 **1s**：截图 mean ≈ `[116.1, 121.2, 112.5]`，中心为地形；
- `GS LOD select: vis=940 cull=1876 scale=2063 total=999947 sel=940 ring=210 fineRes=0 coarseRes=0 pending=1499`（首帧刚创建时）；
- 快速拖拽后：`GS LOD select: vis=930 ring=256 fineRes=896 coarseRes=34`，拖拽后 200ms 截图 mean ≈ `[130.6, 134.3, 131.8]`，无天空/黑屏空隙；
- `GS diag sys: res=1280,2598 prev=1280,2598 scale=1`；
- 全分辨率 tiling 与旧自定义管线地形/天空分界一致。

## 踩坑清单

1. **不要一次创建全部可见 tile**：940 个 `CreateTile` 会让 worker 一次性 drain 大量请求，首帧等整批。
2. **SOG 解码不是瓶颈**：`SOG DecodeSogFile` 0.1s；耗时在 tile 抽取、pool/sort 扩容和 commit。
3. **pool primitive AABB 必须覆盖场景**：默认 0 会导致整个 tiled pool 被剔除。
4. **tile pool 必须在双缓冲两侧有效**：只配一侧会在 flip 后 `GetTilePool()` 为空。
5. **coarse fallback 很重要**：fine 未 resident 时显示 coarsest，避免切换/转视角出现空洞。
6. **ring preload 用投影放宽而不是只加距离**：1.3× FOV、最多 256 个，快速转视角时能先显示粗层。
7. **初始创建顺序**：最近可见 coarse → ring coarse → 其余 coarse → fine；ring 不要等到最后。
8. **预算/画质**：1M 默认；`LUMEAPP_GS_TILING_FULL_RES` 与 `gsScaleFactor=1` 保持全分辨率；配置对齐系统侧。
9. **JSON/权限**：`default.scene.json` 严格 JSON；设备文件 owner 必须是 App 实际 UID（本机 20020212），否则日志会表现为 `LOD: missing ...`。

## 2026-11-07 续作：LOD 重入与拖拽

### 修复：LOD → Doll → LOD 第二次 LOD 无内容

单模型路径会把 render data 的 `isTiled=false`/`tilePool.reset()`，但 `GsTilingSystem` 的
`poolInitialized_`/`tileData_`/queue 仍保留；第二次 LOD 直接跳过 `InitializePool`，没有 pool/SOG。
修复：

- `DestroyGsLodTiles` 对 `IGsTilingSystem` 做一次 `Uninitialize()`/`Initialize()`，清空
  tileData/queue/residency/pool，并销毁/重建 pool entity；
- `PrepareSubmissions` 用 `isPoolReady()`（`isTiled && GetTilePool()`）判断，而不是只看
  `poolInitialized_`；
- `RegisterPoolPrimitive` 在重入时复用已有 pool primitive，避免重复添加。

验证：第二次 LOD 重新出现 `pool initialized`、SOG 解码，截图 mean ≈ `[116.1, 121.2, 112.5]`。

### 拖拽卡顿/回弹的当前结论

- `ORBITCAM[move]` 日志中 `back/target` 单调，没有相机输入反方向回弹；不是 `Tumble` 的问题。
- 卡顿的主要来源：host 每帧对 940 fine + 256 ring 全量调用 `SetTileVisible`（worker 每帧上千事件）。
- 尝试在 host 侧按 visible 状态门控 `SetTileVisible` 后，首帧/coarse fallback 在切换后出现天空，已回退；
  当前保留全量调用以保证重入和首帧稳定。
- 下一步优先在 tiling worker 内做 `SetTileVisible` 去重/合并（同一 tile 相同状态不产生事件），而不是
  host 侧只记 desired state；或者完整移植系统 host 的 `ProcessVisibleTiles`/
  `ProcessPreloadTiles`/`RequestRender` 状态机。
- 拖拽后的 coarse/fine 切换仍有可见内容跳变，需要对比系统 host 的 fallback hold 策略。
- **边界天空是正常的，不是 host bug**：这组 LOD 是无人机采集的有限区域，初始相机在空中俯视；大角度 yaw 会把视线转出采集范围。此时 `GS LOD state: vis=102 ring=118 res=220` 说明仍有一圈边缘 tile 可见，其余是背景/天空；`vis=0` 表示完全转出场景。判断异常要看相机是否仍在 scene bounds 内、边缘 tile 是否可见，而不是把背景当失败。

## 未完成

- 用系统 host 的 `RequestRender` 状态位替换当前 30s 连续渲染窗口；
- 把 `LoadScene`/pool/sort 预热移到 App/场景加载阶段，进一步隐藏首次 433 MB pool 成本；
- ring 覆盖范围（1.3× FOV / 256）和 1.5M/600k 预算的实测调参；
- tiling worker 内 `SetTileVisible` 去重/合并；coarse fallback hold；
- 移除 legacy LOD bridge、清理 App 侧一次性日志、完整 `hb build graphic_3d`/XTS/真机矩阵验证。
