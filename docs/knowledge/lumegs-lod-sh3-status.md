# LumeGS 大场景 LOD：GLB 支持与 3 阶 SH 方案（交接）

> 更新时间：2026-11-09
> 分支：`dev_lumegs`
> 基线 commit：`7b652a4b`（2x 多视角对比文档）
> 关联文档：[LumeGS 接入现状](lumegs-integration-status.md)、
> [tiling host 渐进加载经验](lumegs-tiling-host-lessons.md)、
> [3DGS 渲染集成经验](3dgs-rendering-integration.md)

---

## 1. 新 LOD 数据

设备目录：

```text
/data/app/el2/100/base/com.example.lumeapp/haps/entry/files/assets/lod/11_adongge_sh3_100w_2lod
```

内容：

| 项目 | 值 |
| --- | --- |
| `default.scene.json` | `url` = `lod-meta.json`（GLB） |
| `lod-meta.json` | `lodLevels=2`，5 个文件：`0_0.glb`…`0_3.glb` + `1_0.glb` |
| `lod-meta-sog.json` | 同树，文件为 `0_0.sog`…`1_0.sog` |
| 场景规模 | 64 leaves / 5 files / 2 levels |
| GLB 属性 | `KHR_gaussian_splatting:SH_DEGREE_0_COEF_0` … `SH_DEGREE_3_COEF_6` |
| GLB 扩展 | `UWA_gaussian_splatting_compression_EGSC` |
| SOG meta | `shN.bands = 3`（3 阶 SH），`shN.count` 为调色板行数 |

两种格式都包含 **3 阶球谐系数**。

---

## 2. GLB 大场景 LOD 验证（已通过）

把 app 的 City 临时指向 `lod/11_adongge_sh3_100w_2lod`，`default.scene.json` 使用
`lod-meta.json`（GLB）后，设备日志：

```text
setModel: 11_adongge_sh3_100w_2lod -> index 4
GS LOD scene dir: .../assets/lod/11_adongge_sh3_100w_2lod
GS LOD: leaves=64 files=5 levels=2 budget=1000000
GS tile cache: .../0_0.glb fmt=1 sh=0
GS tile cache: .../0_1.glb fmt=1 sh=0
GS tile cache: .../0_2.glb fmt=1 sh=0
GS tile cache: .../0_3.glb fmt=1 sh=0
GS tile cache: .../1_0.glb fmt=1 sh=0
GS LOD state: vis≈17–27 ring≈0–6 res≈26–29 pending=0
```

- `fmt=1` = `CachedSogData::DataFormat::GLB_EXPANDED`。
- 5 个 GLB 全部成功解码、上传、resident；截图正常出画（mean 104.7）。
- **结论：GLB LOD 的几何 / DC 颜色路径可用。**

验证期间遇到一个 host 侧问题：`Index.ets` 的 `MODELS[4]` 仍是 `syp_uav_0611`，
而 `NapiSetModel` 用 `SceneBaseName(scene)` 匹配，导致
`setModel: unknown model 'syp_uav_0611'`。把 ETS 名称改成新目录名后正常。
生产上建议模型名由元数据/配置生成，避免 ETS 与原生列表硬编码不一致。

---

## 3. 当前 SH 支持矩阵

| 路径 | SH 3 阶 | 证据 |
| --- | --- | --- |
| LumeGS 单文件（Doll 等） | ✅ | `maxShDegreeOverride` 默认 3；app 对 Doll 设 `shDeg1=1/shDeg23=1` |
| LumeGS tiling LOD（SOG） | ✅ Phase A SH3 已生效 | §3.5；16 视角 SH3 vs SH0 平均 diff 7.83/255 |
| LumeGS tiling LOD（GLB） | 🟡 压缩 SH 元数据/gate 已落，ASTC 纹理渲染待接 | `DecodeGlbFile` codec=2/compressedSh=1；当前 GLB tile 只输出 DC；下一步 per-file ASTC 槽 + shader 纹理索引 |
| 旧自研 LOD 管线 | ❌ | 设计即 DC-only（3DGS 集成文档 §4.3） |

### 3.1 关键代码锚点

