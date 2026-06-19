# osgVerse macOS 编译经验（DEFAULT / OpenGL Compatible, arm64）

环境：macOS Darwin 25, Apple Silicon (arm64), Xcode CLT clang 21, cmake 4.3, OSG 3.6.5。
工作目录：/Users/franklee/osgverse，OSG 源码预克隆到 /Users/franklee/OpenSceneGraph（Setup.sh 期望同级 ../OpenSceneGraph）。

## 修复 1：libpng 误包含经典 Mac OS `<fp.h>`
- 现象：编译 3rdparty 时 `helpers/toolchain_builder/png/pngpriv.h:527 fatal error: 'fp.h' file not found`。
- 根因：现代 macOS 的 TargetConditionals.h 定义 `TARGET_OS_MAC=1`，触发了为 Classic Mac OS 写的 `<fp.h>` 包含分支。
- 修复：pngpriv.h ~520 行的条件里删除 `|| defined(TARGET_OS_MAC)`，使其落到 `#else` 的 `<math.h>`。

## 修复 2：OSG tiff 插件链接缺 libjpeg 符号
- 现象：链接 `osgdb_tiff.so` 报一堆 `_jpeg_*` undefined symbols for arm64。
- 根因：捆绑 libtiff 启用 JPEG（tif_jpeg.c），但 OSG tiff 插件只链接 TIFF_LIBRARY，未带 libjpeg；静态库不传递依赖。
- 修复：`../OpenSceneGraph/src/osgPlugins/tiff/CMakeLists.txt` 把 `SET(TARGET_LIBRARIES_VARS TIFF_LIBRARY)` 改为 `... TIFF_LIBRARY JPEG_LIBRARY)`。JPEG_LIBRARY 已由 Setup.sh 传入 OSG cmake 缓存，无需额外定义。

## 修复 3：OSG freetype 插件链接缺 libpng/zlib 符号
- 现象：链接 `osgdb_freetype.so` 报 `_png_*` undefined（freetype 的 sbit PNG 字形支持 `Load_SBit_Png` 依赖 libpng，libpng 又依赖 zlib）。
- 修复：`../OpenSceneGraph/src/osgPlugins/freetype/CMakeLists.txt` 的 `SET(TARGET_EXTERNAL_LIBRARIES ${FREETYPE_LIBRARIES})` 改为追加 `${PNG_LIBRARY_RELEASE} ${ZLIB_LIBRARY_RELEASE}`。
- 关键坑：不能用 `${PNG_LIBRARY}`（OSG FindPNG 未填充该名字，展开为空）；必须用 cmake 缓存里确有值的 `PNG_LIBRARY_RELEASE`。zlib 用 `${ZLIB_LIBRARIES}` 或 `${ZLIB_LIBRARY_RELEASE}` 均可。

## 修复 4：GLFW（Cocoa 后端）链接缺 macOS 系统框架
- 现象：编译 osgVerse 阶段 `osgVerse_PreTest_GL` 链接报 `_CFArrayAppendValue` 等 undefined（来自 libglfw.a 的 cocoa_*.m）。
- 根因：`helpers/toolchain_builder/GLFW/CMakeLists.txt` 的 glfw 静态库未链接 Cocoa/IOKit/CoreFoundation/CoreVideo 框架。
- 修复：在 `ADD_LIBRARY(glfw STATIC ...)` 后加 `IF(APPLE) TARGET_LINK_LIBRARIES(glfw "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation" "-framework CoreVideo") ENDIF()`。静态库的 TARGET_LINK_LIBRARIES 作为 INTERFACE 传播给所有使用者（GLFW_test、readerwriter），一次修复。

## 修复 5：AppleClang 把 -Wnon-pod-varargs 当错误
- 现象：编译 `wrappers/generic_osg/FragmentProgram.cpp:26` 报 `cannot pass object of non-trivial type 'osg::Matrixd' through variadic method`（osgVerse 序列化器 `InputUserData::add(...)` 大量这样传 Vec4d/Matrixd）。
- 根因：osg::Matrixd/Vec4d 平凡可复制、arm64 varargs 实际可用，但 AppleClang 默认把该警告升级为错误。
- 修复：根 `CMakeLists.txt` 的 `IF(NOT WIN32) ADD_COMPILE_OPTIONS(-Wno-psabi)` 之后加 `IF(APPLE) ADD_COMPILE_OPTIONS(-Wno-non-pod-varargs) ENDIF()`，全局降级，避免逐个改 wrapper。
- 注意：该改动是全局编译选项，cmake 重配后会触发 osgVerse 全量重编。

## 加速：避免每次跑完整 Setup.sh
- 续编 OSG：直接 `cd build/osg_def && cmake . && cmake --build . --target install`（增量，仅重链改动插件，~秒级到达失败点）。
- OSG 装好后（build/sdk/bin/osgviewer 存在），再跑 `Setup.sh DEFAULT` 会自动 SkipOsgBuild=1，跳过 3rdparty/OSG，直接编译 osgVerse。

