# 地震实时数据层(Step 1)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 EarthExplorer 上叠加 USGS 实时地震点标记(过去1天 M2.5+,~60s 自动刷新,点击看详情)。

**Architecture:** 新建自包含模块 `quake_data.cpp`(仿 `city_data.cpp`),内部:libhv 后台线程抓取 USGS GeoJSON → picojson 解析 → 主线程重建 `GL_POINTS` 点精灵几何(大小∝震级、颜色∝深度);`GUIEventHandler` 屏幕拾取(前半球过滤)→ ImGui 详情卡片。**完全不碰 globe 着色器/太阳/海洋**,在 4 类已知回归(海洋橙/删晨昏线/倾斜穿模/程序化海洋)之外。

**Tech Stack:** C++ / OpenSceneGraph(CORE GL4.1)、libhv(已编进 `osgVerseDependency`)、picojson(`3rdparty/picojson.h`)、ImGui(既有 UI)。

**验证方式说明:** 本工程是图形应用,无单元测试框架。每个任务的"测试"= 增量编译 + headless 截图/日志断言(用 `EARTH_QUAKES_FILE` 喂固定样本做确定性验证);点击交互项需运行 `.app` 人工确认。增量构建命令:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
headless 截图命令模板:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=400 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1 \
<额外 env> ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
# 然后 Read /tmp/earth_capture_0.png + grep /tmp/run.log
```

---

## File Structure

- **Create** `applications/earth_explorer/quake_data.h` — 公共接口:`QuakeInfo` 结构 + `QuakeLayer` 抽象类 + `configureQuakeData()` 声明。被 `quake_data.cpp`、`EarthControlUI.h`、`earth_main.cpp` 共用。
- **Create** `applications/earth_explorer/quake_data.cpp` — 全部实现:解析、抓取线程、几何重建、拾取 handler、`QuakeLayer` 具体类。
- **Modify** `applications/earth_explorer/CMakeLists.txt` — 加 `quake_data.cpp` 到源列表 + libhv include 目录 + `-DHV_STATICLIB`。
- **Modify** `applications/earth_explorer/earth_main.cpp` — extern 声明、调用 `configureQuakeData` 挂到 sceneCamera、注册 `quakes` 图层、`EARTH_QUAKES` env 钩子、给 `ctrlUI->_quake` 注入指针。
- **Modify** `applications/earth_explorer/EarthControlUI.h` — 加 `QuakeLayer* _quake` 成员 + 「地震详情」ImGui 区块。
- **Create** `applications/earth_explorer/test/quakes_fixture.geojson` — 3 个固定地震样本(浅/中/深、不同震级)供确定性验证。

---

## Task 1: 骨架模块 + CMake 接线(可编译、零行为)

**Files:**
- Create: `applications/earth_explorer/quake_data.h`
- Create: `applications/earth_explorer/quake_data.cpp`
- Modify: `applications/earth_explorer/CMakeLists.txt`
- Modify: `applications/earth_explorer/earth_main.cpp`

- [ ] **Step 1: 写 `quake_data.h`(公共接口)**

```cpp
#ifndef EARTH_QUAKE_DATA_H
#define EARTH_QUAKE_DATA_H

#include <string>
#include <osg/Node>
#include <osgViewer/View>

// 单条地震记录(也是 UI 详情卡片的数据载体)。
struct QuakeInfo
{
    double lon = 0.0, lat = 0.0;   // 度
    double depthKm = 0.0, mag = 0.0;
    long long timeMs = 0;          // 毫秒 epoch (UTC)
    std::string place, url;
    bool valid = false;            // false = 当前无选中
};

// 地震层对外接口。EarthControlUI / earth_main 只依赖这个抽象,不碰内部实现。
class QuakeLayer
{
public:
    virtual ~QuakeLayer() {}
    virtual void setEnabled(bool on) = 0;       // 图层开关:开→联网抓取;关→线程空转+隐藏
    virtual bool isEnabled() const = 0;
    virtual QuakeInfo getSelected() const = 0;  // 点击选中的地震(valid=false 表示无)
    virtual void clearSelected() = 0;
};

// 构建地震层场景节点。返回挂到 sceneCamera 的 osg::Group;
// 通过 outLayer 输出 QuakeLayer* 供 UI/main 控制(生命周期随返回节点)。
extern osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                                     const std::string& mainFolder, QuakeLayer** outLayer);

#endif
```

- [ ] **Step 2: 写 `quake_data.cpp` 骨架(空实现,先保证编译 + 接线)**

```cpp
#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Point>
#include <osgDB/FileUtils>
#include <osgGA/GUIEventHandler>
#include <OpenThreads/Thread>
#include <OpenThreads/Mutex>
#include <modeling/Math.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <readerwriter/EarthManipulator.h>
#include <VerseCommon.h>
#include <iostream>
#include <vector>
#include "quake_data.h"

namespace
{
    // 内部具体类(后续任务逐步填充)。
    class QuakeLayerImpl : public osg::Referenced, public QuakeLayer
    {
    public:
        QuakeLayerImpl() : _enabled(false) {}
        virtual void setEnabled(bool on) { _enabled = on; if (_root.valid()) _root->setNodeMask(on ? ~0 : 0); }
        virtual bool isEnabled() const { return _enabled; }
        virtual QuakeInfo getSelected() const { return QuakeInfo(); }
        virtual void clearSelected() {}

