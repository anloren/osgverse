# EarthExplorer 回退功能分步重做 — 设计文档

**日期**:2026-06-21
**背景**:2026-06-20 会话因"越改越多"被整体回退到 `0f902837`,把整条 6-19/6-20 深瓦片/性能线全带走。
其后已在干净基线上单独重做并验证了**深瓦片高程一步烘焙**(48e38efd)与**相机地形地板**(b5976ac7)。
本文档规划把**其余仍缺失、确有价值**的回退功能逐步做回来。

---

## 1. 范围与现状(已对照当前 master 逐条核实)

### A. 已在 master(只验证,不重做)
- 深瓦片高程"一步烘焙"(z>15 取 z15 祖先 + `elevScaleBias`)— `ReaderWriterTMS.cpp:248`、`earth_main.cpp:205`。
- 相机地形地板 `_terrainLift`(每帧竖直射线测地形、抬眼睛、可回落)— `EarthManipulator.cpp:701-724`。

### B. 确认不在 master、本轮重做(稳健优先、风险递增)
| # | 功能 | 现状证据 | 风险 |
|---|------|----------|------|
| 1 | 最小距离防近平面裁剪 | `_minDistance=50` < near≈127m(`setNearFarRatio(1e-5)` + `EarthProjectionMatrixCallback` 注释,`earth_main.cpp:287,312`) | 低 |
| 2 | 高程感知裙边 | 固定 `skirtRatio="0.05"`(`earth_main.cpp:250`) | 低-中 |
| 3 | 海洋遮罩深瓦片继承(**可选/低优先**) | 继承门 `!emptyPath`(`TileCallback.cpp:575`)→ z>3 mask 路径空→不继承;`createTile:235` DEFERRED 同因不设 | 低-中 |
| 4 | 启动磁盘预热 | grep 无 `prefetchLowLODGlobe`/`EARTH_PREFETCH` | 中(并发) |
| 5 | 5 层并行加载池 | grep 无 `LayerLoadPool`/`EARTH_TILE_POOL` | 高(并发) |

### C. 永不重做(2026-06-20 那 4 类回归的根源)
1. 默认太阳翻面 → 整片海洋橙红 + 撒哈拉过曝白。
2. fullbright"直接显示影像"hack → 删掉晨昏线(**用户要保留晨昏线**)。
3. 倾斜穿模(相机地板只射正下方、漏旁边更高地形)— 已被 A 组的 `_terrainLift`(取最近命中)替代,勿回旧法。
4. `OceanOpaque` 默认开 → 程序化光滑水面(**原本从不显示**)。

### D. 本轮不纳入(各自独立立项)
完整异步层加载、对数深度缓冲(near-plane 根治)、中国 GCJ-02 校正、P2 雷达/地震/航班点要素。

---

## 2. 贯穿原则(铁律)
1. **一步一项**:改完当步立即 headless 验证 + 用户交互确认,**确认不引入 C 组 4 类回归**再进下一步。
2. **每项带 env 旋钮**,`0`/原值=回到原行为,秒级回退、不重编。
3. **每项单独 commit**,不捆绑;改渲染前先 curl 真瓦片排除数据问题。
4. **复现用确切条件**(高度段/开关/太阳),别乱缩放截图。headless 验证用既有钩子(`EARTH_AUTOCAP` / `EARTH_FRAME_SLEEP_MS` / `--goto` / `EARTH_TILT` / `EARTH_SUN_TO_CAMERA` / 新增 `EARTH_CLOUDS`)。
5. headless 够不到 z16、且复现不出斜视穿模 → 深瓦片/穿模类**必须交互验证**。

---

## 3. 分步设计

### 步骤 1 — 最小距离防近平面裁剪(B#1)
- **目标**:海平面街道级拉近不再裁崩成拉丝条纹。
- **根因**:OSG 用整个地球包围球算 far(~12700km),`near=far×1e-5≈127m`;`_minDistance=50<127` → 相机贴到 50m 时近地几何落在近平面前被裁。`EarthProjectionMatrixCallback` 重启会洗白远景大气(延迟管线大气 pass 与 near/far 紧耦合),故不走它 → 本步是 band-aid,根治(对数深度)属 D 组。
- **改法**:`_minDistance` 50→**150**;加 `EARTH_MIN_DIST` env 覆盖。`setByEye`/`--goto` 经 `clampDistance()` 一并受限。
- **文件**:`EarthManipulator.cpp/.h`、`earth_main.cpp`。
- **验证**:交互沿海城市低空拉到最近,无裁崩;远景/全球视图不受影响。高 tilt 时若 150 仍崩,`EARTH_MIN_DIST` 调大(如 250)。
- **回退**:`EARTH_MIN_DIST=50`。
- **回归守卫**:不碰投影回调/大气;只动相机距离下限。

### 步骤 2 — 高程感知裙边(B#2)
- **目标**:消残留"陡坡过度细分 → LOD 边界小透缝"。
- **改法**:瓦片裙边深度由固定 `0.05` 改为**按本瓦片局部高程范围(max-min)自适应**:高差大→裙边深,刚好遮住与相邻 LOD 的高度落差;平地→浅,避免外露。建几何时算。
- **文件**:`TileCallback.cpp`(建几何/裙边)、`earth_main.cpp`(`--skirt` 默认语义)。
- **验证**:陡坡(昆明西山/滇池岸)交互下钻,LOD 边界无透缝;平地裙边不外露(保持 `isSkirt<-0.1 && GlobalOpaque<0.9 → discard` 逻辑不变)。
- **回退**:`--skirt 0.05` 恢复固定。
- **回归守卫**:只改裙边深度计算;不动顶点高程烘焙、不动 discard 条件。

