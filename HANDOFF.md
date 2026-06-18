# osgVerse EarthExplorer — 工作交接（HANDOFF）

> 给**全新会话**的接手文档（你没有之前的上下文）。先读这份，再读 `tasks/lessons.md`（全部排坑细节）。
> 用户主语言中文，请用中文回复。macOS / Apple Silicon。

## 这是什么
osgVerse（基于 OpenSceneGraph 的 3D 引擎，`github.com/anloren/osgverse`）的 **EarthExplorer**：一个在线流式高清地球应用，已移植到 macOS（OpenGL 4.1 Core），有 ImGui 控制面板、可双击 `.app`。

## 关键路径
- 仓库：`/Users/franklee/osgverse`（远端 `origin` = github.com/anloren/osgverse，已用账号 `anloren` 登录 gh）
- OSG 源码（预克隆，含本地补丁）：`/Users/franklee/OpenSceneGraph`
- CORE 构建产物：`/Users/franklee/osgverse/build/sdk_core/`；增量构建目录：`/Users/franklee/osgverse/build/verse_core/`
- 可执行：`build/sdk_core/bin/osgVerse_EarthExplorer`；桌面应用：`dist/EarthExplorer.app`
- 实现计划：`docs/superpowers/plans/`；排坑记录：`tasks/lessons.md`；用户文档：`docs/EarthExplorer.md`

## 必须知道的硬约束（否则白干）
1. **macOS 必须用 CORE 模式**（`Setup.sh CORE` → sdk_core/verse_core）。兼容档只有 GL2.1/GLSL120，osgVerse 着色器要 ≥130；CORE 才有 GL4.1/GLSL410。
2. 直接跑二进制要设 `DYLD_LIBRARY_PATH=build/sdk_core/lib`（rpath 用了 Linux 的 `$ORIGIN`，macOS 无效）。
3. **增量构建**：`cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`（秒级到分钟级，别重跑 Setup.sh）。
4. **无头截图验证**（OS `screencapture` 缺权限，用应用内 `ScreenCaptureHandler`）：
   ```bash
   cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
   DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
   EARTH_AUTOCAP=1500 EARTH_SUN_TO_CAMERA=1 \
   ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
   # 然后用 Read 工具看 /tmp/earth_capture_0.png
   ```
   - `EARTH_AUTOCAP=N`：渲染 N 帧后截图到 `/tmp/earth_capture_0.png` 退出。**注意**：本机约 ~700 帧后 ImGui 后端会 shutdown，N 设太大反而抓不到；定点深瓦片用 `EARTH_FRAME_SLEEP_MS=30` 给网络时间，`EARTH_AUTOCAP=2500` 左右。
   - `EARTH_SUN_TO_CAMERA=1`：太阳对准相机，便于看清（会每帧覆盖 WorldSunDir）。
   - `--goto <纬> <经> <高度km>`：启动直飞某地（俯视）。如 `--goto 37.77 -122.42 6` 看旧金山街道级。
   - `EARTH_FRAME_SLEEP_MS=30`：每帧延时，给在线瓦片流式加载真实时间。
5. **深层瓦片是延迟受限**：拉近后要等几秒瓦片流式加载（z 逐级 3→6→14…到街道级），不是瞬时。已加 TCP/TLS 长连接复用 + 20 HTTP 线程加速。

## Git 现状（重要）
- `master`：已推送到 `origin/master`，打了 **v0.8** 标签 + GitHub Release「0.8 osgverse mac」。这是稳定基线。
- 当前分支 **`feature/earth-improvements`**：领先 master **8 个提交**，**未合并、未推送**。包含：
  ```
  47e0072e Revert "...polar..."        ← 回退了极地修复（见下）
  add916cc docs(...)
  338bf404 feat: 着色器饱和/对比 + 大气强度滑块      ✅好
  e2df2dcb feat: 真实时间太阳（系统时钟+可设UTC）     ✅好
  01521ccb feat: 相机最底点(50m)夹紧，不穿地表        ✅好
  38a4c93d feat: 瓦片磁盘缓存(loadFileData)           ✅好
  2cf369ef docs: 实现计划
  ```
- 本轮计划做 5 项改进（缓存/最底点/极地/光影/真实时间）。除**极地外其余 4 项已完成并各自通过规格+质量双审**。

## ⚠️ 当前未完成的核心问题：极地贴图扭曲
**用户反馈**：原本想修「极地贴图扭曲」，我的修法（commit `50f73d4c`）反而把**整张贴图都搞扭曲了**，已 `git revert`（commit `47e0072e`）回退。

