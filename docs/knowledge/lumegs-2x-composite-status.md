# LumeGS 2x 渲染：白屏根因、当前修复与遗留问题（交接）

> 生成时间：2026-11-08
> 基线 commit：`255ed734`（feat: add dedicated LumeGS tiling host state machine）
> 工作区状态：**4 个文件未提交**（见 §3），当前构建/安装的是 **2x 修复版**
> 关联文档：[LumeGS 接入现状](lumegs-integration-status.md)、[tiling host 渐进加载与首帧经验](lumegs-tiling-host-lessons.md)

---

## 1. 一句话现状

| 项 | 状态 |
| --- | --- |
| LumeGS tiling LOD 主链路（渐进加载 / coarse fallback / ring preload / 状态机） | ✅ 已跑通并提交（`111a7895`/`90054941`/`255ed734`） |
| 1x 全分辨率（`gsScaleFactor=1` + `LUMEAPP_GS_TILING_FULL_RES=1`） | ✅ 稳定，出画正常 |
| 2x 默认分辨率（系统侧同款默认值） | ✅ **白屏根因已定位并修复**，可出画（截图 mean ≈ 117.5） |
| 2x 画质与切换体验 | ❌ 遗留 3 个问题（§4），未解决 |
| 代码提交 | ⏳ 修复只在工作区，未 commit |

三个遗留问题（用户 2026-11-08 反馈）：

1. 2x 默认颜色比 1x 怪，精细度也差一点；
2. 切到小模型（doll）再切回 LOD，能出画但**整个画面偏白**；
3. 切到小模型后**第一次滑动卡约 1s**，之后正常。

---

## 2. 根因：tiling 目标被「缩放两次」

### 2.1 现象

2x 下 Doll 单模型正常，tiling LOD 全白（截图 mean ≈ 194，纯天空/背景色），
但 `GS diag sys` 显示 pool/SOG/可见 tile 全部正常：

```text
GS diag sys: res=1280,2598 prev=640,1299 scale=2 ...
```

### 2.2 调用链与错位点

```text
GsSystem::Update()                          lume/LumeGS/src/systems/gs_system.cpp
  ├─ upscaleRatio      = gsScaleFactor * xegUpscaleFactor        // 2 * 1 = 2
  ├─ effectiveRenderSize = renderResolutionFromCC_ / upscaleRatio // 1280x2598 -> 640x1299
  ├─ previousResolution_ = effectiveRenderSize                   // 存的是"已缩放"尺寸
  └─ tiling-only 分支（无 renderTargetHostEntity_）
       CreateGsRenderTargets(renderData, host, loadCtx, previousResolution_)   // ← 640x1299
         └─ CreateGsRenderImages()  lume/LumeGS/src/util/gs_loading.cpp:166
              renderSize = resolution / scaleFactor / upscaleFactor            // 640/2 -> 320x650
```

最终 `gaussianRenderTarget0` 只有 **320x650**，而 render node 仍按 640x1299 绘制，
composite 采样一张尺寸不匹配的目标 → 全白。Doll 走的是 full resolution 分支
（1280x2598 传进去，内部除 2 得 640x1299），所以单模型正常。

修复前日志铁证：

```text
Gs diag images: res=640x1299 scale=2 upscale=1.00 render=320x650   // 双重缩放
Gs diag target0: 320x650 valid=1
```

### 2.3 修复（`gs_system.cpp`，工作区未提交）

```cpp
// 之前
GS::CreateGsRenderTargets(renderData, renderTargetHostEntity_, loadCtx, previousResolution_);
// 现在：CreateGsRenderImages 内部自己会除 gsScaleFactor，这里必须传原始输出分辨率
GS::CreateGsRenderTargets(renderData, renderTargetHostEntity_, loadCtx, renderResolutionFromCC_);
```

修复后 tiling target 正确生成 **640x1299**，2x 出画：截图 mean ≈ 117.5，地形正常。

### 2.4 顺带修掉的第二处隐患（`gs_loading.cpp:209`）

```cpp
GsRenderStatusUbo renderStatusUbo;        // 之前：未初始化
GsRenderStatusUbo renderStatusUbo {};     // 现在：零初始化
...
renderStatusUbo.enableXegUpscale = renderData->enableXegUpscale ? 1u : 0u;
```

`enableXegUpscale` 原为未初始化值，非零时 2x 分支的 composite shader 会去采样
`uGSTex1`（XEG 输出，本工程未启用）→ 也可能白屏。注意：`UpdateRenderStatusUbo`
（`render_node_gs_render.cpp:902`）每帧用 push constant 走 compute 覆盖同一个 UBO，
所以这处主要影响「UBO 刚创建、compute 还没跑」的前几帧。

### 2.5 回到 2x 默认的两处改动

