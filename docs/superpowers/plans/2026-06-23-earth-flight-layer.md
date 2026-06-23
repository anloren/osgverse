# 实时航班层(OpenSky)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement task-by-task. Checkbox (`- [ ]`) steps.

**Goal:** 叠加 OpenSky 实时航班:按航向旋转的发光飞机图标 + 高度配色 + 平滑滑行,视口包围盒抓取,点击详情(右上角),运行快、不复现 4 类老回归。

**Architecture:** 新模块 `flight_data.{h,cpp}` **仿 `quake_data.cpp`**(libhv 后台抓取 + picojson + 单 `GL_POINTS` 几何 + worker→主线程 mutex 交接 + 拾取 handler + 图层注册)。新增:OpenSky 解析、视口 bbox(主线程算→worker 抓)、航向图标点精灵着色器、主线程每帧滑行插值。独立点图层 + 独立着色器,**不碰 globe 着色器/太阳/海洋**。

**Tech Stack:** C++/OSG、libhv、picojson(均 Step1 地震已接);参考实现 `applications/earth_explorer/quake_data.cpp`。

**铁律:** 不碰 `scattering_globe.frag.glsl`/太阳/OceanOpaque/几何;GL 只在主线程;worker 只 HTTP+解析。

**构建/验证:**
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
headless 模板同地震(`EARTH_AUTOCAP`/`EARTH_FRAME_SLEEP_MS`/`--goto`/`DYLD_LIBRARY_PATH`)。

---

## File Structure
- **Create** `applications/earth_explorer/flight_data.h` — `FlightInfo` 结构 + `FlightLayer` 抽象(setEnabled/isEnabled/setViewBBox/getSelected/clearSelected)+ `configureFlightLayer(...)`。
- **Create** `applications/earth_explorer/flight_data.cpp` — 全部实现。
- **Modify** `applications/earth_explorer/CMakeLists.txt` — 加 `flight_data.cpp`。
- **Modify** `applications/earth_explorer/earth_main.cpp` — extern + 挂节点 + 注册「航班」图层 + `EARTH_FLIGHTS` 钩子 + 每帧算 bbox。
- **Modify** `applications/earth_explorer/EarthControlUI.h` — 右上角「航班详情」信息面板(`FlightLayer* _flight`)。
- **Create** `applications/earth_explorer/test/flights_fixture.json` — 离线样本(OpenSky 格式)。

> 全程对照 `applications/earth_explorer/quake_data.cpp` 作为结构范本(类布局、FetchThread 定义顺序、postSnapshot/syncIfDirty、pick handler、ECEF lift、shader 用 `createShaderDefinitions(vs,100,130)`+裸 Program、MRT 双输出)。

---

## Task 1: 骨架模块 + CMake + 接线(可编译、零行为)

**Files:** Create flight_data.h/.cpp; Modify CMakeLists.txt, earth_main.cpp

- [ ] **Step 1: `flight_data.h`**
```cpp
#ifndef EARTH_FLIGHT_DATA_H
#define EARTH_FLIGHT_DATA_H
#include <string>
#include <osg/Node>
#include <osgViewer/View>

struct FlightInfo
{
    std::string callsign, country;
    double lon = 0.0, lat = 0.0, altM = 0.0, velMS = 0.0, headingDeg = 0.0;
    bool valid = false;
};

class FlightLayer
{
public:
    virtual ~FlightLayer() {}
    virtual void setEnabled(bool on) = 0;
    virtual bool isEnabled() const = 0;
    virtual void setViewBBox(double latMin, double lonMin, double latMax, double lonMax) = 0; // 主线程每帧设
    virtual FlightInfo getSelected() const = 0;
    virtual void clearSelected() = 0;
};

extern osg::Node* configureFlightLayer(osgViewer::View& viewer, osg::Node* earthRoot,
                                       const std::string& mainFolder, FlightLayer** outLayer);
#endif
```

