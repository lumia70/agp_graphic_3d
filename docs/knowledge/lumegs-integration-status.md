# 切换到官方 LumeGS —— 当前现状与方案（交接文档）

> 仓库：`D:\Source\3DGS\graphic_graphic_3d`
> 分支：`dev_lumegs`（从 `dev_3dgs` 拉出，旧通路完整保留）
> 上游参考：`D:\Source\3DGS\open_harmony\graphic_graphic_3d`（gitcode `PVE/graphic_graphic_3d`，`master` @ `08d3b7af`，含 `!681 merge feat_lumegs`）
> 旧 LOD 自研方案的状态见 [3DGS 大场景 LOD 交接](3dgs-lod-status.md)
> Tiling host 渐进/ring/preload 与首帧经验见 [LumeGS tiling host 渐进加载经验](lumegs-tiling-host-lessons.md)

---

## 1. 目标

把 App 的 3DGS 渲染从"自研 CPU 排序 + 自研 LOD"切换到**官方 LumeGS 插件**：

- 小场景单模型：`GsComponent` + `RenderNodeGsRender`
- 大场景 LOD：`GsTilingComponent` + `IGsTilingSystem::CreateTile/SetTileVisible`（官方只提供瓦片流式渲染，**场景 JSON / LOD 选级由 host 负责**）
- 旧的自研通路保留，通过 `g_renderPath`（0 = 自研，1 = LumeGS）隔离，后续可做成应用侧可切换参数

---

## 2. 已完成（提交记录，按时间）

| 提交 | 内容 |
| --- | --- |
| `a70f5718` | `lume/` 全量同步到上游 `08d3b7af`，新增 `lume/LumeGS`（381 文件）；旧 `render_node_gaussian_splatting.*` 保留但不再注册 |
| `e7c24365` | `LumeApp` 在新引擎上编译通过：恢复 `os/ohos_ndk` 平台实现（上游改成了全源码树的 C++ ResourceManager 版本，App SDK 编不了）、补 `native_fence_fd.cpp` |
| `e66d029d` | LumeGS 资产生成工具：`compile_gs_shaders.py`（glslang 编 SPIR-V）+ `pack_gs_rofs.py`（ROFS 打包 + `GS_BIN` C 数组） |
| `538341b1` | LumeGS 编进 App（CMake 目标、`CORE_STATIC_PLUGIN_HEADER`、`GS_BIN` 声明、跳过 libwebp / GSNode、`sscanf_s`→`sscanf`） |
| `9880a111` | 生成真实 ROFS（73 shader stage / 271 文件 / 1.19 MB）并嵌入 |
| `c7a66aa5` | LumeGS 以**静态插件模式**注册（`DEFINE_STATIC_PLUGIN(GSPlugin)`）并让 `lumeapp` 链接它 → `LoadPlugins({})` 能看到 `Plugin[2]: GSPlugin` |
| `24cf00d4` | **修复 `gsshaders://`**：ROFS 内 shader 树必须放在 `shaders/` 前缀下（`gsshaders://` → `gsrofs://shaders/`）→ 73 个 shader 全部解析，`invalid shader` 警告归零 |
| `bcab2c8b` | `systemGraph.json` 增加 `GsTilingSystem`/`GsSystem`（optional）→ GS 组件管理器与系统被创建 |
| `94de11cf` | 创建独立的 GS RNG（后被禁用） |
| `a00618b6` | 把 `RenderNodeGsRender` 插入**相机场景 RNG**（`core3d_rng_cam_scene_lwrp.rng`，位于 CameraController 之后、材质 RenderSlot 之前） |
| `e220f81e` | 节点/系统的诊断日志；相机分辨率刷新 |
| `c90b0b15` | `GsSystem::Update` 入口诊断 |
| `69c074d2` | 默认模型统一为 doll（ETS `modelIndex=3`、原生 `g_modelIndex=3`），LumeGS 跟随下拉菜单 |
| `af7b61cd` | **修复 GS 渲染目标为空**：加载期创建的目标写到临时 render data，正式 render data 永远没有目标；改为 `!renderData->gaussianRenderTarget0` 时强制重建 |
| `dc93d99f` | 场景相机每帧跟随 App 相机；小模型默认 Orbit 模式 |

### 2026-11-06 续作（工作区，未提交）

