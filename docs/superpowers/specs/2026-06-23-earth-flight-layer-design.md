# 实时航班层(OpenSky)设计

> 日期 2026-06-23。第三个实时数据流:全球航班。用户要求"视觉震撼 + 运行快 + 绝不复现 4 类老回归"。复用地震点要素模板;新增:视口包围盒查询、航向图标、平滑滑行插值。

## 决策(已与用户确认)

| 项 | 决定 |
|---|---|
| 可视化 | 方案 A:**按航向旋转的发光飞机图标 + 平滑滑行 + 高度配色** |
| 抓取范围 | **视口包围盒**(只取当前视野内航班) |
| 数据源 | OpenSky `states/all`(免费、无 key、限流) |
| 防回归 | 独立点图层 + 独立着色器,不碰 globe 着色器/太阳/海洋/几何 |

## 数据源(已 curl 验证)

`GET https://opensky-network.org/api/states/all?lamin={}&lomin={}&lamax={}&lomax={}`(无 key;欧洲框 508 架 ≈ 65KB ≈ 1.7s)。返回 `{time, states:[[...], ...]}`,每架 state vector 数组字段(按下标):
`[0]icao24 [1]callsign [2]country [5]lon [6]lat [7]baro_altitude(m) [8]on_ground [9]velocity(m/s) [10]true_track(航向°) [11]vertical_rate [13]geo_altitude`。
picojson 解析;坏/缺字段跳过。在地(on_ground=true)或缺经纬度的跳过。

## 视口包围盒交接

主线程每帧(或每 N 帧)由相机算可见区 bbox,mutex 存入控制器;后台抓取线程读最新 bbox 拼 URL。
- 子相机点 `(lat0, lon0)` = `manipulator->computeEyeLatLonHeight()`(弧度→度)。
- 可见角半径 `θ = degrees(acos(R/(R+h)))`(R=6371km,h=相机高度 m;地平线张角)。
- bbox = `lat0±θ`(钳到 ±85)、`lon0±(θ/cos(lat0))`(经度按纬度放大,钳到合理跨度;θ 很大时整圈 → lomin=-180/lomax=180)。θ≥~80° 视为全球(lamin=-85,lamax=85,lomin=-180,lomax=180)。
- 低空(h 小)→ 小框、点少;高空 → 大框/全球。

## 渲染(震撼 + 快)

单个 `osg::Geometry` + `GL_POINTS` 点精灵 + **独立着色器**(MRT 双输出 `gl_FragData[0]/[1]`,仿地震;`GL_PROGRAM_POINT_SIZE`)。每架一个顶点:
- **位置** = `convertLLAtoECEF(radLat, radLon, baro_altitude)`(高空全贴地、低空见高度;缺高度按 0)。
- **per-vertex 属性**:颜色(高度配色)、`texCoord0 = (sizePx, headingRad)`(大小 + 航向)。
- **航向图标**:片元里把 `gl_PointCoord` 绕中心旋转 `headingRad`,画一个**指向上的三角/箭头**(机头朝航向),发光柔边,圆/框外 `discard`。
- **高度配色**:低空(<3km)暖橙 → 中(3–9km)黄白 → 高(>9km)冷青蓝(平滑插值)。
- on_ground 的航班不进几何(只画在飞的)。

## 平滑滑行(关键震撼点)

控制器存每架的基准 `lat/lon/heading/velocity` + 抓取时刻 `t0`。主线程 update 回调每帧:`elapsed = now - t0`,按 `velocity(m/s) × elapsed` 沿 `heading` 大圆方向外推新经纬度 → 重算 ECEF → 更新顶点位置数组(VBO dirty)。视口内数百架,每帧更新极廉价 → **数千架平滑滑行而非每 15s 瞬移**。后台 ~12s 重抓刷新基准。
(外推用局部平面近似:`dLat = (v·cosθ·elapsed)/R`,`dLon = (v·sinθ·elapsed)/(R·cosLat)`,θ=heading;短时足够准。)

## 交互 + UI

- 点击航班 → 屏幕拾取(前半球过滤,复用地震 `pickAt`)→ **右上角独立可关闭信息面板**(呼号/国家/高度/速度/航向),遵循偏好 [[ui-info-panels-top-right]]。
- LayerManager「实时数据 / Live」组加「航班 (OpenSky)」开关,默认关。
- 测试钩子:`EARTH_FLIGHTS=1`(强制开)、`EARTH_FLIGHTS_FILE=<states.json>`(本地样本离线/确定性验证)。

## 模块隔离

新建 `applications/earth_explorer/flight_data.{h,cpp}`(仿 `quake_data.cpp`):对外 `FlightLayer` 抽象(setEnabled/isEnabled/getSelected/clearSelected/setViewBBox)+ `configureFlightLayer(viewer, earthRoot, mainFolder, FlightLayer**)`。内部:后台 FetchThread(读 bbox→OpenSky→picojson)、worker→主线程 mutex 交接快照、主线程 update 回调重建几何 + 每帧滑行插值、拾取 handler。**GL 只在主线程**。

## 性能 & 防回归(硬约束)

- 快:视口 bbox 限量(通常数百架)+ 单几何 + 每帧轻量插值 + 异步抓取(worker 不碰 GL)。
- 4 类回归之外:独立点图层(挂 sceneCamera,同地震)、独立点精灵着色器;**不碰 `scattering_globe.frag.glsl`/太阳/OceanOpaque/几何/相机地形地板**。
- 拾取/插值/几何重建全在主线程;worker 只 HTTP+解析。

## 验证计划

- headless `EARTH_FLIGHTS_FILE=<固定样本>`(几架已知 lat/lon/alt/heading)→ 截图见**朝向正确的飞机图标 + 高度配色**;日志断言解析架数。
- 联网 `EARTH_FLIGHTS=1` 视口含航班区域(如欧洲/北美低空)→ 见成片航班;挂一会看**平滑滑行**。
- 性能:视口数百架时帧率正常(headless 跑足帧不卡)。
- 4 类回归 headless 批次(海洋不橙/晨昏线/倾斜不穿模/无程序化海洋)——未碰相关代码,确认即可。
- 真机:开关、点击详情(右上角)、滑行观感、缩放重查 bbox、与地震/降水/云图叠加层共存。

## 明确不做(YAGNI)

拖尾航迹(方案 C,后续增强)、呼号常显文字标签(只在点击详情显示)、机型/航司区分、历史回放、全球所有航班抓取。
