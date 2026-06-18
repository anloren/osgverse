# EarthExplorer 图层叠加系统 — 设计文档

- 日期：2026-06-18
- 状态：设计待评审
- 关联：`HANDOFF.md`、`tasks/lessons.md`、记忆 `earth-realtime-data-overlays`、`google-3d-tiles-access`

## 1. 背景与问题

EarthExplorer 当前底图用 Google 混合瓦片 `lyrs=y`——卫星影像 + 路网/地名**烤进同一张图**，无法关闭。贴近地面时标注严重遮挡卫星图。

同时，前期调研了一批可叠加的实时全球数据源（GIBS 云图、RainViewer 雷达、USGS 地震、OpenSky 航班、AIS 船舶、FIRMS 火点、OpenAQ 空气、SWPC 极光、ISS）。本设计要：
1. **立刻**解决标注遮挡（标注变成可开关层）；
2. 建立一套**统一的、可开关、带透明度**的图层叠加系统，把上述数据源**逐步**接入；
3. 统一规划「图层」UI。

## 2. 目标 / 非目标

**目标**
- 标注与卫星底图分离，可开关、可调透明度。
- 一个 `LayerManager` + 「图层」面板，统一管理所有叠加层（开关 / 透明度 / 刷新）。
- 两类叠加层各一套机制：**栅格**（贴球面纹理）、**点/要素**（ECEF 标记）。
- 每个源带策略标志：是否需要 API key、是否可缓存、刷新间隔。
- 分步落地：每一步可独立编译、截图验证、单独成增量。

**非目标（本轮不做）**
- Google Photorealistic 3D Tiles（另一条管线，需付费 key，单独立项，见 `google-3d-tiles-access`）。
- 时间轴回放 / 动画历史数据。
- 数据分析、落盘归档实时数据。
- 极地极冠填充（已知限制，见 `earth-render-verify-and-polar`）。

## 3. 用户已确认的约束

- **顺序**：先栅格类，再点要素。
- **API key**：免 key 源优先（GIBS / RainViewer / USGS / OpenSky）；需 key 的（AIS / FIRMS / OWM）排后面、标 🔑。
- **缓存**：按源区分——实时类（航班/船舶/雷达/地震）在线拉取不缓存；静态影像类（GIBS 历史、标注）可走现有磁盘缓存。
- **UI 布局**：方案 B「分类折叠」——「图层」折叠栏内每个分类是可折叠子树，默认收起。

## 4. 架构

```
            ┌─────────────────────────────────────┐
            │  「图层 Layers」UI 面板 (ImGui, 方案B) │  EarthControlUI.h
            │   分类折叠 · 每层: ☑开关 + ▭透明度     │
            └───────────────┬─────────────────────┘
                            │ 读写
            ┌───────────────▼─────────────────────┐
            │  LayerManager (注册表 + 调度)         │  新增
            │  每层: {id,组,类型,enabled,opacity,   │
            │         refreshSec,needsKey,cacheable}│
            └───────┬───────────────────┬──────────┘
        栅格 enable/opacity        点要素 enable
                    │                   │
   ┌────────────────▼──────┐   ┌────────▼───────────────────┐
   │ 栅格叠加 (GPU 合成)     │   │ 点/要素叠加 (场景子图)        │
   │ ExtraLayer 纹理槽 +    │   │ 每源一个 Group: ECEF billboard│
   │ *Opacity uniform      │   │ + 后台拉取线程 (JSON/WS)     │
   │ 复用 TileCallback 管线 │   │ 复用 city marker 模式        │
   └───────────────────────┘   └────────────────────────────┘
```

**4.1 LayerManager（新增，app 内）**
- 单例 / 成员，持有 `std::vector<OverlayLayer>`。
- `OverlayLayer`：`id, displayName, group, type{RasterTile,PointFeed,Grid}, enabled, opacity, refreshSec, needsKey, cacheable, source(URL/模板/回调)`。
- 提供 `setEnabled(id,bool)`、`setOpacity(id,float)`，被 UI 调用，内部分派到对应机制。
- 不持有渲染细节，只持有状态 + 回调钩子；渲染由两套机制各自实现。