定位并解掉了“模型不显示”的最后一串问题。现在 `doll.glb` / `godzilla.splat` / `fruit_bowl.sog` 可见，Orbit→First Person 不再黑屏，City LOD 走 LumeGS tiling 渲染。

1. **设备上的 LumeGS ROFS 是旧的**：`libLumeGS.so` 仍包含 `shader/...`（没有 `shaders/` 前缀）的 ROFS，日志为 `shader path (gsshaders://) not found`，所有 GS shader 都未注册。重新执行 pack + 构建后，`.so` 内含 `shaders/shader/...`，路径查找恢复。
2. **必须生成 `.lsb` 反射**：`ShaderModuleVk` 只有 `reflectionData.IsValid()` 才创建模块，否则报 `invalid vulkan shader module`（每个 stage 一条）。新增 `LumeApp/tools/gs_assets/gen_gs_lsb.py`，从 SPIR-V 直接生成 v1 LSB；`compile_gs_shaders.py` 编译后自动生成，`pack_gs_rofs.py` 把 `*.spv.lsb` 一并打进 ROFS。
   - push constant 大小优先取对应 `.shaderpl` 的 `pushConstant.size`，避免默认 3D 材质里“声明但未使用”的 8 字节 push block 与显式 PL 不一致。
3. **合成材质需要完整 scene node**：GS loader 给实体加 RenderMesh/Material（全屏 quad）时，实体若只有 Transform + GsComponent，没有 Node/WorldMatrix 组件，RenderSystem 不会把它排进渲染队列。改为 `INodeSystem::CreateNode()` 创建模型实体。
4. **按需渲染漏掉加载完成帧**：模型异步加载完成时，启动阶段的强制帧早已用完。补 `g_gsWasRenderable` 状态转变时 `g_forceRenderFrames = 30`。
5. **First Person 黑屏**：`SetFirstPerson(true)` 把 target 设为 eye，`CameraLookAt(eye, target)` 方向为零 → 视图矩阵退化。first-person 时改用 `-back` 方向取 look target。
6. **City LOD 仍显示 doll**：LumeGS 的 tiling host 通路未接；选 LOD 时把 `g_renderPath` 切到 0、销毁 GS 模型实体，并仅在 LumeApp 构建里重新注册旧 `RenderNodeGaussianSplatting`（`LUMEAPP_ENABLE_LEGACY_GS_NODE=1`，产品构建不受影响）。
7. **两个 SH compute shader 编译**：`gs_spherical_harmonics_common.h` 的 `<3d/shaders/common/...>` 改成引号 include，glslang 可解析；75 个 stage 全部编译通过，启动时不再有 invalid shader 警告。
8. **WebP ImageKit 适配完成**：新增 `lume/LumeGS/src/webp/image_loader_webp_imagekit.cpp`（LumeApp 编译），用 `OH_ImageSourceNative` / `OH_PixelmapNative` 解码 RGBA8888；平台返回预乘 alpha，未请求预乘时反预乘（SOG 的四个通道都存数据）。`WEBP_LOADER` 恢复 `CreateImageLoaderWebP` + `IMAGE_TYPES`。产品 GN 构建仍编译原 `image_loader_webp.cpp`（libwebp），未改 `BUILD.gn`；LumeApp 不链接 libwebp。
9. **LumeGS tiling 主机侧接通**：City LOD 改为 `g_renderPath = 1`，用 `lod::Scene::Load` 解析 `lod/syp_uav_0611`（2816 leaves / 212 files / 4 levels），`CullVisible` + `ChooseScale` + `AssignLevels` 选级；每个 (leaf, level) 懒创建 tile 实体并 `CreateTile` / `SetTileVisible`，`GsTilingSystem` 负责 SOG 解码、GPU pool、residency 和驱逐。旧的 legacy bridge/`RenderNodeGaussianSplatting` 保留但 City LOD 不再使用。
10. **画质/初始构图差异的根因**：LumeGS tiling 默认 `gsScaleFactor = 2`（半分辨率渲染 + 4 帧 jitter/upscale 合成），而旧自定义管线明确保留全分辨率。LumeApp 现在把单模型 `GsComponent.gsScaleFactor` 设为 1，并对 tiling 定义 `LUMEAPP_GS_TILING_FULL_RES=1`（`gs_system.cpp` 的 tiling-only 分支也设为 1）；产品 GN 构建不定义该宏，仍保持默认 2。全分辨率后 `GS diag sys ... scale=1`，地形/天空分界与旧管线一致，mean 从 `[133.6,138.1,133.8]` 变为 `[115.9,121.0,112.4]`（旧自定义 `[114.2,121.9,118.5]`）。
11. **City LOD 首帧渐进 + ring preload + pool AABB 修复**：host 现在先创建最近 64 个可见 leaf 的 coarsest tile，再预加载周围 ring 的 coarsest tile（1.3× FOV，最多 256 个），然后补齐其余 coarse/fine；每帧最多创建 32 个 tile，coarse 作为 fine resident 前的 fallback。LumeGS 侧给 pool primitive 设置全场景 AABB（默认 0 会被 `RenderNodeGsRender` 整体剔除），tile pool 放在 load render data 并与双缓冲共享。首帧从 ~20s 降到约 1s；快速拖拽后 ring 仍有内容（`vis=930 ring=256 fineRes=896 coarseRes=34`）。`GsConfigurationComponent` 对齐系统侧：useGpuSort/use24BitGpuSort/convertGsColorSrgb=true，covariance=2.25，sharpen=0.75。
    - 选级统计确认不是“统一用了粗 LOD”：默认 1M 下空中机位 `vis=940 cull=1876 scale=2.063 total=999947`，贴 1M 预算；旧自定义通路在 900k 时也是 ~900k。tile pool 必须同时存在于双缓冲 render data，否则 `GetTilePool()` 会在 flip 后为空（已在 `InitializePool` 修复）。

