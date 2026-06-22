# EarthExplorer 实时数据叠加:地震 + 降水(设计文档)

> 日期 2026-06-22。把真正的实时全球数据接进 EarthExplorer。当前 GIBS 只是「今天-2天」每日快照、运行中不刷新;本设计引入**定时自动刷新的真·实时数据**。
> **分两步实施**:Step 1 地震(本文档完整规格,先实现并验证)→ Step 2 降水(本文档记录设计,Step 1 验证通过后再做)。

## 背景与约束

- 现有叠加能力:globe 着色器 `scattering_globe.frag.glsl` 仅有**两个**叠加贴图槽 —— `ExtraLayerSampler`(路网/地名,`LabelOpacity`)与 `Overlay2Sampler`(GIBS 云图,`Overlay2Opacity`),**已占满**。
- 点数据(地震)走 ECEF 点标记,**不经过 globe 着色器**,完全在那 4 类回归雷区(海洋橙、删晨昏线、倾斜穿模、程序化海洋)之外。
- 已有轮子:`3rdparty/libhv`(`hv::HttpClient`,web 插件在用,支持 HTTPS/mbedtls)做后台 HTTP GET;`picojson.h`(`render_effects.cpp` 在用)解析 JSON;`city_data.cpp` 提供后台线程 + ECEF marker + `convertLLAtoECEF` + `GUIEventHandler` 拾取的成熟范式可仿。
- 铁律:一次只改一个、当场用真实复现条件验证、确认不引入 4 类回归再继续。

## 决策(已与用户确认)

| 项 | 决定 |
|---|---|
| 实施顺序 | 分两步:先地震,再降水 |
| 刷新模式 | 定时自动刷新(真·实时):地震 ~60s,降水 ~5–10min |
| 地震数据范围 | USGS 过去 1 天 · M2.5+ |
| 降水叠加方式 | 复用云图的同一个 OVERLAY 槽(零着色器改动,云图/降水二选一) |
| 点击详情 | 需要(ImGui 卡片) |

---

# Step 1:地震层(完整规格 — 先实现)

## 数据源

- USGS GeoJSON feed:`https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day.geojson`
  - M2.5+、过去 24h、**无需 API key**、约每分钟更新、HTTPS。
- 用 `picojson` 解析。FeatureCollection,每个 feature:
  - `geometry.coordinates = [经度, 纬度, 深度km]`
  - `properties.mag`(震级)、`properties.place`(地名)、`properties.time`(毫秒 epoch)、`properties.url`(USGS 事件页)

## 模块隔离

新建 `applications/earth_explorer/quake_data.cpp`,仿 `city_data.cpp`。对外只暴露:

```cpp
extern osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                                     osgVerse::EarthAtmosphereOcean& earthRenderingUtils,
                                     const std::string& mainFolder);
```

返回一个可开关的 `osg::Group`(quake root),挂到 `sceneCamera`(与 `configureCityData` 同位置)。模块自带:抓取线程、解析、几何重建、点击拾取。**不碰 globe 着色器/太阳/海洋。**

内部数据结构:
```cpp
struct Quake { double lon, lat, depthKm, mag; long long timeMs; std::string place, url; };
```

## 实时抓取 + 刷新

- 后台 `OpenThreads::Thread`(仿 city_data 的 `ProcessThread`):
  - 循环:`hv::HttpClient` GET feed → picojson 解析 → 在 `_mutex` 内更新 `std::vector<Quake> _quakes` + 置 `_dirty=true`。
  - 每 ~60s 拉一次,**分片 sleep**(如 100ms × N 检查退出标志),关闭时秒退。
  - **仅图层开启时联网**:`_enabled` 原子标志,关闭时空转(不发请求)。
  - 网络失败/解析失败:保留上一次数据、记日志、下一周期重试(不崩、不清空)。
- 主线程:quake root 的 update 回调检测 `_dirty` → 在 GL 线程从快照重建点几何(worker 不碰 GL,锁+标志交接)。

