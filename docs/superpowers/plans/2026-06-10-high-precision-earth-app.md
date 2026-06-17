# 高精度地球 EarthExplorer 完整应用 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把 osgVerse 的 EarthExplorer 升级为接入在线高精度瓦片（ArcGIS 影像 + AWS Terrarium 高程）、带可交互 ImGui 控制面板、可双击运行的 macOS .app 完整应用。

**Architecture:** 三层改动。(1) 数据层：改 `earth_main.cpp` 的 `earthURLs`/`createCustomPath` 指向在线 XYZ 服务，并给 `TileCallback` 新增 Terrarium(RGB→米) 高程解码；(2) UI 层：用仓库自带的 `osgVerse::ImGuiManager`（非现有 Drawer2D）在 EarthExplorer 中加一个控制面板，直接驱动 `EarthManipulator`/`EarthAtmosphereOcean`；(3) 打包层：post-build shell 脚本把 `build/sdk_core` 重排成 `.app`，用 `install_name_tool` 修 rpath，并提供命令行 `run.sh`。

**Tech Stack:** C++14/17，OpenSceneGraph 3.6.5（CORE/OpenGL 4.1），osgVerse，Dear ImGui（osgVerse 封装），libhv（HTTP），CMake，macOS `install_name_tool`。

**测试方法说明（重要）：** 这是 OpenGL 图形应用，无法用常规单元测试断言渲染结果。本计划采用仓库既有的验证方式：**构建 + 无头自动截图 + 日志检查**（已有的 `EARTH_AUTOCAP` 环境变量机制读取 GL framebuffer 截图）。唯一纯逻辑的部分（Terrarium 高度解码）写成可独立编译的真单元测试。每个任务的“验证”步即本项目的“测试”。

**关键路径前缀（全为绝对路径）：**
- 仓库根：`/Users/franklee/osgverse`
- CORE 构建：`/Users/franklee/osgverse/build/sdk_core`（产物）、`/Users/franklee/osgverse/build/verse_core`（增量构建目录）
- 可执行：`/Users/franklee/osgverse/build/sdk_core/bin/osgVerse_EarthExplorer`

**通用增量构建命令（每次改源码后用它，幂等增量）：**
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```

**通用截图验证命令（渲染 1500 帧给在线瓦片下载留时间，太阳朝相机以便看清，输出 /tmp/earth_capture_0.png）：**
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture*.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=1500 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1920 1080 > /tmp/earth_run.log 2>&1; \
ls -la /tmp/earth_capture*.png; \
grep -cE "version '130'|FAILED|No reader/writer plugin" /tmp/earth_run.log
```
之后用 Read 工具查看 `/tmp/earth_capture_0.png` 目视确认。

---

## File Structure

| 文件 | 改动 | 责任 |
|------|------|------|
| `applications/earth_explorer/earth_main.cpp` | 修改 | earthURLs 改在线源；重写 createCustomPath；接入 ImGui 控制面板 |
| `readerwriter/TileCallback.h` | 修改 | 新增 `ElevationEncoding` 枚举、setter、`decodeTerrarium` 声明、`decodeTerrariumHeight` 纯函数 |
| `readerwriter/TileCallback.cpp` | 修改 | 实现 Terrarium 解码；在 `createLayerImage(ELEVATION)` 注入解码 |
| `plugins/osgdb_tms/ReaderWriterTMS.cpp` | 修改 | 解析 `ElevationEncoding=terrarium` 选项并调用 setter |
| `applications/earth_explorer/EarthControlUI.h` | 新建 | ImGui 控制面板（ImGuiContentHandler 子类），含太阳/海洋/曝光/跳转/书签/读数 |
| `tests/terrarium_decode_test.cpp`（仅用 /tmp 编译） | 新建（临时） | Terrarium 解码纯函数单元测试 |
| `packaging/package_macos.sh` | 新建 | 把 sdk_core 打包成 .app，修 rpath，写 Info.plist |
| `packaging/run.sh` | 新建 | 命令行启动脚本（设 DYLD + cwd） |

---

## Phase 1 — 接入在线高精度数据

### Task 1: 影像切换到 ArcGIS World Imagery（高精度，低风险）

**Files:**
- Modify: `applications/earth_explorer/earth_main.cpp`（earthURLs 块 行 224-235；createCustomPath 行 185-207）

- [ ] **Step 1: 重写 `createCustomPath`，统一在线 XYZ 取瓦片**

把 `applications/earth_explorer/earth_main.cpp` 行 185-207 的整个 `createCustomPath` 函数替换为：

```cpp
static std::string createCustomPath(int type, const std::string& prefix, int x, int y, int z)
{
    // 内部瓦片是 TMS（OriginBottomLeft=1，原点左下）。在线服务（ArcGIS / Terrarium）
    // 都是 XYZ（原点左上），所以把 Y 翻转成 XYZ 行号。
    int yXYZ = (int)pow(2.0, (double)z) - 1 - y;
    if (type == osgVerse::TileCallback::ORTHOPHOTO)
        return osgVerse::TileCallback::createPath(prefix, x, yXYZ, z);
    else if (type == osgVerse::TileCallback::ELEVATION)
        return osgVerse::TileCallback::createPath(prefix, x, yXYZ, z);
    else if (type == osgVerse::TileCallback::USER)
        return osgVerse::TileCallback::createPath(prefix, x, yXYZ, z);
    // OCEAN_MASK：仍用本地 mbtiles（TMS，不翻转），深层级丢弃
    if (z > 3) return "";
    return osgVerse::TileCallback::createPath(prefix, x, y, z);
}
```

