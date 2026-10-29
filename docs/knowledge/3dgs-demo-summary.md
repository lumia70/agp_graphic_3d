# 3DGS 演示（LumeApp）实现与踩坑总结

本文汇总本阶段在 OpenHarmony 上用 Lume（AGP）引擎渲染 3D 高斯泼溅的**工作内容、关键决策、踩过的坑和实测数据**。
稳定结论已沉淀到 [3DGS 渲染集成经验](3dgs-rendering-integration.md)、[演示资产清单](3dgs-demo-assets.md)、
[移动端与桌面差异](3dgs-mobile-vs-desktop.md)；本文更偏"这一段做了什么、为什么这么做"。

---

## 1. 目标与当前状态

**目标**：在真机上用 Lume 引擎（Vulkan）实时渲染 3DGS，支持多模型运行时切换，观感对齐 web viewer
与参考 app，交互（拖动/缩放/切换）流畅。

| 能力 | 状态 | 验证方式 |
| --- | --- | --- |
| 自定义 RenderNode 渲染高斯泼溅 | 完成 | 设备出图 |
| 标准 3DGS PLY 加载 | 完成 | `fruit_bowl.ply` 750,322 高斯 |
| 压缩 `.splat`（super-splat chunk） | 完成 | `godzilla.splat` 1,067,182 高斯 |
| UWA glTF + 嵌入压缩 PLY | 完成 | `doll.glb` 426,605 高斯 |
| SOG v2（`.sog`） | 完成 | `fruit_bowl.sog` 与 PLY 版逐像素差 1.2% |
| per-model FOV / SH 权重 / 视角 | 完成 | 三组模型分别对齐参考 |
| 运行时切换模型（ArkTS 按钮） | 完成 | 连续 13 次切换无崩溃、内存不增长 |
| 双击切换近/远视角 | 完成 | 近 4.04→远 10.11→近 4.04 ms 一致 |
| 排序并行 + 异步 | 完成 | 渲染线程成本 30–87 ms → <1 ms |
| 按需渲染（静止不渲染） | 完成 | 静止时 GPU 空闲 |

**未覆盖**：GLES 后端、其它机型、XTS、SOG 之外的压缩格式、大场景 LOD。

---

## 2. 架构总览

```text
ArkTS (Index.ets)
  ├─ XComponent(SURFACE) → 原生回调：Surface 生命周期 / 触摸
  ├─ setInterval(16ms) → lumeapp.drawFrame()  ─┐
  └─ 按钮 → lumeapp.setModel("doll.glb")        │ 只记录意图（g_pendingModelIndex）
                                                ▼
render_main.cpp（app，运行在 XComponent 线程）
  ├─ 引擎初始化：PluginRegistry → Engine → RenderContext → GraphicsContext → ECS
  ├─ 相机：自由滚转 OrbitCamera（right/up/back 基 + Rodrigues）
  ├─ 模型加载：splat_loader.h / sog_loader.h → 共享全局（C 链接）
  └─ DrawFrame：应用待切换模型 → TickFrame → RenderFrame(RNG 列表)
                                                │ dlsym 读取共享全局
                                                ▼
render_node_gaussian_splatting.cpp（LumeRender 内，渲染线程）
  ├─ 上传：gaussians SSBO / SH SSBO / cull records
  ├─ 相机 UBO ×3 + SH 参数 SSBO ×3（三缓冲）
  ├─ 排序：cull → 直方图 → scatter（4 worker 并行，独立线程）
  ├─ Pass 1：高斯 → UNORM 离屏（码值空间原始混合）
  └─ Pass 2：全屏拷贝 → sRGB backbuffer（decodeSrgb + USM）
```

关键文件：

| 路径 | 职责 |
| --- | --- |
| `LumeApp/entry/src/main/cpp/render_main.cpp` | 引擎初始化、相机、触摸、模型切换、按需渲染 |
| `LumeApp/entry/src/main/cpp/splat_loader.h` | PLY / 压缩 chunk / UWA glTF 解码 |
| `LumeApp/entry/src/main/cpp/sog_loader.h` | SOG v2：ZIP + 迷你 JSON + ImageKit WebP |
| `LumeApp/entry/src/main/cpp/gaussian_splat_data.h` | 跨 `.so` 共享全局（`dlsym` 目标） |
| `lume/LumeRender/src/node/render_node_gaussian_splatting.*` | 核心 RenderNode（上传/排序/双 pass/PSO/线程池） |
| `LumeApp/entry/src/main/resources/rawfile/shaders/splat/` | splat + copy shader（`.vert/.frag/.spv/.spv.lsb/.shader/.shaderpl`） |
| `LumeApp/entry/src/main/ets/pages/Index.ets` | 四个模型按钮 + 渲染循环 |

