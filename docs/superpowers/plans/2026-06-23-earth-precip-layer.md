# 降水层 RainViewer(Step 2)Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 EarthExplorer 上叠加 RainViewer 实时降水雷达(复用 GIBS 云图的 OVERLAY 槽、二选一、~8min 自动刷新),零着色器改动。

**Architecture:** `createCustomPath` 的 OVERLAY 分支按 `prefix` 分流(`"gibs"`→GIBS / `http…`→RainViewer 模板 / 空→不加载)。新模块 `precip_data.{h,cpp}`(复用 Step 1 地震的 `FetchThread`+libhv+picojson 范式)后台抓 `weather-maps.json` → 拼 RV 瓦片模板 → `osgVerse::TileManager::instance()->setLayerPath(OVERLAY, 模板)`;每瓦片现有 `check()` 自动重载。UI 在「影像 / 天气」组加「降水雷达」复选框,与「GIBS 影像/云图」互斥(共用 OVERLAY 槽 + `Overlay2Opacity`)。

**Tech Stack:** C++ / OpenSceneGraph、libhv(Step 1 已接进 earth_explorer)、picojson、ImGui;复用现有 TMS/TileCallback 瓦片管线。

**验证方式:** 同 Step 1——增量编译 + headless 截图/日志,`EARTH_PRECIP_FILE` 喂固定样本做确定性验证;切换/刷新真机确认。增量构建:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
headless 模板:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
<env> ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 [--goto ...] > /tmp/run.log 2>&1
# 然后 Read /tmp/earth_capture_0.png + grep /tmp/run.log
```

**关键约束(铁律):** 不碰 `assets/shaders/scattering_globe.frag.glsl`、太阳方向、`OceanOpaque`(4 类回归:海洋橙/删晨昏线/倾斜穿模/程序化海洋)。本计划全程不动这些。

---

## File Structure

- **Modify** `applications/earth_explorer/earth_main.cpp` — `createCustomPath` OVERLAY 分支改为按 prefix 分流;extern + 创建 `PrecipController`(ref_ptr 持有);注册「precip」图层 + 与「clouds」互斥的 apply 逻辑 + `EARTH_PRECIP` 钩子。
- **Create** `applications/earth_explorer/precip_data.h` — `PrecipController`(osg::Referenced 抽象)+ `configurePrecipLayer()` 声明。
- **Create** `applications/earth_explorer/precip_data.cpp` — 控制器实现:libhv 抓 `weather-maps.json`、picojson 解析、拼 RV 模板、后台 `FetchThread`(~8min)、`setEnabled` 门控、`setLayerPath(OVERLAY, …)`。
- **Modify** `applications/earth_explorer/CMakeLists.txt` — 源列表加 `precip_data.cpp`(libhv 已在 Step 1 接好,无需再加)。
- **Create** `applications/earth_explorer/test/weather_maps_fixture.json` — 离线样本(供 `EARTH_PRECIP_FILE`)。

---

## Task 1: OVERLAY 按 prefix 分流 + precip 模块骨架接线(行为保持)

**Files:**
- Modify: `applications/earth_explorer/earth_main.cpp`
- Create: `applications/earth_explorer/precip_data.h`
- Create: `applications/earth_explorer/precip_data.cpp`
- Modify: `applications/earth_explorer/CMakeLists.txt`

- [ ] **Step 1: 改 `createCustomPath` 的 OVERLAY 分支为按 prefix 分流**

`earth_main.cpp` 现有 OVERLAY 分支(约 227-243 行)整段替换为:
```cpp
    else if (type == osgVerse::TileCallback::OVERLAY)
    {
        // OVERLAY 槽按 prefix(= 该层路径,由 TileManager::setLayerPath 设)分流:
        //   "gibs"      → GIBS VIIRS 云图(默认);
        //   以 http 开头 → 当作完整瓦片模板(RainViewer 降水雷达);
        //   其它(空)    → 不加载(返回空)。
        if (prefix.rfind("http", 0) == 0)   // RainViewer 模板(已含 {z}/{x}/{y})
        {
            if (z > 10) return "";          // 雷达分辨率粗,z>10 不取
            return osgVerse::TileCallback::createPath(prefix, x, yXYZ, z);
        }
        if (prefix != "gibs") return "";    // 空/未知 → 不加载叠加瓦片

        if (z > 9) return "";  // GIBS GoogleMapsCompatible_Level9 max zoom is 9; no data above
        // 用 VIIRS_SNPP 真彩而非 MODIS_Terra:MODIS 单星刈幅赤道留黑色刈幅空隙;VIIRS 无缝。
        // 取「今天-2 天」(UTC) 完整日期,只算一次(长跑不刷新;跨 UTC 午夜需重启)。
        static const std::string gibsDate = []() {
            time_t t = time(NULL) - 2 * 86400; struct tm g; gmtime_r(&t, &g);
            char buf[16]; strftime(buf, sizeof(buf), "%Y-%m-%d", &g); return std::string(buf);
        }();
        std::string gibs =
            "https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/"
            "VIIRS_SNPP_CorrectedReflectance_TrueColor/default/" + gibsDate +
            "/GoogleMapsCompatible_Level9/{z}/{y}/{x}.jpg";
        return osgVerse::TileCallback::createPath(gibs, x, yXYZ, z);
    }