- [ ] **Step 2: 把 earthURLs 的 Orthophoto 改为 ArcGIS 在线影像**

把 `earth_main.cpp` 行 225-235 的 `#if !SIMPLE_VERSION ... #else ... #endif` 这整段 earthURLs 定义（连同 `#if`/`#else`/`#endif`）替换为下面这一段（去掉条件编译，固定用在线源）。本步只先改 Orthophoto，Elevation 暂时仍用本地，以便单独验证影像：

```cpp
    std::string earthURLs =
        " Orthophoto=https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"
        " Elevation=mbtiles://" + mainFolder + "/Earth/DEM_lv3.mbtiles/{z}-{x}-{y}.tif"
        " OceanMask=mbtiles://" + mainFolder + "/Earth/Mask_lv3.mbtiles/{z}-{x}-{y}.tif"
        " MaximumLevel=14 UseWebMercator=1 UseEarth3D=1 OriginBottomLeft=1"
        " TileElevationScale=3 TileSkirtRatio=" + skirtRatio;
```

> 注意：ArcGIS World_Imagery 的瓦片 REST 路径是 `/tile/{z}/{row}/{col}` 即 `/{z}/{y}/{x}`，`createPath` 会把 `{y}`→yXYZ、`{x}`→x、`{z}`→z 填入，正确。`MaximumLevel=14` 限制最深层级，避免过度请求。

- [ ] **Step 3: 增量构建**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
Expected: 末尾出现 `Installing: .../build/sdk_core/bin/osgVerse_EarthExplorer`，无 `error:`。

- [ ] **Step 4: 截图验证影像（联网）**

Run（同“通用截图验证命令”）：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture*.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=1500 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1920 1080 > /tmp/earth_run.log 2>&1; \
ls -la /tmp/earth_capture*.png; grep -c "ArcGIS\|arcgisonline\|status_code\|Failed getting" /tmp/earth_run.log
```
Expected: 生成 `/tmp/earth_capture_0.png`；用 Read 查看应见**比之前更清晰的卫星影像**（真实海岸线/地貌纹理，而非低分辨率 DOM_lv4）。日志里若出现大量 `Failed getting` 说明网络/URL 问题，需排查。

- [ ] **Step 5: Commit**

```bash
cd /Users/franklee/osgverse && git add applications/earth_explorer/earth_main.cpp && \
git commit -m "feat(earth): stream high-res ArcGIS World Imagery for orthophoto" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 2: 新增 Terrarium 高程解码 + 接入在线高程

**Files:**
- Create(临时): `/tmp/terrarium_decode_test.cpp`
- Modify: `readerwriter/TileCallback.h`（枚举区 行 52-53 附近；公开方法区；私有成员区）
- Modify: `readerwriter/TileCallback.cpp`（`createLayerImage` 行 87-110）
- Modify: `plugins/osgdb_tms/ReaderWriterTMS.cpp`（createTile 设参区 行 180-194）
- Modify: `applications/earth_explorer/earth_main.cpp`（earthURLs）

- [ ] **Step 1: 先写失败的纯函数单元测试**

写 `/tmp/terrarium_decode_test.cpp`：

```cpp
#include <cassert>
#include <cmath>
#include <cstdio>

// 与 TileCallback::decodeTerrariumHeight 必须逐字一致
static float decodeTerrariumHeight(unsigned char r, unsigned char g, unsigned char b)
{ return ((float)r * 256.0f + (float)g + (float)b / 256.0f) - 32768.0f; }

int main()
{
    // 海平面 0m：r*256+g+b/256 = 32768 → r=128,g=0,b=0
    assert(std::fabs(decodeTerrariumHeight(128, 0, 0) - 0.0f) < 1e-3f);
    // +1m → 32769 → r=128,g=1,b=0
    assert(std::fabs(decodeTerrariumHeight(128, 1, 0) - 1.0f) < 1e-3f);
    // 珠峰 ~8849m → 32768+8849=41617 → 41617/256=162 余 145 → r=162,g=145,b=0
    float h = decodeTerrariumHeight(162, 145, 0);
    printf("everest=%.3f\n", h);
    assert(h > 8848.0f && h < 8850.0f);
    // 海沟 -100m → 32668 → r=127,g=156,b=0 (127*256=32512, +156=32668)
    assert(std::fabs(decodeTerrariumHeight(127, 156, 0) - (-100.0f)) < 1e-3f);
    printf("OK\n");
    return 0;
}
```

- [ ] **Step 2: 运行测试，确认编译并通过（这是纯函数，先验证公式）**

Run:
```bash
clang++ -std=c++14 /tmp/terrarium_decode_test.cpp -o /tmp/terrarium_decode_test && /tmp/terrarium_decode_test
```
Expected: 输出 `everest=8849.000` 与 `OK`，退出码 0。若断言失败需修公式后重跑。

- [ ] **Step 3: 在 `TileCallback.h` 增加枚举、纯函数、声明与成员**

在 `readerwriter/TileCallback.h` 行 52（`enum LayerType ...`）之后新增一行枚举：

