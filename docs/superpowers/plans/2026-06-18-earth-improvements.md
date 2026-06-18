# EarthExplorer 五项改进 实现计划（缓存 / 最底点 / 极地 / 光影 / 真实时间）

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 给现有 macOS 版 EarthExplorer 增加瓦片磁盘缓存、相机最低高度（不穿地表）、极地贴图修复、光照/色调/大气调优、真实时间太阳模式五项改进。

**Architecture:** 全部在现有「2D 影像贴椭球 + RTT 大气/海洋」管线内增量改动，不引入新管线。纯逻辑部分（URL→缓存路径、距离夹紧、太阳位置天文公式、Mercator 反投影）写成可独立编译的真单元测试；集成/观感部分用既有的「构建 + 无头截图」验证。

**Tech Stack:** C++14/17，OpenSceneGraph 3.6.5（CORE/GL4.1），osgVerse，libhv，GLSL，CMake，macOS。

**测试方法**：纯函数 → `clang++` 独立编译的断言测试；渲染/UI → `EARTH_AUTOCAP` 无头截图 + Read 工具目视。

**通用命令：**
- 增量构建：`cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
- 全球截图：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib EARTH_AUTOCAP=1500 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1; ls -la /tmp/earth_capture_0.png
```
- 定点+流式截图（看地面/极地）：在上面命令加 `--goto <纬> <经> <高度km>` 和 `EARTH_FRAME_SLEEP_MS=30`，`EARTH_AUTOCAP=2500`。

---

## File Structure

| 文件 | 改动 | 责任 |
|------|------|------|
| `readerwriter/UtilitiesEx.cpp` | 修改 | `loadFileData` 增加 URL→磁盘的字节级瓦片缓存（含 mimeType 旁文件） |
| `readerwriter/EarthManipulator.h` | 修改 | 新增 `_minDistance`、`setMinDistance`、`clampDistance()`；纯函数 `clampDistanceValue` |
| `readerwriter/EarthManipulator.cpp` | 修改 | 在 calcScrollingMotion/performScale/setByEye 后夹紧距离 |
| `readerwriter/SolarPosition.h` | 新建 | 纯头文件：UTC→(太阳赤纬, 日下点经度) 天文公式 + ECEF 太阳方向 |
| `applications/earth_explorer/EarthControlUI.h` | 修改 | 太阳段加「真实时间」开关+日期/时间输入；渲染段加大气强度滑块 |
| `applications/earth_explorer/earth_main.cpp` | 修改 | 用 EnvironmentHandler 的 FRAME 每帧驱动真实时间太阳（若开启） |
| `assets/shaders/scattering_globe.frag.glsl` | 修改 | hdr() 后加轻度饱和/对比提升（#5 调优） |
| `readerwriter/TileCallback.cpp` | 修改 | `computeTileExtent` 在 Web Mercator 下用反投影算瓦片纬度边界（#3） |
| `modeling/Math.cpp` | 修改 | `convertLLAtoECEF` 移除 ±85.05° 极地硬截断（#3） |

---

## Task 1: 瓦片磁盘缓存

在线瓦片经 `ReaderWriterWeb::readFile` → `osgVerse::loadFileData`（libhv）下载。现有 simdb 缓存写到 `/var/tmp/simdb_earth_cache`、容量仅约 4MB、非持久，等于没用。最稳的做法是在 `loadFileData` 里加**字节级磁盘缓存**：按 URL 哈希存文件，命中则不走网络。

**Files:**
- Create(临时测试): `/tmp/test_tilecache.cpp`
- Modify: `readerwriter/UtilitiesEx.cpp`（`loadFileData`，约 494-562 行）

- [ ] **Step 1: 纯函数单元测试（URL→缓存路径确定性 + 文件往返）**

写 `/tmp/test_tilecache.cpp`：
```cpp
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <functional>
#include <fstream>

static std::string urlToCacheFile(const std::string& dir, const std::string& url)
{
    std::hash<std::string> h;
    char buf[32]; snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h(url));
    return dir + "/" + std::string(buf) + ".tile";
}

int main()
{
    std::string a = urlToCacheFile("/tmp", "https://x/1/2/3.png");
    std::string b = urlToCacheFile("/tmp", "https://x/1/2/3.png");
    std::string c = urlToCacheFile("/tmp", "https://x/1/2/4.png");
    assert(a == b);              // 同 URL → 同路径（确定性）
    assert(a != c);              // 不同 URL → 不同路径
    assert(a.rfind("/tmp/", 0) == 0 && a.size() > 5);
    // 往返：写入再读出一致
    std::string content = "BINARYDATA\0\x01\x02tile", path = urlToCacheFile("/tmp", "roundtrip");
    { std::ofstream o(path.c_str(), std::ios::binary); o.write(content.data(), content.size()); }
    std::ifstream in(path.c_str(), std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    assert(got.size() == content.size());
    printf("OK\n"); return 0;
}
```