#### 设备验证（2026-11-06）

- doll orbit：截图 mean ≈ `[78.5, 70.8, 58.9]`，日志 `GS diag: draw numGs=426605 gpuSort=1 tiled=0`
- doll first-person：切换后画面与 orbit 一致（mean ≈ `[78.5, 70.8, 59.0]`），不再黑屏；日志 `ORBITCAM[first-person] ... dist=0`
- City LOD（LumeGS tiling）：`GS LOD: leaves=2816 files=212 levels=4`、`GsTilingSystem: pool initialized to 2048 slots`；coarse-first/ring 后首帧约 **1s**，截图 mean ≈ `[116.1, 121.2, 112.5]`，中心为地形
- City LOD 快速拖拽：`GS LOD select: vis=930 ring=256 fineRes=896 coarseRes=34`，拖拽后 200ms 截图 mean ≈ `[130.6, 134.3, 131.8]`，无天空/黑屏空隙
- LumeApp tiling 全分辨率：`GS diag sys: res=1280,2598 prev=1280,2598 scale=1`，mean ≈ `[116.1, 121.2, 112.5]`，地形/天空分界与旧自定义管线一致
- 选级统计：默认 1M 下 `GS LOD select: vis=940 cull=1876 scale=2063 total=999947 sel=940`，贴 1M 预算，不是粗 LOD 统一降级；coarse 64 + ring 210 + 其余 coarse/fine 分批 32/帧
- 手改 `default.scene.json` 的 `cameraInfo.position` 到空中后，LOD 以新机位正常渲染；同时验证了严格 JSON 与文件 owner 两个坑（见下）
- LOD 启动失败已改为保留上一个 LumeGS 模型：坏 JSON 时日志 `LOD scene failed to start; keeping previous model`，画面不再全黑
- `fruit_bowl.sog`：`SOG DecodeSogFile: version=2 count=750322`，截图 mean ≈ `[161.8, 118.9, 65.8]`，无 `SOG DecodeWebP: LoadImage failed`
- 最新启动日志无 `invalid shader` / `invalid vulkan shader module`

### 关键验证结果（设备日志）

```
PluginRegistry::LoadPlugins done — 3 plugins loaded
  Plugin[0]: AGP Render
  Plugin[1]: AGP 3D (core)
  Plugin[2]: GSPlugin

LOD scene dir / GS path: loading 'file:///.../files/assets/doll.glb'
GS path: loaded=1 primitiveValid=1          ← 模型加载成功
GS diag sys: res=1280,2598 prev=640,1299 scale=2 host=1 prims=1
（不再出现 "GS diag: handles INVALID"）
```

`af7b61cd` 之后截图统计从 `mean 199.6 / std 35.3`（纯白空帧）变为 `mean 23.0 / std 69.4`（深色 + 高对比），说明画面内容变了。

---