## 渲染(ECEF 点标记)

- 每个地震:`pos = osgVerse::Coordinate::convertLLAtoECEF(osg::Vec3d(radLat, radLon, liftMeters))`,`liftMeters ≈ 3000`(防 z-fight、保可见)。
- **深度测试开启** → 背面地震被地球正确遮挡。
- 单个 `osg::Geometry` + `GL_POINTS` 圆点精灵(数十点,GPU 极廉价):
  - **大小 ∝ 震级**:`pointSizePx = clamp(4 + (mag - 2.5) * 3, 4, 28)`
  - **颜色 ∝ 深度**(USGS 标准):浅 `<70km` 红 → 中 `70–300km` 橙黄 → 深 `>300km` 蓝,平滑插值
- 小 GLSL(osgVerse `VERSE_*` 宏,仿 city_data):顶点写 `gl_PointSize`(由 per-vertex size 属性),片元画柔边圆盘、圆外 `discard`。启用 `GL_PROGRAM_POINT_SIZE` / point sprite。

## UI + 测试钩子

- LayerManager 面板加图层 **「地震 (USGS)」**,**默认关**(同云图)。
  - 开 → quake root 可见 + 置 `_enabled=true`(线程开始抓取);关 → 隐藏 + `_enabled=false`(线程空转)。
- headless 验证钩子(均 env,默认无影响):
  - `EARTH_QUAKES=1` 强制开启该层(仿 `EARTH_CLOUDS`)。
  - `EARTH_QUAKES_FILE=/path.geojson` 用**本地样本**离线、确定性验证(跳过网络)。

## 点击详情

- quake 模块内一个 `osgGA::GUIEventHandler` 捕获左键单击(PUSH→RELEASE 无明显拖动):
  - 用相机 VPW(view*proj*window)把每个地震 ECEF 投影到窗口坐标;
  - **前半球可见性过滤**:仅当该点法向朝向相机(`dot(quakePos - eyePos, quakePos) < 0`)才可选,背面地震不可选;
  - 选中**屏幕像素距离最近**且在(标记半径 + 容差,如 +6px)内的那个;点空白处清除选择。
- 详情用 **ImGui 卡片**(既有 UI 体系):显示 **地名 place、震级 `M x.x`、深度 `xx km`、发生时间(相对"xx 分钟前" + UTC 绝对时间)、USGS 链接(纯文本)**,带关闭按钮。
- 耦合最小:quake 模块暴露 `getSelected()`(返回 `const Quake*` 或 nullptr,加锁/原子),`EarthControlUI` 持一个指针,在选中时渲染「地震详情」区块 —— 与现有 `_layers` / `commonUniforms` 传指针一致。

## 验证计划(运行 app 确认)

1. 增量编译 `cmake --build build/verse_core --target install --config Release`。
2. headless:`EARTH_QUAKES_FILE=<固定样本> EARTH_AUTOCAP=400 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1` 全球视角截图 → 确认点位/大小/颜色正确、背面遮挡正确。
3. 真机(`.app`)交互:开关图层、点击某地震弹出详情、确认 ~60s 自动刷新生效、4 类回归不可能出现(未动相关代码)。

## 明确不做(YAGNI)

脉冲动画、历史回放、震级/时间窗的运行时切换 UI(先写死 2.5_day,后续按需)。

---

# Step 2:降水层 RainViewer(细化设计 — 2026-06-23 用户确认"先这样")

**复用云图的 OVERLAY 槽,零着色器改动。** 机制已读码 + curl 全部验证可行。

## 数据源(已 curl 验证)

