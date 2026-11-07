# 3DGS 大场景 LOD —— 当前实现、问题与规划（交接文档）

> 用途：上下文压缩/换手时的真相快照。仓库 `D:\Source\3DGS\graphic_graphic_3d`，
> 分支 `dev_3dgs`，最新提交 `94e15845`（未 push）。数据样例见
> [3DGS 演示资产清单](3dgs-demo-assets.md) 与 [3DGS 渲染集成经验](3dgs-rendering-integration.md)。

---

## 1. 目标与现状一句话

在 OpenHarmony 手机（Vulkan / Lume 引擎，**CPU 排序、无 compute 路径**）上渲染
**SOG 瓦片化 LOD 大场景**（`500w/syp_uav_0611`：2816 叶子 / 4 级 / 643 MB），
要求：转动视角时新区域能在 1 s 内出现粗糙内容并逐级细化、无闪烁、无空洞、无崩溃。

**当前状态**：功能与稳定性基本可用（无崩溃、无空洞、按需重建 ~10–25 ms、
`level-chg 0`、静止不重建），**但转动中仍有"成片区域跳动"的观感（LOD 替换所致）**，
且**停稳后整体仍偏糊**（预算/数据密度限制）。见第 5、6 节。

---

## 2. 数据格式（已验证）

```text
<scene>/
  default.scene.json    mobileLodDistances[20,60,300,1000], maxRenderCountOfMobile=1,000,000,
                        mobileLodCacheCount=5,000,000, cameraInfo
  lod-meta.json         lodLevels=4, filenames[212], tree
  grid0_y/*.sog ...     每个文件 = 同一 LOD 下若干叶子的连续高斯池
```

- `tree`：二叉空间划分；内部节点只有 `bound`+`children`，**叶子**有 `bound` + `lods`
- 叶子 `lods["0".."3"] = {file, offset, count}`（file 是 `filenames` 下标，offset/count 是该文件内的高斯区间）
- 已验证 212 个 (文件,LOD) 组合：文件 `count` == 所有叶子区间之和（0 处不一致）
- 规模：LOD0 37.5M / LOD1 11.3M / LOD2 4.5M / LOD3 1.5M 高斯；瓦片只含 `sh0`（无 SH rest）
- **世界缩放必须 1.0**：SOG `means` 已是 log 空间世界坐标（`exp(7.02)-1=1115` 等于树 bound），不能 ×100

---

## 3. 代码结构与职责

| 文件 | 职责 |
| --- | --- |
| `LumeApp/entry/src/main/cpp/lod_scene.h` | 解析 scene/lod-meta JSON、扁平化叶子、视锥剔除、距离选级（含 15% 回差 + **粘滞**）、预算升降级（按 `radius/dist` 重要度）|
| `LumeApp/entry/src/main/cpp/lod_streamer.h` | 后台线程：选级 → 解码瓦片 → 块分配 → 组装脏区间 → 发布；块分配器/LRU 淘汰/缓存 |
| `LumeApp/entry/src/main/cpp/render_main.cpp` | 引擎初始化、`LoadLodScene()`、相机（轨道/第一人称）、每帧 `Poll` 发布 |
| `lume/LumeRender/src/node/render_node_gaussian_splatting.cpp` | RenderNode：多段脏区间上传、双 cull-records、按**存活区间**剔除/排序、双 pass 绘制 |
| `LumeApp/entry/src/main/cpp/sog_loader.h` | SOG v2 解码（ZIP+迷你 JSON+ImageKit WebP，5 张图并行，反预乘）|
| `LumeApp/entry/src/main/cpp/gaussian_splat_data.h`（两份同文件）| 跨 `.so` 共享全局（dlsym）|

### 3.1 共享全局（app → RenderNode，`dlsym`）

```c
g_gaussianSplatData        const GpuGaussianData*   池数据
g_gaussianSplatCount       uint32_t                 存活高斯数（绘制/排序）
g_gaussianPoolSize         uint32_t                 池占用槽位数（缓冲 sizing）
g_gaussianSplatCapacity    uint32_t                 缓冲容量（= pool 容量）
g_gaussianSplatGeneration  uint32_t                 代次（变则重传）
g_gaussianSplatUploadedGeneration  节点回显（当前未用）
g_gaussianLiveRanges       const uint32_t*          (start,count) 成对，每选中叶子一条
g_gaussianLiveRangeCount   uint32_t
g_gaussianDirtyRanges      const uint32_t*          (start,count) 成对，变动的池槽位
g_gaussianDirtyRangeCount  uint32_t                 0 = 全量重传并交换缓冲
g_gaussianBackground[4]    float                    离屏清屏色（LOD 场景为天空色）
g_gaussianCameraData       view/proj/viewport/cameraPos/shScale/clip
```