- [ ] **Step 2: `flight_data.cpp` 骨架** — 仿 quake_data.cpp 的骨架:同样的 includes(osg/Geometry、Geode、NodeCallback、osgGA/GUIEventHandler、OpenThreads、modeling/Math.h、pipeline/Pipeline.h、pipeline/Utilities.h、readerwriter/EarthManipulator.h、VerseCommon.h、iostream、vector、"flight_data.h")。匿名 namespace 内 `FlightLayerImpl : public osg::Referenced, public FlightLayer`,成员 `osg::Group* _root`(裸指针,由返回节点 setUserData 持有)、`bool _enabled`、bbox 四个 double(mutex 保护)。`setViewBBox` 加锁存 bbox;`setEnabled`/`isEnabled` 基本实现;`getSelected` 返回 `FlightInfo()`、`clearSelected` 空;`buildScene()` 建 `_root` 默认 nodeMask 0。`configureFlightLayer`:`ref_ptr<FlightLayerImpl>` 局部 → buildScene → `root->setUserData(impl.get())` → `*outLayer=impl.get()` → return root(**所有权同 quake:root 持 impl、impl 裸指针引用 root,无环**)。

- [ ] **Step 3: CMakeLists.txt** 源列表加 `flight_data.cpp`(libhv 已在 Step1 接好)。

- [ ] **Step 4: earth_main.cpp** 顶部 `#include "flight_data.h"`;在 configureQuakeData 挂载之后加:
```cpp
    FlightLayer* flightLayer = nullptr;
    sceneCamera->addChild(configureFlightLayer(viewer, earthRoot.get(), mainFolder, &flightLayer));
```

- [ ] **Step 5: 构建**(`cmake --build ... 2>&1 | tail -6`)→ 成功。
- [ ] **Step 6: headless 冒烟**(默认,无 flights)→ 地球正常、无崩溃、无航班(默认关)。
- [ ] **Step 7: 提交** `feat(earth): flight layer skeleton + wiring (no behavior)`。

---

## Task 2: OpenSky 抓取 + 解析(EARTH_FLIGHTS_FILE 样本 + 日志断言)

**Files:** Create test/flights_fixture.json; Modify flight_data.cpp

- [ ] **Step 1: `test/flights_fixture.json`**(OpenSky 格式,3 架不同高度/航向;字段下标对齐:[1]callsign [2]country [5]lon [6]lat [7]baro_alt [8]on_ground [9]vel [10]true_track):
```json
{ "time": 1782180220, "states": [
  ["aaa111","TEST1   ","Germany",1782180210,1782180210,8.29,49.99,10058.4,false,255.2,152.25,0.3,null,10591.8,"1000",false,0],
  ["bbb222","TEST2   ","France",1782180210,1782180210,2.35,48.86,1200.0,false,90.0,30.0,1.0,null,1300.0,"1000",false,0],
  ["ccc333","GND0    ","Spain",1782180210,1782180210,-3.70,40.42,0.0,true,0.0,0.0,0.0,null,0.0,"1000",false,0]
] }
```
(TEST1 高空东南向、TEST2 低空东北向、GND0 在地应被跳过。)