```cpp
    enum LayerType { ELEVATION = 0, ORTHOPHOTO, OCEAN_MASK, USER };
    enum LayerState { DONE = 0, DEFERRED, FAILED };
    enum ElevationEncoding { RAW_ELEVATION = 0, TERRARIUM_ELEVATION };
```

在 `setCreatePathFunction`（行 82-84）附近的公开方法区新增：

```cpp
    void setElevationEncoding(ElevationEncoding e) { _elevationEncoding = e; }
    ElevationEncoding getElevationEncoding() const { return _elevationEncoding; }

    // Terrarium PNG 把高度编码进 RGB：height = (R*256 + G + B/256) - 32768 (米)
    static float decodeTerrariumHeight(unsigned char r, unsigned char g, unsigned char b)
    { return ((float)r * 256.0f + (float)g + (float)b / 256.0f) - 32768.0f; }

    // 把 8 位 RGB Terrarium 图像转成单通道 GL_FLOAT 高度图（米）
    static osg::Image* decodeTerrarium(const osg::Image* src);
```

在私有成员区（与 `_createPathFunc` 同级）新增成员并在头文件无法初始化时于 .cpp 构造函数初始化：

```cpp
    ElevationEncoding _elevationEncoding;
```

- [ ] **Step 4: 在 `TileCallback.cpp` 构造函数初始化成员，并实现 `decodeTerrarium`**

在 `TileCallback.cpp` 的构造函数初始化列表里把 `_elevationEncoding` 初始化为 `RAW_ELEVATION`（找到构造函数，在成员初始化处补 `, _elevationEncoding(RAW_ELEVATION)`）。

在 `TileCallback.cpp` 文件中 `createLayerImage` 之前（约行 86 之上）新增实现：

```cpp
osg::Image* TileCallback::decodeTerrarium(const osg::Image* src)
{
    if (!src) return NULL;
    int w = src->s(), h = src->t();
    osg::ref_ptr<osg::Image> dst = new osg::Image;
    dst->allocateImage(w, h, 1, GL_RED, GL_FLOAT);
    dst->setInternalTextureFormat(GL_R32F);
    float* out = (float*)dst->data();
    for (int t = 0; t < h; ++t)
        for (int s = 0; s < w; ++s)
        {
            osg::Vec4 c = src->getColor(osg::Vec2(((float)s + 0.5f) / (float)w,
                                                  ((float)t + 0.5f) / (float)h));
            unsigned char r = (unsigned char)(c.r() * 255.0f + 0.5f);
            unsigned char g = (unsigned char)(c.g() * 255.0f + 0.5f);
            unsigned char b = (unsigned char)(c.b() * 255.0f + 0.5f);
            out[t * w + s] = decodeTerrariumHeight(r, g, b);
        }
    return dst.release();
}
```

- [ ] **Step 5: 在 `createLayerImage` 注入解码**

在 `TileCallback.cpp` 的 `createLayerImage`（行 87-110）里，定位到取得 `image` 之后、`tex2D->setImage(image.get())` 之前的位置（即 `if (!image) return NULL; else tex2D->setImage(image.get());` 这一行），替换为：

```cpp
    if (!image) return NULL;
    if (id == ELEVATION && _elevationEncoding == TERRARIUM_ELEVATION &&
        image->getDataType() == GL_UNSIGNED_BYTE)
    {
        osg::ref_ptr<osg::Image> decoded = TileCallback::decodeTerrarium(image.get());
        if (decoded.valid()) image = decoded;
    }
    tex2D->setImage(image.get());
    return tex2D.release();
```

> 之后 `createTileGeometry` 里 `elevation->getDataType() == GL_FLOAT` 判定为真（`useRealElevation`），直接把 `elevColor[0]` 当作米数高度使用，再乘 `_elevationScale`。

- [ ] **Step 6: 在 `ReaderWriterTMS.cpp` 解析 `ElevationEncoding` 选项**

在 `plugins/osgdb_tms/ReaderWriterTMS.cpp` 的 `createTile` 方法里、`tileCB->setElevationScale(...)` 附近（行 190-194 设参区），新增：

```cpp
    std::string elevEnc = opt->getPluginStringData("ElevationEncoding");
    std::transform(elevEnc.begin(), elevEnc.end(), elevEnc.begin(), ::tolower);
    if (elevEnc == "terrarium")
        tileCB->setElevationEncoding(osgVerse::TileCallback::TERRARIUM_ELEVATION);
    else
        tileCB->setElevationEncoding(osgVerse::TileCallback::RAW_ELEVATION);
```

> 若 `createTile` 顶部尚未 `#include <algorithm>`，在文件头补上 `#include <algorithm>`。

- [ ] **Step 7: earthURLs 改为在线 Terrarium 高程**

把 `earth_main.cpp` 的 earthURLs（Task 1 Step 2 改过的那段）整体替换为：

```cpp
    std::string earthURLs =
        " Orthophoto=https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"
        " Elevation=https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
        " OceanMask=mbtiles://" + mainFolder + "/Earth/Mask_lv3.mbtiles/{z}-{x}-{y}.tif"
        " ElevationEncoding=terrarium MaximumLevel=14 UseWebMercator=1 UseEarth3D=1 OriginBottomLeft=1"
        " TileElevationScale=2.0 TileSkirtRatio=" + skirtRatio;
```