---

## 3. 里程碑（本分支提交）

| 提交 | 内容 |
| --- | --- |
| `5e1f5500` | 压缩 splat 格式 + 多模型演示骨架 |
| `f8195cae` | 修正压缩格式四元数分量顺序（尖刺根因） |
| `7123cc9e` | 每格式颜色字节序、每模型 FOV、USM、环境球壳处理 |
| `bb312f14` | 由 `feature_dc_min/max` 还原亮度（"灰蒙蒙"根因） |
| `2f5f1172` | 文档：压缩打包与色域结论 |
| `1ecb3cfe` | per-model SH 权重经 SSBO 下发（UBO 追加字段读不到） |
| `a0d2e525` | 运行时模型切换（ArkTS 按钮）+ 鲁棒取景 |
| `21a19062` | 清理 bring-up DIAG 日志、关闭 validation/dev、删除死代码 |
| `40f3c4ba` | 文档：演示资产清单（体积/许可/复现） |
| `75dca15b` | SOG v2 加载 + 删除 shader 里的 SH A/B 开关 |
| `81e9a265` | 排序并行化（cull / hist / scatter） |
| `90cdea02` | 紧凑 cull 记录 + 后台线程排序（异步） |

---

## 4. 踩坑清单

每条格式：**现象 → 根因 → 修复**（能给出证据的都附上实测数据）。

### 4.1 Lume 引擎集成

1. **RenderNode 不执行**
   - 现象：RNG 创建成功但画面只有清屏色。
   - 根因：节点未声明依赖 `MAIN_CAMERA_BIT` / `CLEAR_COLOR_BIT`，渲染图不会调度它。
   - 修复：在节点契约里声明两个 bit。

2. **跨 `.so` 共享数据**
   - 根因：app 与 LumeRender 是不同 `.so`，RTLD 下全局符号不自动共享。
   - 修复：app 侧用 `extern "C"` 导出指针/数据，RenderNode 用 `dlsym(RTLD_DEFAULT, "g_...")` 取；
     `gaussian_splat_data.h` 两份头文件必须保持一致。

3. **描述符池被重置**
   - 现象：绑定多个 binder 后部分描述符失效。
   - 根因：`ResetAndReserve()` 会重置换整个池。
   - 修复：把所有 binder 的 `DescriptorCounts` 累加后**一次性**预留。

4. **往相机 UBO 追加字段读不到**
   - 现象：新加的 `shScale` 字段在 shader 里恒为 0，用红色探针验证过。
   - 根因：Lume 的 UBO 路径按反射尺寸上传，追加字段不生效。
   - 修复：改用独立 16 B SSBO（`set=0, binding=4`），每帧上传 `[shDeg1, shDeg23, 0, 0]`。

5. **shader 三件套必须同步**
   - `.spv`、`.spv.lsb`（反射）、`.shader/.shaderpl`（管线布局）任一处 binding 不一致 → 静默失效。
   - 本项目 `.lsb` 是脚本生成的（`gen_lsb.py`），改 GLSL 后必须重新编译 `.spv` 并核对 lsb。

6. **glTF importer 生命周期**
   - 现象：模型偶发加载失败/崩溃。
   - 修复：importer 用 `shared_ptr` 在 app 层持有，跨 tick 存活；且首个 `TickFrame()` 之后再加载模型
     （RenderSystem 需先注册事件监听）。

### 4.2 格式解码

1. **压缩 PLY 位置/缩放位序**
   - `pack111011 = (x:11 << 21) | (y:10 << 11) | z:11` —— **z 在最低位**，读反了会出现整体拉伸。

2. **四元数顺序**
   - 2-10-10-10 packing 的三个分量按 PLY 的 `rot_0..2` 顺序（w,x,y,z），最大分量由 tag 重建并取正；
     顺序错 → 高斯沿某个轴拉成尖刺。

3. **颜色字节序**
   - `pack8888` 为 `(A,B,G,R)`；godzilla / doll 都是这个序。判据：环境球壳本应灰白却渲染成粉色。

4. **`feature_dc_min/max`（"灰蒙蒙"根因）**
   - 声明了这两个属性的文件里，颜色字节是**归一化的 SH DC**：
     `f_dc = dcMin + n*(dcMax-dcMin)`，`color = 0.5 + SH_C0*f_dc`。
   - 直接把字节当颜色 → 对比度塌陷（doll 实测饱和度 0.125 vs 参考 0.16–0.21）。

