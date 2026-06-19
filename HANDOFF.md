# osgVerse EarthExplorer — 工作交接（HANDOFF）

> 给**全新会话**的接手文档（你没有之前的上下文）。先读这份，再读 `tasks/lessons.md`（全部排坑细节）。
> 用户主语言中文，请用中文回复。macOS / Apple Silicon。

---
## ⚠️ 2026-06-20 会话已整体回退 —— 必读，别再踩

上一会话试图修"昆明深瓦片错位"，结果**越改越多**，被用户要求**整体回退到会话前版本 `0f902837`**。
当时引入、现已删除的一系列回归 bug —— **下一步迭代严禁再次引入**：

1. **整片海洋变橙红**（全球俯视 33000–43000km，连夜侧也橙）。根因：把默认太阳方向翻成照亮正面后，
   `scattering_globe.frag.glsl` 的**太阳直射辐射 sunL**（低太阳角=落日橙）叠在近黑的海面上 → 全橙；亮地面(撒哈拉)过曝发白。
   **不是瓦片、不是缓存**（Google 海洋瓦片本来就是蓝的，已 curl 验证）。
2. **晨昏线被删**：曾用"直接显示影像(fullbright)"去压橙色，结果把昼夜分界整个干掉了。**用户要保留晨昏线。**
3. **倾斜穿模 / "高耸面片"扇形**：低空(如昆明 2.5km)倾斜时相机钻到地下、看穿地球看见另一侧。
   根因：相机地形地板只**正下方**射线，漏掉**旁边更高的地形**(西山 2.5–2.8km)，倾斜看向它就穿。
4. **桔红程序化水面 / OceanOpaque 默认乱动**：用户明确"程序化光滑水面原来从不显示"，别默认开它。

**下一步迭代铁律**：① 一次只改一个、当场用真实复现条件验证、确认不引入上面 4 类再继续；
② 在**用户报告的确切条件**下复现(高度段/开关/太阳)，别乱缩放截图；③ 改渲染前先 curl 真实瓦片确认不是数据问题；
④ 不要碰默认太阳方向/OceanOpaque 去"修"颜色。详见记忆 [[earth-orange-ocean-rootcause]]、[[earth-debug-verify-at-exact-condition]]。