- [ ] **Step 8: 增量构建（会重编 osgVerseReaderWriter + verse_tms 插件 + 应用）**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
Expected: 无 `error:`，末尾 `Installing: .../osgVerse_EarthExplorer`。

- [ ] **Step 9: 截图验证地形起伏 + 影像（联网）**

Run（通用截图验证命令，建议放大到有地形的区域：默认全球视角起伏不明显，可加 `EARTH_SUN_TO_CAMERA=1` 看明暗，地形起伏在斜射光下更明显）：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture*.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=2000 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1920 1080 > /tmp/earth_run.log 2>&1; \
ls -la /tmp/earth_capture*.png; \
grep -cE "version '130'|FAILED|No reader/writer plugin for" /tmp/earth_run.log; \
grep -c "Failed getting" /tmp/earth_run.log
```
Expected: 生成截图；`version '130'|FAILED|No reader/writer plugin for` 计数为 0。用 Read 查看截图：地球带高清影像；`Failed getting` 计数应较低（偶发网络重试可接受）。若 Terrarium 高程导致地形“尖刺”异常，把 `TileElevationScale` 调小（如 1.0）重试 Step 8-9。

- [ ] **Step 10: Commit**

```bash
cd /Users/franklee/osgverse && \
git add readerwriter/TileCallback.h readerwriter/TileCallback.cpp plugins/osgdb_tms/ReaderWriterTMS.cpp applications/earth_explorer/earth_main.cpp && \
git commit -m "feat(earth): decode AWS Terrarium online elevation tiles" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Phase 2 — ImGui 控制面板

### Task 3: 接入 ImGuiManager 脚手架（空面板能渲染）

**Files:**
- Create: `applications/earth_explorer/EarthControlUI.h`
- Modify: `applications/earth_explorer/earth_main.cpp`（头文件包含区 行 14-22；main() 结尾 viewer 配置区 行 287-298）

> `osgVerseUI`（含 `ImGuiManager`）已被 `applications/earth_explorer/CMakeLists.txt` 链接（`TARGET_LINK_LIBRARIES(... osgVerseUI ...)`），无需改 CMake。字体文件 `build/sdk_core/misc/LXGWFasmartGothic.otf` 已存在。

- [ ] **Step 1: 新建 `EarthControlUI.h` 最小可渲染面板**

写 `applications/earth_explorer/EarthControlUI.h`：

```cpp
#ifndef EARTH_CONTROL_UI_H
#define EARTH_CONTROL_UI_H

#include <osg/Vec3d>
#include <ui/ImGui.h>
#include <ui/ImGuiComponents.h>
#include <readerwriter/EarthManipulator.h>
#include <pipeline/Utilities.h>

// EarthExplorer 的 ImGui 控制面板。直接驱动 EarthManipulator 与 EarthAtmosphereOcean，
// 不经 USER 事件中转。
struct EarthControlUI : public osgVerse::ImGuiContentHandler
{
    osgVerse::EarthManipulator* _mani;
    osgVerse::EarthAtmosphereOcean* _earth;

    EarthControlUI(osgVerse::EarthManipulator* m, osgVerse::EarthAtmosphereOcean* e)
        : _mani(m), _earth(e) {}

    virtual void runInternal(osgVerse::ImGuiManager* mgr)
    {
        ImFont* font = ImGuiFonts.count("LXGWFasmartGothic") ? ImGuiFonts["LXGWFasmartGothic"] : NULL;
        if (font) ImGui::PushFont(font);
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Earth Control / 地球控制台"))
        {
            ImGui::Text("osgVerse EarthExplorer");
        }
        ImGui::End();
        if (font) ImGui::PopFont();
    }
};

#endif
```

- [ ] **Step 2: 在 earth_main.cpp 包含头文件并初始化 ImGui**

在 `earth_main.cpp` 的 include 区（行 14-22 附近，`#include <readerwriter/FileCache.h>` 之后）加：

```cpp
#include "EarthControlUI.h"
```

在 `earth_main.cpp` 的 `main()` 中，定位 `viewer.setCameraManipulator(earthManipulator.get());`（约行 292）之后、`int screenNo = ...`（约行 297）之前，插入：

```cpp
    // ImGui 控制面板
    osg::ref_ptr<osgVerse::ImGuiManager> imgui = new osgVerse::ImGuiManager;
    imgui->setChineseSimplifiedFont(MISC_DIR + std::string("LXGWFasmartGothic.otf"));
    imgui->initialize(new EarthControlUI(earthManipulator.get(), &earthRenderingUtils), false);
    imgui->addToView(&viewer);
    viewer.addEventHandler(imgui->getHandler());
```

> `MISC_DIR` 由 `pipeline/Utilities` 提供（值为 `BASE_DIR + "/misc/"`），`earth_main.cpp` 已间接包含。`earthManipulator` 与 `earthRenderingUtils` 均为 main() 内既有局部变量（行 243、265）。

- [ ] **Step 3: 增量构建**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
Expected: 无 `error:`，安装成功。

- [ ] **Step 4: 截图验证面板出现**