```
(默认 prefix 仍是 `earthURLs` 里的 `Overlay=gibs` → 走 GIBS 分支,行为不变。)

- [ ] **Step 2: 写 `precip_data.h`(骨架接口)**

```cpp
#ifndef EARTH_PRECIP_DATA_H
#define EARTH_PRECIP_DATA_H

#include <osg/Referenced>
#include <osg/ref_ptr>

// 降水(RainViewer)层控制器。后台抓帧并通过 TileManager 设置 OVERLAY 槽的瓦片模板。
// 不渲染节点、不碰着色器——只切瓦片层路径(复用现有 OVERLAY 管线 + check() 重载)。
class PrecipController : public osg::Referenced
{
public:
    virtual void setEnabled(bool on) = 0;   // 开:后台抓帧→设 OVERLAY=RV 模板;关:线程空转
    virtual bool isEnabled() const = 0;
protected:
    virtual ~PrecipController() {}
};

// 创建控制器并启动后台线程(常驻,仅 isEnabled() 时联网)。调用方(main)用 ref_ptr 持有。
osg::ref_ptr<PrecipController> configurePrecipLayer();

#endif
```

- [ ] **Step 3: 写 `precip_data.cpp`(骨架:控制器 stub,先能编译)**

```cpp
#include <readerwriter/TileCallback.h>
#include <OpenThreads/Thread>
#include <OpenThreads/Mutex>
#include <iostream>
#include <string>
#include "precip_data.h"

namespace
{
    class PrecipControllerImpl : public PrecipController
    {
    public:
        PrecipControllerImpl() : _enabled(false) {}
        virtual void setEnabled(bool on) { _enabled = on; }
        virtual bool isEnabled() const { return _enabled; }
    protected:
        virtual ~PrecipControllerImpl() {}
        bool _enabled;
    };
}

osg::ref_ptr<PrecipController> configurePrecipLayer()
{
    return osg::ref_ptr<PrecipController>(new PrecipControllerImpl);
}
```

- [ ] **Step 4: 改 `CMakeLists.txt` 源列表加 `precip_data.cpp`**

把
```cmake
    city_data.cpp ReaderWriterCSV.cpp quake_data.cpp
```
改成
```cmake
    city_data.cpp ReaderWriterCSV.cpp quake_data.cpp precip_data.cpp
```
(libhv 的 `-DHV_STATICLIB` + include 目录 Step 1 已加,无需重复。)

- [ ] **Step 5: 在 `earth_main.cpp` 接入(extern + 创建并持有控制器)**

顶部 include 区(`#include "quake_data.h"` 旁)加:
```cpp
#include "precip_data.h"
```
在创建 `QuakeLayer* quakeLayer` 之后(约 351 行 configureQuakeData 调用之后)加:
```cpp
    osg::ref_ptr<PrecipController> precip = configurePrecipLayer();
```
(本任务暂不注册图层/不接 UI,仅保证创建+持有,生命周期随 main。)

- [ ] **Step 6: 增量编译**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -6`
Expected: 成功,无报错。

- [ ] **Step 7: headless 验证 GIBS 云图行为不变(分流默认分支)**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_CLOUDS=0.8 EARTH_AUTOCAP=300 EARTH_FRAME_SLEEP_MS=40 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
```
Read `/tmp/earth_capture_0.png`。Expected: 与改动前一致——全球可见 GIBS 真彩云图叠加(分流后 `"gibs"` 默认分支仍生效),地球正常、无 4 类回归。