- [ ] **Step 2: flight_data.cpp 加 includes + 内部结构 + 解析/抓取**(仿 quake 的 picojson + libhv):
```cpp
#include <picojson.h>
#include "3rdparty/libhv/all/client/requests.h"
#include <fstream>
#include <sstream>
```
匿名 namespace 内、`FlightLayerImpl` 前:
```cpp
    struct Flight { double lon, lat, altM, velMS, headingRad; std::string callsign, country; osg::Vec3d ecef; };
    static const double kFlightLiftMeters = 0.0;   // 用真实气压高度;若需最小可见可加常量

    // 解析 OpenSky states/all → vector<Flight>(在地/缺经纬度跳过;容错)。
    std::vector<Flight> parseOpenSky(const std::string& text)
    {
        std::vector<Flight> out;
        picojson::value root; std::string err = picojson::parse(root, text);
        if (!err.empty() || !root.is<picojson::object>()) { std::cout << "[Flight] JSON err: " << err << "\n"; return out; }
        const picojson::value& states = root.get("states");
        if (!states.is<picojson::array>()) return out;
        const picojson::array& arr = states.get<picojson::array>();
        for (size_t i = 0; i < arr.size(); ++i)
        {
            if (!arr[i].is<picojson::array>()) continue;
            const picojson::array& s = arr[i].get<picojson::array>();
            if (s.size() < 11) continue;
            if (s[8].is<bool>() && s[8].get<bool>()) continue;          // on_ground 跳过
            if (!s[5].is<double>() || !s[6].is<double>()) continue;     // 缺经纬度跳过
            Flight f;
            f.lon = s[5].get<double>(); f.lat = s[6].get<double>();
            f.altM = s[7].is<double>() ? s[7].get<double>() : 0.0;
            f.velMS = s[9].is<double>() ? s[9].get<double>() : 0.0;
            f.headingRad = osg::DegreesToRadians(s[10].is<double>() ? s[10].get<double>() : 0.0);
            f.callsign = s[1].is<std::string>() ? s[1].get<std::string>() : "";
            f.country = s[2].is<std::string>() ? s[2].get<std::string>() : "";
            f.ecef = osgVerse::Coordinate::convertLLAtoECEF(osg::Vec3d(
                osg::DegreesToRadians(f.lat), osg::DegreesToRadians(f.lon), f.altM + kFlightLiftMeters));
            out.push_back(f);
        }
        return out;
    }

    // 取数据:优先 EARTH_FLIGHTS_FILE,否则 OpenSky 视口 bbox(参数 = 当前 bbox 度)。
    std::vector<Flight> fetchFlights(double latMin, double lonMin, double latMax, double lonMax)
    {
        const char* fixtureFile = getenv("EARTH_FLIGHTS_FILE");
        if (fixtureFile && *fixtureFile)
        {
            std::ifstream in(fixtureFile);
            if (!in) { std::cout << "[Flight] fixture open failed\n"; return std::vector<Flight>(); }
            std::stringstream ss; ss << in.rdbuf();
            std::vector<Flight> fs = parseOpenSky(ss.str());
            std::cout << "[Flight] Parsed " << fs.size() << " flights (fixture)\n";
            return fs;
        }
        char url[256];
        snprintf(url, sizeof(url),
            "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
            latMin, lonMin, latMax, lonMax);
        requests::Request req(new HttpRequest);
        req->method = HTTP_GET; req->url = url; req->timeout = 20;
        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        { std::cout << "[Flight] fetch failed status=" << (resp ? (int)resp->status_code : -1) << "\n"; return std::vector<Flight>(); }
        std::vector<Flight> fs = parseOpenSky(resp->body);
        std::cout << "[Flight] Parsed " << fs.size() << " flights (network)\n";
        return fs;
    }
```

- [ ] **Step 3: 临时**在 `configureFlightLayer` 末尾(return 前)加验证触发(Task 4 删):
```cpp
    if (getenv("EARTH_FLIGHTS_FILE")) { std::vector<Flight> fs = fetchFlights(-85,-180,85,180); }
```
(只验证解析+日志;`fetchFlights` 是匿名 namespace 函数,configureFlightLayer 在同文件可调。)

- [ ] **Step 4: 构建** → 成功。
- [ ] **Step 5: 验证** `EARTH_FLIGHTS_FILE=<fixture> EARTH_AUTOCAP=120` → 日志 `[Flight] Parsed 2 flights (fixture)`(3 架里 GND0 在地被跳过 → 2)。
- [ ] **Step 6:(可选)联网** `EARTH_FLIGHTS=1`(Task4 后才有开关;本步可临时用全球 bbox 验证 fetchFlights network 路径打印架数)。
- [ ] **Step 7: 提交** `feat(earth): OpenSky fetch+parse (EARTH_FLIGHTS_FILE, viewport bbox)`。

---

## Task 3: 渲染航向图标点精灵(高度配色,静态位置)

**Files:** Modify flight_data.cpp