## 3. 当前状态与剩余工作

小模型（doll / godzilla / fruit_bowl.sog）和 City LOD（LumeGS tiling）都已出画；first-person 与 Orbit 切换正常。

剩余工作按优先级：

1. **移除 LOD 过渡桥**：City LOD 已不再使用 `g_renderPath=0` / 旧 `RenderNodeGaussianSplatting`，但 `LUMEAPP_ENABLE_LEGACY_GS_NODE`、`LoadLodScene` 和旧 streamer 仍保留。稳定后可删除宏/旧节点，只留旧代码作历史参考。
2. **LOD 首帧与性能调优**：City LOD 首帧已从 ~20s 降到约 **1s**（粗层 64 + ring 256 + 分批 32/帧 + 全场景 pool AABB）。SOG 解码不是瓶颈；剩余是快速转视角时 ring 覆盖范围（当前 1.3× FOV、256 tile）、1.5M/600k 预算、SOG cache 命中、每帧选级/`IsTileResident` 和全分辨率帧率。可用系统 host 的 `RequestRender` 状态位替代当前 30s 连续渲染窗口。
3. **收尾**：`generated/gs_rofs_embed.cpp` 改为构建时生成；清理 App 侧一次性日志（保留关键切换日志）；更新本文件和 `3dgs-lod-status.md`。

> 说明：当前没有完整的 OHOS 源码根/产品构建环境，验证以 LumeApp 的 HAP 构建 + 真机为准；`hb build graphic_3d` 未执行。

---

## 4. 架构与调用链（现状）

```
App (render_main.cpp)
 ├─ LoadPlugins({})               → Render / 3D / GSPlugin（静态注册）
 ├─ engine->Init / renderContext->Init / graphicsContext->Init
 ├─ ecs->Initialize()             （systemGraph.json 含 GsSystem/GsTilingSystem，implicitly 创建 GS 组件管理器）
 ├─ LoadSplatModel()              → 旧自研通路（仍完整运行，供 g_renderPath=0 使用）
 │    └─ 自研 streamer / 自研 RenderNode（已不注册）
 └─ g_renderPath == 1（当前默认）
      ├─ PollGsRenderPath()
      │    ├─ 首帧 InitGsRenderPath(g_gsModelFile)
      │    │    ├─ 创建唯一 GsConfigurationComponent（useGpuSort = true）
      │    │    ├─ 创建模型实体：Transform + GsComponent{ gsPath = file://<filesDir>/assets/<model> }
      │    │    └─ 独立 GS RNG（当前禁用）
      │    ├─ 模型切换（下拉菜单）→ Destroy/Create GsComponent
      │    └─ 每秒打印 isLoaded / isPrimitiveValid
      ├─ 相机同步：每帧 CameraLookAt(eye/target/up from g_orbitCam)
      └─ TickFrame → GsSystem::Update（创建渲染目标/排序）
RenderFrame(allRngs)  → scene RNG（其中已插入 RenderNodeGsRender）
```

### 关键文件