5. **VQ 高阶 SH**
   - `feature_rest_vq`（uint index）+ `feature_rest`（45×int8 codebook）× `rest_scale` + `zero_point`。

6. **UWA glTF**
   - glTF 只是容器，压缩 PLY 在 GLB 的 BIN chunk 里；需按元素/属性名通用解析，不能写死偏移。

7. **SOG v2**
   - ZIP 容器（stored，不压缩）+ `meta.json` + 若干 WebP；
   - 位置是 **log 空间**：`pos = sign(v)*(exp(|v|)-1)`，16-bit 由 `means_l/means_u` 拼；
   - scale 是 log 码本 → `exp()`；旋转是 smallest-three（tag 252..255）；
   - SH rest 调色板是**通道优先**，写入本项目缓冲要转成系数优先；
   - **最大坑：平台 WebP 解码返回预乘 alpha**（见 4.3-6）。

8. **`f_rest` 顺序：同一概念、两种格式不同（踩了两次）**
   - shader 按系数优先读（`sh[j*3+c]`）。**标准 PLY** 与 **SOG 调色板**是通道优先
     （R[0..14]、G[0..14]、B[0..14]）→ 需要转置；**压缩 VQ codebook** 是系数优先 → 不能转置。
   - 第一次错：以为"SH3 变暗是高阶噪声"，其实 PLY 系数错位。
   - 第二次错：把三条路径一起转置，doll 反而变怪（VQ 被多转了一次）。
   - 判别方法：逐列 `mean(|value|)` 看是否有 3 周期分组；或算相邻列相关系数
     （PLY 的 `corr(0,1)=0.05`、`corr(2,3)=-0.77` 证明通道优先）。
   - 修正后：doll 与修改前逐像素一致；fruit_bowl 的 PLY 与 SOG 的 SH 增量相对 DC 分别
     4.84 / 4.85（p99 都是 20），两个独立解码器互相印证。

### 4.3 色彩与观感

1. **色彩空间**：splat 颜色是 gamma 空间（可直接显示）。做法是
   高斯在 `RGBA8_UNORM` 离屏上**码值空间原始混合** → 全屏拷贝 pass `decodeSrgb` 到 sRGB backbuffer。
   任何线性化/两次 sRGB 都会整体偏色。

2. **SH 阶数**：web viewer 实际是 DC 主导；fruit_bowl 开 SH3 会压低暖色区亮度。
   结论：fruit_bowl/godzilla/SOG 用 DC，doll 用 SH1+（对齐它自己的参考 app）。

3. **shader 里遗留的 A/B 开关（血案）**
   - `gaussian_render.vert` 里有一行 `color = mix(g.color.rgb, color, 0.0);`（commit `53a61530` 引入），
     把 SH 永久关掉。排查"Sog 的 SH 为什么没效果"时才发现：把 SH 权重调到 3.0、
     甚至把 SH 缓冲全填 1.0，画面**逐像素不变**。
   - 教训：任何"临时 A/B 开关"必须带 TODO 且限期删除，否则会把后续调试带进沟里。

4. **环境球壳**
   - godzilla（半径 ~50，粉色）与 doll（半径 ~437，灰白）都带环境球壳，会主导 bounds
     并制造"灰雾"。判据：颜色异常先怀疑字节序，再怀疑球壳。

5. **移动端锐度损失**：拷贝 pass 加 USM（`SHARPEN=0.35`）补偿，与 web 参考对比更接近。

6. **预乘 alpha 毁掉 SOG 数据通道**
   - 现象：SOG 画面几乎全黑，只剩形状。
   - 根因：ImageKit NDK 解码 WebP 返回 **premultiplied** RGBA；SOG 的 opacity / quat tag 存在
     alpha 通道里，RGB 数据被乘脏。证据：`sh0` 原始字节 192/179/123（alpha=37），
     设备读到 27/25/17，且 `27 = floor(192*37/255)`。
   - 修复：读回后反预乘 `v = (p*255 + a/2)/a`（`a==0/255` 跳过）。量化误差实测：
     alpha≥0.25 的高斯颜色误差均值 0.009，alpha≥0.1 为 0.019。
   - 平台不提供"不预乘解码"；请求 `PIXEL_FORMAT_RGB_888` 直接失败；SDK/系统都没有 `libwebp`。

### 4.4 取景与交互