Run（通用截图验证命令）：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture*.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=1500 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1920 1080 > /tmp/earth_run.log 2>&1; \
ls -la /tmp/earth_capture*.png
```
Expected: 用 Read 查看截图，**左上角出现标题为 “Earth Control / 地球控制台” 的 ImGui 窗口**（含中文，证明字体加载成功）。若窗口空白或乱码，检查字体路径与 `setChineseSimplifiedFont`。

- [ ] **Step 5: Commit**

```bash
cd /Users/franklee/osgverse && \
git add applications/earth_explorer/EarthControlUI.h applications/earth_explorer/earth_main.cpp && \
git commit -m "feat(earth-ui): add ImGui control panel scaffold" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 4: 面板加入相机读数 + 太阳/海洋/曝光控制

**Files:**
- Modify: `applications/earth_explorer/EarthControlUI.h`

- [ ] **Step 1: 扩展 `EarthControlUI` 的成员与 runInternal**

把 `EarthControlUI.h` 的 `struct EarthControlUI { ... }` 整体替换为（在 Task 3 基础上新增成员与控件）：

```cpp
struct EarthControlUI : public osgVerse::ImGuiContentHandler
{
    osgVerse::EarthManipulator* _mani;
    osgVerse::EarthAtmosphereOcean* _earth;
    float _sunAz, _sunEl;     // 太阳方位角/高度角（度）
    float _exposure;          // HDR 曝光
    bool  _ocean;             // 海洋开关

    EarthControlUI(osgVerse::EarthManipulator* m, osgVerse::EarthAtmosphereOcean* e)
        : _mani(m), _earth(e), _sunAz(0.0f), _sunEl(0.0f), _exposure(0.25f), _ocean(true) {}

    void updateSun()
    {
        float az = osg::DegreesToRadians(_sunAz), el = osg::DegreesToRadians(_sunEl);
        osg::Vec3 dir(cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el));
        _earth->commonUniforms["WorldSunDir"]->set(dir);
    }

    virtual void runInternal(osgVerse::ImGuiManager* mgr)
    {
        ImFont* font = ImGuiFonts.count("LXGWFasmartGothic") ? ImGuiFonts["LXGWFasmartGothic"] : NULL;
        if (font) ImGui::PushFont(font);
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Earth Control / 地球控制台"))
        {
            // ---- 相机读数 ----
            if (ImGui::CollapsingHeader(u8"相机 Camera", ImGuiTreeNodeFlags_DefaultOpen))
            {
                osg::Vec3d lla = _mani->computeEyeLatLonHeight();  // 弧度, 弧度, 米
                ImGui::Text(u8"纬度 Lat: %.5f", osg::RadiansToDegrees(lla[0]));
                ImGui::Text(u8"经度 Lon: %.5f", osg::RadiansToDegrees(lla[1]));
                ImGui::Text(u8"高度 Alt: %.1f km", lla[2] / 1000.0);
                if (ImGui::Button(u8"回到全球视角 Home")) _mani->home(0.0);
            }

            // ---- 太阳/光照 ----
            if (ImGui::CollapsingHeader(u8"太阳 Sun", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::SliderFloat(u8"方位角 Azimuth", &_sunAz, -180.0f, 180.0f, "%.0f")) updateSun();
                if (ImGui::SliderFloat(u8"高度角 Elevation", &_sunEl, -90.0f, 90.0f, "%.0f")) updateSun();
                if (ImGui::Button(u8"太阳对准相机")) {
                    osg::Vec3d lla = _mani->computeEyeLatLonHeight();
                    _sunAz = (float)osg::RadiansToDegrees(lla[1]);
                    _sunEl = (float)osg::RadiansToDegrees(lla[0]);
                    updateSun();
                }
            }

            // ---- 渲染 ----
            if (ImGui::CollapsingHeader(u8"渲染 Render", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Checkbox(u8"海洋 Ocean", &_ocean))
                    _earth->commonUniforms["OceanOpaque"]->set(_ocean ? 1.0f : 0.0f);
                if (ImGui::SliderFloat(u8"曝光 Exposure", &_exposure, 0.05f, 1.0f, "%.2f"))
                    _earth->commonUniforms["HdrExposure"]->set(_exposure);
            }
        }
        ImGui::End();
        if (font) ImGui::PopFont();
    }
};
```

> 依据：`computeEyeLatLonHeight()`（EarthManipulator.h:100，返回弧度/米）；`home(double)`（EarthManipulator.h:171）；`commonUniforms` 键 `WorldSunDir`/`OceanOpaque`/`HdrExposure`（Utilities.h:262 区，初值 (-1,0,0)/1.0/0.25）。

- [ ] **Step 2: 增量构建**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
Expected: 无 `error:`。

- [ ] **Step 3: 截图验证（面板含读数与滑块）**

Run（通用截图验证命令，见上）。Expected: 用 Read 查看截图，面板出现“相机/太阳/渲染”三个可折叠区，相机区显示经纬度与高度数值，太阳区有两个滑块。

- [ ] **Step 4: Commit**

```bash
cd /Users/franklee/osgverse && git add applications/earth_explorer/EarthControlUI.h && \
git commit -m "feat(earth-ui): camera readout + sun/ocean/exposure controls" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

### Task 5: 面板加入“跳转到经纬度” + 相机书签

**Files:**
- Modify: `applications/earth_explorer/EarthControlUI.h`

- [ ] **Step 1: 增加跳转与书签成员/控件**

在 `EarthControlUI` 结构体中，于现有成员后追加：

```cpp
    float _gotoLat, _gotoLon, _gotoAltKm;            // 跳转目标
    int   _bookmarkTime;                             // 下一个书签的时间戳（帧）