| 文件 | 作用 |
| --- | --- |
| `LumeApp/entry/src/main/cpp/render_main.cpp` | 引擎初始化、旧通路、**LumeGS 通路**（`InitGsRenderPath`/`PollGsRenderPath`/`g_renderPath`/相机同步/模型切换）；单模型实体用 `INodeSystem::CreateNode()`；模型可渲染时补强制帧；first-person look target；**LumeGS tiling host**（`LoadGsLodScene`/`DestroyGsLodTiles`/`UpdateGsLodTiles`：解析 `lod_scene.h`、按 budget 选级、懒创建 tile 实体、同步 scene camera clip/FOV） |
| `LumeApp/entry/src/main/resources/rawfile/shaders/3d/systemGraph.json` | 增加 `GsTilingSystem`/`GsSystem`（optional）|
| `LumeApp/entry/src/main/resources/rawfile/shaders/3d/rendernodegraphs/core3d_rng_cam_scene_lwrp.rng` | 插入 `RenderNodeGsRender`（`CORE3D_RN_CAM_GS`）|
| `LumeApp/tools/gs_assets/compile_gs_shaders.py` | glslang 编 GS 的 GLSL（注入 `GL_GOOGLE_include_directive`，**不要 `-DVULKAN`**）；编译后调用 LSB 生成 |
| `LumeApp/tools/gs_assets/gen_gs_lsb.py` | **新增**：从 SPIR-V 反射生成 v1 `.spv.lsb`（descriptor/push constant/spec constant/vertex input/local size）；push constant 优先取 `.shaderpl` 的 `pushConstant.size` |
| `LumeApp/tools/gs_assets/pack_gs_rofs.py` | 打包 ROFS（shader 树必须在 `shaders/` 前缀下，包含 `.spv` 与 `.spv.lsb`）+ 生成 `GS_BIN` |
| `LumeApp/entry/src/main/cpp/generated/gs_rofs_embed.cpp` | 生成的大数组（约 7 MB，已入库；后续可改构建时生成）|
| `LumeApp/entry/src/main/cpp/CMakeLists.txt` | LumeApp 专属：`LUMEAPP_ENABLE_LEGACY_GS_NODE=1`（City LOD 过渡桥）、`LUMEAPP_GS_TILING_FULL_RES=1`（全分辨率 tiling）；编译 `image_loader_webp_imagekit.cpp` 而不是 libwebp 版本 |
| `lume/LumeRender/src/node/core_render_node_factory.cpp` | `#if defined(LUMEAPP_ENABLE_LEGACY_GS_NODE)` 时重新注册旧 `RenderNodeGaussianSplatting`（产品构建不定义） |
| `lume/LumeGS/assets/app/shaders/common/gs_spherical_harmonics_common.h` | 角度 include 改引号 include，glslang 才能解析（两个 SH compute shader 现在可编译）|
| `lume/LumeGS/src/component_dll.cpp` | 改动：`GS_BIN` 声明为 `const unsigned char[]`；`CORE_STATIC_PLUGIN_HEADER` + `DEFINE_STATIC_PLUGIN`；跳过 GSNode；`WEBP_LOADER` 恢复 `CreateImageLoaderWebP` + `IMAGE_TYPES` |
| `lume/LumeGS/src/webp/image_loader_webp_imagekit.cpp` | **新增**：LumeApp 的 ImageKit WebP 加载器（RGBA8888、预乘/反预乘、垂直翻转、metadata-only；不支持动画 WebP）。产品 GN 仍使用原 `image_loader_webp.cpp`（libwebp），两者只编译其一 |
| `lume/LumeGS/src/systems/gs_system.cpp` | 改动：`GsSystem::Update` 诊断日志；**渲染目标强制重建条件**（`!renderData->gaussianRenderTarget0`）；tiling-only 分支在 `LUMEAPP_GS_TILING_FULL_RES` 下使用 `gsScaleFactor=1` |
| `lume/LumeGS/src/render/render_node_gs_render.cpp` | 改动：原诊断为 120 帧节流（`handles INVALID`、`RecordGaussianDraw`），本次续作已回退临时调高的诊断频率 |
| `LumeApp/entry/src/main/ets/pages/Index.ets` | 三个下拉菜单（模型 / 相机 / 预算），默认 doll + Orbit；LOD 选项会切 first-person |

---

## 5. 构建 / 部署 / 验证

```powershell
# 改过 GS GLSL / ROFS 时，先从仓库根重编 shader + 打包（会生成 .spv 和 .spv.lsb）
python LumeApp/tools/gs_assets/compile_gs_shaders.py
python LumeApp/tools/gs_assets/pack_gs_rofs.py

# 构建（workdir 必须是 LumeApp，命令里不要再 cd）
$env:PATH="D:\Coding\DevEcoStudio\jbr\bin;D:\Coding\DevEcoStudio\tools\hvigor\bin;"+$env:PATH
$env:JAVA_HOME="D:\Coding\DevEcoStudio\jbr"
Remove-Item -Force entry/libs/arm64-v8a/*.so -ErrorAction SilentlyContinue
hvigorw assembleHap --mode module -p product=default --no-daemon

# 部署（不要 uninstall：会清空 files/assets！）
hdc install -r entry\build\default\outputs\default\entry-default-signed.hap
hdc shell "aa force-stop com.example.lumeapp"; hdc shell "hilog -r"
hdc shell "aa start -a EntryAbility -b com.example.lumeapp"

# 观测
hdc shell "hilog -x | grep -E 'GS|Plugin\[|invalid shader|Loading gaussians' | tail -30"
hdc shell "snapshot_display -f /data/local/tmp/a.jpeg"
hdc file recv /data/local/tmp/a.jpeg C:\Users\lambda\AppData\Local\Temp\opencode\a.jpeg
python -c "from PIL import Image,ImageStat; im=Image.open(r'...a.jpeg').convert('L'); s=ImageStat.Stat(im); print('mean',round(s.mean[0],1),'std',round(s.stddev[0],1))"
```