- [ ] **Step 2: 运行测试通过**

Run: `clang++ -std=c++14 /tmp/test_tilecache.cpp -o /tmp/test_tilecache && /tmp/test_tilecache`
Expected: `OK`，退出 0。

- [ ] **Step 3: 在 `UtilitiesEx.cpp` 顶部加 includes 与缓存辅助函数**

确认 `readerwriter/UtilitiesEx.cpp` 顶部已包含 `<fstream>`、`<functional>`、`<osgDB/FileUtils>`；缺则补。在 `loadFileData` 函数之前（同命名空间 `osgVerse` 内）加：
```cpp
static std::string tileCacheDir()
{
    const char* env = getenv("EARTH_TILE_CACHE");
    if (env && env[0]) return std::string(env);
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/.osgverse_earth_cache";
}
static std::string tileCacheFile(const std::string& url)
{
    std::hash<std::string> h; char buf[32];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h(url));
    return tileCacheDir() + "/" + std::string(buf) + ".tile";
}
```

- [ ] **Step 4: 在 `loadFileData` 的远程分支加"先查缓存、后写缓存"**

`loadFileData(const std::string& url, std::string& mimeType, std::string& encodingType, ...)` 里，远程分支（`if (!scheme.empty())`）**最开头**插入查缓存：
```cpp
        // 1) 命中磁盘缓存：直接返回字节，并从旁文件恢复 mimeType
        std::string cf = tileCacheFile(url);
        {
            std::ifstream cin(cf.c_str(), std::ios::binary);
            if (cin)
            {
                std::string s((std::istreambuf_iterator<char>(cin)), std::istreambuf_iterator<char>());
                if (!s.empty())
                {
                    std::ifstream min((cf + ".mime").c_str());
                    if (min) std::getline(min, mimeType);
                    buffer.resize(s.size()); memcpy(buffer.data(), s.data(), s.size());
                    return buffer;
                }
            }
        }
```
然后在该分支**成功下载**之后（已有 `buffer.resize(size); memcpy(...)` 把响应体拷进 buffer，并已解析出 `mimeType` 之后），写缓存：
```cpp
        // 2) 写入磁盘缓存（字节 + mimeType 旁文件）
        if (!buffer.empty())
        {
            osgDB::makeDirectory(tileCacheDir());
            std::ofstream out(cf.c_str(), std::ios::binary);
            if (out) out.write((const char*)buffer.data(), buffer.size());
            std::ofstream mo((cf + ".mime").c_str()); if (mo) mo << mimeType;
        }
```
（写缓存必须放在 mimeType 已从响应头解析之后；查现有代码里解析 `content-type` 的位置，确保写缓存在其后。）

- [ ] **Step 5: 构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
Expected: 无 `error:`，安装 osgVerse_EarthExplorer。

- [ ] **Step 6: 验证缓存生成且二次更快/可离线**

Run（先清缓存跑一次 → 看缓存目录有文件 → 断网二次跑仍出图）：
```bash
rm -rf ~/.osgverse_earth_cache; cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib EARTH_AUTOCAP=1500 EARTH_FRAME_SLEEP_MS=20 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 37.77 -122.42 30 > /tmp/c1.log 2>&1; \
echo "缓存文件数:"; ls ~/.osgverse_earth_cache | grep -c '\.tile$'
```
Expected: `~/.osgverse_earth_cache` 下出现若干 `.tile` 文件（>0）。再跑第二次（同命令、可断网），用 Read 看 `/tmp/earth_capture_0.png` 仍能渲染出影像（命中缓存）。

- [ ] **Step 7: Commit**