1. **bounds 被离群点拉爆**
   - fruit_bowl 99% 高斯半径 < 20（世界单位），但 bounds 是 ±2000（房间墙面）；
     直接按 bounds 取景 → 相机落进墙里，画面是"一堵平坦的墙"（std 15.7 vs 正常 79）。
   - 修复：不透明度加权质心 + `r50/r80/r90` 百分位半径；近景 `1.6*r80`、远景 `4*r80`。

2. **PLY 世界缩放 100**
   - `LoadStandard3DgsPly(path, out, 100.0f)`：位置与 scale 都要乘；只乘一个会出现"针尖"或"巨球"。

3. **缩放后细节不回来**
   - 远端被"小于 2 px"剔除的高斯，缩放后不会自动回到排序列表。
   - 修复：轴向位移超过 2% 也触发重排（不只是旋转）。

4. **双击远近切换**：改为显式 `insideDistance` / `farDistance` 两个值，不再依赖"上次停留距离"。

### 4.5 性能（排序）

1. **单线程排序是拖动卡顿的唯一主因**：1.07M 高斯 87 ms/次 → ~11 fps。
2. **并行化分三段**：cull / 直方图 / scatter，每段按高斯区间切片，各写自己的输出切片与桶游标，
   无原子操作、无归并；桶内顺序与单线程一致（最终**逐像素相同**）。
3. **线程数宁少勿多**：14 个 hw threads 下 6 worker 反而更抖（调度/降频），固定 4。
4. **紧凑 cull 记录收益有限**：64 B → 16 B 只快了 ~15%，说明该段是延迟/调度受限而非带宽受限。
5. **异步排序是决定性优化**：排序搬到独立线程，渲染线程只做快照/取结果/上传，
   `ExecuteFrame` 从 30–87 ms 降到 **0.3–1.0 ms**（max 1.4–4.9 ms）。
   - 请求合并：排序中再次请求只更新快照，worker 用最新相机状态。
   - 首个模型首帧仍同步排序（否则第一帧顺序错乱）。
   - 代价：快速旋转时排序可能落后一次（15–25 ms），与 PlayCanvas 行为一致。

### 4.6 构建与调试

1. **hvigorw 必须在 `LumeApp` 目录执行**：命令里再写 `cd` 会覆盖 workdir，报
   `hvigor-config.json5 does not exist`。
2. **`entry/libs/arm64-v8a/*.so` 重复**会导致打包 `Duplicated files`，构建前先删。
3. **PowerShell**：不支持 heredoc；`%{public}` 等格式串在双引号里会被解析；多行 Python
   一律写成临时脚本再执行。
4. **UTF-8 往返陷阱**：用 `Get-Content -Raw` + `Set-Content` 处理含非 ASCII 注释的源码，
   会把 `—` 变成 `â€”`；改文件优先用编辑工具或 Python 显式 `utf-8`。
5. **截图只能是 JPEG**：`snapshot_display -f x.jpeg`，对比时用 Python 统计均值/方差/逐像素差，
   不依赖肉眼。
6. **ImageKit NDK 链接**：SOG 的 WebP 解码需要同时链接 `libimage_source.so` 与 `libpixelmap.so`。

---

## 5. 性能数据汇总

### 5.1 排序（设备实测，连续拖动，20 次排序的 min/avg/max）

| 模型 | 可见/总数 | 单线程 | +并行 | +紧凑记录 | 异步后渲染线程 |
| --- | --- | --- | --- | --- | --- |
| doll.glb | 19–21 万 / 426,605 | 31–41 ms | 17.8 avg | 16.1 avg | **0.30 / 1.4 ms** |
| fruit_bowl.ply | 62 万 / 750,322 | 61–69 ms | 25 avg | 24 avg | **0.36 / 1.6 ms** |
| fruit_bowl.sog | 62 万 / 750,322 | 68 ms | 25 avg | 23 avg | **0.99 / 4.9 ms** |
| godzilla.splat | 74–77 万 / 1,067,182 | 87 ms | 28 avg | 25 avg | **0.92 / 4.9 ms** |

（"异步后渲染线程"= `ExecuteFrame` 平均/最大耗时；排序本身仍在 15–25 ms，但已在后台线程。）

### 5.2 SOG vs PLY（同场景同视角）

| 指标 | SOG | PLY |
| --- | --- | --- |
| 体积 | 13 MB | 177 MB |
| 高斯数 | 750,322 | 750,322 |
| 包围盒 | (-11.21,-13.62,-15.83)~(20.16,7.19,15.75) | 同左（z 差 0.01，量化） |
| 画面均值 | (167.1, 117.5, 54.7) | (170.3, 120.8, 56.7) |
| 画面 std | (59.5, 53.3, 43.5) | (59.5, 53.8, 44.4) |
| 逐像素平均差 | 2.98/255（1.2%），p99 = 9 | — |