        osg::Group* root() { return _root.get(); }
        void buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);   // 默认关
        }
    protected:
        osg::ref_ptr<osg::Group> _root;
        bool _enabled;
    };
}

osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                              const std::string& mainFolder, QuakeLayer** outLayer)
{
    osg::ref_ptr<QuakeLayerImpl> impl = new QuakeLayerImpl;
    impl->buildScene();
    if (outLayer) *outLayer = impl.get();
    // 让返回节点持有 impl 的引用(impl 同时是 Referenced),保证生命周期。
    impl->root()->setUserData(impl.get());
    return impl->root();
}
```

- [ ] **Step 3: 改 `CMakeLists.txt`(加源 + libhv)**

把
```cmake
SET(EXECUTABLE_FILES
    earth_main.cpp ui.cpp render_effects.cpp
    city_data.cpp ReaderWriterCSV.cpp
)
```
改成
```cmake
SET(EXECUTABLE_FILES
    earth_main.cpp ui.cpp render_effects.cpp
    city_data.cpp ReaderWriterCSV.cpp quake_data.cpp
)
ADD_DEFINITIONS(-DHV_STATICLIB)
INCLUDE_DIRECTORIES(../../3rdparty/libhv ../../3rdparty/libhv/all)
```
(`-DHV_STATICLIB` + include 目录与 `plugins/osgdb_web/CMakeLists.txt` 一致;libhv 已编进 `osgVerseDependency`,executable 已链接它,无需额外 link。)

- [ ] **Step 4: 改 `earth_main.cpp`(extern 声明 + 挂节点)**

在文件顶部 extern 声明区(`configureCityData` 声明之后,约 42-46 行)加:
```cpp
#include "quake_data.h"
extern osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                                     const std::string& mainFolder, QuakeLayer** outLayer);
```
在 `sceneCamera->addChild(configureCityData(...))`(约 350-351 行)之后加:
```cpp
    QuakeLayer* quakeLayer = nullptr;
    sceneCamera->addChild(configureQuakeData(viewer, earthRoot.get(), mainFolder, &quakeLayer));
```

- [ ] **Step 5: 增量编译**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -20
```
Expected: 编译成功,`[100%] ... osgVerse_EarthExplorer` 安装完成,无报错。

- [ ] **Step 6: headless 冒烟(确认 app 仍正常、无回归)**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=400 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
```
然后用 Read 工具看 `/tmp/earth_capture_0.png`。
Expected: 正常地球渲染(与改动前一致),无地震点(默认关),无晨昏线缺失/海洋橙等异常。

- [ ] **Step 7: 提交**

```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/quake_data.h applications/earth_explorer/quake_data.cpp \
        applications/earth_explorer/CMakeLists.txt applications/earth_explorer/earth_main.cpp
git commit -m "feat(earth): quake layer skeleton + CMake/libhv wiring (no behavior yet)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: GeoJSON 解析 + 抓取(EARTH_QUAKES_FILE 固定样本 + 日志断言)

**Files:**
- Create: `applications/earth_explorer/test/quakes_fixture.geojson`
- Modify: `applications/earth_explorer/quake_data.cpp`

- [ ] **Step 1: 写固定样本 `test/quakes_fixture.geojson`**

3 个地震:浅(红)/中(橙)/深(蓝),不同震级。坐标分散便于看点位。
```json
{
  "type": "FeatureCollection",
  "features": [
    {
      "type": "Feature",
      "properties": { "mag": 6.2, "place": "Shallow Test near Tokyo", "time": 1718900000000, "url": "https://earthquake.usgs.gov/eq1" },
      "geometry": { "type": "Point", "coordinates": [139.69, 35.69, 10.0] }
    },
    {
      "type": "Feature",
      "properties": { "mag": 4.8, "place": "Mid Test near Lima", "time": 1718890000000, "url": "https://earthquake.usgs.gov/eq2" },
      "geometry": { "type": "Point", "coordinates": [-77.04, -12.05, 150.0] }
    },
    {
      "type": "Feature",
      "properties": { "mag": 3.1, "place": "Deep Test near Fiji", "time": 1718880000000, "url": "https://earthquake.usgs.gov/eq3" },
      "geometry": { "type": "Point", "coordinates": [178.0, -17.7, 550.0] }
    }
  ]
}
```

- [ ] **Step 2: 在 `quake_data.cpp` 顶部加 include + Quake 内部结构 + 解析函数**