### 3.2 关键常量（现状）

| 项 | 值 | 位置 |
| --- | --- | --- |
| 演示预算 | **1,500,000** 高斯（场景 JSON 是 1M，为俯视密度调高）| `render_main.cpp` `lodBudget` |
| 池容量 | `budget * 4/3` = 2,000,000 | `lod_streamer.h` `capacity_` |
| 解码缓存 | 5,000,000 高斯（约 320 MB）| `lod_streamer.h` `cacheBudget_` |
| 并行解码 | 2 个 future 在飞 | `kMaxDecodesInFlight` |
| 重建限频 | 相机驱动 150 ms（有新瓦片则立即）| `BuildView` 开头守卫 |
| 选级回差 | 15% | `lod_scene.h` `kHysteresis` |
| 排序线程 | 常驻 4 worker + 1 后台排序线程 | `SortPool` / `SortState` |
| 帧槽 | 3（camera UBO / 排序索引 / SH 参数 SSBO）| `FRAME_SLOTS` |

---

## 4. 运行时数据流

```text
渲染线程每帧 (DrawFrame)
  └─ view 变化 → Streamer::UpdateCamera(view,proj,camPos)
  └─ Streamer::Poll(Result) → 有发布则更新共享全局 + generation++（节点据此上传/重排）

后台线程 (Streamer::Loop)
  1) 收集已完成的解码 future（不阻塞）→ dirty
  2) 相机变了或 dirty → BuildView：
       SelectVisible（视锥 + 粘滞层级 + 预算升降级）
       → 每选中叶子：resolveLevel（首选目标层级，缺则退更粗已缓存层级）
       → 块不足则分配（first-fit 空闲表 → bump → LRU 淘汰非选中块）
       → 拷贝该叶子的切片到池中，记录脏区间
       → 存活区间表（每叶一条）+ 选择集哈希
       → 无变化则不发布；否则 Publish
  3) 保持 2 个解码 future（按"视线夹角优先、屏幕内优先"挑瓦片）
```

**RenderNode**：generation 变化 → 若有脏区间则**原地 patch**（不换缓冲），否则全量写另一块缓冲再切换；
按存活区间做视锥/尺寸剔除 → 65536 桶计数排序（CPU，后台线程）→ 双 pass 绘制。

---

## 5. 当前问题（重要）

### 5.1 转动中"成片区域跳动"（未解决）

- 现象：转动时画面持续出现块状跳变；**相机静止后仍会持续一阵**，直到该视野所需细瓦片全部解完
- 原因：**每叶只画一个层级**，细瓦片到达时把该叶的块从粗层**替换**为细层（大高斯消失、小高斯出现）→ 该区域"跳"一下；
  一个瓦片覆盖几十~几百个叶子 → 每解出一个瓦片就有成片区域同时跳（解码约 3 瓦片/秒，所以是持续的）
- 已排除：层级振荡（`level-chg 0`）、空洞（`grow-fail 0`、`no-tile 0`）、缓存抖动（选中瓦片禁止淘汰）、崩溃（cull records 双缓冲）

### 5.2 停稳后仍偏糊（未解决，属预算/数据取舍）

- 1.5M 预算铺 1.6×1.6 km 的俯视城区，平均密度低；远处/次要叶子只能停 LOD3（每叶约 500 高斯）
- 场景 JSON 自带预算是 1M（更糊）；参考实现在手机端也是 1M
- 可提升：提高预算（显存/fill rate 代价）或重新导出密度更高/更小的 LOD 瓦片

### 5.3 首次进入/大角度切换的延迟（部分缓解）

- 单瓦片解码 0.3–0.7 s（纹理 ~96 ms + 逐高斯循环 ~60–85 ms + 争用），2 路并行约 3 瓦片/秒
- 效果：新区域约 0.5–1 s 出现（粗），细化 1–3 s
- 未做：逐高斯转换多线程、按层次预算的重导出