| 文件 | 改动 | 含义 |
| --- | --- | --- |
| `render_main.cpp`（2 处：`InitGsRenderPath` / `PollGsRenderPath`） | 删除 `gs->gsScaleFactor = 1;` | 不再强制 1x，走 LumeGS 默认 2x |
| `CMakeLists.txt` | 删除 `target_compile_definitions(LumeGS PRIVATE LUMEAPP_GS_TILING_FULL_RES=1)` | 同上，去掉全分辨率宏 |

---

## 3. 当前工作区差异（`git diff` 摘要）

```text
 LumeApp/entry/src/main/cpp/CMakeLists.txt   | 2 --
 LumeApp/entry/src/main/cpp/render_main.cpp  | 2 --
 lume/LumeGS/src/systems/gs_system.cpp       | 6 +++++-
 lume/LumeGS/src/util/gs_loading.cpp         | 5 ++++-
 4 files changed, 9 insertions(+), 6 deletions(-)
```

另有未跟踪文件 `docs/knowledge/.gitignore`，以及本文件。
**建议提交信息**：`fix: create tiled LumeGS targets at the output resolution`

---

## 4. 遗留问题与下一步排查（按优先级）

### 4.1 问题 A：2x 默认颜色偏怪、精细度略差于 1x

**预期内的部分**（不是 bug）：

- 2x = 半分辨率 target（640x1299）+ 4 帧 jitter + upscale + sharpening 合成，
  与 1x 直接采样 `uGSTex0` 路径不同；
- 精细度必然下降，颜色经过 jitter 合成与 sharpen 会偏移。

**可疑的调参项**（app 侧覆盖了点，`render_main.cpp:1077`）：

| 参数 | 默认 | app 当前值 | 影响 |
| --- | --- | --- | --- |
| `renderSharpeningCoefficient` | 0.25 | **0.75** | 强化过度会让边缘过冲、观感偏亮偏「脏」 |
| `renderCovarianceStddev` | 2.0 | **2.25** | 覆盖率变大，叠层更厚，颜色更糊 |
| `convertGsColorSrgb` | true | true | 与 swapchain 是否 sRGB 相关，错了会整体偏色 |

**下一步实验**（一次只改一个）：把 sharpen 降回 0.25（或 0）→ 对比截图；
covariance 降回 2.0 → 再对比；两者都正常后，再判断剩余差异是 jitter 相位还是 color space。

### 4.2 问题 B：Doll → LOD 回切后整体发白（最高优先级）

**现象**：LOD → Doll（正常）→ 再切回 LOD，能出画但整屏偏白。

**已排除**：不是「重入后没内容」——那是 `90054941` 修过的 `poolInitialized_` 问题，
现在 pool/SOG/tile 都正常。

**当前假设**（按可能性排序）：

1. **色彩空间双转换**：回切时 `RemoveGsRenderTargets` + `CreateGsRenderTargets` 重建，
   新 `GsRenderData` 的 `convertGsColorSrgb` / `enableXegUpscale` 未必继承 app 配置
   （`EnsureGsConfiguration` 只创建一次），导致 linear→sRGB 被做两次 → 发白。
   位置：`gs_loading.cpp:209 CreateGsStatusUbo`、`render_node_gs_render.cpp:902 UpdateRenderStatusUbo`。
2. **renderStatus UBO 竞态**：`CreateGsStatusUbo` 写初值、`UpdateRenderStatusUbo`
   每帧用 push constant 覆盖，重建首帧可能拿到 `renderScaleFactor=0xFFFFFFFF`
   或 `DRSScale` 不匹配的值 → upscale 系数与 target 不符 → 过曝。
3. **jitter 状态未复位**：`jitterRendered_` 持续自增，回切后 target 已清空但相位/帧计数没重置，
   4 帧 jitter 叠加采样 → 泛白。
4. **深度/累积状态**：`gsEnableDepthPass` 或 GS 累积缓冲在重入时未清。

**判别实验（关键，先做这个）**：

```text
把 gsScaleFactor 临时改回 1（1x），做同样的 Doll → LOD 回切：
  - 1x 也发白  → 与 2x/jitter 无关，是重入时 material/UBO/color 状态；
  - 1x 正常    → 锁定 2x composite（jitter/upscale/sharpen）状态未复位。
```

然后在「首次进 LOD」和「回切进 LOD」两处各打一次同样的日志做对比：
`scale / drs / xeg / srgb / sharp / jitter`（用 `CORE_LOG_E` + `%d`，
HiLog 会丢长格式，见 §6）。

### 4.3 问题 C：切到 Doll 后第一次滑动卡约 1s

**假设**：