| 位置 | 说明 |
| --- | --- |
| `lume/LumeGS/src/render/render_node_gs_render.cpp:1800` | `canComputeSh = !renderData->isTiled && ...`：tiled 明确禁用 SH compute |
| `lume/LumeGS/src/render/render_node_gs_render.cpp:842` | `dynamicData.shDegree = gsPrimitives[ii].shDegree` |
| `lume/LumeGS/src/render/render_node_gs_render.cpp:2900` | `ExecuteComputeSphericalHarmonics` |
| `lume/LumeGS/api/plugin_gs/render/intf_render_data_store_default_gs.h:76` | `GsTilePool::shPoolBuffer`（已声明，**从未分配/绑定/使用**） |
| `.../intf_render_data_store_default_gs.h:166` | `GsPrimitive::shDegree`，默认 3 |
| `.../intf_render_data_store_default_gs.h:206` | `RequireSH() { return codecVersion < 1 && shDegree > 0; }` |
| `.../intf_render_data_store_default_gs.h:101` | `GsHarmonicsData`：24×uint32（16-bit 打包，96 B/splat） |
| `lume/LumeGS/src/data/glb_parsing.cpp:172` | `cache.shDegree = 0`（GLB tile 硬编码） |
| `lume/LumeGS/src/data/glb_parsing.cpp:362` | `ExtractGlbSplatsToBuffer` 只取 `POSITION/SCALE/SH0/OPACITY/ROTATION` |
| `lume/LumeGS/src/data/sog_parsing.cpp:1043` | `ExtractSplatsToBuffer` 设置 `primitive.shDegree`，但 `ExtractSogChunk(..., nullptr, nullptr, ...)` 不取 SH |
| `lume/LumeGS/src/systems/gs_tiling_system.cpp:1860` | `RegisterPoolPrimitive` 设 `codecVersion=1`、`numGs=0`、`gsBufferSize=0` |
| `lume/LumeGS/src/systems/gs_tiling_system.cpp:655` | `ExtractTileDirectToPool`：tile 只写 64 B splat 数据 |
| `LumeApp/entry/src/main/cpp/render_main.cpp` | City `kDemoModels[4].scene`、`GS LOD` host 选级 |

### 3.2 GLB tile 解码现状

`DecodeGlbFile` 使用 `SpatialModel`/UWA 解码，但：

```cpp
cache.shDegree = 0;  // glb_parsing.cpp:172
```

`ExtractGlbSplatsToBuffer` 的属性列表只有：

```cpp
POSITION, SCALE, SPHERICAL_HARMONICS_DEGREE_0_COEFFICIENT_0, OPACITY, ROTATION
```

因此即使 GLB 文件里有 `SH_DEGREE_1..3` 属性，tile 路径也不会读取。

### 3.3 SOG tile 解码现状

`DecodeSogFile` 会从 `meta.json` 得到 `cache.shDegree = shN.bands`（本例为 3）。
但 `ExtractSplatsToBuffer` 调用 `ExtractSogChunk` 时 SH 指针传 `nullptr`，
且 tile pool 没有 SH 目标缓冲区，所以 SH 数据被丢弃。

### 3.4 相机姿态：使用 cameraInfo.lookUp / lookAt

此前 `lod::Scene` 只解析 `cameraInfo.position/lookAt`，`lookUp` 被忽略，相机
up 硬编码为世界 Y；`LoadLodScene` 和 LumeGS `ApplyGsLodCamera` 各有一份
重复的基向量构造。

现在：

- `lod_scene.h` 解析 `cameraInfo.lookUp`（默认 `(0,1,0)`）。
- 新增 `SetOrbitBasisFromLookAt()`：back = normalize(position - lookAt)；
  right = normalize(lookUp × back)；up = normalize(back × right)，保证正交。
  legacy LOD 和 LumeGS tiling 两条路径共用。
- 新场景 `lookUp=(0,1,0)`，日志基向量：
  `right=(-1,0,0) up=(0,1,0) back=(0,0,-1)`，目标为 cameraInfo.position；
  画面与修改前基本一致（mean 103.1 vs 103.2），没有 roll。
- **原地转圈 roll 修复**：`Tumble` 原来 yaw 绕当前 camera up；一旦 pitch 过，
  up 已倾斜，再 yaw 就绕倾斜轴旋转，产生 roll（“其他视角有点歪”）。
  现在 yaw 绕 `upHint`（cameraInfo.lookUp，默认世界 Y），每次 yaw/pitch 后
  重新正交化 `right/up`。设备验证：pitch 后
  `right=(1,0,0) up=(0,0.022,-0.9998) back=(0,0.9998,0.022)`；再 yaw 后
  `right.y=0`，`up` 与 `back` 正交，horizon 不再被滚转。
