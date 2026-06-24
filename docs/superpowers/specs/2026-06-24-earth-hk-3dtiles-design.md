# 香港官方 3D Tiles 接入 — 最小验证 spike 设计

> 日期 2026-06-24。第四个数据源方向:香港地政总署官方 3D 城市模型(建筑/基础设施)。与前三个**实时流**(地震/降水/航班)不同,这是**静态高精度城市模型**,补足长期缺的"下钻到街道级观感"。
> 本 spec 只覆盖**最小验证 spike**:证明 osgVerse 能把一个 WGS84 Cesium 3D Tiles 流式加载并正确摆到地球真实经纬位置。完整图层(UI/样式/全港接入)待 spike 通过后另立 spec。

## 背景:数据与引擎匹配度(已调研确认)

香港地政总署经 CSDI Portal / Open3Dhk 提供全港(2025-03 已全覆盖)3D 数字地图:

- **API 版(目标形态)**:3D Spatial Data API,**Cesium 3D Tiles + WGS84**,免 key(已发邮件 `3dmap@landsd.gov.hk` 申请,待回)。端点:
  - `https://data.map.gov.hk/api/3d-data/3dsd/WGS84/building/tileset.json?key=KEY`
  - `https://data.map.gov.hk/api/3d-data/3dsd/WGS84/infrastructure/tileset.json?key=KEY`
  - 授权:可商用/非商用,**需署名(attribution)**,as-is 无担保。
- 引擎侧已有(无需造轮子):
  - `plugins/osgdb_3dtiles`:tileset.json 递归 + root `transform` 应用 + region/box/sphere bounding + HTTP 流式 + PagedLOD(geometricError→屏幕误差 LOD 切换)。
  - `plugins/osgdb_gltf`:b3dm/glb/i3dm 瓦片内容。
  - `modeling/Math.h` `Coordinate::convertLLAtoECEF`(Z-up ECEF),地震/航班同款。

**结论**:加载 + 地理定位 + 流式 LOD 的轮子都在;spike 的工作是"接线 + 实测坐标对齐",不是造引擎。

## 决策(已与用户确认)

| 项 | 决定 |
|---|---|
| 起步方式 | 申请免费 key(已发邮件)+ **先用公开样本打通链路**,不等 key |
| 公开样本 | 先试插件注释现成的 earthsdk 大雁塔 `https://earthsdk.com/v/last/Apps/assets/dayanta/tileset.json`;不通再换其他公开 WGS84 tileset |
| 集成形态 | **env 钩子 + 独立小模块**(仿 `quake_data`),**不**先做 UI 开关 |
| 数据获取 | **流式(API/HTTP),非批量下载**——按视口拉,不本地全存(全港数百 GB+,且爬取或违反 ToS) |
| 坐标对齐 | 先按标准 ECEF 直接挂、headless 实测;有偏差再包矫正 `MatrixTransform` |
| 防回归 | 独立节点挂 `sceneCamera`,不碰 globe 着色器/太阳/海洋/几何 → 4 类回归之外 |

## 目标与非目标

**目标** — 证明 osgVerse 能把一个 WGS84 Cesium 3D Tiles **流式加载并正确摆到地球真实经纬位置**(落位 + 贴地 + 流式 LOD + 不破坏现有渲染),扫清接香港 building API 前的最大技术风险。

**非目标(本 spike 不做)** — UI 开关、香港专属样式、光照/纹理精调、infrastructure 层、全港性能压测、city_data 式 LOD 调参。

## 模块与集成

新建 `applications/earth_explorer/tiles3d_data.{h,cpp}`(仿 `quake_data`):

- 对外:`osg::Node* configure3DTilesLayer(osgViewer::View& viewer, osg::Node* earthRoot, const std::string& url)`,返回挂到 `sceneCamera` 的节点(未设钩子时返回空 `osg::Group`,挂上去零影响)。
- 内部:读 env `EARTH_3DTILES=<tileset_url>`;若设,`osgDB::readNodeFile(url + ".verse_tiles")`(HTTP 流式,PagedLOD 自动按视口拉)。
- `earth_main.cpp` 仿 `:367`(地震挂载行)加一行 `sceneCamera->addChild(configure3DTilesLayer(...))`,**仅 env 设置时**有内容,否则零影响。

## 坐标对齐(spike 核心风险)

- Cesium 3D Tiles 标准:tileset root `transform`(4×4)把 gltf 本地坐标变换到 **ECEF (EPSG:4978, Z-up)**;插件已应用(`ReaderWriter3dTiles.cpp:283-294`)。
- EarthExplorer 地震/航班同挂 `sceneCamera`、用 `convertLLAtoECEF`(Z-up ECEF)→ 预期同系、自动对齐。
- **已识别风险**:插件 `getBoundingSphere` 的 `region` 路径用 Y-up `(x, z, -y)`(`:464`),与几何的 Z-up 不一致 → 可能影响 PagedLOD 的 LOD/裁剪判定(不一定影响落位)。**这是实测重点**。
- 处置:headless 实测落位/贴地;若几何偏移或 LOD 异常,在 `configure3DTilesLayer` 内包一层矫正 `MatrixTransform`(**不改插件**,保持隔离)。

## 成功标准(明确)

1. 样本 tileset 能渲染出来;
2. 落在地球**正确经纬**(西安大雁塔位置 ≈ 34.218°N, 108.964°E);
3. **大致贴地**(基座不明显悬空/陷地);
4. 缩放/平移 **PagedLOD 流式 LOD 正常切换**、不崩、不冻结现有地球;
5. 不设 env 钩子 = 零影响;设了也不破坏 globe/大气/海洋/地震/航班。

## 验证计划

- headless:`EARTH_3DTILES=<大雁塔 url>` + 相机移到样本经纬低空 → 截图见模型落在正确位置、贴地;日志确认 tileset/瓦片加载、无致命错误。
- 4 类回归 headless 批次(海洋不橙 / 晨昏线 / 倾斜不穿模 / 无程序化海洋)——未碰相关代码,确认即可。
- 真机(用户,遵循"务必肉眼验证"铁律):落位/贴地、缩放时流式细化、与地震/航班/降水叠加共存、关钩子无残留。

## 后续路径(spike 通过后,另立 spec/plan)

1. key 到位 → 换 `EARTH_3DTILES=<香港 building tileset.json?key=...>`,实测全港落位/性能。
2. 处理 URL 带 `?key=` 查询串的扩展名 / pseudo-ext 解析(可能需走 web reader / 缓存那套)。
3. 做成正式可开关图层(LayerManager「3D 城市 / HK」组)+ UI 数据来源署名(授权硬要求)。
4. 视性能加视口/距离裁剪、与地形/海洋的协调。

## 明确不做(YAGNI)

完整 UI、香港专属渲染样式、infrastructure/terrain 层、全港压测、离线批量下载(架构上不做本地全存)。

## 实现注记(2026-06-24)

- 决策表里的公开样本(earthsdk 大雁塔)**实测 404**,搜到的备选 ArcGIS Stuttgart 亦 404。**改用合成本地 fixture**(`applications/earth_explorer/test/gen_hk_3dtiles_fixture.py`,WGS84 ENU→ECEF transform 烘焙在香港坐标)——更可控、可复现,直接验香港落位。
- 核心落位链路**已数值验证通过**(bound center 与香港理论 ECEF 三轴吻合到几十米)。详见 plan「实现结果」。
