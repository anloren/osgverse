# EarthExplorer 性能/体验优化 Plan（自主执行）

分支 `feature/earth-perf`（基于 P1）。目标：消除导航卡顿、让缓存真正生效、鼠标顺滑。每步自检达标才提交；始终保持可构建可运行；搞不定就回退该步、记为待办，不留破损。最后合并 master + 重打包 .app + 写结果报告。

构建：`cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`

## 根因（已用代码定位，见会话）
- ① 主线程同步网络：`TileCallback::operator()`(update=主线程) 在 `_layersDone=false`/图层切换时 `readImage()` 阻塞下载。异步 `requestImageFile` 被注释。
- ② 每次鼠标操作同步场景求交：`EarthManipulator::calcIntersectPoint` 跑 `IntersectionVisitor` 全遍历。
- ③ 每瓦片顺序下 5 层；16 pager 线程。
- ④ 缓存仅 ~4MB（`FileCache("earth_cache")` 默认 blockSize=1024×blockCount=4096）、相对路径 → 深 zoom 即被挤空。

## 步骤（goal + 自检）

### Step 1 — 缓存扩容 + 绝对路径【安全/高收益】
- 目标：缓存 ~512MB 于 `$HOME/.osgverse_earth_cache`（env `EARTH_TILE_CACHE` 覆盖路径）。
- 改 `earth_main.cpp`：`FileCache(path, blocks=65536, size=8192)`（=512MB；simdb 大值跨块，故仅需提总量）。
- **自检**：删旧缓存→跑 `--goto 39.9 116.4 300`→确认缓存文件在绝对路径生成且增长；同区再跑一次，临时 cache-hit 计数证明二次基本命中。截图全球无回归。
- 状态：⬜

### Step 2 — 层加载移出主线程【中风险/高收益=消卡死】
- 目标：`operator()` 不再主线程阻塞下载。后台线程加载 deferred/切换层，就绪帧再应用。
- **自检**：插桩确认 update 线程不再调 `readImage`（或耗时<5ms）；深 zoom 截图无回归；导航不崩。Step1 缓存已让重访变快，本步消除首访卡死。
- 状态：⬜（若反复搞不定→回退，靠 Step1 缓解 + 记待办）

### Step 3 — 求交优化【中风险】
- 目标：`calcIntersectPoint` 改解析 ray–椭球求交（或节流），去掉每次鼠标的场景遍历。
- **自检**：`--goto`/`setByEye` + 截图相机定位正确、不崩；标注"鼠标手感待交互确认"。
- 状态：⬜（不稳→回退）

### Step 4/5 — 暂缓：吞吐(③，1&2 后已缓解)、中国 GCJ-02（仅中国、风险高，已知限制）。

## 收尾
- 全部验证后：合并 `feature/earth-perf`→master、push（同步，不 release）、重打包 .app、写结果报告、更新 HANDOFF。
- 上下文 ~95% 前：把进度/剩余写回本文件再继续。

## 进度日志
- **Step 1 缓存：结论=本来就好，无需修。** 真缓存在 `loadFileData`(UtilitiesEx.cpp:496-609)：per-tile `.tile` 文件、`$HOME/.osgverse_earth_cache`(绝对、env 可覆盖)、无限大小。实测 run1 缓存 8537 块、run2 同视角新增 0、0 网络 → 完美命中。先前以为的 simdb FileCache 不用于 HTTP 瓦片；我误把它指到瓦片目录致崩溃，已 `git checkout` 回退。用户"缓存没用"实为卡顿错觉。
- **重排优先级**：缓存非问题 → 主攻卡顿。Step 3(求交) 优先；Step 2 仅做安全缓解。
- **Step 3 求交：完成**(commit 99d3f223)。`calcIntersectPoint` 改解析 ray-椭球，去掉每次鼠标的全场景遍历。构建/渲染/无崩溃验证；手感待交互确认。
- **Step 2 层加载：安全部分完成**(commit dfd8fd09)。elevation z>15 截断——深zoom的 AWS terrarium z16-19 必 404→主线程 DEFERRED 重试阻塞(近地卡死主因)+刷屏，已消除(z16-19 404 实测归零)，复用父级 z15 高程不丢地形。**完整异步线程化(后台 thread pool 加载 deferred 层) 未做**：高风险、headless 无法验 UX，留作交互会话。
- **结论**：本轮交付 2 个已验证的卡顿改善(鼠标求交 + 近地 elevation 404)；缓存查明本就好。其余(完整异步、吞吐并行、中国GCJ-02)留待后续。