- 如果仍觉得“视角歪”，更可能是 `default.scene.json` 的相机位置/lookAt
  （本场景 lookAt 只在前方 1 单位）或场景坐标本身，而不是 lookUp 丢失；
  可再调整初始 lookAt 到场景中心/包围盒或使用参考机位。

### 3.5 Phase A 实施结果（PoC，2026-11-10）

已实现：

- `GsTilePool.shPoolBuffer` 在 `CreateGsRenderDataBuffers` 创建/扩容（host-visible/coherent，
  96 B/splat），`InitializePool` 同步到双缓冲两侧；`EnsureSplatPoolMapped` 持久映射 SH buffer。
- SOG tile：`ExtractSplatsToBuffer` 新增 SH 目的指针，`ExtractSogChunk` 写入
  `GsHarmonicsData`；`TileLoadResult` 携带 `shDegree`。
- `RegisterPoolPrimitive` 初始 `shDegree=0`、`codecVersion=0`；首个 SOG tile 完成后把
  pool primitive 的 `shDegree` 升到 3，并 bump `tilePool->resourceVersion`。
- `canComputeSh` 去掉 `!isTiled`；tiled 时 SH compute 绑定
  `pool.splatPoolBuffer` 和 `pool.shPoolBuffer`；`GetTotalGsCount()` 在 tiled 返回
  pool capacity；`tilePool->resourceVersion` 变化触发 SH 重算。
- `ApplyInitialPoolConfig` 的 `bytesPerSplat` 加入 `sizeof(GsHarmonicsData)`，
  SH pool capacity 上限 1.5M（host 预算 1M）。
- app `EnsureGsConfiguration` 设 `enableTilePackedData=false`（SH 需要 64 B 非 packed
  tile layout）。

设备验证（SOG wrapper，`lod-meta.json` = SOG 列表）：

```text
GS LOD: leaves=64 files=5 levels=2 budget=1000000
SOG DecodeSogFile: version=2 count=241858 / 235384 / 252560 / 296906
GsTilingSystem: tiled SH degree -> 3
GS tile pool allocated: splatCapacity=1500000 (91.6 MB splat pool)
```

> 关键修正：tile cache 的 `DecodeSogFile(..., false)` 不解析 SHN，导致
> `cache.shDegree=0 / haveSHN=0`、SH buffer 全零。改为 `true` 后
> `SOG SH sample` 非零，`degree=3 haveSHN=1`。

#### 根因与修复（2026-11-10，已确认）

早期 16 视角 SH3 vs SH0 diff ≈0.01/255（负结果），根因不在颜色计算，而在
**pool dynamic UBO 的 `gsBufferSize` 为 0**：

- `RegisterPoolPrimitive` 首次创建 pool primitive 时，`pool->splatCapacity` 还不可见，
  `gsBufferSize` 落成 0；tile commit 的更新只在新 `shDegree >` 旧值时触发，
  而初始 `shDegree` 已经是 3，因此 `gsBufferSize` 永远没有同步。
- dynamic UBO 的 `endOffset=0` → `GetGsObjectIndex()` 返回 -1 → SH compute 直接
  return，画面保持 DC。
- 修复：
  1. `gsBufferSize` 在 pool capacity 不可见时回退到 `splatPoolCapacity_`，
     并在 tile commit 时**始终**同步 `gsBufferSize`（不只 shDegree 增长时）；
  2. `UpdateDynamicDataUBO()` 对 tiled 数据不再被 `areResourcesLoaded=false` 早退。

验证（默认 City 机位，公平 A/B）：
- SH0：edge=4.21，RGB=124.3/92.6/85.8，sat=103.6
- SH3：edge=4.56，RGB=124.2/98.1/93.5，sat=93.9
- 平均 diff **8.44/255**，75.5% 像素差异 >2；SH3 明显更亮、更绿/蓝、饱和度更低。

单机位报告：
`D:\Source\3DGS\data\lumegs-lod-sh3-fixed-20261110\index.html`
（`metrics.txt`、`compare.jpeg`、`compare_full.jpeg`）。

#### 修复后 16 视角 A/B