在 `#include "quake_data.h"` 之前加:
```cpp
#include <picojson.h>
#include "3rdparty/libhv/all/client/requests.h"
#include <fstream>
#include <sstream>
```
在匿名 namespace 内、`QuakeLayerImpl` 之前加内部记录结构 + 解析函数:
```cpp
    struct Quake
    {
        double lon, lat, depthKm, mag;
        long long timeMs;
        std::string place, url;
        osg::Vec3d ecef;   // 预算好的世界坐标(抬升后)
    };

    // USGS feed:M2.5+、过去24h、无 key、HTTPS、约每分钟更新。
    static const char* kQuakeFeedUrl =
        "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day.geojson";

    static const double kQuakeLiftMeters = 3000.0;  // 标记抬离地表,防 z-fight、保可见

    // 解析 GeoJSON FeatureCollection → vector<Quake>。容错:坏 feature 跳过,不抛。
    std::vector<Quake> parseQuakeGeoJson(const std::string& text)
    {
        std::vector<Quake> out;
        picojson::value root;
        std::string err = picojson::parse(root, text);
        if (!err.empty() || !root.is<picojson::object>()) {
            std::cout << "[Quake] JSON parse error: " << err << "\n"; return out;
        }
        const picojson::value& feats = root.get("features");
        if (!feats.is<picojson::array>()) return out;
        const picojson::array& arr = feats.get<picojson::array>();
        for (size_t i = 0; i < arr.size(); ++i)
        {
            const picojson::value& f = arr[i];
            if (!f.is<picojson::object>()) continue;
            const picojson::value& geom = f.get("geometry");
            const picojson::value& props = f.get("properties");
            if (!geom.is<picojson::object>() || !props.is<picojson::object>()) continue;
            const picojson::value& coords = geom.get("coordinates");
            if (!coords.is<picojson::array>()) continue;
            const picojson::array& c = coords.get<picojson::array>();
            if (c.size() < 3 || !c[0].is<double>() || !c[1].is<double>() || !c[2].is<double>()) continue;

            Quake q;
            q.lon = c[0].get<double>(); q.lat = c[1].get<double>(); q.depthKm = c[2].get<double>();
            q.mag = props.get("mag").is<double>() ? props.get("mag").get<double>() : 0.0;
            q.timeMs = props.get("time").is<double>() ? (long long)props.get("time").get<double>() : 0;
            q.place = props.get("place").is<std::string>() ? props.get("place").get<std::string>() : "";
            q.url = props.get("url").is<std::string>() ? props.get("url").get<std::string>() : "";
            q.ecef = osgVerse::Coordinate::convertLLAtoECEF(osg::Vec3d(
                osg::DegreesToRadians(q.lat), osg::DegreesToRadians(q.lon), kQuakeLiftMeters));
            out.push_back(q);
        }
        return out;
    }

    // 取数据:优先 EARTH_QUAKES_FILE 本地样本(离线确定性验证),否则联网 USGS。
    std::vector<Quake> fetchQuakes()
    {
        const char* fixtureFile = getenv("EARTH_QUAKES_FILE");
        if (fixtureFile && *fixtureFile)
        {
            std::ifstream in(fixtureFile);
            if (!in) { std::cout << "[Quake] fixture open failed: " << fixtureFile << "\n"; return {}; }
            std::stringstream ss; ss << in.rdbuf();
            std::vector<Quake> qs = parseQuakeGeoJson(ss.str());
            std::cout << "[Quake] Parsed " << qs.size() << " quakes (fixture)\n";
            return qs;
        }
        auto resp = requests::get(kQuakeFeedUrl);
        if (!resp || resp->status_code != 200)
        {
            std::cout << "[Quake] fetch failed, status="
                      << (resp ? resp->status_code : -1) << "\n";
            return {};
        }
        std::vector<Quake> qs = parseQuakeGeoJson(resp->body);
        std::cout << "[Quake] Parsed " << qs.size() << " quakes (network)\n";
        return qs;
    }
```

- [ ] **Step 3: 在 `QuakeLayerImpl::buildScene()` 里临时调用 `fetchQuakes()` 验证解析**

把 `buildScene()` 临时改为(本任务只验证解析链路,渲染在 Task 3):
```cpp
        void buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);
            std::vector<Quake> qs = fetchQuakes();   // 临时:验证解析(Task 3 移到线程/回调)
            for (size_t i = 0; i < qs.size(); ++i)
                std::cout << "[Quake]   M" << qs[i].mag << " depth=" << qs[i].depthKm
                          << "km " << qs[i].place << "\n";
        }
```

- [ ] **Step 4: 增量编译**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -20
```
Expected: 编译成功,无报错(尤其确认 `requests.h`、`picojson.h` 找到)。

- [ ] **Step 5: 用固定样本跑 headless,断言解析数 = 3**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_QUAKES_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/quakes_fixture.geojson \
EARTH_AUTOCAP=200 ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep "\[Quake\]" /tmp/run.log
```
Expected 输出含:
```
[Quake] Parsed 3 quakes (fixture)
[Quake]   M6.2 depth=10km Shallow Test near Tokyo
[Quake]   M4.8 depth=150km Mid Test near Lima
[Quake]   M3.1 depth=550km Deep Test near Fiji
```

- [ ] **Step 6: (可选)验证联网路径**(需外网;失败不阻塞,Task 4 会再测)

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=200 ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep "\[Quake\] Parsed" /tmp/run.log
```
Expected: `[Quake] Parsed N quakes (network)`,N 为当前真实地震数(通常数十)。

- [ ] **Step 7: 提交**

```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/quake_data.cpp applications/earth_explorer/test/quakes_fixture.geojson
git commit -m "feat(earth): quake GeoJSON parse + USGS/libhv fetch (EARTH_QUAKES_FILE hook)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: 渲染点标记(GL_POINTS,大小∝震级、颜色∝深度)