- [ ] **Step 8: 提交**
```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/earth_main.cpp applications/earth_explorer/precip_data.h \
        applications/earth_explorer/precip_data.cpp applications/earth_explorer/CMakeLists.txt
git commit -m "feat(earth): OVERLAY prefix-dispatch + precip controller skeleton (no behavior)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: 抓取 weather-maps.json + 拼 RainViewer 模板(EARTH_PRECIP_FILE + 日志断言)

**Files:**
- Create: `applications/earth_explorer/test/weather_maps_fixture.json`
- Modify: `applications/earth_explorer/precip_data.cpp`

- [ ] **Step 1: 写固定样本 `test/weather_maps_fixture.json`**(精简版,字段同真实 API)
```json
{
  "version": "2.0",
  "generated": 1782145800,
  "host": "https://tilecache.rainviewer.com",
  "radar": {
    "past": [
      { "time": 1782145200, "path": "/v2/radar/aaaa1111" },
      { "time": 1782145800, "path": "/v2/radar/bbbb2222" }
    ],
    "nowcast": []
  }
}
```

- [ ] **Step 2: 在 `precip_data.cpp` 加 include + 抓取/解析/拼模板函数**

顶部加:
```cpp
#include <picojson.h>
#include "3rdparty/libhv/all/client/requests.h"
#include <fstream>
#include <sstream>
```
在匿名 namespace 内、`PrecipControllerImpl` 之前加:
```cpp
    static const char* kWeatherMapsUrl = "https://api.rainviewer.com/public/weather-maps.json";

    // 解析 weather-maps.json → 最新观测帧的瓦片模板(空串=失败)。
    // 模板形如 https://tilecache.rainviewer.com/v2/radar/<hash>/256/{z}/{x}/{y}/4/1_1.png
    std::string parseRainViewerTemplate(const std::string& text)
    {
        picojson::value root;
        std::string err = picojson::parse(root, text);
        if (!err.empty() || !root.is<picojson::object>()) {
            std::cout << "[Precip] JSON parse error: " << err << "\n"; return "";
        }
        const picojson::value& host = root.get("host");
        const picojson::value& radar = root.get("radar");
        if (!host.is<std::string>() || !radar.is<picojson::object>()) return "";
        const picojson::value& past = radar.get("past");
        if (!past.is<picojson::array>()) return "";
        const picojson::array& arr = past.get<picojson::array>();
        if (arr.empty()) return "";
        const picojson::value& last = arr.back();   // 最新观测帧
        if (!last.is<picojson::object>()) return "";
        const picojson::value& path = last.get("path");
        if (!path.is<std::string>()) return "";
        return host.get<std::string>() + path.get<std::string>() + "/256/{z}/{x}/{y}/4/1_1.png";
    }

    // 取最新 RV 模板:优先 EARTH_PRECIP_FILE 本地样本,否则联网。空串=失败。
    std::string fetchRainViewerTemplate()
    {
        const char* fixtureFile = getenv("EARTH_PRECIP_FILE");
        if (fixtureFile && *fixtureFile)
        {
            std::ifstream in(fixtureFile);
            if (!in) { std::cout << "[Precip] fixture open failed: " << fixtureFile << "\n"; return ""; }
            std::stringstream ss; ss << in.rdbuf();
            std::string tmpl = parseRainViewerTemplate(ss.str());
            std::cout << "[Precip] template (fixture): " << tmpl << "\n";
            return tmpl;
        }
        requests::Request req(new HttpRequest);
        req->method = HTTP_GET; req->url = kWeatherMapsUrl; req->timeout = 15;
        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        {
            std::cout << "[Precip] fetch failed, status=" << (resp ? (int)resp->status_code : -1) << "\n";
            return "";
        }
        std::string tmpl = parseRainViewerTemplate(resp->body);
        std::cout << "[Precip] template (network): " << tmpl << "\n";
        return tmpl;
    }
```

- [ ] **Step 3: 临时在 `setEnabled` 里调用验证(本任务只验抓取链路;Task 3 移到线程)**

把 `PrecipControllerImpl::setEnabled` 临时改为:
```cpp
        virtual void setEnabled(bool on)
        {
            _enabled = on;
            if (on) { std::string t = fetchRainViewerTemplate(); std::cout << "[Precip] got len=" << t.size() << "\n"; }
        }