## 关键：macOS 桌面必须用 CORE 模式，不能用 DEFAULT
- 现象：DEFAULT（OpenGL Compatible）构建的 EarthExplorer 能启动，但着色器全线失败 `version '130' is not supported`；运行日志显示上下文是 `OpenGL 2.1 Metal; GLSL: 120`。
- 根因：现代 macOS 原生 OpenGL 在兼容档(compatibility)只给 GL2.1/GLSL120；osgVerse 着色器要 `#version 130+`（`Pipeline.cpp:1459` 强制 ≥130）。只有 **Core Profile** 才能拿到 GL3.3+/GLSL150+。
- 机制：`pipeline/ShaderLibrary.cpp:232` 在定义了 `OSG_GL3_AVAILABLE`（CORE 编译的 OSG）时发出 `#version 330 core`，macOS 支持。
- 解决：用 `Setup.sh CORE` 构建（sdk_core/verse_core）。之前所有源码编译修复都在源码树，自动复用。结果在 `build/sdk_core/bin/`，运行需 `DYLD_LIBRARY_PATH=build/sdk_core/lib`。

## 修复 6：verse_tiff 插件未构建（mbtiles .tif 高程/遮罩解码失败）
- 现象：运行日志 `[mbtiles] No reader/writer plugin for tif`；`build/sdk/lib/osgPlugins-*/` 里没有 `osgdb_verse_tiff.so`。
- 根因：插件被 `IF(VERSE_WITH_TIFF_LIBRARY)` 门控；根 CMakeLists line 732 用 `IF(TIFF_FOUND)` 置位，但 Setup.sh 只传 `TIFF_INCLUDE_DIR/TIFF_LIBRARY_RELEASE`、从不跑 `find_package(TIFF)`，故 `TIFF_FOUND` 为空。
- 修复：根 CMakeLists 改为 `IF(TIFF_FOUND OR TIFF_LIBRARY_RELEASE) SET(TIFF_FOUND ON) SET(VERSE_WITH_TIFF_LIBRARY ON) ...`。

## 截图：用 OSG ScreenCaptureHandler，不要 OS screencapture
- macOS `screencapture` 报 `could not create image from display`（缺屏幕录制 TCC 权限，无法编程授予）。
- 改用 OSG `osgViewer::ScreenCaptureHandler`（glReadPixels 读 framebuffer）截图，绕过权限。

## 运行注意
- 后台启动构建务必先 `cd /Users/franklee/osgverse`（Bash 后台 shell cwd 可能被重置到 claudecode，导致 exit 127）。
- 重新运行 Setup.sh DEFAULT 是增量的：3rdparty/OSG 已缓存在 build/ 下，只重配+重链+继续。

---

## feature/high-precision-earth-app 功能记录（Tasks 1-7）

本节记录 EarthExplorer 在线高精度地球 + ImGui 控制面板 + macOS .app 打包的关键实现细节。

### 在线数据源

- **正射影像**：ArcGIS World Imagery（XYZ 瓦片）
  URL 模板：
  https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}
  层级 0-19，已覆盖全球，JPEG 压缩，支持跨域。

- **高程 (Terrarium 编码)**：AWS Terrain Tiles
  URL 模板：
  https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png
  PNG 格式，RGB 三通道编码高程。

### Terrarium 高程解码

解码公式（`TileCallback::decodeTerrarium`）：

```
height_m = (R * 256 + G + B / 256.0) - 32768
```

实现路径：`readerwriter/TileCallback.cpp` 的 `TileCallback::decodeTerrarium()`（声明在 `readerwriter/TileCallback.h`，含纯函数 `decodeTerrariumHeight`）。
输出格式为 `GL_RED / GL_R32F` 单通道浮点图像（`GL_FLOAT`），下游 `createTileGeometry` 按 `getDataType()==GL_FLOAT` 直接当米数高度使用。
在 `plugins/osgdb_tms/ReaderWriterTMS.cpp`（verse_tms 插件）解析 earthURLs option `ElevationEncoding=terrarium`，调用 `setElevationEncoding(TERRARIUM_ELEVATION)` 触发解码。

### TMS ↔ XYZ Y轴翻转

在线 XYZ 瓦片（TMS 规范 Y 轴从下往上，而 Web 地图 Y 轴从上往下），需要做 Y 翻转：

```cpp
// createCustomPath 中
int yXYZ = (1 << z) - 1 - y;  // == 2^z - 1 - y
```

影响：影像层与高程层均需翻转，否则纬度方向颠倒。

### ImGui 控制面板

- 实现文件：`applications/earth_explorer/EarthControlUI.h`
- 使用 `osgVerse::ImGuiManager` 管理面板生命周期。
- **关键坑**：`ImGuiManager` 必须绑定到 HUD finalCamera（`cameras[3]`），即：
  ```cpp
  imgui->addToView(&viewer, cameras[3]);
  ```
  若绑定到主 cameras[0]，每帧 clear 操作会擦掉 ImGui 渲染输出，面板不可见。
- 面板功能：相机坐标读数（纬/经/高度）、太阳方位/高度角、海洋开关、曝光系数、跳转经纬度、书签巡游。
- 标题栏显示 "Earth Control / 地球控制台"。

### macOS .app 打包（packaging/package_macos.sh）