- [ ] **Step 1: 加点精灵着色器源 + 高度配色 + buildFlightGeode**(仿 quake 的 shader 设置 + MRT 双输出 + GL_PROGRAM_POINT_SIZE;**新增航向旋转 + 箭头形状**):
```cpp
    const char* flightVertCode = {
        "VERSE_VS_OUT vec4 pointColor;\n"
        "VERSE_VS_OUT float headingRad;\n"
        "void main() {\n"
        "    pointColor = osg_Color;\n"
        "    gl_PointSize = osg_MultiTexCoord0.x;\n"
        "    headingRad = osg_MultiTexCoord0.y;\n"   // 航向(弧度)
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };
    const char* flightFragCode = {
        "VERSE_FS_IN vec4 pointColor;\n"
        "VERSE_FS_IN float headingRad;\n"
        "#ifdef VERSE_GLES3\n"
        "layout(location=0) VERSE_FS_OUT vec4 fragColor;\n"
        "layout(location=1) VERSE_FS_OUT vec4 fragOrigin;\n"
        "#endif\n"
        "void main() {\n"
        "    vec2 d = gl_PointCoord - vec2(0.5);\n"
        "    float s = sin(headingRad), c = cos(headingRad);\n"
        "    vec2 r = vec2(c*d.x - s*d.y, s*d.x + c*d.y);\n"  // 旋转到机头朝航向(+y=机头)
        "    r.y = -r.y;\n"                                    // gl_PointCoord y 向下→翻成 +y 朝上
        "    // 箭头三角:apex(0,0.42) base(±0.30,-0.32);点在三角内则画\n"
        "    float inTri = step(0.0, r.y + 0.32) * step(abs(r.x), 0.30 * (0.42 - r.y) / 0.74);\n"
        "    if (inTri < 0.5) discard;\n"
        "    vec3 rgb = pointColor.rgb;\n"
        "#ifdef VERSE_GLES3\n"
        "    fragColor = vec4(rgb, 1.0); fragOrigin = vec4(1.0);\n"
        "#else\n"
        "    gl_FragData[0] = vec4(rgb, 1.0); gl_FragData[1] = vec4(1.0);\n"
        "#endif\n"
        "}\n"
    };

    // 高度→颜色:低空暖橙 → 中黄白 → 高冷青蓝。
    osg::Vec4 altColor(double altM)
    {
        if (altM < 3000.0)  return osg::Vec4(1.00f, 0.55f, 0.20f, 1.0f);
        if (altM < 9000.0)  return osg::Vec4(1.00f, 0.95f, 0.70f, 1.0f);
        return osg::Vec4(0.45f, 0.85f, 1.00f, 1.0f);
    }
    static const float kFlightSizePx = 16.0f;

#ifndef GL_PROGRAM_POINT_SIZE
#define GL_PROGRAM_POINT_SIZE 0x8642
#endif

    osg::Geode* buildFlightGeode(const std::vector<Flight>& fs, osg::StateSet* sharedSS)
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        osg::ref_ptr<osg::Vec2Array> attrs = new osg::Vec2Array;  // (sizePx, headingRad)
        for (size_t i = 0; i < fs.size(); ++i)
        {
            verts->push_back(fs[i].ecef);
            colors->push_back(altColor(fs[i].altM));
            attrs->push_back(osg::Vec2(kFlightSizePx, (float)fs[i].headingRad));
        }
        geom->setVertexArray(verts.get());
        geom->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
        geom->setTexCoordArray(0, attrs.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_POINTS, 0, (GLsizei)verts->size()));
        geom->setUseDisplayList(false); geom->setUseVertexBufferObjects(true);
        geom->setCullingActive(false);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geom.get()); geode->setStateSet(sharedSS);
        return geode.release();
    }
```

- [ ] **Step 2: FlightLayerImpl 加 postSnapshot/syncIfDirty/buildScene(shader+ss)/SyncCallback**(**完全仿 quake**:`_ss` 共享 stateset 用 flightVert/FragCode + createShaderDefinitions(100,130)+裸 Program + GL_PROGRAM_POINT_SIZE + renderBin 11;`syncIfDirty` 重建 geode;存 `_flights`)。

- [ ] **Step 3: 临时** configureFlightLayer 直接灌一次 fixture + 强制开(Task 4 改):`impl->postSnapshot(fetchFlights(-85,-180,85,180)); impl->setEnabled(true);`

- [ ] **Step 4: 构建** → 成功。
- [ ] **Step 5: 验证** `EARTH_FLIGHTS_FILE=<fixture>` + `--goto 49 6 1500`(欧洲低空,对准 TEST1/2)截图 → 见**箭头朝各自航向**、TEST1 高空冷蓝、TEST2 低空暖橙;globe 正常。
- [ ] **Step 6: 提交** `feat(earth): render flight heading-arrow sprites (altitude color)`。

---

## Task 4: 后台抓取线程(~12s)+ 视口 bbox + 图层开关 + EARTH_FLIGHTS

**Files:** Modify flight_data.cpp, earth_main.cpp