- RainViewer `weather-maps.json`(无 key):`GET https://api.rainviewer.com/public/weather-maps.json` → `host`(如 `https://tilecache.rainviewer.com`)+ `radar.past[]`(每项 `{time, path}`,**path 是哈希如 `/v2/radar/7f68c5ecd736`,直接用此字段,别从 time 拼**)。取 `radar.past` 最后一项 = 最新观测帧。约每 10min 一帧(返回约 13 帧 ≈ 2h)。
- 瓦片模板:`{host}{path}/256/{z}/{x}/{y}/{color}/{options}.png`,默认 `color=4`、`options=1_1`(平滑+含雪)→ 例 `https://tilecache.rainviewer.com/v2/radar/<hash>/256/{z}/{x}/{y}/4/1_1.png`。
- 瓦片实测:HTTP 200、`image/png`、256×256 **RGBA(带 alpha)** → 无降水/无覆盖处透明,叠加合成天然干净。XYZ webmercator(原点左上,沿用现有 `yXYZ` 翻转)。

## 接入方式(复用 OVERLAY 槽 + 现有瓦片重载机制)

读码确认的链路:`createLayerImage` 取 `inputAddr = _layerPaths[OVERLAY].first` → 调 `createCustomPath(type, prefix=inputAddr, x,y,z)`(prefix 空→不加载/移除贴图;URL 空→有意空,如 GIBS z>9)。`TileManager::instance()->setLayerPath(OVERLAY, 路径)` 改全局路径 → 每瓦片 `operator()` 里 `TileManager::check()` 比对、不一致即 `updateLayerData` **重载该层**(city_data 切 USER 层同款)。

- **`createCustomPath` 的 OVERLAY 分支按 `prefix` 分流**:
  - `prefix == "gibs"` → 现有 GIBS VIIRS 逻辑(z>9 返回空)。
  - `prefix` 以 `http` 开头 → 当作 RainViewer 完整模板,`createPath(prefix, x, yXYZ, z)`;z 上限 ~10(雷达分辨率粗,过高返回空)。
  - 其它(空) → 返回空(不加载叠加瓦片)。
- **后台控制器**(新模块 `precip_data.{h,cpp}`,复用地震 `FetchThread` 范式):仅降水图层开启时,每 ~8–10min `requests`(15s 超时)抓 `weather-maps.json` → 取最新帧拼模板 → `TileManager::instance()->setLayerPath(OVERLAY, 模板)`。最新帧 `path` 变了才重设(避免无谓重载)。关闭时空转。

## 互斥 + UI(三选一:无 / 云图 / 降水)

「影像 / 天气」组两个互斥复选框:「GIBS 影像/云图」(原有)、「降水雷达 RainViewer」(新,`PointFeed`? 否——栅格层,`hasOpacity=true`)。apply 回调强制互斥:
- 降水 ON → 强制云图 OFF;通知控制器开始抓帧(它会 `setLayerPath(OVERLAY, RV模板)`);`Overlay2Opacity` = 降水透明度。
- 降水 OFF → 控制器停;`setLayerPath(OVERLAY, "gibs")`;`Overlay2Opacity` = 云图开?云图透明度:0。
- 云图 ON → 强制降水 OFF(连带控制器停 + 路径回 "gibs")。
- 两者皆关 → OVERLAY 路径保持 "gibs"(已加载、opacity 0,同现状),三选一中的"无"。

## 测试钩子

- `EARTH_PRECIP=1`:启动即强制开降水层(同 `EARTH_CLOUDS`/`EARTH_QUAKES`)。
- `EARTH_PRECIP_FILE=<path>`:用本地 `weather-maps.json` 替代联网(离线/确定性验证)。

## 验证计划

headless:`EARTH_PRECIP=1`(+ 可选 `EARTH_PRECIP_FILE` 固定样本)→ 确认 RainViewer 瓦片加载、陆地降水可见;确认互斥(开降水自动关云图、OVERLAY 路径切到 RV、再关切回 gibs);确认 4 类回归不可能(未碰 `scattering_globe.frag.glsl`/sun/OceanOpaque)。真机:开关切换、~10min 刷新、低空看陆地雨区。

## 明确不做(YAGNI)

nowcast 预测帧、历史回放/时间轴、色板/选项的运行时切换 UI(先写死 color=4/options=1_1,后续按需)。RainViewer 海洋无雷达覆盖属数据本身,不补。