```bash
cd /Users/franklee/osgverse && git add readerwriter/UtilitiesEx.cpp && \
git commit -m "feat(earth): persistent on-disk tile cache in loadFileData" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: 相机最低高度（不穿地表）

`EarthManipulator` 的 `_distance`（眼到视点中心的距离）在缩放/定位时无任何下限，可缩到地表以下。加一个 `_minDistance` 下限并在所有改 `_distance` 处夹紧。`_distance` 是眼到地表视点的距离，夹紧 `_distance >= minDist` 即可保证眼点离地表视点至少这么远，不会穿过。

**Files:**
- Create(临时测试): `/tmp/test_clamp.cpp`
- Modify: `readerwriter/EarthManipulator.h`、`readerwriter/EarthManipulator.cpp`

- [ ] **Step 1: 纯函数单元测试**

写 `/tmp/test_clamp.cpp`：
```cpp
#include <cassert>
#include <cstdio>
static double clampDistanceValue(double dist, double minDist)
{ return (dist < minDist) ? minDist : dist; }
int main()
{
    assert(clampDistanceValue(10.0, 50.0) == 50.0);   // 低于下限 → 抬到下限
    assert(clampDistanceValue(80.0, 50.0) == 80.0);   // 高于下限 → 不变
    assert(clampDistanceValue(50.0, 50.0) == 50.0);   // 等于下限
    printf("OK\n"); return 0;
}
```

- [ ] **Step 2: 运行通过**

Run: `clang++ -std=c++14 /tmp/test_clamp.cpp -o /tmp/test_clamp && /tmp/test_clamp`
Expected: `OK`。

- [ ] **Step 3: `EarthManipulator.h` 增加成员、纯函数与夹紧方法**

在 `EarthManipulator.h` 的 public 区加：
```cpp
        static double clampDistanceValue(double dist, double minDist)
        { return (dist < minDist) ? minDist : dist; }
        void setMinDistance(double d) { _minDistance = d; }
        double getMinDistance() const { return _minDistance; }
```
在 protected/private 区加成员（与 `_distance` 同级）：
```cpp
        double _minDistance;
```
加一个内部方法（protected）：
```cpp
        void clampDistance()
        { _distance = clampDistanceValue(_distance, _minDistance);
          if (_animationDistance < _minDistance) _animationDistance = _minDistance; }
```

- [ ] **Step 4: `EarthManipulator.cpp` 构造函数初始化 + 三处调用夹紧**

在 EarthManipulator 构造函数初始化列表里加 `, _minDistance(50.0)`（默认离地表视点最少 50m；放在初始化 `_distance` 附近）。
在以下三处的 `_distance` 改动之后各加一行 `clampDistance();`：
1. `calcScrollingMotion` 的 `SCROLL_UP` 分支（`_distance /= ...;` 之后）和 `SCROLL_DOWN` 分支（`_distance *= ...;` 之后）。
2. `EarthManipulator.h` 的 `performScale(double x0,double y0,double dx,double dy)` 末尾（`_distance *= ...` 之后）加 `clampDistance();`。
3. `setByEye(const osg::Vec3d& eye, float doa)` 里 `_distance = new_distance;` 之后加 `clampDistance();`。

- [ ] **Step 5: 构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
Expected: 无 `error:`。

- [ ] **Step 6: 验证：拉到最近也不穿地表**

Run（跳到极低高度 0.001km=1m，请求比下限更低，看相机海拔被夹在 ~50m 而非负）：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib OSG_NOTIFY_LEVEL=NOTICE \
EARTH_AUTOCAP=600 EARTH_FRAME_SLEEP_MS=20 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 37.77 -122.42 0.001 > /tmp/clamp.log 2>&1; \
echo "看 ImGui 面板 Alt 读数应 >= ~0.05km，不应为 0 或负"
```
用 Read 看 `/tmp/earth_capture_0.png`：面板「高度 Alt」读数应被夹在约 0.05km（50m）而非 0/负；画面不应出现"钻到地表下方/翻面"。

- [ ] **Step 7: Commit**

```bash
cd /Users/franklee/osgverse && git add readerwriter/EarthManipulator.h readerwriter/EarthManipulator.cpp && \
git commit -m "feat(earth): clamp camera to a minimum distance so it can't pass through the ground" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: 真实时间太阳（系统时钟 + 可设日期/时间）

`WorldSunDir` 是 ECEF 指向太阳的单位向量（不被 update/updateOcean 覆盖，可每帧安全设置）。`convertLLAtoECEF` 约定：经度 0→+X、90°E→+Y、+Z=北极、右手系。所以从 UTC 时刻算出「日下点」(太阳赤纬 decl、日下点经度 subsolarLon)，`WorldSunDir = normalize(convertLLAtoECEF(decl, subsolarLon, R))`。

**Files:**
- Create: `readerwriter/SolarPosition.h`
- Create(临时测试): `/tmp/test_solar.cpp`
- Modify: `applications/earth_explorer/EarthControlUI.h`、`applications/earth_explorer/earth_main.cpp`

- [ ] **Step 1: 新建 `readerwriter/SolarPosition.h`（纯头，NOAA 近似）**

```cpp
#ifndef OSGVERSE_SOLAR_POSITION_H
#define OSGVERSE_SOLAR_POSITION_H
#include <cmath>