- [ ] **Step 1: flight_data.cpp 加 FetchThread**(仿 quake;读 bbox 抓取):线程循环每 ~12s(`kIntervalTicks=120`,100ms tick),仅 `isEnabled()` 时:在锁内取当前 bbox → `fetchFlights(bbox)` → `postSnapshot`。`setEnabled(true)` 置 `_refreshNow`(force,绕过任何去重——本层无去重也无妨,但保留即时抓取)。析构 cancel+join+delete(仿 quake)。
- [ ] **Step 2: configureFlightLayer** 去掉 Task3 临时强开,改 `impl->startFetch()`(默认关、线程常驻仅开启时联网)。
- [ ] **Step 3: earth_main.cpp** 注册「航班」图层(「实时数据 / Live」组,默认关,apply→`flightLayer->setEnabled`)+ `EARTH_FLIGHTS` 钩子(仿 `EARTH_QUAKES`)。**并加每帧 bbox 计算**:新增一个 FRAME `GUIEventHandler`(或复用既有 handler)——每帧从 `view->getCamera()` 取 eye ECEF→`convertECEFtoLLA`→lat/lon/alt,按 spec 公式算 bbox(`θ=acos(R/(R+h))`,经度按 cosLat 放大,θ≥80° 全球),调 `flightLayer->setViewBBox(...)`。（handler 在 flight_data.cpp 内提供 `FlightBBoxHandler` 更内聚;或 earth_main 内联。择内聚:在 configureFlightLayer 里 `viewer.addEventHandler(new FlightBBoxHandler(impl))`,handler 自己算 bbox 调 setViewBBox。）
- [ ] **Step 4: 构建** → 成功。
- [ ] **Step 5: 验证** `EARTH_FLIGHTS=1 EARTH_FLIGHTS_FILE=<fixture>` `--goto 49 6 1500` → 日志 `Parsed 2 flights (fixture)`(经线程→postSnapshot→syncIfDirty)、截图见箭头。默认(无 EARTH_FLIGHTS)→ `grep -c Parsed`=0、无航班。
- [ ] **Step 6:(联网)** `EARTH_FLIGHTS=1 --goto 49 6 1500` → 见欧洲成片真实航班、各自朝向、高度配色。
- [ ] **Step 7: 提交** `feat(earth): flight bg refresh thread + viewport bbox + layer toggle + EARTH_FLIGHTS`。

---

## Task 5: 平滑滑行插值(主线程每帧外推)

**Files:** Modify flight_data.cpp

- [ ] **Step 1: 记录基准时刻** `syncIfDirty` 应用快照时存 `_t0 = referenceTime`(从 update 回调的 `nv->getFrameStamp()->getReferenceTime()` 取;存 `_flights` 的同时存各自 lon/lat/heading/vel 基准——已在 Flight 里)。
- [ ] **Step 2: 每帧外推**:`SyncCallback::operator()`(主线程 update)里,除了 `syncIfDirty`,再对当前 `_flights` 做插值更新顶点位置:
```cpp
        // 每帧按 速度×航向×elapsed 外推经纬度(局部平面近似),更新顶点位置。
        void interpolate(double refTime)
        {
            if (!_geode.valid() || _flights.empty()) return;
            double elapsed = refTime - _t0; if (elapsed < 0.0) elapsed = 0.0;
            const double R = 6371000.0;
            osg::Geometry* g = _geode->getDrawable(0)->asGeometry();
            osg::Vec3Array* va = static_cast<osg::Vec3Array*>(g->getVertexArray());
            for (size_t i = 0; i < _flights.size(); ++i)
            {
                const Flight& f = _flights[i];
                double dist = f.velMS * elapsed;             // 米
                double dLat = (dist * cos(f.headingRad)) / R;
                double dLon = (dist * sin(f.headingRad)) / (R * cos(osg::DegreesToRadians(f.lat)));
                double lat2 = f.lat + osg::RadiansToDegrees(dLat);
                double lon2 = f.lon + osg::RadiansToDegrees(dLon);
                (*va)[i] = osgVerse::Coordinate::convertLLAtoECEF(osg::Vec3d(
                    osg::DegreesToRadians(lat2), osg::DegreesToRadians(lon2), f.altM + kFlightLiftMeters));
            }
            va->dirty(); g->dirtyBound();
        }
```
在回调里:`_owner->syncIfDirty(); _owner->interpolate(nv->getFrameStamp()->getReferenceTime());`（syncIfDirty 内 `_t0=refTime` 需让回调把 refTime 传进去,或 syncIfDirty 也取 frameStamp——简单做法:回调先 `syncIfDirty(refTime)` 设 `_t0=refTime`,再 `interpolate(refTime)`。首帧 elapsed=0 不动,之后滑行。）
- [ ] **Step 3: 构建** → 成功。
- [ ] **Step 4: 验证(联网,挂久一点看滑行)** `EARTH_FLIGHTS=1 --goto 49 6 1500 EARTH_AUTOCAP=1500 EARTH_FRAME_SLEEP_MS=30` → 截图见航班(滑行靠肉眼/真机更明显;headless 至少确认不崩、位置随帧变化合理)。真机确认平滑滑行。
- [ ] **Step 5: 提交** `feat(earth): smooth per-frame flight motion interpolation`。