```
在构造函数初始化列表末尾追加：`, _gotoLat(35.36f), _gotoLon(138.73f), _gotoAltKm(50.0f), _bookmarkTime(0)`（默认富士山附近）。

在 `runInternal` 的 `ImGui::End();` 之前、`渲染 Render` 折叠区之后，新增两段：

```cpp
            // ---- 跳转 ----
            if (ImGui::CollapsingHeader(u8"跳转 Go To", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::InputFloat(u8"纬度 Lat", &_gotoLat, 0.0f, 0.0f, "%.4f");
                ImGui::InputFloat(u8"经度 Lon", &_gotoLon, 0.0f, 0.0f, "%.4f");
                ImGui::InputFloat(u8"高度 km", &_gotoAltKm, 0.0f, 0.0f, "%.1f");
                if (ImGui::Button(u8"飞过去 Go"))
                {
                    _mani->setByEye(osg::DegreesToRadians((double)_gotoLat),
                                    osg::DegreesToRadians((double)_gotoLon),
                                    (double)_gotoAltKm * 1000.0);
                }
            }

            // ---- 书签/巡游 ----
            if (ImGui::CollapsingHeader(u8"书签 Bookmarks"))
            {
                if (ImGui::Button(u8"记录当前视角 Save"))
                {
                    _mani->insertControlPointFromCurrentView((float)_bookmarkTime);
                    _bookmarkTime += 60;
                }
                ImGui::SameLine();
                ImGui::Text(u8"已存 %d", (int)_mani->getControlPoints().size());
                if (ImGui::Button(u8"播放巡游 Play"))
                {
                    if (!_mani->getControlPoints().empty()) _mani->startAnimation();
                }
                ImGui::SameLine();
                if (ImGui::Button(u8"停止 Stop")) _mani->stopAnimation(false);
                if (ImGui::Button(u8"清空 Clear")) { _mani->clearControlPoints(); _bookmarkTime = 0; }
            }
```

> 依据：`setByEye(double lat, double lon, double height, float doa=0)`（EarthManipulator.h:85，弧度/米）；`insertControlPointFromCurrentView(float time)`（h:339）；`getControlPoints()`（h:332）；`startAnimation()/stopAnimation(bool)`（h:358-359）；`clearControlPoints()`（h:344）。

- [ ] **Step 2: 增量构建**

Run:
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
Expected: 无 `error:`。

- [ ] **Step 3: 截图验证“跳转”生效（用脚本预设跳转，无头确认相机移动）**

由于无头模式无法点按钮，改为命令行交互验证：正常启动应用（有显示），手动在面板输入经纬度点 Go，确认相机飞到目标。自动验证退而求其次——确认编译链接通过且面板新增“跳转/书签”折叠区出现在截图中：

Run（通用截图验证命令）。Expected: 截图中面板底部出现“跳转 Go To”（含三个输入框与 Go 按钮）与“书签 Bookmarks”折叠区。

- [ ] **Step 4: 人工交互验证（需显示器）**

Run:
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800
```
手动：在“跳转”里保留默认富士山经纬度，点 “飞过去 Go”，确认相机跳到日本本州中部并能看到富士山地形；点“记录当前视角”几次后“播放巡游”，确认相机沿书签平滑移动。ESC 退出。

- [ ] **Step 5: Commit**

```bash
cd /Users/franklee/osgverse && git add applications/earth_explorer/EarthControlUI.h && \
git commit -m "feat(earth-ui): go-to lat/lon and camera bookmarks/tour" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Phase 3 — 打包为可运行的 macOS 应用

### Task 6: 打包脚本（.app + rpath 修复）与命令行启动脚本

**Files:**
- Create: `packaging/run.sh`
- Create: `packaging/package_macos.sh`

- [ ] **Step 1: 命令行启动脚本 `packaging/run.sh`**

写 `packaging/run.sh`：

```bash
#!/bin/sh
# 命令行启动 EarthExplorer（CORE 构建产物，设好 DYLD 与工作目录）
SDK="$(cd "$(dirname "$0")/../build/sdk_core" && pwd)"
cd "$SDK/bin" || exit 1
DYLD_LIBRARY_PATH="$SDK/lib" exec ./osgVerse_EarthExplorer "$@"
```

Run（赋可执行 + 冒烟，1 秒后用 Ctrl-C 或超时退出即可）：
```bash
chmod +x /Users/franklee/osgverse/packaging/run.sh && \
echo "run.sh ready"
```
Expected: 输出 `run.sh ready`。

- [ ] **Step 2: 打包脚本 `packaging/package_macos.sh`**

写 `packaging/package_macos.sh`：

```bash
#!/bin/bash
# 把 build/sdk_core 打包成可双击运行的 EarthExplorer.app（修 rpath / 写 Info.plist）
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SDK="$REPO/build/sdk_core"
APP="$REPO/dist/EarthExplorer.app"
PLUGVER="osgPlugins-3.6.5"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
mkdir -p "$APP/Contents/lib/$PLUGVER"
mkdir -p "$APP/Contents/bin"

# 1) 可执行文件
cp "$SDK/bin/osgVerse_EarthExplorer" "$APP/Contents/MacOS/"

# 2) 所有 dylib/.so（含符号链接）到 Contents/lib
cp -a "$SDK/lib/"*.dylib "$APP/Contents/lib/" 2>/dev/null || true
cp -a "$SDK/lib/"*.so "$APP/Contents/lib/" 2>/dev/null || true

# 3) 插件到 Contents/lib/osgPlugins-3.6.5 （真实位置）并在 Contents/bin 建同名软链
#    （代码按 BASE_DIR/bin/osgPlugins-3.6.5 搜索；同时 rpath 指 ../lib 找 dylib）
cp -a "$SDK/lib/$PLUGVER/"*.so "$APP/Contents/lib/$PLUGVER/"
ln -s "../lib/$PLUGVER" "$APP/Contents/bin/$PLUGVER"

# 4) 资源目录（代码按 BASE_DIR=".." 即 Contents 下查找）
for d in shaders skyboxes textures misc models; do
    cp -a "$SDK/$d" "$APP/Contents/$d"
done

# 5) 修 rpath：可执行文件 —— 删掉 Linux 的 $ORIGIN，加 @executable_path/../lib
install_name_tool -delete_rpath '$ORIGIN:$ORIGIN/../lib' "$APP/Contents/MacOS/osgVerse_EarthExplorer" 2>/dev/null || true
install_name_tool -add_rpath '@executable_path/../lib' "$APP/Contents/MacOS/osgVerse_EarthExplorer"

# 6) 修 rpath：Contents/lib 下每个 dylib/.so —— 加 @loader_path（同级互相引用）
for f in "$APP/Contents/lib/"*.dylib "$APP/Contents/lib/"*.so; do
    [ -f "$f" ] || continue
    install_name_tool -delete_rpath '$ORIGIN:$ORIGIN/../lib' "$f" 2>/dev/null || true
    install_name_tool -delete_rpath "$SDK/lib" "$f" 2>/dev/null || true
    install_name_tool -add_rpath '@loader_path' "$f" 2>/dev/null || true
done

# 7) 修 rpath：插件（在 Contents/lib/osgPlugins-3.6.5）引用 @rpath/libosg* → 指向上一级 lib
for f in "$APP/Contents/lib/$PLUGVER/"*.so; do
    [ -f "$f" ] || continue
    install_name_tool -delete_rpath "$SDK/lib" "$f" 2>/dev/null || true
    install_name_tool -add_rpath '@loader_path/..' "$f" 2>/dev/null || true
done

# 8) Info.plist
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>EarthExplorer</string>
  <key>CFBundleDisplayName</key><string>osgVerse EarthExplorer</string>
  <key>CFBundleIdentifier</key><string>com.osgverse.earthexplorer</string>
  <key>CFBundleVersion</key><string>1.0.0</string>
  <key>CFBundleShortVersionString</key><string>1.0.0</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleExecutable</key><string>osgVerse_EarthExplorer</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSMinimumSystemVersion</key><string>11.0</string>
</dict>
</plist>
PLIST

# 9) ad-hoc 代码签名（本机运行；Gatekeeper 对未签名 dylib 较严）
codesign --force --deep --sign - "$APP" 2>/dev/null || echo "[warn] codesign skipped"

echo "Built: $APP"
```

- [ ] **Step 3: 运行打包脚本**

Run:
```bash
chmod +x /Users/franklee/osgverse/packaging/package_macos.sh && \
/Users/franklee/osgverse/packaging/package_macos.sh
```
Expected: 末尾输出 `Built: .../dist/EarthExplorer.app`，无 `install_name_tool` 致命错误。

- [ ] **Step 4: 验证 .app 的 rpath 已无 `$ORIGIN`/绝对路径**

Run:
```bash
otool -l /Users/franklee/osgverse/dist/EarthExplorer.app/Contents/MacOS/osgVerse_EarthExplorer | grep -A2 LC_RPATH; \
echo "---one dylib---"; \
otool -l /Users/franklee/osgverse/dist/EarthExplorer.app/Contents/lib/libosg.3.6.5.dylib | grep -A2 LC_RPATH
```
Expected: 可执行文件 rpath 含 `@executable_path/../lib`，dylib rpath 含 `@loader_path`，均不再出现 `$ORIGIN` 或 `/Users/franklee/...` 绝对路径。

- [ ] **Step 5: 双击式启动验证（用 `open`，不依赖 DYLD 环境变量）**

Run（`open` 模拟 Finder 双击；随后等几秒截屏确认窗口起来，再关闭）：
```bash
open /Users/franklee/osgverse/dist/EarthExplorer.app; sleep 12; \
screencapture -x /tmp/app_launch.png 2>/dev/null; \
osascript -e 'tell application "osgVerse_EarthExplorer" to quit' 2>/dev/null || \
pkill -f "EarthExplorer.app/Contents/MacOS" 2>/dev/null; \
ls -la /tmp/app_launch.png
```
Expected: 若 `screencapture` 因屏幕录制权限失败，改为检查进程是否存活来判断启动成功：
```bash
open /Users/franklee/osgverse/dist/EarthExplorer.app; sleep 10; \
pgrep -fl "EarthExplorer.app/Contents/MacOS/osgVerse_EarthExplorer" && echo "APP RUNNING (launch OK)"; \
pkill -f "EarthExplorer.app/Contents/MacOS" 2>/dev/null
```
Expected: 输出 `APP RUNNING (launch OK)`，证明 .app 不依赖 DYLD_LIBRARY_PATH 即可加载所有 dylib/插件并启动（rpath 修复成功）。若进程秒退，用 `Console.app` 或 `/tmp` 重定向看 dyld 报错，按缺失库补对应 rpath。

- [ ] **Step 6: Commit**

```bash
cd /Users/franklee/osgverse && git add packaging/run.sh packaging/package_macos.sh && \
git commit -m "feat(packaging): macOS .app bundler with rpath fixup + run.sh" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Phase 4 — 整体验收

### Task 7: 端到端验收

**Files:** 无（仅运行验证）

- [ ] **Step 1: 命令行整链验证（在线数据 + UI + 截图）**

Run（通用截图验证命令，EARTH_AUTOCAP=2000 给在线瓦片足够下载时间）：
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture*.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=2000 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1920 1080 > /tmp/earth_final.log 2>&1; \
echo "shader/plugin errors:"; grep -cE "version '130'|FAILED|No reader/writer plugin for" /tmp/earth_final.log; \
echo "GL context:"; grep "OpenGL Driver" /tmp/earth_final.log
```
Expected: 错误计数 0；GL 上下文为 `4.1 ... GLSL: 410`；用 Read 看 `/tmp/earth_capture_0.png`：高清地球 + 左上 ImGui 中文面板。

- [ ] **Step 2: .app 双击启动验证**

Run（同 Task 6 Step 5 的进程存活检查）。Expected: `APP RUNNING (launch OK)`。

- [ ] **Step 3: 人工交互终检（需显示器）**

Run:
```bash
open /Users/franklee/osgverse/dist/EarthExplorer.app
```
逐项确认：(a) 鼠标拖动旋转、滚轮缩放，近景影像越放越清晰（ArcGIS 深层级）；(b) 面板“跳转”到富士山，地形有起伏（Terrarium 高程）；(c) 太阳滑块改变明暗；(d) 海洋开关、曝光滑块生效；(e) 书签记录+播放巡游。全部正常即验收通过。ESC/退出。

- [ ] **Step 4: 更新经验与文档**

把本次新增能力记入 `tasks/lessons.md`（在线数据源 URL、Terrarium 解码公式、.app 打包 rpath 要点），并在仓库根 `README` 或 `docs/` 增加一段“运行 EarthExplorer 应用”的说明（命令行用 `packaging/run.sh`，桌面用 `dist/EarthExplorer.app`）。

- [ ] **Step 5: Commit**

```bash
cd /Users/franklee/osgverse && git add tasks/lessons.md docs/ && \
git commit -m "docs(earth): record online-data + Terrarium + .app packaging notes" \
  -m "Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## 风险与回退

1. **在线服务可用性 / 服务条款**：ArcGIS World_Imagery、AWS Terrarium 为公开 XYZ 瓦片；若被限流或不可用，回退方案：影像改 `https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}`，高程回退本地 `DEM_lv3.mbtiles`（去掉 `ElevationEncoding=terrarium`，Elevation 改回 mbtiles）。两者只需改 earthURLs 一行。
2. **Terrarium 地形尖刺/接缝**：相邻瓦片边缘高度不连续可能造成裂缝。先用 `TileElevationScale` 调小缓解；若仍明显，作为后续任务在 `decodeTerrarium` 做边缘 1px 复制或在 `createTileGeometry` 加邻接采样（不在本计划范围）。
3. **ImGui 在 CORE/GL4.1 渲染**：`tests/imgui_test.cpp` 同在 CORE 构建中，理论可用；若面板不显示，先单独构建并运行 `osgVerse_Test_ImGui` 确认 ImGui 后端在本机 OK，再排查 EarthExplorer 集成。
4. **.app dyld 失败**：若双击秒退，几乎都是某个 dylib/插件 rpath 没修到。用 `DYLD_PRINT_LIBRARIES=1 ./osgVerse_EarthExplorer` 或 Console.app 定位缺失库，对应补 `install_name_tool -add_rpath`。
5. **截图权限**：OS 级 `screencapture` 需屏幕录制权限，不可编程授予；本计划一律用应用内 `EARTH_AUTOCAP`（glReadPixels）截图，仅 .app 启动验证用进程存活判断，规避该限制。

---

## 自检（Self-Review）记录

- **Spec 覆盖**：高精度数据=Task1(影像)+Task2(高程)；UI=Task3-5；完整可运行应用=Task6(.app)+Task7(验收)。✓
- **占位符扫描**：无 TBD/“适当处理”；所有代码步给出完整可粘贴代码。✓
- **类型/命名一致**：`ElevationEncoding`/`TERRARIUM_ELEVATION`/`decodeTerrarium`/`decodeTerrariumHeight`/`setElevationEncoding` 在 .h 声明与 .cpp/插件/earthURLs 用法一致；UI 用的 `computeEyeLatLonHeight`/`setByEye`/`home`/`insertControlPointFromCurrentView`/`startAnimation`/`stopAnimation`/`clearControlPoints`/`getControlPoints` 与 EarthManipulator.h 签名一致；uniform 键 `WorldSunDir`/`OceanOpaque`/`HdrExposure` 与 Utilities.h 一致。✓