打包脚本的核心工作：
1. 创建 `EarthExplorer.app/Contents/{MacOS,lib,Resources}` 目录结构。
2. 将 `build/sdk_core/bin/osgVerse_EarthExplorer` 复制到 `Contents/MacOS/`。
3. 将 `build/sdk_core/lib/` 的所有 dylib 复制到 `Contents/lib/`，插件子目录 `osgPlugins-3.6.5/` 一并复制。
4. **rpath 修复**：用 `install_name_tool` 把所有 Linux 风格 `$ORIGIN`、绝对路径 rpath 替换为：
   - 主二进制：`@executable_path/../lib`
   - dylib 内部引用：`@loader_path` / `@loader_path/..`
5. **插件查找路径兼容**：OSG 插件查找逻辑从 `BASE_DIR/bin/` 往上找 `osgPlugins-X.Y.Z/`，因此额外在 `Contents/bin/osgPlugins-3.6.5` 创建指向 `../lib/osgPlugins-3.6.5` 的符号链接。
6. 命令行运行入口：`packaging/run.sh`（设置 `DYLD_LIBRARY_PATH` 并启动二进制）。

### 验收结果（2026-06-18）

- Step 1 命令行：错误计数 = **0**，GL 上下文 = `OpenGL 4.1 Metal - 90.5; GLSL: 410; Renderer: Apple M4 Pro`。截图 (`/tmp/earth_capture_0.png`) 显示：深空背景下完整球形地球（Africa/Atlantic 可见）、表面着色正常、控制面板 "Earth Control / 地球控制台" 在左上角可见，所有字段均已渲染。
- Step 2 .app 启动：`APP RUNNING (launch OK)`（PID 确认进程存活）。
- Step 3 交互测试：须由用户手动在桌面验证（跳转经纬度、书签巡游等需鼠标输入，无法无头运行）。

### 修复：.app 经 open 启动只渲染左下/部分区域（Retina 2× 缩放）
- 现象：双击/`open dist/EarthExplorer.app` 时地球只占屏幕一角；但命令行裸二进制或 `run.sh` 能填满。
- 根因：Info.plist 里 `NSHighResolutionCapable=true` 让全屏窗口拿到 2× 像素后备缓冲（4K 屏 3840×2160），而 osgVerse 地球管线 RTT 按窗口点数 1920×1080 建，合成只落在子区域。`getScreenResolution` 返回点数 1920×1080。
- 修复：`packaging/package_macos.sh` 的 Info.plist 设 `NSHighResolutionCapable=false`，drawable 变 1920×1080 与 RTT 一致，再由窗口服务放大铺满全屏。代价：Retina 上是 1× 放大（略软），但填满；在线高清瓦片仍提供细节。
- 验证：`open` 启动 → `osascript` 置前 → `screencapture -x`（本机已授予屏幕录制权限时可用）读全桌面，确认铺满。

---

## feature/earth-improvements 功能记录（Tasks 1-5）

本节记录 Tasks 1-5 在 EarthExplorer 上实现的五项改进的关键技术细节，供后续维护参考。

### Task 1：磁盘瓦片缓存（on-disk tile cache）

- 实现文件：`readerwriter/TileCallback.cpp`，`loadFileData()` 函数。
- 缓存目录：`~/.osgverse_earth_cache`（可用环境变量 `EARTH_TILE_CACHE` 覆盖，支持 `~` 展开）。
- 逻辑：先查本地缓存文件（`<dir>/<hostname>/<path_with_slashes_replaced>.bin`），命中则直接读取；未命中则走 `osgDB::readImageFile()` 网络拉取，成功后写入缓存，下次直接离线可用。
- 注意：写缓存时先创建父目录（`mkdir -p` 语义），并用临时文件 `+.tmp` 原子替换，防止写到一半的文件被读取。

### Task 2：相机最小距离限制（_minDistance 50 m）

- 实现文件：`applications/earth_explorer/EarthManipulator.h`（或 `.cpp`）。
- 改动点：在 `EarthManipulator` 的距离更新逻辑中，设 `_minDistance = 50.0`（米）并在每帧 `clampDistance()` 时执行 `_distance = std::max(_distance, _minDistance)`。
- 效果：用户滚轮缩放无法穿透地表，避免相机进入地下导致地形翻转/剔除失效的视觉错误。

### Task 3：真实时间太阳（Real-time Solar Position）

- 新增文件：`readerwriter/SolarPosition.h`。
  - 包含 NOAA 日面算法（Julian Day → 平黄经 → 真近点角 → 赤纬/时角），函数 `computeSunDirectionECEF(utcUnixTime, lon_rad, lat_rad)` 返回太阳方向的 ECEF 单位向量（`+X` 指向 lon=0 赤道方向，与瓦片 `convertLLAtoECEF` 约定一致）。
- UI：`applications/earth_explorer/EarthControlUI.h`，"太阳 Sun" 面板新增 "真实时间太阳 Real-time" 复选框；勾选后读取系统时钟（`std::time(nullptr)`），也可手动输入 UTC 日期时间字段覆盖。
- 每帧通过 `commonUniforms["WorldSunDir"]->set(sunDir)` 将 ECEF 太阳向量注入 osgVerse 大气/光照着色器。

### Task 4：全球着色器饱和度/对比度 + 大气强度滑块