---

## 6. 验证方法（可复现）

```powershell
# 构建（必须在 LumeApp 目录）
$env:PATH="D:\Coding\DevEcoStudio\jbr\bin;D:\Coding\DevEcoStudio\tools\hvigor\bin;"+$env:PATH
$env:JAVA_HOME="D:\Coding\DevEcoStudio\jbr"
Remove-Item -Force entry/libs/arm64-v8a/*.so -ErrorAction SilentlyContinue
hvigorw assembleHap --mode module -p product=default --no-daemon

# 安装 + 冷启动
hdc install -r entry\build\default\outputs\default\entry-default-signed.hap
hdc shell "aa force-stop com.example.lumeapp"
hdc shell "hilog -r"
hdc shell "aa start -a EntryAbility -b com.example.lumeapp"

# 施压：连续拖动（触发重排），按钮坐标见 uitest dumpLayout
hdc shell "uitest uiInput drag 950 1300 350 1300 1200"

# 观测
hdc shell "hilog -x | grep -E 'GS::Sort stats|GS::ExecuteFrame cost|Loading splat model'"

# 截图 + 统计对比
hdc shell "snapshot_display -f /data/local/tmp/a.jpeg"
hdc file recv /data/local/tmp/a.jpeg "D:\Source\3DGS\captures\a.jpeg"
```

着色器改动后需要重新生成 `.spv`：

```powershell
& "...\sdk\default\openharmony\toolchains\glslang_validator.exe" -V gaussian_render.vert -o gaussian_render.vert.spv
```

---

## 7. 遗留 TODO

| 项 | 状态 / 说明 |
| --- | --- |
| ~~PLY `f_rest` 转置~~ | 已修；注意压缩 VQ 不转置（4.2-8）|
| ~~性能日志~~ | 已收进 `RENDER_DEV_ENABLED`，需要时把该宏置 1 即可恢复 |
| ~~glTF 演示死代码~~ | 已删除 |
| ~~`.hvigor/` 构建缓存~~ | 已加入根 `.gitignore` |
| ~~SOG 失败原因不明确~~ | 已改为明确报错（版本 / ZIP method / 纹理数量 / bands 范围）|
| splat 解析 fuzz | 截断、畸形头、超大声明；SOG 的 ZIP 中央目录与 WebP 尺寸、GLB chunk 长度 |
| 大场景 LOD | 5M+ 高斯需要分块/流式与每帧预算裁剪（当前全量加载 + 全量剔除）|
| 多设备 / GLES | 当前仅 Vulkan 单机验证；GLES 后端未跑 3DGS 路径，排序线程的调度表现也未验证 |
| SOG 其它变体 | 只支持 stored ZIP + v2 meta；v1/deflate 现在会明确报错但不支持 |
| 打包体积 | `fruit_bowl.ply` 169 MB 全量进 HAP，安装慢；可改成默认 SOG 或提供抽稀包 |
| GPU 数据压缩 | 每高斯 64 B（+48 float SH）；fp16 位置/缩放、打包颜色可进一步降带宽 |
| 观感 grade | 与 web 参考仍有锐度/饱和度差（USM 0.35 为经验值），可做成可调且默认关 |
| 切模型首帧 | 首次排序仍同步（~285 ms，含缺页与上传），可改成"先画上一帧顺序" |
| 资产许可 | `docs/knowledge/3dgs-demo-assets.md` 里来源/许可仍待补齐 |
| 稳定性检视 / push | 仓库规则要求 GPU/Shader 改动前跑稳定性检视；push 由你负责 |

---

## 8. 参考实现对照

**PlayCanvas 引擎（`src/scene/gsplat/gsplat-sorter.js` + `gsplat-sort-worker.js`）**
- CPU 计数排序，跑在**独立 Web Worker**，渲染线程从不等待；结果延迟一帧、双缓冲上传。
- 桶数自适应 `2^clamp(round(log2(n/4)),10,20)`；先用 chunk 包围球做 32 桶粗直方图按密度分配。
- 只按距离排序，用二分裁掉相机后方高斯；没有视锥/像素尺寸剔除。
- 本项目在"排序三段并行 + 后台线程"上与之一致，额外保留了视锥/尺寸剔除与 65536 固定桶。