**Files:**
- Modify: `applications/earth_explorer/quake_data.cpp`

- [ ] **Step 1: 加点精灵 shader 源 + 深度配色/尺寸辅助函数(匿名 namespace 内)**

```cpp
    // 点精灵:VS 写 gl_PointSize(取自 texcoord0.x),FS 画圆盘、圆外 discard。
    // 不接大气散射 → 标记昼夜两侧都清晰可读。沿用 VERSE 宏与 MRT 双输出(同 city_data)。
    const char* quakeVertCode = {
        "VERSE_VS_OUT vec4 pointColor;\n"
        "void main() {\n"
        "    pointColor = osg_Color;\n"
        "    gl_PointSize = osg_MultiTexCoord0.x;\n"
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };
    const char* quakeFragCode = {
        "VERSE_FS_IN vec4 pointColor;\n"
        "#ifdef VERSE_GLES3\n"
        "layout(location = 0) VERSE_FS_OUT vec4 fragColor;\n"
        "layout(location = 1) VERSE_FS_OUT vec4 fragOrigin;\n"
        "#endif\n"
        "void main() {\n"
        "    vec2 d = gl_PointCoord - vec2(0.5);\n"
        "    float r2 = dot(d, d);\n"
        "    if (r2 > 0.25) discard;\n"
        "    float edge = smoothstep(0.25, 0.16, r2);\n"   // 内实外略亮边
        "    vec3 rgb = mix(pointColor.rgb * 0.7, pointColor.rgb, edge);\n"
        "#ifdef VERSE_GLES3\n"
        "    fragColor = vec4(rgb, 1.0); fragOrigin = vec4(1.0);\n"
        "#else\n"
        "    gl_FragData[0] = vec4(rgb, 1.0); gl_FragData[1] = vec4(1.0);\n"
        "#endif\n"
        "}\n"
    };

    // 深度→颜色(USGS 习惯:浅红 → 中橙黄 → 深蓝)。
    osg::Vec4 depthColor(double depthKm)
    {
        if (depthKm < 70.0)  return osg::Vec4(0.90f, 0.18f, 0.15f, 1.0f);   // 浅:红
        if (depthKm < 300.0) return osg::Vec4(0.98f, 0.66f, 0.12f, 1.0f);   // 中:橙黄
        return osg::Vec4(0.20f, 0.45f, 0.95f, 1.0f);                        // 深:蓝
    }

    // 震级→屏幕点像素大小。
    float magSizePx(double mag)
    {
        float s = 4.0f + (float)(mag - 2.5) * 3.0f;
        return s < 4.0f ? 4.0f : (s > 28.0f ? 28.0f : s);
    }

#ifndef GL_PROGRAM_POINT_SIZE
#define GL_PROGRAM_POINT_SIZE 0x8642
#endif

    // 由一批 Quake 构建一个 Geode(GL_POINTS)。
    osg::Geode* buildQuakeGeode(const std::vector<Quake>& qs, osg::StateSet* sharedSS)
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        osg::ref_ptr<osg::Vec2Array> sizes = new osg::Vec2Array;  // .x = 点像素大小
        for (size_t i = 0; i < qs.size(); ++i)
        {
            verts->push_back(qs[i].ecef);
            colors->push_back(depthColor(qs[i].depthKm));
            sizes->push_back(osg::Vec2(magSizePx(qs[i].mag), 0.0f));
        }
        geom->setVertexArray(verts.get());
        geom->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
        geom->setTexCoordArray(0, sizes.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_POINTS, 0, (GLsizei)verts->size()));
        geom->setUseDisplayList(false); geom->setUseVertexBufferObjects(true);
        geom->setCullingActive(false);   // 点集包围盒小,避免被整体剔除

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geom.get());
        geode->setStateSet(sharedSS);
        return geode.release();
    }
```

- [ ] **Step 2: 给 `QuakeLayerImpl` 加几何 stateset 构建 + 重建方法 + update 回调**