```
并在 `configurePrecipLayer` 末尾临时加一行强制触发(仅本任务,Task 3 删):
```cpp
osg::ref_ptr<PrecipController> configurePrecipLayer()
{
    osg::ref_ptr<PrecipController> c(new PrecipControllerImpl);
    if (getenv("EARTH_PRECIP_FILE") || getenv("EARTH_PRECIP")) c->setEnabled(true);  // 临时:验抓取
    return c;
}
```

- [ ] **Step 4: 增量编译**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -6`
Expected: 成功(确认 `requests.h`/`picojson.h` 解析无误,同 Step 1 地震)。

- [ ] **Step 5: 固定样本验证模板拼装**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_PRECIP_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/weather_maps_fixture.json \
EARTH_AUTOCAP=120 ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep "\[Precip\]" /tmp/run.log
```
Expected 含:
```
[Precip] template (fixture): https://tilecache.rainviewer.com/v2/radar/bbbb2222/256/{z}/{x}/{y}/4/1_1.png
[Precip] got len=...
```
(取的是 `past` 最后一项 `bbbb2222`,非第一项。)

- [ ] **Step 6: (可选)联网验证**
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_PRECIP=1 EARTH_AUTOCAP=120 ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep "\[Precip\] template (network)" /tmp/run.log
```
Expected: 打印真实 `https://tilecache.rainviewer.com/v2/radar/<hash>/256/{z}/{x}/{y}/4/1_1.png`。失败(离线)不阻塞。

- [ ] **Step 7: 提交**
```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/precip_data.cpp applications/earth_explorer/test/weather_maps_fixture.json
git commit -m "feat(earth): fetch+parse RainViewer weather-maps.json -> tile template (EARTH_PRECIP_FILE)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: 后台刷新线程 + setLayerPath(OVERLAY) + 开关门控

**Files:**
- Modify: `applications/earth_explorer/precip_data.cpp`

- [ ] **Step 1: 加后台 `FetchThread`(仿 Step 1 quake 的 FetchThread)**

在匿名 namespace 内,`PrecipControllerImpl` 之前加前向声明 + 线程类(`run()` 定义在 Impl 之后):
```cpp
    class PrecipControllerImpl;

    class FetchThread : public OpenThreads::Thread
    {
    public:
        FetchThread(PrecipControllerImpl* o) : _owner(o), _done(false) {}
        virtual int cancel() { _done = true; return OpenThreads::Thread::cancel(); }
        virtual void run();   // 定义在 PrecipControllerImpl 之后
    protected:
        PrecipControllerImpl* _owner; bool _done;
    };
```

- [ ] **Step 2: 重写 `PrecipControllerImpl`(线程 + setLayerPath + 门控)**

整类替换为:
```cpp
    class PrecipControllerImpl : public PrecipController
    {
    public:
        PrecipControllerImpl() : _enabled(false), _thread(nullptr) {}
        virtual void setEnabled(bool on)
        {
            _enabled = on;
            if (on) _tick = 0;   // 立即抓一次
            // 关闭时不在这里改 OVERLAY 路径(由 earth_main 切回 "gibs"),避免与互斥逻辑打架。
        }
        virtual bool isEnabled() const { return _enabled; }

        void startFetch() { if (!_thread) { _thread = new FetchThread(this); _thread->startThread(); } }

        // 后台线程调用:抓最新帧,若变化且仍开启则设 OVERLAY 路径。
        void refreshIfEnabled()
        {
            if (!_enabled) return;
            std::string tmpl = fetchRainViewerTemplate();
            if (tmpl.empty()) return;
            if (!_enabled) return;   // 抓取期间可能被关
            if (tmpl != _lastTemplate)
            {
                _lastTemplate = tmpl;
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, tmpl);
                std::cout << "[Precip] OVERLAY set to RainViewer frame\n";
            }
        }
        int _tick;                 // 线程用:倒计时到 0 抓一次
    protected:
        virtual ~PrecipControllerImpl()
        { if (_thread) { _thread->cancel(); _thread->join(); delete _thread; _thread = nullptr; } }
        bool _enabled;
        std::string _lastTemplate;
        FetchThread* _thread;
    };

    void FetchThread::run()
    {
        const int kIntervalTicks = 4800;   // ~8min ÷ 100ms/tick
        _owner->_tick = 0;
        while (!_done)
        {
            if (_owner->isEnabled())
            {
                if (_owner->_tick <= 0) { _owner->refreshIfEnabled(); _owner->_tick = kIntervalTicks; }
                else _owner->_tick--;
            }
            else _owner->_tick = 0;
            OpenThreads::Thread::microSleep(100000);  // 100ms
        }
        _done = true;
    }