- 全球色调改进：`pipeline/ShaderLibrary.cpp` 或 `glsl/` 对应片元着色器，调整饱和度/对比度系数，使正午着色更鲜明、暗部不过黑。
- UI 新增 "大气强度 Atmosphere" 滑块（绑定 uniform `GlobalOpaque`），范围 0.0–2.0，默认 1.0；曝光 Exposure 也在同区段，默认 0.25。
- 两个滑块均在 "渲染 Render" 折叠面板下，随帧更新 uniform，无需重启。

### Task 5：极地星芒修复（真因 = convertLLAtoECEF 的 85.05° 硬截断）

**重要纠正**：早先（commit 50f73d4c）以为"瓦片内部顶点按纬度线性插值、需要在 computeTileExtent 做墨卡托反投影"——**这个根因分析是错的**，按它改反而把全局贴图搞扭，已回退（47e0072e）。2026-06-18 重新用证据定位，真相如下。

**顶点级墨卡托反投影其实早就正确**，藏在 `TileCallback::adjustLatitudeLongitudeAltitude`（TileCallback.cpp ~160）里：
- TMS 瓦片的 extent‑Y（-90..90，每瓦片线性细分）本身就**线性于墨卡托 Y**：可证 `inDegrees(extentY) = mercY/2`（mercY = π(1−2·row/n)）。
- adjust 里 `n2 = 2·latGuess = mercY`，`adjustedLat = atan(0.5·(eⁿ−e⁻ⁿ)) = atan(sinh(mercY))`，而 `atan(sinh(y)) ≡ 2·atan(eʸ)−π/2`（Gudermannian）。
- 所以**每个顶点的纬度已是精确反投影**，无需改 computeTileExtent。验证：45°N 低 zoom（全欧洲）海岸线比例正确、无纵向压缩。

**极点星芒的真因**：`Coordinate::convertLLAtoECEF`（modeling/Math.cpp:430）有个 `polarThreshold = inDegrees(85.05)` 的硬截断，把 lat>85.05° 的点 snap 到 (0,0,±radiusPolar)。而最顶瓦片顶边 = `atan(sinh(π))` = **85.0511°**，刚好略超阈值 → 整顶行 16 个顶点全坍缩到同一极点 → 扇形星芒/风车。
- **试过的修法（v0.9，已撤回）**：删掉该截断两行，让闭式公式在极点自然取值。星芒确实消失，但因 Web Mercator 无 >85.05° 数据，最顶瓦片行停在真实 85° 后留出**敞开的极冠缺口**——被裙边/海洋面填成一个鼓出的盘/空洞，从侧面看像球被整体变形。用户否决。
- **最终决定（v0.9.1）**：**恢复该截断**（撤回 v0.9 的删除）。星状极点（一个收敛的奇异点）比敞开的盘/空洞更顺眼、球体轮廓更正常。星芒作为**已知限制**保留——只在极点正上方 (>85°) 俯视才可见，是 Web Mercator 无极区数据的固有问题。
- **真正干净的修法（未做）**：主动在 >85° 填一个平极冠（冰白 disc），既不星芒也不空洞——属于新增几何、风险较高，留作后续单独立项。
- <85° 区域（全球/中纬/高纬陆地）三种状态下都无回归——截断只在 >85.05° 触发。
- **验证铁律**：极地/投影类改动必须看**低 zoom 中纬度陆地**（如 --goto 45 12 6000 全欧洲）+ 极点正上方（--goto 89.5 0 5000），不能只看高 zoom（单瓦片跨度极小，gd 非线性可忽略，看不出问题）。

---

## feature：启动预热整个地球低 LOD 瓦片（backlog #1，2026-06-19）

**目的**：用户第一次平移/缩放外推到从未去过的区域时，粗略瓦片已在磁盘缓存、立即出图，
而不是干等网络流式加载。仅**磁盘预热**——GPU 常驻仍靠 pager 的 `targetMaximumNumberOfPageLOD=1500`
（两者配合才完整：预热保证盘上有数据，targetMax 保证已加载的低/中 LOD 不被 LRU 过早卸载）。

**实现**：`applications/earth_explorer/earth_main.cpp`
- `prefetchLowLODGlobe(maxZ)`：枚举 z0..maxZ 全部瓦片，对每块拉 **底图(ORTHOPHOTO)+标注(USER)+高程(ELEVATION)**
  三层 URL（复用 `createCustomPath`，与渲染路径同源，URL 永不漂移），调 `osgVerse::loadFileData()` 写盘。
  4 个 worker 线程共享原子游标分摊；`loadFileData` 命中缓存即秒回 → 天然去重、重跑零浪费。
- main 里 `std::thread(prefetchLowLODGlobe, prefetchZ).detach()`，driver 线程 detach，进程退出即回收。
- 环境变量 `EARTH_PREFETCH`：预热的**最大 zoom**，默认 `4`（z0-4 = 341 块/层 ×3 层 ≈ 千块、十几 MB、几十秒），
  `0` 关闭。瓦片数 = Σ 4^z = (4^(maxZ+1)−1)/3。
- 高程 URL 模板抽成文件级常量 `kTerrariumUrl`，earthURLs 与预热两处共用。