### 步骤 3 — 海洋遮罩深瓦片继承(B#3,**可选/低优先**)
- **目标**:开「海洋」且下钻 z>3 时,深层**陆地不再被染蓝**(z>3 无 mask → `maskValue=0` → 全判海洋 → `OceanOpaque>0` 时染蓝盖陆地)。
- **改法(与高程一致用"祖先烘焙",不用运行时继承)**:运行时继承的多级 UvOffset 跨瓦片竞态正是当初尖刺/黑洞根源,已被高程一步烘焙取代;OCEAN_MASK 同样:z>3 取 z3 **祖先瓦片**,`createTile` 算 mask 的 scale/bias、采样走 `UvOffset2` 子区,texUnit1 正常绑定。z3 分辨率(~2.5km)粗但连续。
- **取舍**:仅在 Ocean 实际着色(`OceanOpaque>0`)+ 深 zoom 才显形;默认 `OceanOpaque=0` 不显 → 优先级低,**可在执行时决定跳过或后置**。
- **文件**:`earth_main.cpp`(`createCustomPath` OCEAN_MASK 分支)、`ReaderWriterTMS.cpp`(mask scaleBias)、`TileCallback.cpp`(mask 采样)。
- **验证**:`EARTH_OCEAN=1` 强制开 + 斜视 + 深瓦片,修复前陆地深蓝块、修复后消失、陆地正常。
- **回退**:OCEAN_MASK 保留 z>3 cutoff 即回原状。
- **回归守卫**:不动 `OceanOpaque` 默认值;不碰太阳/scattering。

### 步骤 4 — 启动磁盘预热(B#4,中风险·并发)
- **目标**:首次平移到新区域立即出粗瓦片,不干等网络流式。仅磁盘预热(GPU 常驻仍靠 `targetMaximumNumberOfPageLOD=1500`)。
- **改法**:`prefetchLowLODGlobe(maxZ)` 枚举 z0..maxZ,每块拉 底图+标注+高程 三层(复用 `createCustomPath`,URL 与渲染同源永不漂移),调 `loadFileData()` 写盘;4 worker 共享原子游标分摊、命中缓存秒回天然去重;main 里 detach 后台线程。`EARTH_PREFETCH`=最大 zoom(默认 4,0=关)。
- **配套并发安全(必做)**:`loadFileData` 写缓存由截断直写改 **per-thread 临时文件 + `rename()` 原子替换**(预热 worker 与 pager 20 HTTP 线程会同抢同一 URL,半写瓦片会坏图)。
- **文件**:`earth_main.cpp`、`UtilitiesEx.cpp`。
- **验证**:独立缓存目录 `EARTH_TILE_CACHE=/tmp/...` + `EARTH_PREFETCH=2`,日志 `Warmed N tiles`、目录增长、**0 个残留 `.tmp`**、地球/大气/面板正常、无 fatal。
- **回退**:`EARTH_PREFETCH=0`。
- **回归守卫**:预热只写盘、不改渲染路径;原子写不改 `loadFileData` 读语义。

### 步骤 5 — 5 层并行加载池(B#5,最高风险·并发)
- **目标**:每块瓦片 5 层(elevation/ortho/mask/labels/overlay)由 DR 线程串行拉取改并行,深下钻 ~4× 吞吐。
- **改法**:常驻 keep-alive 线程池 `LayerLoadPool`(`EARTH_TILE_POOL` 默认 8,0=关=原逐层串行)。**绝不用临时线程**(`loadFileData` keep-alive 是 thread_local 长连接,TLS 握手 ~0.6s 只首次付;临时线程每次重握手反而更慢)。Meyers 单例故意 leak + worker detach,避退出 use-after-free。`runAll`:tasks[0] 在调用方 inline、其余派发、countdown 等齐;空/单任务/池关 → 全 inline。5 层各自 emptyPath 标志(原 elev/ortho 共用 `emptyPath0` 并行下是 race,拆开)。
- **配套并发安全(必做)**:两处无锁 mutable map 加锁 — `TileManager::getReaderWriter`(`TileCallback.cpp`)+ `ReaderWriterWeb::getReaderWriter`(`osgdb_web`);`createLayerImage` 读 `_layerPaths` 由 `operator[]` 改 `find()`。
- **文件**:`ReaderWriterTMS.cpp`、`TileCallback.cpp`、`plugins/osgdb_web/ReaderWriterWeb.cpp`。
- **验证**:`EARTH_TILE_POOL=8` 全球 home + 街道级 `--goto`:渲染正常、不镜像、无崩溃;`=0` 回退正常;对比同帧预算内缓存瓦片数提速。注:冷缓存 headless 截图偶现面板字形镜像=`ScreenCaptureHandler` 并发帧回读伪影、非渲染回归(暖缓存消失)。
- **回退**:`EARTH_TILE_POOL=0`。
- **回归守卫**:并行只触及数据加载,不触及 HUD/着色器/太阳/海洋。

---

## 4. 不在本轮范围
- 对数深度缓冲(near-plane 根治)、完整异步层加载、中国 GCJ-02、P2 实时数据图层 — 各自独立立项。
- 不重打包 `.app`、不发 Release(发布须用户显式确认)。
- GIBS→VIIRS 修复(本会话已单独完成,未提交)与本计划无关,独立处理。

## 5. 完成判据
B 组各步均:env 旋钮在、headless 验证项通过、用户交互确认无 4 类回归、单独 commit。步骤 3 视用户取舍可跳过。