**资产**（不放进 HAP，放设备沙箱）：
- 目录：`/data/app/el2/100/base/com.example.lumeapp/haps/entry/files/assets/`
- 单模型：`D:\Source\3DGS\data\single_object`（`doll.glb`、`the_fruit_bowl.sog`、`godzilla.splat`、`cube.glb`）
- 大场景：`D:\Source\3DGS\data\lod\SanYaPo_UAV_diffuse_gsbox_lod_0611_edit`
- 推送后：**先查当前 App 的实际 UID**，不要照抄固定值：
  ```powershell
  hdc shell "pidof com.example.lumeapp"
  hdc shell "cat /proc/<pid>/status | grep Uid"   # 本机当前是 20020212
  hdc shell "chown -R <uid>:<uid> /data/app/el2/100/base/com.example.lumeapp/haps/entry/files/assets"
  hdc shell "chmod -R u+rwX,g+rX,o+rX /data/app/el2/100/base/com.example.lumeapp/haps/entry/files/assets"
  ```
  owner 用错（例如文档早期写的 20020208）时 `fopen` 会失败，日志表现为 `LOD: missing default.scene.json / lod-meta.json`，很容易误判成文件不存在。

**设备上需要手动同步的两个资产**（App 从 `files/shaders/3d/` 读，不是 HAP rawfile）：
- `files/shaders/3d/systemGraph.json`
- `files/shaders/3d/rendernodegraphs/core3d_rng_cam_scene_lwrp.rng`

---

## 6. 已知坑（勿重踩）