**当前仍未解决（原始 bug，回退后又回来了，是这次会话最初要修的）**：昆明/滇池岸**深瓦片错位**——
z16 平瓦片(0m) vs z15 高程(~1890m)在 LOD 边界形成裂缝/看穿。修它时务必避开上面 4 类回归。
回退说明见提交 `revert(earth): roll back this session's earth changes`；我的会话工作分支已按用户要求删除。
---

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
- `master`：已推送 `origin/master`，含 **P0 标注层 + P1 GIBS 影像层 + 性能优化 + 橙块/晨昏线修复 + 启动低-LOD 预热(#1) + 瓦片 5 层并行加载(#3) + 深瓦片高程继承修复**。
- **深瓦片高程继承修复（2026-06-19）**：z>15 瓦片原本建成海平面平地（既存 bug，非 #3 引入），高海拔区（昆明 1890m）斜视呈视差错位 + 深蓝块。修复让其继承父级 z15 高程（`createTile` 标 DEFERRED + `updateLayerData` 对 ELEVATION 放行 emptyPath 复用父级）。受控实验（cutoff 临时降 z>4）已证实修复生效、无崩溃。**最终视觉待用户在昆明交互验证**。细节见 `tasks/lessons.md`「深瓦片…平地」节。
- **海洋遮罩继承修复（2026-06-19）**：开「海洋」后 z>3 瓦片缺 mask → 整块（含陆地）被海洋 pass 染蓝（矩形深蓝块、多处）。同一类继承 bug；修复让 OCEAN_MASK 也继承父级 z3 mask。headless（`EARTH_OCEAN=1`+斜视）已证实修复前有蓝块、修复后消失。细节见 `tasks/lessons.md`「海洋遮罩…染蓝」节。**基础层（elevation/mask）应继承；overlay/labels 保留 !emptyPath 门。**
- **极近距离崩坏缓解（2026-06-19）**：~50m 极近时整屏拉丝崩坏 = 近平面裁剪（行星深度精度，near≈127m）。**`EarthProjectionMatrixCallback` 不能用**（启用会洗白远景——大气 pass 从深度反算世界坐标、与 near/far 紧耦合；已试已回退）。缓解：相机最小距离 50m→**150m**（`EARTH_MIN_DIST` 可调）。真正修法=对数深度缓冲，留作单独立项。细节见 `tasks/lessons.md`「极近距离…近平面裁剪」节。
- **深 zoom 真实高程 + 相机地形地板（2026-06-19，最终方案）**：经历多轮反复后定下来的正解。两件事缺一不可：
  - **① 深瓦片真实高程（消除 LOD 边界视差错位/空白缝）**：`createCustomPath` 对 z>15 ELEVATION 取 **z15 祖先瓦片**，`createTile` 算 `elevScaleBias` 子区在 `createTileGeometry` 一步烘焙正确高度。**已用探针验证**：昆明 z16/17/18 瓦片高程 = 1883~1891m（正确，与 z15 连续）。**关键：必须配 ②，否则相机会穿模**（高程瓦片 + 椭球最小距离 = 相机钻进高地形 → 尖刺扇）。
  - **② 相机地形地板 `_terrainFloor`（防穿模，不抖）**：`EarthManipulator` 每帧向下射线取**地形顶面**（first-hit + 最高 LOD），平滑求 `_terrainFloor`（升快降慢 + 2m 死区，杜绝抖动/闪烁）。**只在视图矩阵里**用 `max(_distance,_terrainFloor)`，**不改 `_distance`**（用户 zoom 意图不变）→ 无振荡。`getDistance()` 返回**有效（floored）距离**，让 LOD/分页按相机实际位置请求瓦片（否则会空屏）。
  - **headless 验证**（多机位，用户要求）：昆明俯视 3km 干净有细节、斜视地形连续无缝无扇、相机被抬到地形+150m（如山麓 2.4km）。**唯一 headless 伪影**：`--goto` 到地形以下(0.3km)→地板瞬间抬到 2.4km→该高度瓦片没缓存→空屏；交互式渐进 zoom 不会触发（相机连续被托住、瓦片逐步加载）。**待用户交互最终确认**。
  - 调试：`EARTH_TILT=<弧度>` 给 --goto 加俯仰角（截斜视图看缝/穿模）。
  - **保留的好改进**：预热、5层并行、缓存原子写、reader-writer 锁、海洋遮罩继承、最小距离 150m。
  - **用户实测反馈（继续修）**：① 大空缝已消失 ✅（高程修复生效）。② **瓦片接缝小黑块**（陡坡/湖岸/海岸的 LOD 边界裂缝）——已做**高程感知裙边**：`updateSkirtData` 的 skirtHeight 取 `max(瓦片尺寸项, 瓦片高程range×1.5)`（`_tileElevRange` 在 `createTileGeometry` 顶点循环里算），陡坡瓦片裙边自动变高盖住裂缝。headless 复现不出那个角度，**待用户验证**。③ **特定角度仍穿模**（如 Easter Island 陡峭海岸：岛 0m 邻海床 -4000m）——地形地板只向正下方射线、**不管旁边更高的峰/陡坎**；要彻底修需地板考虑邻域最高地形（多向射线或邻域采样），**留待下一步**。
- **相机地形感知钳制（2026-06-19）**：穿地是**全球**问题（非昆明特例）。`EarthManipulator::clampToTerrain()` 每帧射线求交、保相机高于真实地形 150m。三坑：只在 <20km 求交(性能)、`_terrainLift` 每帧撤销重算(不累加)、取**最近命中**非最高(避开漂浮尖刺)。headless 验：昆明 0.5km→抬到 2.7km 干净渲染、全球远景不受影响。细节见 `tasks/lessons.md`「相机地形感知钳制」。
- **仍待办/待查**：① 瓦片细缝（issue 2）——高程继承"先平地后抬升"两步的瞬时高度缝。② issue 1 近平面对数深度正规修法。③ **深瓦片高程是否偏高**：headless 上海(海平面)粗瓦片读成 ~890m，疑似坏高程/漂浮尖刺(那条"虚线"同源)；待交互(精瓦片就位)确认是 headless 假象还是真 bug。
- **.app 重打包状态**：截至本修复后已重打包（含 #1/#3/高程修复）。Tag v0.8/v0.9/v0.9.1 都在（历史），但**只有 v0.8 是 GitHub Release**——用户政策：release 须本人确认，否则只同步仓库（见记忆 `release-requires-confirmation`）。
- 设计 spec：`docs/superpowers/specs/2026-06-18-earth-overlay-layers-design.md`（整体路线）。计划：`plans/2026-06-18-...p0-labels.md`、`...p1-gibs-clouds.md`、`2026-06-19-earth-perf.md`。
- **P0 标注层**：底图 `lyrs=s`；标注 `lyrs=h`(USER,texUnit2/UvOffset3)；`LabelOpacity` 门控；`LayerManager`(`applications/earth_explorer/LayerManager.h`)+「图层」UI(方案B 分类折叠)。
- **P1 GIBS 影像层**：GIBS MODIS 真彩(OVERLAY,texUnit3/UvOffset4,`GoogleMapsCompatible_Level9`,**z>9截断**,日期=`UTC今天-2天`)；`Overlay2Opacity` 门控(**大气采样器起始单元 3→4**,applyToGlobe)；面板「影像/天气」组、默认关、透明度0.7。**取舍**：earthURLs 常驻加载→默认关时仍下载(P1.1 改按需)。
- **性能/视觉优化**(plan `2026-06-19-earth-perf.md`)：
  - 求交(99d3f223)：`EarthManipulator::calcIntersectPoint` 改解析 ray-椭球，去掉每次鼠标的全场景 `IntersectionVisitor` 遍历。
  - elevation z>15 截断(dfd8fd09)：AWS terrarium 最高 ~z15，深 zoom 的 z16-19 必 404→主线程阻塞(近地卡死主因)+刷屏，已消除；深瓦片复用父级高程。
  - 橙块(7a6d1635)：未加载底图默认 白→深海蓝`Vec4(0.03,0.06,0.10)`(Scene 单独，Mask 仍白=陆地，见 `render_effects.cpp:89`)；`pager->setTargetMaximumNumberOfPageLOD(1500)`(默认300太小→低/中LOD过期重载露白底)。**用户实测：半个地球橙块已消失** ✅。
  - 晨昏线(0f902837)：橙带=昼夜分界大气 inscatter+饱和过强。`scattering_globe.frag.glsl` inscatter `*0.5`、饱和 `1.18→1.08`(仅夜/晨昏侧 cTheta→0 用到，不动白天影像)。**调淡/调浓就改这两个数。**
  - **缓存查明本就好**：真缓存在 `loadFileData`(`UtilitiesEx.cpp:496`)，per-tile `.tile`、`$HOME/.osgverse_earth_cache`(绝对、env `EARTH_TILE_CACHE` 可覆盖)、无限大小；实测 8537 块/重访 0 新增。registry 的 simdb `FileCache` 不用于 HTTP 瓦片（别再去改它，曾误改致崩溃）。

## 待用户交互验证（headless 无法验）
鼠标旋转/缩放手感、近地流畅度、晨昏线观感（太重→inscatter 再降到 0.3；太淡→回 0.7）。

## 下一步 backlog（下个 session 逐步做，按优先）
1. ✅ **启动预载整个地球低 LOD 瓦片**（2026-06-19 已做并推送 master）：`earth_main.cpp` 新增 `prefetchLowLODGlobe(maxZ)` + main 里 detach 后台线程；4 worker 拉 z0..maxZ 全球 **底图+标注+高程** 三层进磁盘缓存。`EARTH_PREFETCH` 控制最大 zoom（默认 4，0 关闭）。配套把 `loadFileData` 缓存写改成 **temp+rename 原子替换**（`UtilitiesEx.cpp`，并发安全）。headless 已验：Warmed 日志 + 缓存增长 + 0 残留 .tmp + 渲染正常。细节见 `tasks/lessons.md`「启动预热」节。**待交互验证**：拉近某地→缩回从未去过的区域，是否真的秒出图（headless 无法验外推 UX）。**.app 未重打包**（feature 是后台缓存预热、不改渲染；要双击版生效需 `packaging/package_macos.sh` 重打包）。
2. **完整异步层加载**（消除首访顿挫）：`TileCallback::operator()`(update=主线程) 在 `_layersDone=false`/`TileManager::check()` 时同步 `readImage` 阻塞(`TileCallback.cpp` updateLayerData，异步 `requestImageFile` 被注释)。改后台 thread-pool 加载、就绪帧应用。**高风险(并发)、headless 无法验 UX**——留交互会话谨慎做。注意：现在已有 `LayerLoadPool`（见 #3），#2 可复用同一池把 operator() 的同步重试也甩到后台。
3. ✅ **瓦片吞吐并行**（2026-06-19 已做并推送 master）：`ReaderWriterTMS::createTile` 的 5 层从串行改成**常驻 keep-alive 线程池** `LayerLoadPool` 并行加载（`EARTH_TILE_POOL`，默认 8，0=关闭=原串行）。**不能用临时线程**（会丢 thread_local TLS keep-alive→更慢）。配套加锁两处既存无锁 reader-writer 缓存 race（`TileManager::getReaderWriter` + `ReaderWriterWeb::getReaderWriter`）、`_layerPaths` operator[]→find()。实测冷下钻 ~4× 吞吐(528 vs 128 块/16.8s)、渲染正常、无崩溃。**待交互验证**：街道级下钻是否更快、有无任何镜像（headless 冷载截图有镜像伪影，已确证是回读伪影非回归）。细节见 `tasks/lessons.md`「瓦片 5 层并行加载」节。**.app 未重打包**。
4. **中国 GCJ-02 偏移**(已知限制)：中国区 `lyrs=h` 标注是 GCJ-02、卫星 `lyrs=s` 是 WGS-84，偏 ~50-700m。可在标注层 UV 做顶点级 GCJ-02→WGS-84 校正(仅中国、近似)。用户说"实在不行就算了"。
5. 之后按 spec：P2 RainViewer 雷达 → USGS 地震 / OpenSky 航班(点要素=独立 billboard 子图+后台拉取线程)。
- **其它已知限制**：headless 截图 `EARTH_AUTOCAP`≤~280（交互不受影响）；`gmtime_r` 仅 POSIX（Windows 需适配，既有模式）。

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