再次加回临时 pose harness，用修复后的同一份代码分别构建 SH0 / SH3：

| 指标 | 结果 |
| --- | --- |
| 视角 | yaw 0/45/…/315（8）+ 4 个 1.5 m 偏移 × yaw 0/180（8） |
| 逐视角 diff | mean **7.83/255**，min 5.52，max 11.18 |
| 差异像素 | >2 的像素占比 54.5%–85.9% |
| edge | SH0 3.58–5.49 → SH3 3.88–11.30 |
| 结论 | SH3 在所有视角都有可见的、随视角变化的效果 |

16 视角报告：
`D:\Source\3DGS\data\lumegs-lod-sh3-16view-fixed-20261110\index.html`
（`metrics.txt` / `metrics.csv`、`compare/`、`compare_full/`）。

> 旧的 16 视角报告 `lumegs-lod-sh3-ab-20261110` 是修复前（gsBufferSize=0）的采集，
> 结论已被本次覆盖；保留作历史。

#### 单文件 SOG 对照

把同一个 `0_0.sog` 作为单文件模型加载（`maxShDegreeOverride` 3 vs 0）：

- 单文件 SH3 vs SH0：diff **2.04/255**，RGB 115.3/114.2/108.0 vs 116.8/116.0/109.5，
  饱和度/edge 基本不变。
- 说明同一份 SOG 的 SH3 数据在单文件路径也生效；LOD 的 7.83 差异不是空的。
- 单文件是自动 framing（近景），与 LOD 默认机位不同，颜色变化方向/幅度不能直接逐像素对比。

#### LOD SH 度数 sweep（默认 City 机位）

| 对比 | diff | RGB 变化 | sat 变化 | edge 变化 |
| --- | --- | --- | --- | --- |
| SH0 → SH1 | 2.99 | +0.1/+1.1/+1.6 | -3.4 | -0.01 |
| SH1 → SH2 | 3.07 | -0.6/+1.4/+2.5 | -3.0 | +0.16 |
| SH2 → SH3 | 2.77 | +0.6/+1.1/+1.4 | -1.0 | +0.11 |

三个 band 都带来同量级的渐进变化，没有某一级爆炸/突变；支持“LOD 的 SH1/2/3
系数按度数正确应用”，也说明 16 视角的 7.83/255 是 SH 累积效果，而不是单一颜色 bug。

**仍不能证明**：与参考 viewer 的绝对颜色数值完全一致（当前没有 web/viewer 同机位图）。
后续有参考图时，优先做同机位、同相机的单文件 vs LOD vs viewer 三点对照。

限制/风险：

1. 目前只有默认机位的公平 A/B；需要多视角复测（临时 pose harness 已移除）。
2. SH compute 当前遍历 pool capacity（上限 1.5M），不是只遍历 resident ranges；
   性能和功耗待测。
3. SH pool 是 host-visible/coherent，1.5M × 96 B ≈ 144 MB，需关注设备内存/带宽。
4. GLB SH 仍未接入（Phase B）；Phase A 的 pool 初始 `shDegree=3` 只适用于 SOG，
   GLB 需要单独的 gate（否则会用空 SH 数据计算）。

---

## 4. Phase A：SOG + SH3

目标：先打通 SOG tile 的 3 阶 SH，验证 tile pool + SH compute 的完整链路；
GLB 留到 Phase B。

### A1. Tile pool SH 缓冲区

- `GsTilePool::shPoolBuffer` 已存在但未用。
- 在 `InitializePool()`/`CreateGsRenderDataBuffers()` 中按
  `splatCapacity * sizeof(GsHarmonicsData)`（96 B/splat）创建 SH storage buffer，
  并像 `splatPoolBuffer` 一样保持 CPU 可见/持久映射，便于 tile 解码直接写入。
- `ResizeTilePool`/`EnsurePoolCapacity` 扩容时同步重建 SH buffer。
- 驱逐/重分配 tile 时必须按期 SH 范围清零或重写，避免旧数据被新 tile 读到。

### A2. SOG 提取写入 SH

- `ExtractSplatsToBuffer` 增加 SH 目的指针（`shDst = shPoolBase + poolOffset * 96`）。
- `ExtractSogChunk` 传入非空 `sphericalHarmonics`；`sog_parsing.cpp` 已支持
  系数转置、`GsHarmonicsData` 打包。
