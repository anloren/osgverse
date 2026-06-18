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
- `master`：已推送 `origin/master`，含 **P0 图层叠加 + 标注开关**。Tag v0.8/v0.9/v0.9.1 都在（历史），但**只有 v0.8 是 GitHub Release**——用户政策：release 须本人确认，否则只同步仓库（见记忆 `release-requires-confirmation`）。
- 设计 spec：`docs/superpowers/specs/2026-06-18-earth-overlay-layers-design.md`（整体路线）。
- **P0 已完成并合并 master**：底图 `lyrs=y`→`lyrs=s`；标注 `lyrs=h` 进 USER 层(texUnit2/UvOffset3)；`LabelOpacity` uniform 门控；`LayerManager`(`applications/earth_explorer/LayerManager.h`) +「图层」UI 折叠栏(方案B)。计划 `docs/superpowers/plans/2026-06-18-earth-overlay-p0-labels.md`。
- 当前分支 **`feature/earth-overlay-gibs`**（**未合并、未推送**）：**P1 GIBS 近实时影像层**，已完成。计划 `docs/superpowers/plans/2026-06-19-earth-overlay-p1-gibs-clouds.md`。
  - 第二 GPU 合成栅格槽：着色器 `Overlay2Sampler`(unit3)/`UvOffset4`/`Overlay2Opacity`；**大气采样器起始单元从 3 挪到 4**（applyToGlobe）。
  - `TileCallback::OVERLAY=4`(texUnit3)；TMS 插件 `Overlay` 选项；GIBS MODIS Terra 真彩 `GoogleMapsCompatible_Level9`、**z>9 截断**、**日期用 UTC今天-2天**（`default`=今天马赛克未拼全→404 风暴）。
  - 面板新增「影像 / 天气」组、云图层默认关、透明度 0.7。实证：开→GIBS 真彩/云带混合、关→干净底图；全球无回归；.app 已验证。
  - **已知取舍**：GIBS 层走 earthURLs 常驻加载（换开关即时），故**云图默认关时 GIBS 瓦片仍会下载**。改"按需加载"（enable 才 setLayerPath）是 P1.1 跟进项。
  - 无头截图注意：云图加载时 app 约 ~300 帧 ImGui 后端 shutdown（比 P0 早），`EARTH_AUTOCAP` 要 ≤ ~280；交互运行不受影响。
- **下一步 P2+**：按 spec 接 RainViewer 雷达 → USGS 地震 → OpenSky 航班 →…（免 key 优先；点要素走独立 billboard 子图 + 后台拉取线程）。

## 极地处理：v0.9 去截断造成空洞 → v0.9.1 撤回，保留星状（2026-06-18）
**最终结论**：全球/中纬贴图本来就正确（早先以为不正确是误诊）。极点 v0.9 试过"去截断"反而留出敞开极冠盘/空洞、像球被整体变形，用户否决；**v0.9.1 已恢复截断**，回到星状极点（更顺眼、轮廓正常），星芒作为已知限制保留。

**早先误诊（别再犯）**：commit `50f73d4c` 以为"瓦片内部顶点按纬度线性插值、要在 `computeTileExtent` 做墨卡托反投影"，照此改反而把整张贴图搞扭，已回退（`47e0072e`）。

**重新定位的真相（证据驱动）**：
1. **顶点级墨卡托反投影早就正确**，藏在 `TileCallback::adjustLatitudeLongitudeAltitude`（TileCallback.cpp ~160）：extent‑Y 本身线性于墨卡托 Y（`inDegrees(extentY)=mercY/2`），adjust 里 `atan(sinh(mercY)) ≡ 2·atan(eᵐ)−π/2`（Gudermannian），所以每顶点纬度已是精确反投影。45°N 低 zoom（全欧洲）海岸线比例正确即证。**不需要改 computeTileExtent，重做是空操作。**
2. **极点星芒真因 = `convertLLAtoECEF`（Math.cpp:430）的 85.05° 硬截断**：最顶瓦片顶边 `atan(sinh(π))=85.0511°` 刚好略超阈值 → 整顶行 16 顶点 snap 到同一极点 → 扇形坍缩成星。
   - **v0.9 试过删截断**：星芒消失但极冠敞开成盘/空洞（裙边/海洋面填充、轮廓鼓出），用户认为比星状更差。
   - **v0.9.1 撤回，恢复截断**：保留星状极点（收敛点，轮廓正常）。<85° 区域三种状态都无回归（截断只在 >85.05° 触发）。
3. **遗留（已知限制）**：Web Mercator 无 >85.05° 瓦片数据，极冠无法贴图。当前取"星状收敛点"。真正干净做法 = 主动在 >85° 填平极冠（冰白 disc），属新增几何、风险高，留作后续单独立项。

**验证铁律**：极地/投影类改动必须看**低 zoom 中纬度陆地**（`--goto 45 12 6000` 全欧洲，海岸线比例）+ 极点正上方（`--goto 89.5 0 5000`）；别只看高 zoom（单瓦片跨度极小，gd 非线性可忽略，看不出问题）。

## 下一步建议（给新会话）
1. ✅ 回退已确认正常（全球/北京 40°/欧洲 45° 低 zoom/斯瓦尔巴 78° 全部无扭曲）。
2. ✅ 极点：v0.9 去截断的空洞已撤回，v0.9.1 恢复星状（用户偏好）。极冠 >85° 作为已知限制。
3. ✅ 已合并 master、打了 v0.8/v0.9 并推送。v0.9.1 = 撤回极地去截断的小修版（需重打包 .app + 推送 tag）。
4. 用户**单独立项、本轮未做**的需求：**真实感 3D 瓦片**（Google Photorealistic 3D Tiles / OGC 3D Tiles 网格，需 Google Cloud API key，另一条管线）——单独 brainstorm + 计划。

## 已完成的全部能力（截至 v0.8 + 本分支 4 项）
- 在线 Google 混合影像(lyrs=y) + AWS Terrarium 高程，多级流式到街道级(z19)
- ImGui 面板：经纬度/海拔读数、太阳方位/高度、**真实时间太阳**、海洋、曝光、**大气强度**、跳转、书签、退出
- 瓦片磁盘缓存(`~/.osgverse_earth_cache`，env `EARTH_TILE_CACHE`)
- 相机最底点 50m（不穿地表）
- 滚轮缩放(macOS SCROLL_2D)、Go To 俯视、`--goto` 命令行
- 可双击 `dist/EarthExplorer.app`（`packaging/package_macos.sh` 打包，`packaging/run.sh` 命令行）

全部移植/实现细节见 `tasks/lessons.md`。