### 5.4 内存

- RSS 约 780–800 MB（池 2×2M×64 B=256 MB + 缓存 320 MB + 引擎/其他）

---

## 6. PlayCanvas 参考实现（upstream `main`，2026）

目录 `src/scene/gsplat-unified/`（`gsplat-budget-balancer.js`、`gsplat-octree*.js`、
`gsplat-interval-compaction.js`、`gsplat-world.js`、`gsplat-placement.js`、`gsplat-work-buffer*.js`、
`gsplat-unified-sorter.js` 等）。

1. **work buffer + 每节点 interval**：所有可见高斯放在固定大小缓冲，用 `BlockAllocator`
   管理，节点换级只重写自己的块（**与我们相同，因此同样会"替换跳变"**）
2. **预算 = 单个全局缩放因子 s**：`band = 1 + floor(log_m(d/(s·b)))`；把每个节点切级点投到
   ln(距离) 轴，直方图 + sub-bin 扫描选 s（`target` 填满 / `limit` 只降）
   - 关键细节："遇到第一个放不下的 sub-bin 就停，不跳过" → 层级不依赖无关节点，**大幅减少翻转**
3. **节奏**：每 10 帧一次全量 LOD + 背压门（排序忙不更新）+ 相机位移/旋转/FOV 阈值
4. **GPU compute 剔除+压缩 + GPU radix sort + indirect draw**（我们这条路径没有 compute，**不可移植**）
5. **`lodBehindPenalty`**：相机背后的节点额外降级（唯一的方向相关项；**没有**空中/地面之分）
6. 卸载有 `cooldownTicks` 冷却，文件引用计数

**结论**：PlayCanvas 不做"空中 vs 地面"区分；平滑来自"全局 s + 10 帧节流 + 资产映射"，
而不是消除替换。完全照做可移植的是 ①全局 s 预算 ②节流/背压 ③interval/placement 模型；
GPU 压缩与排序不可移植。

---

## 7. 规划（待确认）

### A. 运动时冻结层级升级（最便宜，先做）

- 相机移动中只处理"已经在预算内的层级"，**不升级叶子层级**；停手 ≥300 ms 后逐个升级（每瓦片间隔 ~150 ms）
- 预期：转动中完全无跳变；停手后是"一块一块变清晰"
- 改动点：`BuildView` 增加 `moving` 判定（距上次 `UpdateCamera` 的时间），移动时把 `resolveLevel` 限制在
  当前块层级（不允许 finer），停手后允许

### B. 全局缩放预算分配（PlayCanvas 做法）

- 替换 `lod_scene.h` 的逐叶重要度升降级：用 ln(距离) 直方图（大 bin + sub-bin）选一个全局 s，
  `band = 1 + floor(log_m(dist/(s·b)))`，`m` 取 1.5（LOD 距离由场景提供，可映射为 b）
- 预期：层级变化集中在一个阈值附近，跳变面积显著减小；天然无抖动
- 工作量约半天；需要替换 `SelectVisible` 的预算段

### C. 交叉淡入（彻底消除跳变，最后考虑）

- 升级时保留粗块 300 ms 并让细块淡入（每块权重 uniform 或两次绘制）
- 风险：overdraw、alpha 叠加变亮、实现复杂

### D. 密度/性能

- 预算 1.5M → 2M（RSS +64 MB，池容量需 2.7M）
- 或重新导出更小瓦片（例如每瓦片 50k 高斯），可同时降低首次延迟与空洞感

---

## 8. 构建 / 部署 / 验证（可用命令）

```powershell
# 构建（必须在 LumeApp 目录执行；命令里不要再写 cd，会破坏 workdir）
$env:PATH="D:\Coding\DevEcoStudio\jbr\bin;D:\Coding\DevEcoStudio\tools\hvigor\bin;"+$env:PATH
$env:JAVA_HOME="D:\Coding\DevEcoStudio\jbr"
Remove-Item -Force entry/libs/arm64-v8a/*.so -ErrorAction SilentlyContinue
hvigorw assembleHap --mode module -p product=default --no-daemon

# 部署
hdc install -r entry\build\default\outputs\default\entry-default-signed.hap
hdc shell "aa force-stop com.example.lumeapp"; hdc shell "hilog -r"
hdc shell "aa start -a EntryAbility -b com.example.lumeapp"

# 交互（按钮坐标，1280x2832：PLY 126 / SOG 399 / Godzilla 672 / Doll 945 / City LOD 1186，y≈2580；
#         模式按钮 (640,2443)；双击 = 近/远切换）
hdc shell "uitest uiInput click 1186 2580"
hdc shell "uitest uiInput drag 700 1500 620 1500 3000"   # 拖动

# 观测
hdc shell "hilog -x | grep -E 'LOD: set|decoded tile|GS: uploaded|GS: set shrink'"
hdc shell "grep VmRSS /proc/$(hdc shell pidof com.example.lumeapp)/status"
hdc shell "snapshot_display -f /data/local/tmp/a.jpeg"; hdc file recv /data/local/tmp/a.jpeg <本地路径>
```