1. **不要 `hdc uninstall`** —— 会清空 `files/`，资产全丢（已踩过一次）
2. HiLog 会丢消息：`CORE_LOG_I` 在部分场景不出现；`%{public}u/%{public}zu` 组合的长格式也可能被丢 → 诊断日志用 `CORE_LOG_E` + 简单 `%d`
3. `compile_gs_shaders.py` **不能加 `-DVULKAN`**（glslang 已预定义，会 macro redefined）；但 include 需要注入 `GL_GOOGLE_include_directive`
4. ROFS：`gsshaders://` → `gsrofs://shaders/`，`gspipelinelayouts://` → `gsrofs://pipelinelayouts/`，`gsvertexinputdeclarations://` → `gsrofs://vertexinputdeclarations/`
5. LumeGS 编译约束：`CORE_PLUGIN=1` 会导致 `PLUGIN_DATA` 未定义且不注册；必须用 `CORE_STATIC_PLUGIN_HEADER` + `DEFINE_STATIC_PLUGIN` 静态模式，并让 `lumeapp` 链接 `LumeGS`
6. `GsSystem` 的渲染目标创建存在"临时 render data"陷阱（已在 `af7b61cd` 修复），若再改动加载流程注意回归
7. 瓦片模式要求 `useGpuSort = true` 且所有模型 `computeLods = false`
8. 瓦片只支持 `.sog` / `.glb`
9. 官方没有 scene/LOD-meta 解析器，也没有自动 LOD 选级；这些要在 host 侧做（可复用旧 `lod_scene.h` 的解析与选级）
10. 非瓦片模式最多 4 个模型；默认内存预算 SOG 缓存 256 MB、GPU pool 704 MB（`lsgsLevel` 目前只影响这两个预算）
11. **`.lsb` 不是可选的**：Vulkan `ShaderModuleVk` 只有 `reflectionData.IsValid()` 才创建模块；缺 `.lsb` 会逐 shader 报 `invalid vulkan shader module`，表现仍像“shader 没加载”。改 shader 后必须重跑 compile（生成 `.spv.lsb`）+ pack。
12. **GS 合成 quad 的实体必须是完整 scene node**：只有 `TransformComponent + GsComponent` 时，GS loader 虽然加了 RenderMesh/Material，但缺 Node/WorldMatrix，RenderSystem 不会排队 → GS 已画到离屏 target，屏幕仍黑。用 `INodeSystem::CreateNode()`。
13. **改完 assets 必须重建 `.so`**：设备上可能残留旧 ROFS（例如 `shader/...` 没有 `shaders/` 前缀）。先检查 `libLumeGS.so` 里是否有 `shaders/shader/...`，否则会出现 `shader path (gsshaders://) not found`。
14. **`LUMEAPP_ENABLE_LEGACY_GS_NODE` 只给 LumeApp**：产品构建不要定义；这是 City LOD 在 LumeGS tiling 完成前的过渡桥。
15. **first-person 不能把 eye 当 look target**：`SetFirstPerson(true)` 后 target==eye、distance=0；`CameraLookAt(eye, eye)` 会退化。用 `eye - back * lookAhead`。
16. **`default.scene.json` 是严格 JSON**：手改时多一个字符（例如 `1000S`）就会让整个文件 `parse failed`，LOD 不启动。改完先 `python -m json.tool` 校验。
17. **设备文件 owner 必须是 App 实际 UID**（本机 `20020212`）：owner 错误时 `fopen` 失败，日志是 `LOD: missing default.scene.json / lod-meta.json`，不是文件不存在。
18. **LOD 启动失败现在保留上一个模型**：坏 JSON/缺文件时 `LOD scene failed to start; keeping previous model`，不会清掉 LumeGS 实体。修好文件后，ETS `selectModel` 对相同索引会早退，需要先切到别的模型再切回 LOD（或重启 App）。
19. **`cameraInfo.lookUp` 当前未使用**：`LoadLodScene` 硬编码世界 up `(0,1,0)`；需要非世界 up/roll 时要补解析。
20. **ImageKit WebP 返回预乘 alpha**：`sog_parsing.cpp` 用 `LoadImage(view, 0u)`（未请求预乘），加载器必须反预乘 RGB；否则 SOG 的 opacity/quat/SH 通道会被破坏。只有 `IMAGE_LOADER_PREMULTIPLY_ALPHA` 置位时才保留预乘并打 `FLAGS_PREMULTIPLIED_ALPHA_BIT`。
21. **WebP 加载器二选一**：LumeApp 编译 `image_loader_webp_imagekit.cpp`，产品 GN 编译 `image_loader_webp.cpp`（libwebp）；同一目标不能同时链接，否则 `CreateImageLoaderWebP` 重复定义。LumeApp 的动画 WebP 返回不支持（SOG 不需要）。
22. **`WEBP_LOADER` 必须带 createLoader + IMAGE_TYPES**：只有 UID 的占位 `ImageLoaderTypeInfo` 会让 `LoadImage` 找不到 WebP loader，SOG 解码失败。
23. **tiling host 要懒创建 (leaf, level) 实体**：`GsTilingSystem` 第一次 Update 会 `QueueExistingTileRequests()` 把所有已 `CreateTile` 的实体排入加载队列。启动时一次性创建 4 个 level 会尝试解码整个场景；只创建当前选中的 level。
24. **tile 的 `uri` / `offset` / `count`**：`uri` 是 `file://` + scene dir + `lod-meta.json` 的 `filenames[file]`；`offset/count` 是 SOG 内的 **splat 索引范围**（不是字节偏移），直接来自 `lod_scene.h` 的 `Leaf::lods[level]`。
25. **scene camera 的 zFar 默认是 100**：LOD 相机距离约 880，必须在 `ApplyGsLodCamera` 里同步 scene camera 的 `yFov/zNear/zFar`，否则 LumeGS render node 会把整个场景裁掉。
26. **tiling 期间必须持续强制渲染**：tile 异步解码/上传期间若回到按需空闲，会出现“切 Orbit 后只剩天空”。选级变化时延长 `g_gsLodForceUntil` 并在 `IsTileResident` 为 false 时设 `g_forceRenderFrames=3`。
27. **`lsgsLevel` 在首次 pool 初始化后应用**：日志可能看到 `lastAppliedQualityLevel_ 4294967295`；当前各 preset 的 SOG cache/GPU pool 相同（256 MB / 704 MB），LOD 预算实际由 host 的 `ChooseScale` 控制。
28. **tiling 默认是 2x 超分**：`gs_system.cpp` 的 tiling-only 分支默认 `SetGsScaleFactor(2U)`，即半分辨率 + jitter/upscale；旧自定义管线是全分辨率。LumeApp 用 `LUMEAPP_GS_TILING_FULL_RES=1` + 单模型 `gsScaleFactor=1` 改为全分辨率，画质和构图才与旧管线一致；产品构建仍默认 2。
29. **`SetGsScaleFactor` 只生效一次**：`RenderDataStoreDefaultGS::SetGsScaleFactor` 在已设置时直接跳过。LumeApp 在启动加载单模型时就把 `gsScaleFactor` 设为 1，之后切 tiling 不会被 2 覆盖；若直接从 tiling 启动，则靠 `LUMEAPP_GS_TILING_FULL_RES` 分支设 1。
30. **判断是否“LOD 太粗”先看选级统计**：`GS LOD select: vis/cull/scale/total/sel`。`total` 应接近预算（默认 1M → 999947）；若 far below，才检查相机/预算/`ChooseScale`。当前空中机位 `vis=940 cull=1876`，不是粗 LOD 统一降级。
31. **tile pool 必须放在 load render data 并与双缓冲共享**：只调 `dataStoreGs_->SetTilePoolConfig`（当前 render data）时，`GetSafeLoadingGsRenderData()`/`FlipValidGsRenderData()` 后 `GetTilePool()` 可能为空 → `prep fail tile pool missing`、无 SOG decode。现在 `InitializePool` 直接把 pool 配置写到 `loadRd`，并让 `currentRd->tilePool` 指向同一个对象。
32. **City LOD 首帧（已从 ~20s 降到 ~1s）**：旧实现一次创建 940 tile，且 pool primitive 的 AABB 默认 0，`RenderNodeGsRender` 会先按 pool primitive AABB 做整体视锥剔除 → 即使 tile resident 也整池被剔掉。修复：
    - `RegisterPoolPrimitive` 给 pool primitive 设全场景 AABB（`±1e30`），每个 tile 仍按自身 AABB 剔除；
    - host 改成粗层优先：最近 64 个可见 leaf 的 coarsest tile → 周围 ring 的 coarsest preload（拓宽 1.3× FOV，最多 256 个）→ 其余可见 coarse → 可见 fine；
    - 每帧最多创建 32 个 tile；可见 leaf 用 coarsest 作为 fallback，fine resident 后隐藏；
    - `GsConfigurationComponent` 对齐系统侧：useGpuSort/use24BitGpuSort/convertGsColorSrgb=true，covariance=2.25，sharpen=0.75；
    - tile pool 在 load render data 上并和双缓冲共享（见 31）。
    实测首帧 ~1s；快速拖拽后 `GS LOD select: vis=930 ring=256 fineRes=896 coarseRes=34`，画面保持地形、无天空。