1. 首次 `GetPsoData` / `BindPipeline` 触发 PSO 创建与 shader 变体编译（2x 分支变体不同）；
2. 换模型时 `RemoveGsRenderTargets` + 重建 material/descriptor set 的一次性成本；
3. 换入 Doll 后第一次相机变化才触发「需要重画」路径，把上面的初始化成本集中到这一帧。

**判别实验**：在 `RenderNodeGsRender::Execute` 的分阶段（`UpdateRenderStatusUbo` /
`BindPipeline` / `Dispatch` / `RecordGaussianDraw`）打时间戳，看这 1s 落在哪一段；
同时把 `gsScaleFactor` 改回 1x 复测一次——若 1x 同样慢，则与 2x 无关，是切模型的通用成本。

---

## 5. 架构速查（谁负责什么）

| 层 | 位置 | 职责 |
| --- | --- | --- |
| host（应用侧） | `LumeApp/entry/src/main/cpp/render_main.cpp` | 模型/相机/预算切换、LOD 场景状态机（`RebuildTileSelection` → `ProcessPendingTileCreates` → `ProcessVisibleGsLodTiles` → `ProcessPreloadGsLodTiles`）、GS 配置实体（`EnsureGsConfiguration`）|
| LumeGS 引擎侧 | `lume/LumeGS/` | `GsSystem`（渲染目标/缩放/primitive 参数）、`GsTilingSystem`（pool/tile/residency）、`gs_loading`（target/material/UBO 创建）、`RenderNodeGsRender`（排序、绘制、composite） |
| host 传给引擎的入参 | `render_main.cpp:1128` / `:1170` | `gs->gsPath`、`computeLods=false`；**当前不再设置 `gsScaleFactor`**（默认 2x） |

**`gsScaleFactor` 语义**：渲染目标的降采样倍数。2 → target 为输出分辨率的 1/2，
composite 阶段再 upscale 回来（4 帧 jitter + sharpen）。`SetGsScaleFactor()` 实际只生效一次，
所以「Doll 设 2x、LOD 想单独 1x」这条路走不通——这也是之前必须用编译宏的原因。

**相关默认值**（`gs_configuration_component.h`）：`enableXegUpscale=false`、
`xegUpscaleFactor=1.0`、`convertGsColorSrgb=true`、`renderCovarianceStddev=2.0`、
`renderSharpeningCoefficient=0.25`、`lsgsMaxLodRenderCount=1000000`、`lsgsLodBias=1.8`。

---

## 6. 构建 / 部署 / 验证（照抄即可）

```powershell
# 改过 GS GLSL / ROFS 时先从仓库根重编 shader（本轮的改动不涉及 shader，可跳过）
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

判读基线：正常地形 ≈ 110–125；纯天空/白屏 ≈ 190+。

---

## 7. 不要重踩的坑（本轮新增）

1. **不要再用「旧 host + 旧 LumeGS 混编」做 A/B 二分**。本轮做过 4 组矩阵
   （旧/新 host × 旧/新 LumeGS），2x **全部白屏**，但根因并不在 host/迁移，而在
   运行时 target 尺寸；混编引入的接口/宏不一致反而制造了假象。
   定位这类问题直接加 `target 尺寸 / renderStatus 数值` 一次性日志，比二分快一个数量级。
2. `CreateGsRenderImages` **内部自己会除 `gsScaleFactor`**，任何调用方传进来的必须是
   「原始输出分辨率」；传已缩放尺寸就是双重缩放。
3. `renderStatusUbo` 一定要零初始化再逐字段赋值（未初始化字段会改变 shader 分支）。
4. 诊断日志一律 `CORE_LOG_E` + 简单 `%d`，长格式会被 HiLog 丢掉。
5. 临时 `Gs diag` 日志验证完要清掉（本轮已清理，勿再提交）。

---

## 8. 下一步建议顺序

1. 把工作区这 4 个文件提交（2x 白屏修复是独立成果，先落库）；
2. 做 §4.2 的 1x/2x 回切对比实验 → 定位「回切发白」；
3. 再做 §4.3 的分段耗时日志 → 定位「首次滑动 1s」；
4. 最后回到 §4.1 的调参（sharpen/covariance），把 2x 观感拉齐 1x；
5. 收尾：补 `docs/knowledge/lumegs-tiling-host-lessons.md` 的「未完成」列表（本轮 2x 结论需并入）。

---

## 9. 给新 session 的开场提示

```text
读 docs/knowledge/lumegs-2x-composite-status.md 和 docs/knowledge/lumegs-tiling-host-lessons.md，
继续处理 2x 渲染的遗留问题：优先 4.2「Doll→LOD 回切后发白」，再做 4.3「首次滑动 1s」。
工作区有 4 个未提交文件（2x 白屏修复），先确认基线再改动。
```