在 `QuakeLayerImpl` 类内加成员与方法(替换 Task 1/2 的临时 `buildScene`):
```cpp
    public:
        // 后台线程交付的新快照(加锁)。
        void postSnapshot(const std::vector<Quake>& qs)
        { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex); _pending = qs; _dirty = true; }

        // 主线程(update 遍历)调用:若有新数据则重建 geode。
        void syncIfDirty()
        {
            std::vector<Quake> qs;
            { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex);
              if (!_dirty) return; qs = _pending; _dirty = false; }
            _quakes = qs;   // 给拾取用
            if (_geode.valid()) _root->removeChild(_geode.get());
            _geode = buildQuakeGeode(qs, _ss.get());
            _root->addChild(_geode.get());
        }

        void buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);
            _dirty = false;

            // 共享 stateset:shader + 启用 program point size。
            osg::Shader* vs = new osg::Shader(osg::Shader::VERTEX, quakeVertCode);
            osg::Shader* fs = new osg::Shader(osg::Shader::FRAGMENT, quakeFragCode);
            vs->setName("Quake_VS"); fs->setName("Quake_FS");
            osgVerse::Pipeline::createShaderDefinitions(vs, 100, 130);
            osgVerse::Pipeline::createShaderDefinitions(fs, 100, 130);
            osg::ref_ptr<osg::Program> prog = new osg::Program;
            prog->addShader(vs); prog->addShader(fs);
            _ss = new osg::StateSet;
            _ss->setAttributeAndModes(prog.get(), osg::StateAttribute::ON);
            _ss->setMode(GL_PROGRAM_POINT_SIZE, osg::StateAttribute::ON);
            _ss->setRenderBinDetails(11, "RenderBin");  // 在地表之后画

            // update 回调:每帧检查 dirty(便宜)。
            _root->addUpdateCallback(new SyncCallback(this));
        }

        const std::vector<Quake>& quakes() const { return _quakes; }
```
加成员:
```cpp
    protected:
        osg::ref_ptr<osg::Geode> _geode;
        osg::ref_ptr<osg::StateSet> _ss;
        std::vector<Quake> _quakes, _pending;
        OpenThreads::Mutex _mutex;
        bool _dirty;
```
在匿名 namespace 内、`QuakeLayerImpl` 之后(或之前用前向声明)加回调类:
```cpp
    class SyncCallback : public osg::NodeCallback
    {
    public:
        SyncCallback(QuakeLayerImpl* o) : _owner(o) {}
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
        { _owner->syncIfDirty(); traverse(node, nv); }
    protected:
        QuakeLayerImpl* _owner;
    };
```
> 注:`SyncCallback` 引用 `QuakeLayerImpl`,需把 `QuakeLayerImpl` 的 `syncIfDirty`/成员放在 `SyncCallback` 可见处。实现时把 `SyncCallback` 定义放在 `QuakeLayerImpl` 之后,并在 `buildScene` 里 `new SyncCallback(this)`(C++ 允许成员函数体内引用后定义的类型,只要在该 .cpp 内可见;若顺序报错,前向声明 `class QuakeLayerImpl;` 并把回调实现挪到类后)。

- [ ] **Step 3: 临时在 `configureQuakeData` 里直接灌一次固定样本(本任务先验证渲染,不接线程)**

把 `configureQuakeData` 改为:
```cpp
osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                              const std::string& mainFolder, QuakeLayer** outLayer)
{
    osg::ref_ptr<QuakeLayerImpl> impl = new QuakeLayerImpl;
    impl->buildScene();
    impl->root()->setUserData(impl.get());
    if (outLayer) *outLayer = impl.get();

    // 临时:本任务直接同步取一次数据并 post(Task 4 改为后台线程)。
    impl->postSnapshot(fetchQuakes());
    impl->setEnabled(true);   // 临时:强制开,便于截图(Task 4 改回默认关)
    return impl->root();
}
```

- [ ] **Step 4: 增量编译**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -20
```
Expected: 编译成功。

- [ ] **Step 5: headless 截图验证点标记(固定样本,全球视角)**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_QUAKES_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/quakes_fixture.geojson \
EARTH_AUTOCAP=400 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
```
然后 Read `/tmp/earth_capture_0.png`。
Expected: 全球视角下可见 3 个圆点 —— 东京附近大红点(M6.2 浅)、利马附近中橙点(M4.8 中)、斐济附近小蓝点(M3.1 深);大小/颜色与震级/深度对应;地球本体渲染正常(无晨昏线缺失/海洋橙)。背面(地球另一侧)的点应被遮挡不可见。

- [ ] **Step 6: 提交**

```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/quake_data.cpp
git commit -m "feat(earth): render quake GL_POINTS markers (size~mag, color~depth)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: 后台自动刷新线程(~60s)+ 图层开关接线 + EARTH_QUAKES 钩子

**Files:**
- Modify: `applications/earth_explorer/quake_data.cpp`
- Modify: `applications/earth_explorer/earth_main.cpp`

- [ ] **Step 1: 在 `quake_data.cpp` 加抓取线程类(匿名 namespace 内)**

```cpp
    class FetchThread : public OpenThreads::Thread
    {
    public:
        FetchThread(QuakeLayerImpl* o) : _owner(o), _done(false), _lastFetch(-1.0e9) {}
        virtual int cancel() { _done = true; return OpenThreads::Thread::cancel(); }
        virtual void run()
        {
            const double intervalSec = 60.0;
            int tick = 0;
            while (!_done)
            {
                // 仅图层开启时联网;每 ~60s 一次。分片 sleep 便于秒退。
                if (_owner->isEnabled())
                {
                    if (tick <= 0)
                    {
                        _owner->postSnapshot(fetchQuakesShared());
                        tick = (int)(intervalSec * 10);   // 100ms × 600 = 60s
                    }
                    else tick--;
                }
                else tick = 0;   // 关闭时,下次开启立即抓
                OpenThreads::Thread::microSleep(100000);  // 100ms
            }
            _done = true;
        }
    protected:
        QuakeLayerImpl* _owner;
        bool _done; double _lastFetch;
    };