**4.2 栅格叠加层（GPU 纹理合成）**
- 复用现有地球表面着色器 `assets/shaders/scattering_globe.frag.glsl`，它**已有** `ExtraLayerSampler`(USER, unit2, `UvOffset3`) 并已 `mix` 合成到地面。
- 每个栅格 overlay = 一个被着色器采样的额外纹理层 + 一个 `*Opacity` uniform 门控。
- **开关**：enable → 通过 `TileManager::setLayerPath(layerId, url)` 加载该层瓦片（现有 `check()` 机制会触发流式加载）；disable → 清空路径卸载。
- **透明度**：`*Opacity` uniform，着色器 `mix(ground, layer.rgb, layer.a * Opacity)`。
- **扩展性**：现成 1 个 ExtraLayer 槽（给标注）。框架再扩 2 个槽（`ExtraLayer2/3` + `UvOffset4/5` + `Overlay2/3Opacity`），共支持**同时 3 个 GPU 合成栅格 overlay**（如 标注 + 云图 + 雷达）。超出则用独立透明 overlay 瓦片树（后续按需）。
- TileCallback 的 `LayerType` 枚举需从 `{ELEVATION,ORTHOPHOTO,OCEAN_MASK,USER}` 扩展出额外 overlay 槽。

**4.3 点 / 要素叠加层（场景子图）**
- `earthRoot` 下挂一个 `Group overlays`；每个点源一个子 `Group`（如 `quakeGroup`）。
- 每源一个**后台拉取线程**：按 `refreshSec` 拉 JSON（或 WebSocket），解析出 `(lat,lon,[alt,属性])` 列表，主线程用 `Coordinate::convertLLAtoECEF` 放置 billboard/标记，复用 `city_data.cpp` / `ReaderWriterCSV.cpp` 的标记模式与符号着色器 `poi_symbols.frag.glsl`。
- **开关** = 子 Group 的 node mask；不渲染时也暂停拉取线程。
- **数量上限**：全球航班/船舶可达上万，全画成 billboard 会拖垮。每源设 `maxMarkers`（如 2000）+ 视口/缩放过滤（只取当前可视范围；低 zoom 抽稀）。超限时 `log` 丢弃数量，不静默截断。

**4.4 每源策略**
- `needsKey`：UI 标 🔑；免 key 源先实现，需 key 源 key 缺失时该层灰显禁用 + 提示。
- `cacheable`：true 才走现有磁盘缓存（`EARTH_TILE_CACHE`）；实时类一律 false。
- `refreshSec`：点/要素与雷达类的轮询间隔（如雷达 300s、航班 10s、地震 60s、AIS 实时流）。

## 5. UI 设计（方案 B：分类折叠）

在 `EarthControlUI.h` 新增一个 `CollapsingHeader("图层 Layers")`，内部按分类用 `TreeNode` 折叠：
- **底图 / 标注**：卫星影像（基础，常开）、路网·地名（开关 + 透明度）。
- **影像 / 天气**：GIBS 云图、降水雷达（各开关 + 透明度）。
- **动态点**：地震、航班、船舶🔑（各开关）。
- **空间**：极光（开关 + 透明度）、ISS（开关）。
每行：`Checkbox` 驱 `LayerManager::setEnabled`；栅格层额外 `SliderFloat` 驱 `setOpacity`；需 key 且缺 key 的行 `BeginDisabled()` 灰显 + tooltip。UI 只读写 LayerManager，不直接碰渲染。

## 6. 数据流

1. 启动：app 构造 `LayerManager`，注册所有层（默认仅底图 + 标注 enabled）。
2. 用户在面板勾选某层 → `LayerManager::setEnabled(id,true)`。
3. 栅格层 → `TileManager::setLayerPath(...)` 触发瓦片流式加载，`*Opacity` 置为当前 slider 值。
4. 点要素层 → 启动该源拉取线程；线程定时拉数据 → 主线程更新该 Group 的 billboard；node mask 打开。
5. 透明度滑动 → 仅改 `*Opacity` uniform（栅格）。
6. 关闭 → 栅格清路径 / 点要素停线程 + 关 node mask。