- 把 tile 的 `shDegree` 记录到 tile 元数据/pool primitive（Phase A 假设全场景 3 阶）。

### A3. Pool primitive 与 render node

- `RegisterPoolPrimitive` 设置 `poolPrim.shDegree = 3`、`startShOffset = 0`；
  `codecVersion` 当前为 1（用于 tile packed 路径），会令 `RequireSH()` 为 false，
  需要为 tiled SH 单独放行（例如 `poolPrim.codecVersion = 0` 或新增 tiled 标志）。
- `canComputeSh` 去掉 `!renderData->isTiled`，但只在 pool 有 SH 且 `shDegree>0` 时启用。
- `ExecuteComputeSphericalHarmonics` 增加 tiled 分支：绑定 `shPoolBuffer`，按 resident
  范围/分帧计算；`dynamicData.shDegree` 取 pool primitive 的 3。
- 检查主渲染 shader 的 SH 采样索引：pool splat 的全局索引必须与 SH buffer 的
  索引一致（`poolOffset`）。

### A4. Host 配置

- Phase A 可先用 app 侧全局配置/常量（`g_lodShDegree = 3`），避免改
  `GsTilingComponent` 的公开 ABI。
- 若要按场景/格式配置，建议后续加 `GsConfigurationComponent` 属性或
  `GsTilingComponent.shDegree`（公开 ABI，需确认）。

### A5. 验证

- 用 `lod-meta-sog.json` 的 SOG 场景（新目录的 SOG 文件）。
- 固定 5 个相机位，分别截 SH0 / SH3 图，比较颜色/亮度/方向性变化；
  确认不是“所有视角都变亮/变暗”的系数错位。
- 检查内存：1M splats × (64 B splat + 96 B SH) ≈ 160 MB；
  当前 pool 704 MB 预算可容纳约 3M，1M 预算安全。
- 检查 SH compute 帧耗时；必要时只对可见/近处 tile 计算，或降低 SH 度。

---

## 5. Phase B：GLB + SH3

### 5.1 已实现（未提交）

1. `DecodeGlbFile` 从 `SpatialTag::SH_DEGREE` 读取真实 SH 度；日志确认
   `DecodeGlbFile: gsCount=262144 shDegree=3` 等。
2. `TileExtractBuffer` 增加 `shDst/shDstBytes`，`ExtractTileDirectToPool` 从
   `ctx.shDst` 传入。
3. `ExtractGlbSplatsToBuffer` 在 `shDegree>0` 时追加
   `SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER` 属性；`GetBlockData` 按
   `LoadShN` 相同逻辑打包 `GsHarmonicsData` 写入 tile SH pool。
4. Phase A 的 `shPoolBuffer`/SH compute 复用。

### 5.2 方案 2 尝试：把 GLB SHN 展开成 float

已在 tile GLB 路径加入 SHN float 提取（`GD_DEGREE_1_AND_HIGHER` → `GsHarmonicsData`），
用同一个 `0_0.glb` 验证：

- 单文件 GLB SH3 vs SH0：**2.02/255**，与同 tile 的 SOG 单文件 2.04 一致 → 数据和
  单文件路径没问题。
- tiled LOD GLB SH3 vs SH0：**57.85/255**（RGB 110/103/104 vs 119/100/91）→ 明显错误。

结论：**方案 2（按 float 展开）不成立**。该 GLB 使用
`UWA_gaussian_splatting_compression_EGSC`，SHN 在单文件路径里是
`codecVersion>0` 的 **UV/ASTC** 表示，并不是一组可以直接当 float 读的 SHN；
tile 路径把它当 float 写进 96 B `GsHarmonicsData`，所以颜色严重偏离。

### 5.3 方案 1：第一阶段已落地（未提交）

- `CachedSogData` 增加 compressed SH 元数据：`codecVersion`、`compressedShWidth/Height`、
  `attribute2DSingleWidth/Height`、`attribute2DTexNum`、`shDcMins/Maxs`、
  `shRestMins/Maxs`、`hasCompressedSh`。
- `DecodeGlbFile` 解析 `SPHERICAL_HARMONICS_DEGREE_1_AND_HIGHER` 的 ASTC 元数据与
  量化参数；日志会打印 `codec=2 compressedSh=1 texNum=... map=... region=...`。