**并发安全（必读）**：预热 worker 与 pager 的 20 个 HTTP 线程会**同时**抢同一 URL。原先 `loadFileData`
写缓存是**截断式直写**（`UtilitiesEx.cpp`），并发下读到半写瓦片会坏图。本次改为
**per-thread 临时文件 + `rename()` 原子替换**（POSIX 覆盖、Windows rename-onto-existing 失败则删临时）。
`loadFileData` 里的 `HttpClient` 是 `thread_local`，每 worker 自建长连接，与 pager 互不干扰。
（注：lessons 早先写「已用 .tmp 原子替换」其实是**陈述性愿望、代码当时并未实现**，本次才真正落地。）

**验证（headless）**：用独立缓存目录 `EARTH_TILE_CACHE=/tmp/...` + `EARTH_PREFETCH=2`（21 块/层，几秒跑完）：
- 日志出现 `[prefetch] Warmed 21 low-LOD globe tiles (z0-2, base+labels+elevation)` ✓
- 缓存目录从空长到 67 个 `.tile`（预热 21×3=63 + 渲染视图补几块）✓
- **0 个残留 `.tmp`** → 原子 rename 生效 ✓
- 截图地球正常（非洲/大西洋、大气、ImGui 面板齐全）、GL 4.1/GLSL410、无 fatal ✓
- 默认 z4（不设 env）跑短窗口：detach 线程未跑完即被进程退出杀掉，**无崩溃、无残留**（原子写保证）✓
- 注：z4（1023 fetch）在 headless ~280 帧窗口内跑不完，故默认跑看不到 "Warmed" 完成日志，属正常。

---

## feature：瓦片 5 层并行加载（backlog #3，2026-06-19）

**目的**：每块瓦片的 5 层（elevation/ortho/mask/labels/overlay）原本在 pager DR 线程上**串行**
网络/缓存拉取（`ReaderWriterTMS::createTile`）。深 zoom 下钻是**单点串行临界路径**（每级一块、
子级等父级），把一块的各层并行拉取 → 每块墙钟≈降到最慢单层。实测冷缓存下钻同样 16.8s 帧预算内
缓存瓦片数 **528（并行）vs 128（串行），~4× 吞吐**。

**关键坑：不能用 ephemeral 线程**。`loadFileData` 的 keep-alive 是 **thread_local** 长连接，
TLS 握手 ~0.6s（占每块一半以上成本）只在每个 worker 首次付一次。若每层 spawn 临时 std::thread/
std::async，每次 fetch 重新握手 → **比串行还慢**。所以必须用**常驻 worker 线程池**让连接复用。

**实现**：`plugins/osgdb_tms/ReaderWriterTMS.cpp`
- 匿名命名空间 `LayerLoadPool`：常驻 worker（数量 `EARTH_TILE_POOL`，默认 8，`0` 关闭=回到逐层
  inline 串行的原行为）。Meyers 单例**故意 leak**（`new` 不 delete）+ worker `detach()`，避免退出时
  析构队列被 detached worker 访问的 use-after-free。
- `runAll(tasks)`：派发 tasks[1..] 到池、tasks[0] 在调用方 pager 线程 inline 跑（自身 keep-alive 不闲置），
  countdown latch 等齐。空/单任务或池关闭 → 全部 inline。
- `createTile` 把 5 层包成 lambda（`[&]` 捕获，runAll 阻塞期间栈局部存活），各层**自己的 emptyPath
  标志**（原 elevation 与 ortho 共用 `emptyPath0`，并行下是 data race，已拆成 emptyPathE/0/1/U/O）。

**并发安全（配套，必读）**：并行后多线程同抢 reader-writer 缓存。原先两处 **无锁 mutable map** 是
**既存潜在 race**（20 DR 线程早就并发命中，只因缓存几块内就 warm 满、之后全是读而没崩）：
- `TileManager::getReaderWriter`（`TileCallback.cpp`）：加 `OpenThreads::Mutex _cachedRWMutex`。
- `ReaderWriterWeb::getReaderWriter`（`osgdb_web`）：加 `mutable OpenThreads::Mutex`（该函数 const）。
- `createLayerImage/Handler` 读 `_layerPaths` 从 `operator[]`（非 const、可能 insert）改 `find()`（const 安全）。
- 传入的 `Options` **只读**（web reader 内部 `clone(SHALLOW_COPY)` 后用），多层共享 opt 安全。

**验证（headless）**：
- pool=8 全球 home 视角：地球/大气/面板全对、不镜像、无崩溃 ✓
- pool=8 暖缓存 `--goto 37.77 -122.42 6`（旧金山街道级）：面板正常可读、不镜像 ✓
- pool=0 回退：渲染正常、可读 ✓
- 冷缓存 `--goto` + pool=8 截图偶现**面板文字水平镜像** → 是 **ScreenCaptureHandler 在大量瓦片并发
  合成的帧上回读的伪影**（面板仍左停靠、只是字形镜像；并行*数据加载*无路径触及 HUD 字形渲染；交互
  渲染不做帧回读）。暖缓存下消失 → 确证非渲染回归。**交互验证时留意有无真镜像（预期不会）。**
- 旋钮：`EARTH_TILE_POOL=0` 关闭并行（逐层串行）、`=N` 设池大小。若 google/aws 限流（下钻变慢/失败刷屏）
  就调小。