```
(注:`_tick` 设为 public 供线程访问,与 Step 1 quake 的 tick-in-thread 略不同——这里 tick 归属 owner 以便 setEnabled 立即重置。也可把 tick 放线程内并加一个 `_justEnabled` 标志,实现等价,择一即可。)

- [ ] **Step 3: `configurePrecipLayer` 改为启动线程(去掉 Task 2 临时强制触发)**
```cpp
osg::ref_ptr<PrecipController> configurePrecipLayer()
{
    osg::ref_ptr<PrecipControllerImpl> c(new PrecipControllerImpl);
    c->startFetch();   // 线程常驻;仅 isEnabled() 时联网
    return osg::ref_ptr<PrecipController>(c.get());
}
```
并把 Task 2 临时改的 `setEnabled`(带 fetch+log 的版本)恢复为 Step 2 内已含的最终版(上面 Step 2 的 `setEnabled` 只设 `_enabled`+`_tick`)。

- [ ] **Step 4: 增量编译**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -6`
Expected: 成功(注意 FetchThread 定义顺序,同 Step 1 quake)。

- [ ] **Step 5: headless 验证线程设置 OVERLAY 路径(需 earth_main 还没接 UI,用临时强制开)**

本任务 earth_main 尚未注册图层,先用一次性临时验证:在 `configurePrecipLayer` 里临时加 `if (getenv("EARTH_PRECIP")||getenv("EARTH_PRECIP_FILE")) c->setEnabled(true);`(验证后删),编译后:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_PRECIP_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/weather_maps_fixture.json \
EARTH_AUTOCAP=300 EARTH_FRAME_SLEEP_MS=30 ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep "\[Precip\]" /tmp/run.log
```
Expected 含 `[Precip] OVERLAY set to RainViewer frame`(线程→refreshIfEnabled→setLayerPath 链路通)。验证后删掉该临时行(Task 4 由 UI/钩子驱动)。

- [ ] **Step 6: 提交**
```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/precip_data.cpp
git commit -m "feat(earth): precip background refresh thread (~8min) + setLayerPath(OVERLAY) gating

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 4: earth_main 集成 — 注册 precip 图层 + 与 clouds 互斥 + EARTH_PRECIP 钩子

**Files:**
- Modify: `applications/earth_explorer/earth_main.cpp`

- [ ] **Step 1: 在图层注册块加 OVERLAY 状态助手 + clouds/precip 互斥 apply**

现有 `clouds` 图层注册(约 419-426 行)替换 + 其后新增 `precip` 图层。把 clouds 块替换为如下整段(注意:`precip` 变量是 Task 1 Step 5 创建的 `osg::ref_ptr<PrecipController> precip`,此处在作用域内):
```cpp
        osgVerse::EarthAtmosphereOcean* eptr2 = &earthRenderingUtils;
        LayerManager* lmptr = &layerMgr;
        PrecipController* pcptr = precip.get();
        // OVERLAY 槽可见度:降水开→降水透明度;否则云图开→云图透明度;都关→0。
        auto applyOverlayOpacity = [eptr2, lmptr]() {
            float op = 0.0f;
            OverlayLayer* cl = lmptr->find("clouds");
            OverlayLayer* pp = lmptr->find("precip");
            if (pp && pp->enabled) op = pp->opacity;
            else if (cl && cl->enabled) op = cl->opacity;
            if (eptr2->commonUniforms.count("Overlay2Opacity"))
                eptr2->commonUniforms["Overlay2Opacity"]->set(op);
        };

        OverlayLayer clouds; clouds.id = "clouds"; clouds.displayName = u8"GIBS 影像/云图";
        clouds.group = u8"影像 / 天气"; clouds.enabled = false; clouds.hasOpacity = true; clouds.opacity = 0.7f;
        clouds.apply = [lmptr, pcptr, applyOverlayOpacity](const OverlayLayer& l) {
            if (l.enabled)   // 互斥:开云图 → 关降水 + OVERLAY 切回 gibs
            {
                if (OverlayLayer* pp = lmptr->find("precip")) pp->enabled = false;
                if (pcptr) pcptr->setEnabled(false);
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, "gibs");
            }
            applyOverlayOpacity();
        };
        layerMgr.add(clouds);

        OverlayLayer precipL; precipL.id = "precip"; precipL.displayName = u8"降水雷达 RainViewer";
        precipL.group = u8"影像 / 天气"; precipL.enabled = false; precipL.hasOpacity = true; precipL.opacity = 0.85f;
        precipL.apply = [lmptr, pcptr, applyOverlayOpacity](const OverlayLayer& l) {
            if (l.enabled)   // 互斥:开降水 → 关云图;先清 OVERLAY,待控制器拼好 RV 模板再上
            {
                if (OverlayLayer* cl = lmptr->find("clouds")) cl->enabled = false;
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, "");
                if (pcptr) pcptr->setEnabled(true);   // 控制器抓帧后 setLayerPath(OVERLAY, RV模板)
            }
            else   // 关降水 → OVERLAY 回 gibs(云图开则可见,否则 opacity 0)
            {
                if (pcptr) pcptr->setEnabled(false);
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, "gibs");
            }
            applyOverlayOpacity();
        };
        layerMgr.add(precipL);
```
> 注意 `#include <readerwriter/TileCallback.h>` 是否已在 earth_main 可见(`osgVerse::TileManager`/`TileCallback::OVERLAY`)。若未包含,在顶部 include 区补 `#include <readerwriter/TileCallback.h>`。