- `ExtractGlbSplatsToBuffer` 对 `hasCompressedSh` 的 GLB **暂时只输出 DC**
  （`primitive.shDegree=0`），避免把 ASTC 载荷当 float SHN 写进 96 B SH pool。
- `TileLoadResult.shSupported` 会把压缩 GLB 的 pool primitive `shDegree` 降为 0，
  防止 tile SH compute 用空数据计算出错误颜色。

**下一步（渲染）**：

1. 每文件一个 ASTC 纹理槽，存到 `renderData->gsShASTCDataRefs`（现有 16 槽）；
   tile 系统维护 URI → 槽 + 引用计数。
2. 每 splat 的 `astcOffsetColor` 写 UV/DC，`.w` 存文件槽索引（当前为空）。
3. pool primitive 设置 `codecVersion=2`、`textIdOffset`、`attribute2DTexNum` 和
   compressed 元数据；`RequireSH()` 为 false，关闭 96 B SH compute。
4. shader `GetAstcSHCoefV2` 的 `texidx` 改为 `texOffset + astcOffsetColor.w`。
5. 用 `11_adongge_sh3_100w_2lod` 默认机位做 GLB SH3 vs SH0，目标 diff 2–10/255。

在完成前，City 保持 SOG SH3（当前 mean 105.9），GLB LOD 仅 DC。

---

## 6. Phase C：画质/性能

- 按 tile level 控制 SH 度：近处 SH3、coarse fallback SH0/1，减少显存与 compute。
- SH3 vs SH0 固定视角 A/B，确认高阶项方向正确（避免系数顺序/转置错误）。
- 每个 tile 的 SH 范围与 splat 范围对齐，驱逐时释放。
- 评估 1M/1.5M 预算下的 SH compute 成本和帧率。

---

## 7. 当前工作区与临时改动

当前工作区（未提交）：

- `LumeApp/entry/src/main/cpp/render_main.cpp`
  - `kDemoModels[4].scene` → `lod/11_adongge_sh3_100w_2lod_sog`
  - `EnsureGsConfiguration` 设 `enableTilePackedData=false`
- `LumeApp/entry/src/main/ets/pages/Index.ets`
  - `MODELS[4]` 和 firstPerson 判断改为 `11_adongge_sh3_100w_2lod_sog`
- LumeGS Phase A：
  - `api/.../intf_render_data_store_default_gs.h`：tiled `GetTotalGsCount()`
  - `src/util/gs_loading.cpp`：`shPoolBuffer` 创建/扩容
  - `src/systems/gs_tiling_system.cpp`：SH 映射、提取、pool shDegree、capacity
  - `src/data/sog_parsing.*`：SH 提取参数
  - `src/render/render_node_gs_render.*`：tiled SH compute + pool binding + version 触发
- 设备上额外创建了 SOG wrapper：
  `/data/.../assets/lod/11_adongge_sh3_100w_2lod_sog`
  （`lod-meta.json` = SOG 列表，含 5 个 `.sog`）
- `GsTilingSystem: tiled SH degree -> 3` 仍是 `CORE_LOG_E` 临时诊断，提交前改回 I 或删除。
- SOG SH3/SH0 截图：`C:\Users\lambda\AppData\Local\Temp\opencode\sog_sh3b.jpeg`、
  `sog_sh0.jpeg`。

---

## 8. 下一步

1. ~~修复后重跑 16 视角~~ ✅ mean diff 7.83/255。
2. ~~单文件 SOG 对照 + LOD SH 度数 sweep~~ ✅ 单文件 diff 2.04；0→1/1→2/2→3 约 2.8–3.1/255。
3. 测 SH compute 性能/内存；考虑只遍历 resident ranges、降 SH 度或预算。
4. 把 LOD SH 度做成配置（避免全局硬编码 3），并给 GLB 加 `shDegree=0` gate。
5. 清理 Phase A 测试场景入口（City → `..._sog`）和临时日志。
6. Phase B：GLB SH3。已试 float SHN 展开（同一 GLB 单文件 2.02，tiled 57.85，
   不正确）；GLB 使用 EGSC 压缩 SH/ASTC，需要 tile pool 支持 compressed SH/UV，
   或改用导出的未压缩 GLB/SOG。
7. 全量 OHOS 构建 / XTS / 稳定性检视在提交前补。
