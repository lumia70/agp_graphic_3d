# 切换到官方 LumeGS —— 当前现状与方案（交接文档）

> 仓库：`D:\Source\3DGS\graphic_graphic_3d`
> 分支：`dev_lumegs`（从 `dev_3dgs` 拉出，旧通路完整保留）
> 上游参考：`D:\Source\3DGS\open_harmony\graphic_graphic_3d`（gitcode `PVE/graphic_graphic_3d`，`master` @ `08d3b7af`，含 `!681 merge feat_lumegs`）
> 旧 LOD 自研方案的状态见 [3DGS 大场景 LOD 交接](3dgs-lod-status.md)

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

## 3. 当前唯一阻塞（下一步从这里开始）

**现象**：画面仍然看不到模型（全黑 / 深色），日志每帧刷：

```
W shader_manager.cpp:1043: ShaderManager: invalid shader
    gsshaders://shader/gs3d_gaussian_splatting.shader
```

这是 **GS 实体的材质 shader**（`gs_loading.cpp:291` 用它给模型实体创建材质，该材质负责把 GS 渲染目标的贴图合成到场景）。它加载失败 → 模型没有可见材质 → 画面黑。

**已确认这些依赖文件都存在**：

| 资源 | 位置 | 状态 |
| --- | --- | --- |
| `gs3d_gaussian_splatting.shader` | `lume/LumeGS/assets/app/shaders/shader/` | 存在（JSON 内联 state，`variantName: TRANSLUCENT_FW`、`renderSlot: CORE3D_RS_DM_FW_TRANSLUCENT`） |
| `gs3d_gaussian_splatting.vert.spv` / `.frag.spv` | `LumeApp/entry/.cxx/gs_assets/shader/` | 已生成（2140 B / 25116 B） |
| `gs3d_gaussian_splatting.shaderpl` | `lume/LumeGS/assets/app/pipelinelayouts/` | 存在 |
| `core3d_gs_dm_fw_depth.shadervid` | `lume/LumeGS/assets/app/vertexinputdeclarations/` | 存在 |

**下一步排查建议（按顺序）**

1. 确认 ROFS 里确实包含该 shader 及其 `.spv`：`pack_gs_rofs.py` 打印的文件清单里 grep `gs3d_gaussian_splatting`（注意脚本输出的 PowerShell 格式会显示成空行，建议用 `python ... > out.txt` 后读取）
2. 在 `shader_loader.cpp` / `ShaderDataLoader` 里给**解析失败原因**加日志（当前只报 "invalid shader"）：是 `.shader` JSON 解析失败、还是引用的 `.spv`/`.shaderpl`/`.shadervid` 找不到
3. 若 `.spv` 加载失败：检查 glslang 输出是否满足引擎要求（其他 73 个 shader 能加载，说明流程本身可用；注意这个 frag 25 KB 偏大，可能包含了不该有的 `/r/n` 分支）
4. 若 JSON 解析失败：对比同目录能加载的 `gs3d_gaussian_splatting_render.shader`（差异：`variantName`/`renderSlot`/`renderSlotDefaultShader:false`/`triangle_strip`）

> 另一个可疑点：`gs3d_gaussian_splatting.shader` 的 `renderSlot` 是 `CORE3D_RS_DM_FW_TRANSLUCENT`，而我们的 `systemGraph` 场景 RNG 用的是 LWRP；如果该 shader 未被注册到材质，也会走到 "invalid shader"。下一步日志会区分。

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
| `LumeApp/entry/src/main/cpp/render_main.cpp` | 引擎初始化、旧通路、**LumeGS 通路**（`InitGsRenderPath`/`PollGsRenderPath`/`g_renderPath`/相机同步/模型切换） |
| `LumeApp/entry/src/main/resources/rawfile/shaders/3d/systemGraph.json` | 增加 `GsTilingSystem`/`GsSystem`（optional）|
| `LumeApp/entry/src/main/resources/rawfile/shaders/3d/rendernodegraphs/core3d_rng_cam_scene_lwrp.rng` | 插入 `RenderNodeGsRender`（`CORE3D_RN_CAM_GS`）|
| `LumeApp/tools/gs_assets/compile_gs_shaders.py` | glslang 编 GS 的 GLSL（注入 `GL_GOOGLE_include_directive`，**不要 `-DVULKAN`**）|
| `LumeApp/tools/gs_assets/pack_gs_rofs.py` | 打包 ROFS（shader 树必须在 `shaders/` 前缀下）+ 生成 `GS_BIN` |
| `LumeApp/entry/src/main/cpp/generated/gs_rofs_embed.cpp` | 生成的大数组（约 7 MB，已入库；后续可改构建时生成）|
| `lume/LumeGS/src/component_dll.cpp` | 改动：`GS_BIN` 声明为 `const unsigned char[]`；`CORE_STATIC_PLUGIN_HEADER` + `DEFINE_STATIC_PLUGIN`；跳过 GSNode/WebP |
| `lume/LumeGS/src/systems/gs_system.cpp` | 改动：`GsSystem::Update` 诊断日志；**渲染目标强制重建条件**（`!renderData->gaussianRenderTarget0`）|
| `lume/LumeGS/src/render/render_node_gs_render.cpp` | 改动：`handles INVALID` 逐项诊断、`RecordGaussianDraw` 诊断 |
| `LumeApp/entry/src/main/ets/pages/Index.ets` | 三个下拉菜单（模型 / 相机 / 预算），默认 doll + Orbit |

---

## 5. 构建 / 部署 / 验证

```powershell
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
- 推送后：`chown -R 20020208:20020208 ...` + `chmod -R u+rwX,g+rX,o+rX ...`

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

---

## 7. 后续计划（顺序）

1. **修好材质 shader 加载**（见第 3 节）→ 小模型出画
2. **ImageKit WebP 适配**（方案 A 已定）：替换 `lume/LumeGS/src/webp/image_loader_webp.cpp` 的解码段为 `OH_ImageSourceNative`（复用旧 `sog_loader.h` 的逻辑，含反预乘），恢复 `WEBP_LOADER` 的 `ImageLoaderTypeInfo` 注册 → 才能读 `.sog`（`fruit_bowl.sog` 及 LOD 瓦片）
3. **S3 大场景 LOD**：用 `lod_scene.h` 解析新数据集 `SanYaPo_UAV_diffuse_gsbox_lod_0611_edit`（`default.scene.json` + `lod-meta.json`），把叶子映射成 `GsTilingComponent` + `CreateTile`，用相机/预算策略驱动 `SetTileVisible`
4. **收尾**：对比新旧通路帧率/画质；`generated/gs_rofs_embed.cpp` 改为构建时生成；清理诊断日志；更新本文档与 `3dgs-lod-status.md`

---

## 8. 旧自研通路状态（`g_renderPath = 0` 时）

完整保留、可编译、可运行，历史优化都在 `dev_3dgs`：

- 全局缩放预算 + 运动冻结 + 交叉淡入（`e8f4cfca`）
- 配额 32 + 新叶直达目标 + LODDIAG（`7f7d0392`）
- 4 顶点 triangle strip（`0f293e00`）
- 下拉菜单 + 预算切换（`e260bd6b`、`9255b549`、`c9d87cc0`）
- 平均帧日志（`6239200f`）、删除双击切预算（`f9787167`）、dirty range 合并（`799d98e1`）
- 性能结论、已否定方向都记录在 [3DGS 大场景 LOD 交接](3dgs-lod-status.md) 第 11 节