**资产投放**（已推送到设备，643 MB）：

```powershell
hdc file send "D:\Source\3DGS\data\500w\syp_uav_0611" "/data/app/el2/100/base/com.example.lumeapp/haps/entry/files/assets/lod/"
hdc shell "chown -R 20020208:20020208 .../assets/lod"; hdc shell "chmod -R u+rwX,g+rX,o+rX .../assets/lod"
```

**日志含义（`LOD: set ...`）**

```
set 1499642 live gaussians (pool 1596136, 54 dirty ranges, 54 new blocks,
    no-tile 0, grow-fail 0, level-chg 0, 22 ms = sel 5 + blocks 15 + live 2) gen 0xa
```

- `no-tile` 非 0：叶子无任何已缓存层级（空洞风险）
- `grow-fail` 非 0：池满且无可淘汰块（叶子保持旧层级，不空洞）
- `level-chg` 非 0：本帧层级变化数（粘滞后稳态应为 0）
- `sel/blocks/live`：各段耗时，`blocks` 大=拷贝多，`live` 大=存活区间表构建慢

---

## 9. 已修复的关键 bug（避免重踩）

| 现象 | 根因 | 修复 |
| --- | --- | --- |
| 切模型/拖动崩溃 | `visibleCount_` 未重置，`memset` 长度下溢 4 GB | count 变化时清零 + 上传前 clamp |
| 排序线程崩溃 | 内联排序与后台排序共用 scratch/线程池 | 内联改单线程 + 独立 scratch |
| 又一次崩溃（`ComputeSort` cull 段）| 排序线程读 `cullRecords_` 时渲染线程 resize | **双缓冲 cull records** + 索引越界 clamp |
| 缩放极限黑屏 | pinch 到 0.1 而 near 剔除阈值 ~1.05 | near 固定 0.1、`minDistance=diag*0.002`、最远 0.75×far、剔除为 0 时回退全量绘制 |
| 整块压缩"变粗闪" | 压缩把累积集合换成预算集合，数量骤降 30–55% | 不再整块压缩；改为块替换 |
| 镜头不动也刷新/粗细抖动 | 缓存抖动：选中瓦片被 LRU 淘汰→重新解码 | 选中集瓦片禁止淘汰 + 缓存 5M |
| 拖动中不刷新 | 只在有瓦片解码时发布（选择变化不发布）| 选择集哈希，变化即发布 |
| 解出瓦片却不生效（十几秒不刷新）| 守卫 `if (!changed) return` 把 dirty 也挡了 | 仅当"无新数据且无变化"才跳过 |
| 块升级失败导致空洞 | 先释放旧块再分配，失败则无块 | 先分配成功再释放；失败保留旧块 |
| 峰值内存 950 MB+ | LRU 遇钉住块直接 return（缓存无上限）| 跳过钉住项继续淘汰 |
| 每帧重建 live 索引 40–80 ms | 90 万条索引 + 4 MB 跨线程拷贝 | 改为发布**存活区间**（每叶一条） |

---

## 10. 其他待办（与 LOD 无关）

- `docs/knowledge/3dgs-demo-assets.md`：资产来源/许可仍待补齐
- splat 解析 fuzz（PLY/压缩/SOG/GLB）
- GLES 后端下 3DGS 路径未验证；仅一台 Vulkan 设备
- 打包体积：`fruit_bowl.ply` 169 MB 进 HAP（可改默认 SOG）
- 提交前：性能日志（`GS::Sort stats` 已收进 `RENDER_DEV_ENABLED`；LOD 的 `LOGI` 仍常驻）
- 未 push：本地 `dev_3dgs` 领先远端多个提交，push 由用户负责