---

## 7. 后续计划（顺序）

1. **移除过渡桥**：City LOD 已走 LumeGS tiling；稳定后删除 `LUMEAPP_ENABLE_LEGACY_GS_NODE`、旧 `LoadLodScene`/`g_lodStreamer` 使用路径和 `g_renderPath=0` 分支
2. **LOD 性能与画质**：验证 600k / 1.5M 预算切换、首次 tiling 加载延迟、SOG cache 命中率、每帧选级/`IsTileResident` 开销；全分辨率 tiling 帧率对比；必要时调 covariance/sharpen/颜色空间/SH 上限进一步对齐旧管线
3. **收尾**：`generated/gs_rofs_embed.cpp` 改为构建时生成；清理 App 侧一次性日志；更新本文档与 `3dgs-lod-status.md`

---

## 8. 旧自研通路状态（`g_renderPath = 0` 时）

完整保留、可编译；旧 `RenderNodeGaussianSplatting` 默认不注册，LumeApp 通过 `LUMEAPP_ENABLE_LEGACY_GS_NODE=1` 恢复。City LOD 已切到 LumeGS tiling，旧通路仅作回滚/参考。历史优化都在 `dev_3dgs`：

- 全局缩放预算 + 运动冻结 + 交叉淡入（`e8f4cfca`）
- 配额 32 + 新叶直达目标 + LODDIAG（`7f7d0392`）
- 4 顶点 triangle strip（`0f293e00`）
- 下拉菜单 + 预算切换（`e260bd6b`、`9255b549`、`c9d87cc0`）
- 平均帧日志（`6239200f`）、删除双击切预算（`f9787167`）、dirty range 合并（`799d98e1`）
- 性能结论、已否定方向都记录在 [3DGS 大场景 LOD 交接](3dgs-lod-status.md) 第 11 节