```
> `fetchQuakesShared()` = 把 Task 2 的 `fetchQuakes()` 直接用即可(同名调用)。线程内只调用 `fetchQuakes()` + `postSnapshot()`,不碰 GL/场景图,安全。把上面 `fetchQuakesShared()` 改写为 `fetchQuakes()`。

- [ ] **Step 2: `QuakeLayerImpl` 持有并管理线程,析构时停**

给 `QuakeLayerImpl` 加成员与启停:
```cpp
    public:
        void startFetch() { if (!_thread) { _thread = new FetchThread(this); _thread->startThread(); } }
    protected:
        virtual ~QuakeLayerImpl()
        { if (_thread) { _thread->cancel(); _thread->join(); delete _thread; _thread = nullptr; } }
        FetchThread* _thread = nullptr;
```
> `FetchThread` 引用 `QuakeLayerImpl`,把其定义放在 `QuakeLayerImpl` 之后;`startFetch` 实现可挪到类后或用前向声明。

- [ ] **Step 3: 改回 `configureQuakeData`(默认关、启动线程,去掉 Task 3 临时强开)**

```cpp
osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                              const std::string& mainFolder, QuakeLayer** outLayer)
{
    osg::ref_ptr<QuakeLayerImpl> impl = new QuakeLayerImpl;
    impl->buildScene();
    impl->root()->setUserData(impl.get());
    if (outLayer) *outLayer = impl.get();
    impl->startFetch();   // 线程常驻;仅 isEnabled() 时才真正联网
    return impl->root();
}
```

- [ ] **Step 4: 在 `earth_main.cpp` 注册 `quakes` 图层 + EARTH_QUAKES 钩子**

在图层注册块(约 419-426 的 `clouds` 之后,`}` 之前)加:
```cpp
        OverlayLayer quakes; quakes.id = "quakes"; quakes.displayName = u8"地震 (USGS)";
        quakes.group = u8"实时数据 / Live"; quakes.enabled = false; quakes.hasOpacity = false;
        QuakeLayer* qptr = quakeLayer;
        quakes.apply = [qptr](const OverlayLayer& l) { if (qptr) qptr->setEnabled(l.enabled); };
        layerMgr.add(quakes);
```
在初值同步块(约 431-443 的 `clouds` 同步之后)加:
```cpp
    if (OverlayLayer* qk = layerMgr.find("quakes"))
    {
        // EARTH_QUAKES=1 强制开启该层(headless 验证用,同 EARTH_CLOUDS 钩子)。
        const char* qkEnv = getenv("EARTH_QUAKES");
        if (qkEnv && *qkEnv) qk->enabled = (atoi(qkEnv) != 0);
        layerMgr.setEnabled("quakes", qk->enabled);
    }
```
> `quakeLayer` 是 Task 1 Step 4 声明的局部变量,在此处可见(同一函数作用域,注册块之前已赋值)。确认 `configureQuakeData` 调用在图层注册块之前(本计划中它在 ~351 行,注册块在 ~404 行,顺序正确)。

- [ ] **Step 5: 增量编译**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -20
```
Expected: 编译成功。

- [ ] **Step 6: headless 验证 EARTH_QUAKES 开关 + 固定样本仍渲染**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_QUAKES=1 \
EARTH_QUAKES_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/quakes_fixture.geojson \
EARTH_AUTOCAP=500 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep "\[Quake\]" /tmp/run.log
```
然后 Read `/tmp/earth_capture_0.png`。
Expected: 日志含 `[Quake] Parsed 3 quakes (fixture)`;截图与 Task 3 一致显示 3 个点(经线程→postSnapshot→update 回调链路);默认不带 `EARTH_QUAKES=1` 时无点(可另跑一次确认默认关)。

- [ ] **Step 7: 验证默认关闭(无 EARTH_QUAKES)不联网不显示**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=300 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep -c "Parsed" /tmp/run.log
```
Expected: `0`(默认关 → 线程空转不抓取);截图无地震点。

- [ ] **Step 8: 提交**

```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/quake_data.cpp applications/earth_explorer/earth_main.cpp
git commit -m "feat(earth): quake background auto-refresh thread (~60s) + layer toggle + EARTH_QUAKES

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: 点击详情(屏幕拾取 + ImGui 卡片)

**Files:**
- Modify: `applications/earth_explorer/quake_data.cpp`
- Modify: `applications/earth_explorer/EarthControlUI.h`
- Modify: `applications/earth_explorer/earth_main.cpp`

- [ ] **Step 1: `QuakeLayerImpl` 实现选中状态 + 拾取查询**

把 `getSelected`/`clearSelected` 实现为:
```cpp
    public:
        virtual QuakeInfo getSelected() const
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_selMutex);
            return _selected;
        }
        virtual void clearSelected()
        { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_selMutex); _selected = QuakeInfo(); }

        // 屏幕拾取:给定相机与窗口鼠标坐标(y 向上),选最近的前半球地震。
        void pickAt(osg::Camera* cam, float mx, float my)
        {
            if (!_enabled || _quakes.empty()) return;
            osg::Vec3d eye, center, up; cam->getViewMatrixAsLookAt(eye, center, up);
            osg::Matrixd VPW = cam->getViewMatrix() * cam->getProjectionMatrix()
                             * cam->getViewport()->computeWindowMatrix();
            double bestD2 = 1e18; int best = -1;
            for (size_t i = 0; i < _quakes.size(); ++i)
            {
                const osg::Vec3d& P = _quakes[i].ecef;
                if ((eye * P) <= (P * P)) continue;          // 前半球:dot(eye,P) > |P|^2
                osg::Vec3d win = P * VPW;
                if (win.z() < 0.0 || win.z() > 1.0) continue; // 裁剪范围外
                double d2 = (win.x() - mx) * (win.x() - mx) + (win.y() - my) * (win.y() - my);
                float tol = magSizePx(_quakes[i].mag) * 0.5f + 6.0f;
                if (d2 < (double)(tol * tol) && d2 < bestD2) { bestD2 = d2; best = (int)i; }
            }
            if (best >= 0)
            {
                const Quake& q = _quakes[best];
                QuakeInfo info; info.valid = true;
                info.lon = q.lon; info.lat = q.lat; info.depthKm = q.depthKm; info.mag = q.mag;
                info.timeMs = q.timeMs; info.place = q.place; info.url = q.url;
                OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_selMutex); _selected = info;
            }
        }
    protected:
        QuakeInfo _selected;
        mutable OpenThreads::Mutex _selMutex;