---

## Task 6: 点击详情(拾取 + 右上角信息面板)

**Files:** Modify flight_data.cpp, EarthControlUI.h, earth_main.cpp

- [ ] **Step 1: flight_data.cpp** 加 `getSelected/clearSelected/pickAt`(**完全仿 quake** `pickAt`:前半球过滤 + VPW 投影 + 像素容差;选中存 `FlightInfo`)+ `FlightPickHandler`(仿 quake,左键点击非拖动 + Y 翻转守 viewport)+ configureFlightLayer 注册 handler。`FlightInfo` 填 callsign/country/altM/velMS/headingDeg。
- [ ] **Step 2: EarthControlUI.h** 加 `#include "flight_data.h"` + `FlightLayer* _flight=nullptr;` + 在右上角信息面板区(地震详情同款,End() 之后)加「航班详情 Flight」窗口(右上角锚定、可关闭 `&open`、`if(!open) clearSelected()`):显示 呼号/国家/高度 km/速度 km·h⁻¹(velMS×3.6)/航向°。**多个右上角信息面板沿 Y 叠放**(地震卡在上、航班卡其下;用各自 `SetNextWindowPos` 的 y 偏移错开,如地震 y=20、航班 y=20 但若两者都可能显示,航班 y 设为 20 + 估高;简单起见航班锚 `io.DisplaySize.x-20, 160`)。
- [ ] **Step 3: earth_main.cpp** `ctrlUI->_flight = flightLayer;`(在 `ctrlUI->_quake=...` 旁)。
- [ ] **Step 4: 构建** → 成功;headless 冒烟不崩(点击详情真机验)。
- [ ] **Step 5: 提交** `feat(earth): flight click-to-details + top-right info panel`。

---

## Task 7: 验证 + .app 重打包 + 文档/记忆

- [ ] **Step 1: 真机交互(用户)**:开「航班 (OpenSky)」→ 视口内航班、箭头朝向、高度配色、**平滑滑行**;点击→右上角航班详情;缩放/拖动重查 bbox;与地震/降水/云图共存;**4 类回归不现**;流畅。
- [ ] **Step 2: headless 批次**:fixture 朝向/配色;联网欧洲航班;4 类回归(海洋不橙/晨昏线/倾斜不穿模)；性能(视口数百架跑足帧不卡)。
- [ ] **Step 3: 重打包 `.app`** + 打包二进制验证。
- [ ] **Step 4: 文档** HANDOFF + EarthExplorer.md（航班层 + `EARTH_FLIGHTS`/`EARTH_FLIGHTS_FILE` 钩子）。
- [ ] **Step 5: 提交 + push**(用户确认后)。
- [ ] **Step 6: 记忆** `earth-flight-layer-done.md`(type: project):OpenSky 视口 bbox、航向图标点精灵、每帧滑行插值、复用地震模板、不碰 globe 着色器；更新 [[earth-realtime-data-overlays]]、MEMORY.md。

---

## Self-Review
**Spec 覆盖**:OpenSky 视口 bbox(Task2/4)、航向图标+高度配色(Task3)、滑行插值(Task5)、点击右上角详情(Task6)、开关+钩子(Task4)、验证含 4 回归+性能(Task7)。
**类型一致**:`FlightInfo`/`FlightLayer`(flight_data.h)在 UI/earth_main 一致;内部 `Flight`(含 ecef + lon/lat/heading/vel 基准供插值)Task2 定义、Task3/5/6 复用;`configureFlightLayer(viewer,earthRoot,mainFolder,FlightLayer**)` 三处一致;`setViewBBox` 主线程写/worker 读(mutex)。
**占位符**:无 TODO;临时验证代码标注"Task N 删/改";fixture 具体。
**回归边界**:独立点图层+独立着色器,不碰 globe 着色器/太阳/海洋/几何;GL 只主线程;worker 只 HTTP+解析;bbox 主线程算。