- [ ] **Step 2: 初值同步 + EARTH_PRECIP 钩子**

现有 clouds 初值同步块(约 431-443,读 `EARTH_CLOUDS`)之后,新增 precip 同步:
```cpp
    if (OverlayLayer* pp = layerMgr.find("precip"))
    {
        // EARTH_PRECIP=1 强制开启降水层(headless 验证用;与 EARTH_CLOUDS 互斥,后开者生效)。
        const char* pEnv = getenv("EARTH_PRECIP");
        if (pEnv && *pEnv) pp->enabled = (atoi(pEnv) != 0);
        layerMgr.setEnabled("precip", pp->enabled);
    }
```
(保持顺序:`EARTH_CLOUDS` 块在前、`EARTH_PRECIP` 块在后 → 同时设时降水生效,符合互斥语义。)

- [ ] **Step 3: 增量编译**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -6`
Expected: 成功。

- [ ] **Step 4: headless 验证降水开(固定样本,瓦片不可达不影响路径切换日志;再用联网看真瓦片)**

先用固定样本确认链路(样本的 hash 瓦片不存在 → 瓦片 404 但路径切换/日志可验):
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_PRECIP=1 EARTH_PRECIP_FILE=/Users/franklee/osgverse/applications/earth_explorer/test/weather_maps_fixture.json \
EARTH_AUTOCAP=350 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep "\[Precip\]" /tmp/run.log
```
Expected: `[Precip] OVERLAY set to RainViewer frame`(降水图层开 → 控制器设了 OVERLAY=RV 模板)。

再用**联网**看真实降水瓦片渲染(降水多见于陆地中纬度,选欧洲/北美视角更易见):
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_PRECIP=1 EARTH_AUTOCAP=450 EARTH_FRAME_SLEEP_MS=40 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 48 10 6000 > /tmp/run.log 2>&1
```
Read `/tmp/earth_capture_0.png`。Expected: 欧洲上空可见 RainViewer 降水色块(有雨时);地球正常、无 4 类回归。日志含 `template (network)` + `OVERLAY set to RainViewer frame`。

- [ ] **Step 5: 验证互斥 + 默认关**

```bash
# 默认(都不开):应是 GIBS 默认路径、无降水
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=200 ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
grep -c "OVERLAY set to RainViewer" /tmp/run.log   # 期望 0(降水默认关,不联网不设RV)
```
Expected: `0`。互斥的交互(开降水自动关云图复选框)在 Task 5 真机确认。

- [ ] **Step 6: 提交**
```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/earth_main.cpp
git commit -m "feat(earth): register precip layer + mutual-exclusion with clouds + EARTH_PRECIP

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 5: 真机验证 + .app 重打包 + 文档/记忆

**Files:**
- Modify: `HANDOFF.md`, `docs/EarthExplorer.md`

- [ ] **Step 1: 真机交互验证(用户操作)**