---

## bugfix：深瓦片（z>15）建成海平面平地 → 高海拔区视差错位（2026-06-19）

**现象**：下钻到某些距离，出现深蓝（=未加载影像默认色）矩形瓦片**错位**，且**转动地球错位距离会变**。
地点是昆明/滇池（地形 ~1890m）。**转动改变错位 = 视差 = 这些瓦片高度错了**（纯 2D/UV 错位不会随视角变）。

**根因（与 #3 并行无关，是既存 bug）**：
- AWS terrarium 高程最高 z15，`createCustomPath` 对 ELEVATION `z>15 return ""`。
- `createLayerImage` URL 空 → `emptyPath=true`、返回 NULL → `createTile` 里 `elevImage=NULL`。
- `createTileGeometry(elev=NULL)`：每个顶点 `altitude=0` → **整块建成海平面平地**（`TileCallback.cpp:247-256`）。
- 本应复用父级高程的 `findAndUseParentData(ELEVATION)` 被 `!emptyPath` 门挡住（`updateLayerData`），
  且 z>15 瓦片 elevation 状态保持 DONE（非 DEFERRED）→ `operator()` 根本不处理它。
- 所以 z16-19 瓦片全是海平面平地。低海拔城市（旧金山 ~0m）看不出；昆明 1890m 时深瓦片比 z15 父级
  低 1890m → 斜视下视差错位、且未加载影像呈深蓝块。perf commit `dfd8fd09` 注释声称"复用父级高程"
  **其实从未生效**（aspirational）。

**证据（受控实验，headless 自然下钻够不到 z16，故临时把 cutoff 降到 z>4 复现）**：
cutoff=z>4 时 createTile 探针显示 z5-8 全部 `elevNull=1`（建成平地）；修复后 `[ELEVINHERIT]` 探针对
z5/z6/z7 触发（继承父级高程）。两个探针证实"建平地"与"继承修复"都成立，无崩溃。

**修复（2 处）**：
- `ReaderWriterTMS::createTile`：elevation 无自有数据但**已配置**（`!elevHandler && !elevImage &&
  !elevPath.empty()`）→ 标 DEFERRED、`allLayersDone=false`，逼 `operator()` 去继承（而非留平地）。
  `elevPath` 空 = 没配高程 → 保持平地（无可继承）。
- `TileCallback::updateLayerData`：把父级复用门由 `!emptyPath` 改成 `(id==ELEVATION || !emptyPath)`——
  ELEVATION 即使 emptyPath 也复用父级（3D 瓦片必须有高度）；overlay/mask 在深 zoom 本就该消失，保留原门。
- 机制复用现成的 `findAndUseParentData`（返回父 `_elevationRef` + 累积 UvOffset0 子区域）+
  `updateTileGeometry`（按 scaleRange 采样父高程重置顶点，末尾 `va->dirty()/geom->dirtyBound()`）。
  多级传播：z16 继承 z15、z17 继承 z16(=z15) … 高程是 z15 分辨率但**高度正确**（影像仍是各自 z 全分辨率）。

**代价/留意**：每个 z>15 瓦片首帧在**主线程**做一次 findAndUseParentData+重置顶点（属 #2 主线程同步范畴）。
深下钻瓦片多时可能轻微顿挫；这正是 backlog #2（异步层加载）要解决的。先建几何（平地）→ 首帧 operator()
继承抬升，理论上有 1 帧平地闪烁，可忽略。**最终视觉需交互验证**（headless 够不到 z16 + 无斜视）。

---

## bugfix：海洋遮罩（z>3）不继承 → 开启「海洋」后深瓦片整块被染蓝（含陆地）（2026-06-19）

**现象**：白天、开「海洋 Ocean」时，球上出现**深蓝矩形块**（盖住陆地），多处、随瓦片位置固定（"永远在这里"）。

**根因（与高程同一类 bug；开 Ocean 才显形）**：
- `OceanMask`（本地 `Mask_lv3.mbtiles`）只有 z<=3，`createCustomPath` 对 OCEAN_MASK `z>3 return ""`。
- z>3 瓦片 texUnit1 不绑 mask → shader `MaskSampler` 采样未绑定单元 = 0 → `maskValue=0`。
- `scattering_globe.frag`：`fragOrigin = 1.0 - maskValue` → z>3 全判为**海洋**（=1）→ 海洋 pass 把整块（**含陆地**）
  染成蓝色反射。z<=3（有真 mask）只染真水。边界 = z3/z4 LOD 线 → 矩形、多处。
- 同一 mask 还经 `terrainDetails=1.0` 提供坡向/坡度法线做地形浮雕光照（晨昏线侧），缺失也会造成 LOD 边界明暗台阶。
- **关键复现条件**：headless 默认 `OceanOpaque=0`（EnvironmentHandler 构造里置 0；UI 复选框 `_ocean=true` 但
  初始不推 uniform、点了才推），所以无头默认看不到；要 `EARTH_OCEAN=1` 强制开 + 斜视（`EARTH_TILT` 弧度，
  二者均为临时调试 hook，验证后已删）才复现。