---

## 11. 2026-11 现状：A+B+C 落地与实测性能画像

基线提交 **`799d98e1`**（其上依次为 `f9787167`…`e8f4cfca`）。

### 11.1 已定型行为

| 机制 | 位置 | 说明 |
| --- | --- | --- |
| 全局缩放预算（B） | `lod_scene.h` `ChooseScale`/`AssignLevels` | 对 log2(s) 二分取"能满足预算的最大 s"，层级 = `dist < distances[k]·s`；取代逐叶重要度排序 |
| 运动冻结（A） | `lod_streamer.h` `IsMoving`/apply pass | 相机 300 ms 内有更新时，已绘制块层级不变；新叶若目标瓦片已解码直接取目标，否则取最粗层 |
| 空闲细化 | `lod_streamer.h` | 每 build 最多 32 叶换级，s 每次最多 ×1.06；`defer` 队列驱动 120 ms 节拍 |
| 交叉淡入（C） | `lod_streamer.h` | 换级总是写新块；旧块留 live 集合，1.0→0.40→0.16 淡出后释放；新块 0.30→0.45→0.68→1.0 淡入 |
| Pool 模式 | 共享全局 `g_gaussianPoolMode` | dirty range 为 0 不再全量重传（旧行为是一次 128 MB 重传 + 100 ms 内联排序，表现为白闪） |
| 排序同步 | 节点 `dataEpoch_`/`orderVersion_` | 异步排序结果带 epoch，过期丢弃；排序索引仅在新顺序落地时上传 |
| 其它 | 节点 | 实例化 `Draw(4, visibleCount)` + triangle strip；`MapBuffer` 后按 dirty range patch |
| 调参入口 | UI 下拉菜单 | 模型 / 相机模式 / 预算（1.5M / 1M / 600k）；**默认 LOD 场景 + 1M + 第一人称** |

### 11.2 实测性能画像（本机 Vulkan，1280×2598）

```text
稳态帧:          avg 2–9 ms, passes 0.1 ms        ← 渲染本身很便宜
发布帧 upload:   40–120 ms（按拷贝字节线性，1050 ranges 时 120 ms）
发布帧 set:      46–96 ms（upload 为 0 的帧：相机 UBO map / 排序结果拷贝 / 描述符更新）
```

- 预算曲线：1.5M live → 84–103 ms 慢帧；900k → 基本无 >40 ms 帧；600k → 同样无慢帧
- 900k 是本机"跟手"的临界（≈25 fps 以上）
- 该视角下目标层级全为 LOD2：相机高 488 m，最近叶子 ≥410 m；`s=1.99~2.64` 时 LOD1 阈值仅 158 m，且 1040 叶在 LOD2 已吃满预算

### 11.3 已验证不可行 / 无效的方向（勿重复）

| 尝试 | 结果 |
| --- | --- |
| 半分辨率离屏（片元 1/4） | 帧时间无变化 → 非填充受限 |
| 实例化绘制 / 4 顶点代替 6 顶点 | 帧时间无变化 → 非顶点数受限 |
| 去掉 gaussian SSBO 的 `DYNAMIC_BARRIERS` | 无变化 |
| **持久映射** gaussian/sort 缓冲（只 map 一次） | **画面损坏**（截图 408 KB → 84 KB，mean 177.8），引擎依赖 Map/Unmap 的可见性语义 → 已回退 |
| 运动期降密（620‰ + 允许变粗） | 停手后画面慢慢变粗糙残缺、触摸才恢复 → 已回退 |
| `g_forceRenderFrames` 30→4 | 观感变差 → 已回退（现为 30） |

### 11.4 下一步（按性价比）

1. **限制单次发布的新增块数**（如每次 ≤64 块，其余排队）：把"大转角一次 400+ 块"摊成几次，upload 尖峰 120 ms → ~20 ms，代价是新区域铺开慢 ~0.5 s
2. **细分 `set` 阶段**：相机 UBO / 排序结果拷贝 / 描述符更新三段计时（上次作用域写错编译失败，容易修），若出在相机 UBO 的 Map，可考虑复用/小缓冲
3. 预算与清晰度：`1.5M` 时 LOD1 仍难出现（阈值随 s 缩放）；要更清晰应提高预算或重导出更密瓦片