namespace osgVerse
{
    struct SubsolarPoint { double declRad; double lonRad; };  // 太阳赤纬、日下点经度(弧度)

    // UTC 年月日 + UTC 小时(0-24, 含小数)。NOAA 近似，精度约 ±0.5°，足够光照演示。
    inline SubsolarPoint computeSubsolarPoint(int year, int month, int day, double utcHours)
    {
        static const int cum[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
        int doy = cum[(month-1)%12] + day;                 // 年内第几天(忽略闰年误差)
        const double PI = 3.14159265358979323846;
        double gamma = (2.0*PI/365.0) * ((double)(doy-1) + (utcHours-12.0)/24.0);
        double decl = 0.006918 - 0.399912*cos(gamma) + 0.070257*sin(gamma)
                    - 0.006758*cos(2*gamma) + 0.000907*sin(2*gamma)
                    - 0.002697*cos(3*gamma) + 0.00148*sin(3*gamma);
        double eqtimeMin = 229.18*(0.000075 + 0.001868*cos(gamma) - 0.032077*sin(gamma)
                    - 0.014615*cos(2*gamma) - 0.040849*sin(2*gamma));
        double subsolarLonDeg = -15.0*(utcHours - 12.0) - eqtimeMin/4.0;  // 度
        // 归一化到 [-180,180]
        while (subsolarLonDeg > 180.0) subsolarLonDeg -= 360.0;
        while (subsolarLonDeg < -180.0) subsolarLonDeg += 360.0;
        SubsolarPoint s; s.declRad = decl; s.lonRad = subsolarLonDeg * PI / 180.0;
        return s;
    }
}
#endif
```

- [ ] **Step 2: 单元测试（春分正午、UTC 0 点）**

写 `/tmp/test_solar.cpp`：
```cpp
#include <cassert>
#include <cstdio>
#include <cmath>
#include "/Users/franklee/osgverse/readerwriter/SolarPosition.h"
int main()
{
    using namespace osgVerse;
    const double PI = 3.14159265358979323846, D = 180.0/PI;
    // 春分(≈3/20) UTC 12:00：赤纬≈0、日下点经度≈0
    SubsolarPoint a = computeSubsolarPoint(2024, 3, 20, 12.0);
    printf("equinox decl=%.2f lon=%.2f\n", a.declRad*D, a.lonRad*D);
    assert(std::fabs(a.declRad*D) < 2.0);     // 赤纬接近 0
    assert(std::fabs(a.lonRad*D) < 5.0);      // 日下点经度接近 0
    // UTC 0:00：日下点经度接近 ±180（对日点）
    SubsolarPoint b = computeSubsolarPoint(2024, 3, 20, 0.0);
    assert(std::fabs(std::fabs(b.lonRad*D) - 180.0) < 6.0);
    // 夏至(≈6/21) 正午：赤纬接近 +23.4°
    SubsolarPoint c = computeSubsolarPoint(2024, 6, 21, 12.0);
    assert(c.declRad*D > 22.0 && c.declRad*D < 24.5);
    printf("OK\n"); return 0;
}
```
Run: `clang++ -std=c++14 /tmp/test_solar.cpp -o /tmp/test_solar && /tmp/test_solar`
Expected: 打印赤纬/经度且 `OK`。若断言失败先修公式。

- [ ] **Step 3: `EarthControlUI.h` 太阳段加「真实时间」开关 + 日期/时间**

在 `EarthControlUI.h` 顶部加 `#include <ctime>` 和 `#include <readerwriter/SolarPosition.h>`、`#include <modeling/Math.h>`。
在 struct 成员区加：
```cpp
    bool _realTimeSun, _followClock;     // 真实时间太阳；是否跟随系统时钟
    int  _year, _month, _day; float _utcHour;  // 手动设定的 UTC 日期/时间
```
构造函数初始化列表追加：`, _realTimeSun(false), _followClock(true), _year(2024), _month(6), _day(21), _utcHour(18.0f)`。
加一个方法（把当前选择的时间→WorldSunDir）：
```cpp
    void applyRealtimeSun()
    {
        int Y=_year, Mo=_month, D=_day; double H=_utcHour;
        if (_followClock)
        {
            time_t t = time(NULL); struct tm g; gmtime_r(&t, &g);
            Y=g.tm_year+1900; Mo=g.tm_mon+1; D=g.tm_mday;
            H = g.tm_hour + g.tm_min/60.0 + g.tm_sec/3600.0;
        }
        osgVerse::SubsolarPoint s = osgVerse::computeSubsolarPoint(Y, Mo, D, H);
        osg::Vec3d ecef = osgVerse::Coordinate::convertLLAtoECEF(
            osg::Vec3d(s.declRad, s.lonRad, 6.371e6));
        ecef.normalize();
        _earth->commonUniforms["WorldSunDir"]->set(osg::Vec3(ecef));
    }
```
在 `runInternal` 的「太阳 Sun」CollapsingHeader 内、现有滑块之后加 UI：
```cpp
                ImGui::Separator();
                if (ImGui::Checkbox(u8"真实时间太阳 Real-time", &_realTimeSun)) { if (_realTimeSun) applyRealtimeSun(); }
                if (_realTimeSun)
                {
                    ImGui::Checkbox(u8"跟随系统时钟", &_followClock);
                    if (!_followClock)
                    {
                        ImGui::InputInt(u8"年 Year", &_year);
                        ImGui::InputInt(u8"月 Month", &_month);
                        ImGui::InputInt(u8"日 Day", &_day);
                        ImGui::SliderFloat(u8"UTC 小时", &_utcHour, 0.0f, 24.0f, "%.1f");
                    }
                }
```
并在 `runInternal` 最开头（PushFont 之后）加：`if (_realTimeSun) applyRealtimeSun();`（每帧刷新，使跟随系统时钟时太阳随时间走；手动模式也即时反映输入）。

> 注意：真实时间开启时，原有「方位角/高度角」滑块与「太阳对准相机」按钮会被每帧覆盖——这是预期（真实时间优先）。可在那两个滑块外层加 `if (!_realTimeSun) { ... }` 使其在真实时间下禁用/隐藏（可选）。

- [ ] **Step 4: 确认 earth_main.cpp 无需额外改动**

`EarthControlUI::runInternal` 每帧被 ImGui 回调调用，已能驱动太阳；`OceanUpdater`/`update` 不覆盖 WorldSunDir。无需改 earth_main.cpp。

- [ ] **Step 5: 构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
Expected: 无 `error:`。

- [ ] **Step 6: 验证（注意：EARTH_SUN_TO_CAMERA 会抢太阳，验证时不要带它）**

Run（不带 EARTH_SUN_TO_CAMERA；定到某地，靠真实时间太阳照明）：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib EARTH_AUTOCAP=1500 EARTH_FRAME_SLEEP_MS=20 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 0 0 12000 > /tmp/sun.log 2>&1; ls -la /tmp/earth_capture_0.png
```
说明：自动截图默认 `_realTimeSun=false`，画面用初始 WorldSunDir。真实时间是交互功能，**人工终检**：`open dist/EarthExplorer.app`，勾选「真实时间太阳」，取消「跟随系统时钟」，把 UTC 小时从 0 拖到 24，观察晨昏线（昼夜分界）随时间在地球上自西向东扫过、日下点亮区位置与时间/经度一致。

- [ ] **Step 7: Commit**

```bash
cd /Users/franklee/osgverse && git add readerwriter/SolarPosition.h applications/earth_explorer/EarthControlUI.h && \
git commit -m "feat(earth-ui): real-time sun mode (system clock + settable UTC date/time)" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: 光照/色调/大气调优（#5，不改管线结构）

在不动管线结构的前提下提升观感：(a) 地球着色器 `hdr()` 之后加轻度饱和/对比；(b) UI 暴露已注册的 `GlobalOpaque`（大气/全局）滑块，配合已有曝光滑块。

**Files:**
- Modify: `assets/shaders/scattering_globe.frag.glsl`（`hdr()` 函数，约 23-30 行）
- Modify: `applications/earth_explorer/EarthControlUI.h`（渲染 Render 段）

- [ ] **Step 1: 着色器 `hdr()` 后加饱和/对比提升**

把 `assets/shaders/scattering_globe.frag.glsl` 的 `hdr()` 函数体最后 `return L;` 改为：
```glsl
    // 轻度提升饱和与对比，改善大气下偏灰的观感
    float luma = dot(L, vec3(0.299, 0.587, 0.114));
    L = mix(vec3(luma), L, 1.18);                 // +18% 饱和
    L = clamp((L - 0.5) * 1.06 + 0.5, 0.0, 1.0);  // +6% 对比
    return L;
```

- [ ] **Step 2: UI 渲染段加「大气强度」滑块**

在 `EarthControlUI.h` 成员区加 `float _globalOpaque;`，构造函数初始化追加 `, _globalOpaque(1.0f)`。在「渲染 Render」CollapsingHeader 内、曝光滑块之后加：
```cpp
                if (ImGui::SliderFloat(u8"大气强度 Atmosphere", &_globalOpaque, 0.0f, 1.0f, "%.2f"))
                    _earth->commonUniforms["GlobalOpaque"]->set(_globalOpaque);
```

- [ ] **Step 3: 构建（着色器是运行时读取，但仍重装以拷贝到 sdk_core/shaders）**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
然后确认着色器已更新到运行目录：
```bash
cp /Users/franklee/osgverse/assets/shaders/scattering_globe.frag.glsl /Users/franklee/osgverse/build/sdk_core/shaders/
```
（构建一般会拷贝 assets/shaders；若没有则手动拷如上。）

- [ ] **Step 4: 截图对比观感**

Run 全球截图（见通用命令，带 EARTH_SUN_TO_CAMERA），用 Read 看 `/tmp/earth_capture_0.png`：相比之前颜色更饱和、对比更分明、地表更通透。无过曝/死黑。若过冲，把 1.18/1.06 调小重做 Step 1、3、4。

- [ ] **Step 5: Commit**

```bash
cd /Users/franklee/osgverse && git add assets/shaders/scattering_globe.frag.glsl applications/earth_explorer/EarthControlUI.h && \
git commit -m "feat(earth): saturation/contrast tone-map tweak + atmosphere(GlobalOpaque) slider" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: 极地贴图扭曲修复（#3，高风险，需强验证）

根因：`TileCallback::computeTileExtent` 把 Web Mercator 的 XYZ 瓦片按**经纬度线性等分**纬度（`pow(0.5,z)`），而非按 Mercator 反投影算纬度边界——导致越靠近两极越拉伸。另外 `convertLLAtoECEF` 在 ±85.05° 有硬截断，使极区顶点退化。

**风险**：computeTileExtent 改动会影响所有纬度的瓦片定位，需对照「赤道不变形、海岸线对齐、两极不再拉伸」做强验证。若执行中发现全局错位，按"回退"处理。

**Files:**
- Create(临时测试): `/tmp/test_mercator.cpp`
- Modify: `readerwriter/TileCallback.cpp`（`computeTileExtent`，约 172-189 行）
- Modify: `modeling/Math.cpp`（`convertLLAtoECEF`，约 430-441 行）

- [ ] **Step 1: Mercator 反投影纯函数测试**

写 `/tmp/test_mercator.cpp`：
```cpp
#include <cassert>
#include <cstdio>
#include <cmath>
// Web Mercator: 行 row∈[0,n] 自上而下 → 纬度(度)。row=0→+maxLat, row=n/2→0, row=n→-maxLat
static double mercatorRowToLatDeg(double row, double n)
{
    const double PI = 3.14159265358979323846;
    double mercY = PI * (1.0 - 2.0 * row / n);            // [-PI, PI]
    double latRad = 2.0 * atan(exp(mercY)) - PI / 2.0;
    return latRad * 180.0 / PI;
}
int main()
{
    double n = 4.0;
    assert(std::fabs(mercatorRowToLatDeg(n/2.0, n) - 0.0) < 1e-6);   // 中线=赤道
    double top = mercatorRowToLatDeg(0.0, n);
    assert(top > 85.0 && top < 85.1);                                // 顶≈+85.05
    double bot = mercatorRowToLatDeg(n, n);
    assert(bot < -85.0 && bot > -85.1);                              // 底≈-85.05
    assert(mercatorRowToLatDeg(1.0, n) > mercatorRowToLatDeg(2.0, n)); // 单调
    printf("top=%.4f bot=%.4f OK\n", top, bot); return 0;
}
```
Run: `clang++ -std=c++14 /tmp/test_mercator.cpp -o /tmp/test_mercator && /tmp/test_mercator`
Expected: `top=85.05.. bot=-85.05.. OK`。

- [ ] **Step 2: 改 `computeTileExtent`，Web Mercator 下用反投影算纬度边界**

先读取 `readerwriter/TileCallback.cpp:172-189` 现有 `computeTileExtent`（确认 `_useWebMercator`、`_bottomLeft`、`_x/_y/_z`、`_extentMin/_extentMax` 成员名与单位：extent 为**度**）。把函数体替换为：
```cpp
void TileCallback::computeTileExtent(osg::Vec3d& tileMin, osg::Vec3d& tileMax,
                                     double& tileWidth, double& tileHeight) const
{
    if (_useWebMercator)
    {
        const double PI = osg::PI;
        double n = pow(2.0, (double)_z);
        // 经度：线性等分 [-180,180]
        double lonMinDeg = -180.0 + 360.0 * (double)_x / n;
        double lonMaxDeg = -180.0 + 360.0 * (double)(_x + 1) / n;
        // 行号：内部瓦片若 bottomLeft，则 y=0 在最底（南）；XYZ 行 0 在最顶（北）
        double rowTop = _bottomLeft ? (n - 1.0 - (double)_y) : (double)_y;       // 该瓦片上边对应的 XYZ 行
        double rowBot = rowTop + 1.0;
        double mercTop = PI * (1.0 - 2.0 * rowTop / n);
        double mercBot = PI * (1.0 - 2.0 * rowBot / n);
        double latMaxDeg = osg::RadiansToDegrees(2.0 * atan(exp(mercTop)) - PI * 0.5);
        double latMinDeg = osg::RadiansToDegrees(2.0 * atan(exp(mercBot)) - PI * 0.5);
        tileMin = osg::Vec3d(lonMinDeg, latMinDeg, 0.0);
        tileMax = osg::Vec3d(lonMaxDeg, latMaxDeg, 1.0);
        tileWidth = lonMaxDeg - lonMinDeg;
        tileHeight = latMaxDeg - latMinDeg;
        return;
    }
    // 非 Web Mercator：保留原线性逻辑
    double multiplier = pow(0.5, double(_z));
    tileWidth = multiplier * (_extentMax.x() - _extentMin.x());
    tileHeight = multiplier * (_extentMax.y() - _extentMin.y());
    if (!_bottomLeft)
    {
        osg::Vec3d origin(_extentMin.x(), _extentMax.y(), _extentMin.z());
        tileMin = origin + osg::Vec3d(double(_x) * tileWidth, -double(_y + 1) * tileHeight, 0.0);
        tileMax = origin + osg::Vec3d(double(_x + 1) * tileWidth, -double(_y) * tileHeight, 1.0);
    }
    else
    {
        tileMin = _extentMin + osg::Vec3d(double(_x) * tileWidth, double(_y) * tileHeight, 0.0);
        tileMax = _extentMin + osg::Vec3d(double(_x + 1) * tileWidth, double(_y + 1) * tileHeight, 1.0);
    }
}
```
> 关键不确定点：`_bottomLeft` 与 `_y` 的行号方向。earthURLs 设了 `OriginBottomLeft=1`，且 createCustomPath 对在线层做了 `yXYZ = 2^z-1-y` 翻转。务必在 Step 5 验证"南北没有上下颠倒"；若颠倒，把 `rowTop` 的 `_bottomLeft` 分支三元改为另一支。

- [ ] **Step 3: 移除 `convertLLAtoECEF` 的极地硬截断**

`modeling/Math.cpp:430-441`，删除这两行极地早返回（保留下面的通用公式）：
```cpp
    if (latitude > polarThreshold) return osg::Vec3d(0.0, 0.0, wgs84.radiusPolar + height);
    else if (latitude < -polarThreshold) return osg::Vec3d(0.0, 0.0, -(wgs84.radiusPolar + height));
```
（同时可删除其上不再使用的 `polarThreshold` 局部变量声明，避免未使用告警。）

- [ ] **Step 4: 构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
Expected: 无 `error:`。

- [ ] **Step 5: 强验证——赤道/中纬不变形 + 极区不再拉伸 + 南北不颠倒**

(a) 中纬对齐与不颠倒（旧金山，北半球；看海岸线方向正确、文字非镜像）：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib EARTH_AUTOCAP=2500 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 37.77 -122.42 60 > /tmp/p1.log 2>&1; ls -la /tmp/earth_capture_0.png
```
Read 看图：旧金山湾/海岸线形状正确、不上下颠倒、不左右镜像。

(b) 高纬/极区（如北纬 78°，斯瓦尔巴）不再极端拉伸：
```bash
rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib EARTH_AUTOCAP=2500 EARTH_FRAME_SLEEP_MS=30 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 78.2 15.6 200 > /tmp/p2.log 2>&1
```
Read 看图：高纬地形/海岸线比例正常，无向极点汇聚的极端拉伸/撕裂。

(c) 全球视角整体正常（见通用全球截图命令）。

若 (a) 南北颠倒 → 调 Step 2 的 `rowTop` 方向；若整体经度错位 → 检查 lon 公式与 `_x` 翻转的配合；若仍有问题且短时无法解决，**回退**本任务（`git checkout -- readerwriter/TileCallback.cpp modeling/Math.cpp` 并重建），其余 4 项不受影响。

- [ ] **Step 6: Commit**

```bash
cd /Users/franklee/osgverse && git add readerwriter/TileCallback.cpp modeling/Math.cpp && \
git commit -m "fix(earth): correct Web Mercator tile latitude mapping to remove polar distortion" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: 整体回归 + 文档 + 重打包

**Files:** 文档 + 打包（无新代码）

- [ ] **Step 1: 全链路回归**

依次跑：全球视角截图（通用命令）、定点街道级（`--goto 37.77 -122.42 6` + AUTOCAP=2500 + FRAME_SLEEP=30）、极区截图。Read 三张图确认：地球+大气+面板正常、街道级细节仍能流式下钻、极区不变形。`grep -cE "version '130'|FAILED|No reader/writer plugin for"` 应为 0。

- [ ] **Step 2: 更新文档**

在 `tasks/lessons.md` 追加本轮 5 项的要点（磁盘缓存路径/`EARTH_TILE_CACHE`、相机 `_minDistance` 夹紧、SolarPosition 公式与 WorldSunDir 注入、shader 色调调优、Web Mercator 纬度反投影修复 + 移除极地截断）。在 `docs/EarthExplorer.md` 增加「真实时间太阳」「大气/曝光调节」「瓦片缓存（`EARTH_TILE_CACHE` 环境变量）」的使用说明。

- [ ] **Step 3: 重打包 .app**

Run: `/Users/franklee/osgverse/packaging/package_macos.sh` （确认输出 `Built: .../dist/EarthExplorer.app`）

- [ ] **Step 4: Commit**

```bash
cd /Users/franklee/osgverse && git add tasks/lessons.md docs/EarthExplorer.md && \
git commit -m "docs(earth): document tile cache, min-distance, real-time sun, tone tuning, polar fix" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## 风险与回退

1. **极地修复（Task 5）最高风险**：computeTileExtent 改全局瓦片定位。验证不通过即 `git checkout` 回退该任务两个文件，不影响其余 4 项（各自独立提交）。
2. **缓存（Task 1）**：mimeType 旁文件必须在响应头解析之后写；命中后若解码失败，多半是 mimeType 没正确缓存——检查写入时机。可用 `EARTH_TILE_CACHE` 指定/隔离缓存目录，`rm -rf` 清缓存重试。
3. **真实时间太阳（Task 3）**：截图默认不开启该模式（且 `EARTH_SUN_TO_CAMERA` 会覆盖太阳）；务必人工交互终检晨昏线。NOAA 近似精度约 ±0.5°，演示足够，不追求天文级精度。
4. **光影（Task 4）**：着色器改动需确保拷到 `build/sdk_core/shaders/`；过冲就调小饱和/对比系数。
5. **相机最底点（Task 2）**：`_distance` 下限是"眼到视点中心"的距离，强倾斜时与海拔不完全等价，但足以防止穿地表；如需"严格离地海拔"可在 Task 2 基础上改用 computeEyeLatLonHeight()[2] 迭代夹紧（YAGNI，先不做）。

## 自检
- **Spec 覆盖**：缓存=Task1；最底点=Task2；真实时间=Task3；光影=Task4；极地=Task5；回归/文档=Task6。第 4 项真实感 3D 瓦片按你的决定**不在本计划**。✓
- **占位符扫描**：无 TBD/“适当处理”，纯逻辑均有可编译断言测试，集成均有截图验证。✓
- **类型/命名一致**：`computeSubsolarPoint`/`SubsolarPoint{declRad,lonRad}`/`applyRealtimeSun`、`clampDistanceValue`/`_minDistance`/`clampDistance`、`tileCacheFile`/`tileCacheDir`/`EARTH_TILE_CACHE`、`mercatorRowToLatDeg`(测试) 与 computeTileExtent 内联公式一致；uniform 键 `WorldSunDir`/`GlobalOpaque`/`HdrExposure`/`OceanOpaque` 与 UtilitiesEx.cpp 注册一致。✓