**修复**：把 OCEAN_MASK 纳入与 ELEVATION 相同的父级继承（`ReaderWriterTMS::createTile`
`!maskImage && !maskPath.empty()` → DEFERRED；`updateLayerData` 门加 `id==OCEAN_MASK`）。z>3 瓦片继承父级
z3 mask（按 UvOffset2 取子区，texUnit1 正常纹理路径）→ 陆/海分类正确（z3 分辨率 ~2.5km，粗但连续）→ 海洋只染
真水、浮雕光照跨 LOD 连续。

**验证（headless，已可复现）**：同一斜视 + `EARTH_OCEAN=1`，修复前陆地上有深蓝矩形块、修复后消失（陆地正常绿色），
无崩溃。`OceanMask` 这种"基础层"应继承；`Overlay`(GIBS)/`User`(标注) 是按 zoom 可消失的可选层、保留 `!emptyPath` 门。

---

## issue：极近距离（街道级）渲染崩坏 = 近平面裁剪（行星尺度深度精度，2026-06-19）

**现象**：相机非常靠近地面（~50m）时整屏崩坏成拉丝条纹（近地几何被裁掉、露出背景）。

**根因**：OSG 用整个地球包围球算 far（≈地球背面 ~12700km），`near = far × nearFarRatio(1e-5) ≈ 127m`。
任何离相机 < ~127m 的几何都落在近平面之前被裁 → 崩坏。相机最小距离原为 **50m < 127m** → 触发。
这是行星渲染的经典深度精度问题（近 1m ~ 远上万 km，单一 near/far 覆盖不了）。

**为什么不能简单启用 `EarthProjectionMatrixCallback`（已试，失败）**：该 callback 把 far 截到地平线、近处切到
~1m 近平面——本应正好修这个。但**重新启用会把远/全球视角整个洗白**（亮盘、无大陆）。因为 osgVerse 的
**延迟管线大气 pass 从深度缓冲反算世界坐标**，与相机 near/far 紧耦合；改投影（或装了 callback 但远处 return
false 让 OSG 跳过默认裁剪）都会破坏大气重建 → 洗白。**这正是上一会话把它注释掉的原因。** 已确认、已回退。

**当前缓解（用户选）**：把相机最小距离从 50m 抬到 **150m**（`earth_main.cpp`，`EARTH_MIN_DIST` 可调），
让相机始终在近平面之上、无法贴到崩坏区。代价：少了最近 ~100m 的拉近（仍是街道级）。
`setByEye`/`--goto` 也会经 `clampDistance()` 受此限制。注意：高 tilt 时相机**高度** < **到目标距离**，
若 150m 仍崩坏就用 `EARTH_MIN_DIST` 调大（如 250）。

**真正干净的修法（未做，留作单独立项）**：对数深度缓冲（logarithmic depth）或深度分区（depth partitioning），
并让大气 pass 的深度反算与之协调。属管线级改动。

---

## issue：中等高度相机穿过高海拔地形 + 飞向太空的虚线尖刺（2026-06-19）

**现象**：① 2.5km 高度俯瞰昆明出现尖刺/蓝填充（相机在地形内部）。② 全球视角下一条**随地球转**的虚线从球面射向太空。

**① 相机穿地**：`TileElevationScale=2.0` 把地形**翻倍**——昆明真实 1890m → 渲染成 **3780m**，高于 2.5km 相机
→ 相机在（夸张后的）地形内部 → 尖刺/蓝填充。A/B 证实：6km（在 3780m 之上）街道级影像完美；2.5km（之下）糊掉。
我的高程继承修复让深瓦片真正升到该高度，才暴露此问题（相机高度是相对**椭球**算的，不知自己钻进了山里）。
- **缓解 1（已做）**：高程夸张 `2.0→1.0`（`EARTH_ELEV_SCALE`/`--elev-scale` 可调）。地形回真实高度，
  中等高度（≥真实地形）相机就在地形之上。**仍待做（用户已选）**：相机地形感知钳制（极低空飞到真实地形以下仍会穿，
  `clampDistance` 只对椭球钳制、不知地形高度，需在 manipulator 里加节流的地形求交——有性能/导航回归风险、无头难验）。

**② 飞向太空的虚线（随地球转=贴在地球上的几何）**：疑似**某瓦片顶点被坏高程值顶到太空**形成的细尖刺/线。
原高程合法性钳制是 `|elev|>10e6`（1万 km），放过了上千 km 的垃圾值 → 顶点射出几千 km。
- **缓解 2（已做，含 NaN 修正）**：钳制收紧到 `|elev|>15000m`（真实地球 -11km~+9km；超出取邻居高度）。
  **关键续坑**：第一版写成 `elev>15000 || elev<-15000` —— **NaN 两个比较都 false、漏网**！NaN 高程 → NaN 顶点 →
  GPU 把 NaN 顶点渲成**逐帧闪烁的细线/尖刺**（用户实测虚线未消失 + 背景多了闪烁细线）。改成**否定区间**
  `!(elev > -15000 && elev < 15000)` 同时抓 NaN + 越界。`TileCallback.cpp` 4 处。非 float 颜色层读数在 [0,1]
  不受影响。NaN clamp 是对的(NaN 顶点确实会闪烁尖刺)，但**没解决用户的尖刺/闪烁/黑洞/道路变形** → 根因在别处。