**根因（关键，别再犯）**：在线瓦片是 **Web Mercator**，纹理纵向按**墨卡托 Y 非线性**分布。我当时只改了 `TileCallback::computeTileExtent` 让瓦片的纬度**边界**走墨卡托反投影，但**瓦片内部顶点仍按纬度线性插值**（`createTileGeometry` 的顶点循环 + 那个坏掉的 `adjustLatitudeLongitudeAltitude`）。边界对了、内部没对 → 每个瓦片内纹理被非均匀拉伸 → **低 zoom（全球/区域）整张图扭曲**。高 zoom 瓦片小、内部非线性可忽略，所以无头高纬截图"看着没事"，漏掉了全局问题（教训：极地/投影类改动必须在**低 zoom 中纬度**目视验证，不能只看高 zoom）。

**正确修法（待做）**：让**每个顶点的纬度**也走墨卡托反投影。
- 文件：`readerwriter/TileCallback.cpp` 的 `createTileGeometry`（顶点循环，约 232-248 行）+ `adjustLatitudeLongitudeAltitude`（约 160-170，目前公式是错的/循环的）。
- 顶点在瓦片内归一化纵坐标 `v∈[0,1]`，其纬度应为：
  ```
  n = 2^z;  rowTop = _bottomLeft ? (n-1-_y) : _y
  mercY_top = PI*(1 - 2*rowTop/n);  mercY_bot = PI*(1 - 2*(rowTop+1)/n)
  mercY = mix(mercY_top, mercY_bot, v)        // 在墨卡托 Y 空间线性插值
  lat   = 2*atan(exp(mercY)) - PI/2            // Gudermannian 反投影
  lon   = mix(lonMin, lonMax, u)               // 经度本来就线性，OK
  ```
  即**顶点要在墨卡托 Y 空间均分**，再反投影成纬度——不是在纬度空间均分。
- `convertLLAtoECEF`（`modeling/Math.cpp` ~430）的 ±85.05° 极地硬截断也应去掉（仅影响 >85°，Web Mercator 最大 85.05°，去掉无害且让顶 Mercator 瓦片正确）。
- **验证必须包含**：全球视角 + 中纬陆地（如 30-50°，巴黎/北京）+ 高纬，确认**整张贴图不扭、海岸线比例对、不翻转/不镜像**。建议先 `--goto 39.9 116.4 300`（北京华北平原，规则地物易看出扭曲）。
- 风险高：若搞不定，就**保持回退状态**（极地扭曲作为已知限制），不要再把全局搞坏。

## 下一步建议（给新会话）
1. **先确认回退已恢复正常**：增量构建后跑全球 + 中纬截图（见上命令，如 `--goto 39.9 116.4 300`），用 Read 确认贴图正常、无扭曲。
2. 然后**决定极地**：要么按上面「正确修法」重做（在 createTileGeometry 顶点循环做墨卡托反投影，强验证低 zoom），要么保持回退、记为已知限制。
3. 完事后**收尾分支**：当前 4 项好改进 + 文档已就绪。可合并 `feature/earth-improvements` → `master`（fast-forward 或 merge），推送，并视情况打 **v0.9** 标签 + Release。用户之前的偏好是：合并到 master、推送、打版本（推送是对外动作，**做前先问用户确认**）。
4. 也有一个用户**单独立项、本轮未做**的需求：**#4 真实感 3D 瓦片**（Google Photorealistic 3D Tiles / OGC 3D Tiles 网格，需 Google Cloud API key，是另一条管线）——单独 brainstorm + 计划。

## 已完成的全部能力（截至 v0.8 + 本分支 4 项）
- 在线 Google 混合影像(lyrs=y) + AWS Terrarium 高程，多级流式到街道级(z19)
- ImGui 面板：经纬度/海拔读数、太阳方位/高度、**真实时间太阳**、海洋、曝光、**大气强度**、跳转、书签、退出
- 瓦片磁盘缓存(`~/.osgverse_earth_cache`，env `EARTH_TILE_CACHE`)
- 相机最底点 50m（不穿地表）
- 滚轮缩放(macOS SCROLL_2D)、Go To 俯视、`--goto` 命令行
- 可双击 `dist/EarthExplorer.app`（`packaging/package_macos.sh` 打包，`packaging/run.sh` 命令行）

全部移植/实现细节见 `tasks/lessons.md`。