## 7. 错误处理与性能

- **源不可达 / 超时**：后台线程 try/超时；失败保留上一帧数据或空，记 `log`，不崩、不卡主线程。该层 UI 显示「⚠ 离线」。
- **缺 key**：注册时探测，灰显该层。
- **点数量**：每源 `maxMarkers` + 视口过滤；超限 `log` 丢弃数。
- **栅格槽耗尽**：同时启用的 GPU 合成栅格 overlay > 槽数时，UI 阻止再开并提示「先关一个影像/天气层」。
- **线程安全**：拉取线程只产出数据队列，billboard 更新在主线程 update 回调里做（沿用现有 city 更新模式）。

## 8. 测试 / 验证

沿用既有无头截图流程（`EARTH_AUTOCAP` + `ScreenCaptureHandler`，见 `tasks/lessons.md`）。每步：
- **标注 P0**：贴地视角（`--goto 39.9 116.4 0.5`）开/关标注对比，确认关后卫星图无遮挡；低 zoom 中纬确认底图 `lyrs=s` 无回归。
- **每个栅格层**：全球 + 区域截图，确认叠加位置对齐、透明度生效、关后干净。
- **每个点层**：已知热点区域（如地震带、航路、港口）截图，确认标记落点正确、数量受控。
- **回归**：每步都跑全球 + 中纬陆地，确认不破坏底图/大气（吸取极地教训：改动后必验证再提交/推送）。

## 9. 分步路线（每步一个增量，可独立验证 / 提交）

| 步 | 内容 | 源 | key | 缓存 |
|----|------|----|----|----|
| **P0** | 图层框架雏形 + LayerManager + UI 折叠栏 + **标注开关**（lyrs=s 底图 + lyrs=h USER 层 + LabelOpacity） | Google | 否 | 标注可缓存 |
| **P1** | 栅格 overlay 框架（扩 ExtraLayer 槽 + 通用 RasterTile 层）+ **GIBS 云图/近实时影像** | NASA GIBS | 否 | 可缓存 |
| **P2** | **降水雷达** 叠加（5 分钟刷新） | RainViewer | 否 | 否 |
| **P3** | 点要素框架（overlay Group + 拉取线程 + billboard）+ **地震** | USGS | 否 | 否 |
| **P4** | **航班 ADS-B**（视口过滤 + maxMarkers + 10s 刷新） | OpenSky | 否 | 否 |
| **P5** | **船舶 AIS**（WebSocket 流） | aisstream | 🔑 | 否 |
| **P6** | **火点 / 空气质量 / 极光 / ISS**（逐个小增量） | FIRMS🔑 / OpenAQ / SWPC / OpenNotify | 混 | 否 |

P0–P4 全程免 key。P5 起需注册免费 key。

**计划拆分**：本设计是整体蓝图。紧接其后的实现计划只覆盖 **P0（LayerManager + UI 折叠栏 + 标注开关）**——它建立框架并交付立即可见的价值。P1 起每个阶段在 P0 落地后各自单独成实现计划（spec → plan → 实现循环）。

## 10. 风险 / 待定

- **着色器扩展**：`scattering_globe.frag.glsl` 是 osgVerse 共享着色器；扩 ExtraLayer 槽要确认不影响其他用法（tests/earth_test、wasm/earth_demo 也引用它）。倾向新增 uniform 默认值不变行为，向后兼容。
- **TileCallback 枚举扩展**：加 overlay 层槽要动 `LayerType` 与 `updateLayerData` 的 texUnit 映射，小心 USER 既有语义。
- **透明 overlay 与大气合成顺序**：GPU 合成在地面色阶段做，理论上在大气前，需截图确认云图/雷达不被大气洗白过度（可给 overlay 单独的 `GlobalOpaque` 豁免）。
- **点要素海量数据**：航班/船舶的视口过滤策略需实测调参。
- **GIBS 投影**：GIBS 提供 EPSG:3857（Web Mercator）的 WMTS，与现有瓦片方案对齐；需选对 TileMatrixSet。
```