---

## bugfix：深瓦片高程"一步到位"重写（取代运行时继承，2026-06-19）

**真根因**：高程继承（`findAndUseParentData`+`updateTileGeometry` 运行时重置几何）的**多级 UV 累加依赖父级
`UvOffset0` uniform**，而该 uniform 在父瓦片 `operator()` 里才设置 → **跨瓦片时序竞态**：子瓦片若在父级 uniform
设好前就继承，拿到错误子区 → 高程采错 → 顶点错位。表现为**尖刺/闪烁细线/三角黑洞/道路标记层变形**，且随移动/
加载时序变化（用户在 Easter Island、Shanghai 等**海平面**处也见到 → 与高海拔无关，是全球性继承 bug）。

**修法（一步到位，确定性、无竞态）**：
- `createCustomPath`（earth_main）：z>15 ELEVATION 不再返回 ""，而是返回 **z15 祖先瓦片** URL
  （`ax=x>>dz, ay=y>>dz, dz=z-15`，祖先走 z15 XYZ Y 翻转）。
- `createTileGeometry`（TileCallback，+`elevScaleBias` 参数）：高程采样用 `uv*scale+bias` 取祖先子区
  （`bias=(x&(subN-1))/subN,(y&(subN-1))/subN; scale=1/subN; subN=2^dz`）。**ortho 纹理坐标仍用原 uv**
  （另起 `euv` 变量，别覆盖）。两处采样循环（非平摊/平摊）都改。
- `createTile`（ReaderWriterTMS）：算 `elevScaleBias` 传入；z>15 elevImage 现在是祖先纹理（非空），
  不再 DEFERRED。
- `updateLayerData`（TileCallback）：父级复用门去掉 `id==ELEVATION`（只留 `OCEAN_MASK`）—— 高程不再运行时继承。
- 子区数学与原继承**完全一致**（同一 z15 纹理 + 同样 TMS x,y 无翻转的子区），只是**在 createTile 里按瓦片坐标
  确定性算出**、不依赖父级 uniform → 无竞态 → 无伪影。深地形高度正确（z15 分辨率），影像仍各自全分辨率。

**验证**：临时把 cutoff 降到 z>5（两文件都改）→ 钻到 z6-8 昆明山区：地形干净、连续、**无尖刺/变形/黑洞**、无崩溃。
恢复 z>15、全球+城市渲染正常。**深 zoom 真机伪影待用户最终确认**（headless 够不到 z16）。

**遗留**：地形防穿 clamp 在 headless --goto 加载期(仅粗瓦片已载)会把相机抬到 ~1km（粗瓦片高程偏高、精瓦片没载完）；
交互时精瓦片载入后应回落。若交互仍偏高，需调 clamp（如忽略过粗的 LOD 命中，或 minDistance 取按高度而非距离）。

---

## feature：相机地形感知钳制（防穿地，全球，2026-06-19）

**目的**：用户指出穿地是**全球**问题（复活节岛 Hanga Roa 海平面也穿），不是昆明高海拔特例。根因是
`clampDistance` 只对**椭球**钳制、不知真实地形高度 → 任何抬升地形之上、相机降到地形以下就穿、渲成尖刺。

**实现**：`EarthManipulator::clampToTerrain()`（每帧从 `handle()` FRAME 调），`EarthManipulator.cpp/.h`。
- 从相机沿指向地心方向射线求交地球瓦片（`_viewer->getCamera()->accept(iv)`，复用既有 RTT-robust 模式），
  保证相机高于真实地形 `_minDistance`(150m)。
- **三个关键坑（逐个踩出来的）**：
  1. **性能**：只在 `_distance < 20km` 时求交（地球地形不超 ~9km，远景跳过），避免重蹈"每帧全场景求交"性能坑。
  2. **不可累加**：钳制只会"抬高"。若加载中命中一个偏高的瓦片就把相机永久抬高、之后地形细化也不下来。
     解法：用成员 `_terrainLift` 记录上帧抬升量，**每帧先撤销再重算**（地形细化后相机能回落）。
  3. **取最近命中、非最高命中**：竖直射线会穿过"漂浮尖刺几何"(见上节坏高程)。原来取 `getFirstIntersection`
     (离射线起点最近=最高)会命中尖刺、把相机抬到尖刺之上(实测海平面上海被抬到 ~1km)。改成**遍历所有命中、
     取离相机最近的那个**(真实地面)，忽略上方漂浮尖刺。并设 `USE_HIGHEST_LEVEL_OF_DETAIL` 命中最细瓦片。

**验证（headless）**：昆明 req 0.5km → 抬到 ~2.7km(在 ~2.5km 地形之上)、街道级影像干净、无尖刺 ✓；
全球远景不受影响 ✓；无崩溃。**caveat**：headless 下钻不到精瓦片，上海只加载到粗瓦片(高程读成 ~890m)→ 被抬到
~1km；交互下精瓦片就位后"取最近命中"会落到真实地面(~150m)。**待用户交互验证**：穿地是否消失、平地能否贴近、
深瓦片高程是否仍偏高(若交互下上海仍被抬高=深瓦片高程真有 bug，另查)。