裸二进制带界面:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib ./osgVerse_EarthExplorer --resolution 1280 800
```
确认:① 「影像 / 天气」→「降水雷达 RainViewer」勾选 → 陆地上空出现降水色块,且「GIBS 影像/云图」自动取消(互斥);② 反向:勾选云图 → 降水自动取消、切回 GIBS;③ 都不勾 → 无叠加;④ 挂 ~8min 看是否刷新到新帧(日志再现 `OVERLAY set to RainViewer frame`);⑤ **4 类回归全程不出现**。

- [ ] **Step 2: headless 回归批次**(全球 + 欧洲低空,联网)
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin
for goto in "" "--goto 48 10 6000"; do
  rm -f /tmp/earth_capture_0.png
  DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
  EARTH_PRECIP=1 EARTH_AUTOCAP=450 EARTH_FRAME_SLEEP_MS=40 EARTH_SUN_TO_CAMERA=1 \
  ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 $goto > /tmp/run.log 2>&1
  cp /tmp/earth_capture_0.png /tmp/precip_$(echo $goto | tr ' /' '__').png
done
```
Read 两张图。Expected: 降水叠加正常、地球正常、无 4 类回归。

- [ ] **Step 3: 重打包 `.app`**
```bash
cd /Users/franklee/osgverse && bash packaging/package_macos.sh 2>&1 | tail -1
```
打包二进制 headless 跑一次 `EARTH_PRECIP=1 EARTH_PRECIP_FILE=<样本>` 确认 `[Precip] OVERLAY set to RainViewer frame` + 不崩。

- [ ] **Step 4: 更新文档**
- `HANDOFF.md` 顶部加 2026-06-23 会话条目:降水层(RainViewer,复用 OVERLAY 槽与云图互斥,~8min 刷新,`EARTH_PRECIP`/`EARTH_PRECIP_FILE` 钩子,零着色器改动)。
- `docs/EarthExplorer.md`:图层小节 +「降水雷达 RainViewer」说明 + env 钩子表加 `EARTH_PRECIP`/`EARTH_PRECIP_FILE`。

- [ ] **Step 5: 提交 + push**
```bash
cd /Users/franklee/osgverse
git add HANDOFF.md docs/EarthExplorer.md
git commit -m "docs(earth): HANDOFF + user doc — RainViewer precipitation layer done

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git push origin master
```
(push 前需用户确认真机验证 OK,同 Step 1 惯例。)

- [ ] **Step 6: 写记忆**
更新 [[earth-quake-layer-done]] 或新建 `earth-precip-layer-done.md`(type: project):降水层已接(RainViewer,OVERLAY prefix 分流 + TileManager::setLayerPath 重载 + 与云图互斥,RGBA 瓦片,~8min 刷新,`EARTH_PRECIP*` 钩子)。更新 [[earth-realtime-data-overlays]] 标注降水已实现。MEMORY.md 加索引。

---

## Self-Review(写计划后自查)

**Spec 覆盖**:数据源 + curl 验证(Task 2)、prefix 分流(Task 1)、TileManager 重载切换(Task 3)、互斥 + UI 三选一(Task 4)、~8min 刷新(Task 3)、`EARTH_PRECIP`/`EARTH_PRECIP_FILE` 钩子(Task 2/4)、验证(Task 5)、YAGNI(无 nowcast/时间轴)——全覆盖。

**类型一致**:`PrecipController`(precip_data.h)抽象在 main/控制器一致;`configurePrecipLayer()` 返回 `osg::ref_ptr<PrecipController>` 三处一致;`fetchRainViewerTemplate`/`parseRainViewerTemplate` Task 2 定义、Task 3 线程调用;`FetchThread`/`PrecipControllerImpl` 定义顺序同 Step 1 quake(前向声明 + run() 外联)。

**占位符**:无 TODO/TBD;每步含可编译代码与可断言命令。Task 2/3 的临时验证代码均注明"Task N 删/恢复"。

**潜在坑(已在步骤内提示)**:① earth_main 需可见 `<readerwriter/TileCallback.h>`(Task 4 Step1 注明补 include);② `_tick` 归属 owner 的并发读写是 plain int,与 Step 1 plain-bool 同属项目惯例(单写单读 + 100ms 轮询);③ 互斥用直接改 `OverlayLayer::enabled` 字段(不调 setEnabled)避免 apply 递归——UI 每帧读 enabled 故复选框会更新。