```

- [ ] **Step 2: 加拾取 `GUIEventHandler`(匿名 namespace 内)+ 在 configureQuakeData 注册**

```cpp
    class QuakePickHandler : public osgGA::GUIEventHandler
    {
    public:
        QuakePickHandler(QuakeLayerImpl* o) : _owner(o), _downX(0), _downY(0) {}
        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
        {
            if (ea.getEventType() == osgGA::GUIEventAdapter::PUSH
                && ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
            { _downX = ea.getX(); _downY = ea.getY(); }
            else if (ea.getEventType() == osgGA::GUIEventAdapter::RELEASE
                     && ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
            {
                float dx = ea.getX() - _downX, dy = ea.getY() - _downY;
                if (dx * dx + dy * dy < 25.0f)   // <5px 位移 = 点击(非拖动)
                {
                    osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
                    osg::Camera* cam = view->getCamera();
                    float my = ea.getY();
                    if (ea.getMouseYOrientation() == osgGA::GUIEventAdapter::Y_INCREASING_DOWNWARDS)
                        my = cam->getViewport()->height() - my;
                    _owner->pickAt(cam, ea.getX(), my);
                }
            }
            return false;   // 不拦截,放行给 manipulator
        }
    protected:
        QuakeLayerImpl* _owner; float _downX, _downY;
    };
```
在 `configureQuakeData` 末尾(`return` 之前)加:
```cpp
    viewer.addEventHandler(new QuakePickHandler(impl.get()));
```

- [ ] **Step 3: `EarthControlUI.h` 加 `_quake` 指针 + 详情区块**

顶部 include 区(`#include "LayerManager.h"` 之后)加:
```cpp
#include "quake_data.h"
```
在 struct 成员区(`LayerManager* _layers = nullptr;` 之后)加:
```cpp
    QuakeLayer* _quake = nullptr;      // 由 main 注入
```
在「图层 Layers」区块之后(约第 176 行 `}` 之后、「跳转 Go To」之前)加:
```cpp
            // ---- 地震详情 ----
            if (_quake)
            {
                QuakeInfo q = _quake->getSelected();
                if (q.valid && ImGui::CollapsingHeader(u8"地震详情 Quake", ImGuiTreeNodeFlags_DefaultOpen))
                {
                    ImGui::Text(u8"震级 Mag: M %.1f", q.mag);
                    ImGui::Text(u8"深度 Depth: %.1f km", q.depthKm);
                    ImGui::Text(u8"位置 Place: %s", q.place.c_str());
                    ImGui::Text(u8"经纬 LatLon: %.3f, %.3f", q.lat, q.lon);
                    // 相对时间
                    long long nowMs = (long long)time(NULL) * 1000;
                    long long ageMin = (nowMs - q.timeMs) / 60000;
                    if (ageMin < 60) ImGui::Text(u8"时间 Time: %lld 分钟前", ageMin);
                    else if (ageMin < 1440) ImGui::Text(u8"时间 Time: %.1f 小时前", ageMin / 60.0);
                    else ImGui::Text(u8"时间 Time: %.1f 天前", ageMin / 1440.0);
                    ImGui::TextWrapped(u8"%s", q.url.c_str());
                    if (ImGui::Button(u8"关闭 Close")) _quake->clearSelected();
                }
            }
```

- [ ] **Step 4: `earth_main.cpp` 注入 `_quake` 指针**

在 `ctrlUI->_layers = &layerMgr;`(约 449 行)之后加:
```cpp
    ctrlUI->_quake = quakeLayer;
```

- [ ] **Step 5: 增量编译**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -20
```
Expected: 编译成功(确认 `EarthControlUI.h` 引入 `quake_data.h` 后无循环依赖/重定义)。

- [ ] **Step 6: headless 冒烟(确认不崩、详情区块代码路径编入)**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_QUAKES=1 \
EARTH_QUAKES_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/quakes_fixture.geojson \
EARTH_AUTOCAP=400 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
```
然后 Read `/tmp/earth_capture_0.png`。
Expected: 仍正常显示 3 个点 + 控制面板(headless 无法点击,详情卡片此处不出现,属正常);无崩溃。

- [ ] **Step 7: 提交**

```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/quake_data.cpp applications/earth_explorer/EarthControlUI.h \
        applications/earth_explorer/earth_main.cpp
git commit -m "feat(earth): click-to-details picking + ImGui quake info card

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 6: 真机交互验证 + .app 重打包 + 文档/记忆

**Files:**
- Modify: `HANDOFF.md`
- Modify: `docs/EarthExplorer.md`

- [ ] **Step 1: 真机交互验证(用户操作,关键确认)**

启动 `.app` 前先用裸二进制带界面跑(可交互):
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
./osgVerse_EarthExplorer --resolution 1280 800
```
请用户确认:
1. 控制面板「实时数据 / Live」→「地震 (USGS)」复选框,**勾选后** 地球上出现真实地震点(联网,数十个),**取消** 后消失。
2. 点击某个地震点 → 面板出现「地震详情」卡片(震级/深度/位置/经纬/时间/USGS 链接);点「关闭」消失。
3. 等 ~60s 观察点集是否自动刷新(可对比 `[Quake] Parsed N quakes (network)` 日志多次出现)。
4. **4 类回归全程不出现**:海洋无橙红、晨昏线在、低空倾斜不穿模、无桔红程序化水面。

- [ ] **Step 2: 全量 headless 回归批次(固定样本,多视角)**

Run(全球 + 低空各一次,确认点标记不破坏渲染):
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin
for goto in "" "--goto 35.69 139.69 200"; do
  rm -f /tmp/earth_capture_0.png
  DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
  EARTH_QUAKES=1 EARTH_QUAKES_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/quakes_fixture.geojson \
  EARTH_AUTOCAP=500 EARTH_FRAME_SLEEP_MS=30 \
  ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 $goto > /tmp/run.log 2>&1
  cp /tmp/earth_capture_0.png /tmp/quake_view_$(echo $goto | tr ' /' '__').png
done
```
然后 Read 两张截图。Expected: 全球视角 3 点清晰;东京低空视角能看到东京那颗大红点贴在地表附近;渲染正常无 4 类回归。

- [ ] **Step 3: 重打包 `.app`**

按既有打包流程(参考 HANDOFF「无头截图验证」与历史 `.app` 重打包描述)用最新 `build/sdk_core/bin/osgVerse_EarthExplorer` + `lib` 重打包 `dist/EarthExplorer.app`(全屏、`NSHighResolutionCapable=false`)。
> 若仓库有打包脚本(查 `dist/` 或 `tasks/`),用脚本;否则照搬上次打包步骤。打包后双击确认能起、地震开关可用。

- [ ] **Step 4: 更新 HANDOFF + 用户文档**

- `HANDOFF.md` 顶部「✅ 已完成」区加一条:地震实时层(USGS 2.5_day,~60s 刷新,点击详情,`EARTH_QUAKES`/`EARTH_QUAKES_FILE` 钩子),并把 backlog「全球数据只接了 GIBS」更新为「地震已接,降水待 Step 2」。
- `docs/EarthExplorer.md` 图层小节加「地震 (USGS)」说明 + env 钩子。

- [ ] **Step 5: 提交 + 同步**

```bash
cd /Users/franklee/osgverse
git add HANDOFF.md docs/EarthExplorer.md
git commit -m "docs(earth): HANDOFF + user doc — real-time USGS quake layer done

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git push origin master
```

- [ ] **Step 6: 写记忆**

新建记忆 `earth-quake-layer-done.md`(type: project):记录地震层已接入(USGS 2.5_day GeoJSON、libhv+picojson、GL_POINTS 点精灵、~60s 后台刷新、点击详情、`EARTH_QUAKES`/`EARTH_QUAKES_FILE` 钩子、不碰 globe 着色器),并更新 [[earth-realtime-data-overlays]] 指明地震已实现、降水(RainViewer 复用云图槽)为下一步。MEMORY.md 加索引行。

---

## Self-Review(写计划后自查)

**Spec 覆盖:** 数据源(Task 2)、模块隔离(Task 1)、实时刷新(Task 4)、ECEF 渲染+大小∝震级+颜色∝深度(Task 3)、UI 开关(Task 4)、测试钩子 `EARTH_QUAKES`/`EARTH_QUAKES_FILE`(Task 2/4)、点击详情(Task 5)、验证计划(Task 6)、YAGNI(未做动画/回放)——全覆盖。

**类型一致性:** `QuakeInfo`/`QuakeLayer`(quake_data.h)在 Task 5 UI + earth_main 一致使用;内部 `Quake`(含预算 `ecef`)在 Task 2 定义、Task 3/4/5 复用;`configureQuakeData(viewer, earthRoot, mainFolder, QuakeLayer**)` 签名在 h/cpp/earth_main 三处一致;`fetchQuakes()` 在 Task 2 定义、Task 4 线程调用(Task 4 Step1 笔误的 `fetchQuakesShared` 已注明改回 `fetchQuakes`)。

**占位符:** 无 TODO/TBD;每步含可编译代码与可断言命令。Task 6 Step3 打包"参考既有流程"为对现有未脚本化流程的合理引用,非代码占位。
