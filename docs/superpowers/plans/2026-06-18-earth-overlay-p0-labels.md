# P0：图层框架雏形 + 标注开关 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 把烤进卫星图的 Google 标注拆成可开关、可调透明度的独立图层，并搭起最小 LayerManager + 「图层」UI 折叠栏，为后续叠加层打基础。

**Architecture:** 底图 Google `lyrs=y`→`lyrs=s`（纯卫星）；标注 `lyrs=h`（透明）走现成的 USER/ExtraLayer 通道（texUnit 2 + UvOffset3），着色器 `scattering_globe.frag.glsl` 已合成该层，只需加一个 `LabelOpacity` uniform 门控。一个轻量 `LayerManager` 持有图层状态，「图层」UI（方案 B 分类折叠）读写它，标注层最终驱动 `commonUniforms["LabelOpacity"]`。

**Tech Stack:** C++ / OpenSceneGraph (CORE/GL4.1) / osgVerse / ImGui / GLSL。

**验证方式（重要）：** 本项目无 app 层单元测试框架，既定验证方式是 **增量构建 + 无头截图 + Read 看图**（见 `tasks/lessons.md`）。本计划每个任务以「构建 + 定点截图 + 目视确认」作为测试，而非单元测试。

**构建命令：**
```
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```

**截图命令模板（贴地看标注遮挡最关键）：**
```
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=550 EARTH_SUN_TO_CAMERA=1 EARTH_FRAME_SLEEP_MS=40 \
./osgVerse_EarthExplorer --no-wait --goto <lat> <lon> <altKm> --resolution 1280 800 > /tmp/run.log 2>&1
```
然后用 Read 看 `/tmp/earth_capture_0.png`。`EARTH_AUTOCAP` 勿超 ~650（之后 ImGui 后端 shutdown 抓不到）。

---

## 前置：确认运行时着色器路径

- [ ] **Step 0.1：确认 `SHADER_DIR` 指向哪里**

Run:
```
grep -rn "define SHADER_DIR\|SHADER_DIR =" /Users/franklee/osgverse --include=*.h --include=*.cpp | grep -v "build/\|dist/"
```
确认编辑 `assets/shaders/scattering_globe.frag.glsl` 后，构建/安装会把它复制到运行时读取目录。若运行时从别处读（如安装目录），构建步骤需带上 shader 复制。把结论记到本任务下，后续截图才可信。

---

## Task 1：核心加 `LabelOpacity` uniform + 着色器门控（向后兼容）

**Files:**
- Modify: `pipeline/UtilitiesEx.cpp:503`（commonUniforms 初始化处）
- Modify: `assets/shaders/scattering_globe.frag.glsl`（uniform 声明 + 合成行）

- [ ] **Step 1.1：在 commonUniforms 注册 LabelOpacity（默认 1.0）**

在 `pipeline/UtilitiesEx.cpp` 第 503 行 `commonUniforms["HdrExposure"] = ...` 之后加一行：
```cpp
    commonUniforms["LabelOpacity"] = new osg::Uniform("LabelOpacity", 1.0f);
```
默认 1.0 保证 `tests/earth_test`、`wasm/earth_demo` 行为不变（它们走同一 create 路径）。

- [ ] **Step 1.2：着色器声明 uniform**

在 `assets/shaders/scattering_globe.frag.glsl` 的 `uniform float HdrExposure, GlobalOpaque;` 行改为：
```glsl
uniform float HdrExposure, GlobalOpaque, LabelOpacity;
```

- [ ] **Step 1.3：着色器合成行加门控**

把：
```glsl
    groundColor.rgb = mix(groundColor.rgb, layerColor.rgb, layerColor.a);
```
改为：
```glsl
    groundColor.rgb = mix(groundColor.rgb, layerColor.rgb, layerColor.a * LabelOpacity);
```
`LabelOpacity` 默认 1.0，未接标注层时 `layerColor.a==0`，无视觉变化。

- [ ] **Step 1.4：构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
Expected: 构建成功，install 到 `build/sdk_core/bin/osgVerse_EarthExplorer`。若 Step 0.1 发现 shader 不自动复制，这里手动 cp `assets/shaders/scattering_globe.frag.glsl` 到运行时目录。

- [ ] **Step 1.5：回归截图（此时还没改底图，应与现状一致）**

Run 截图模板：`--goto 39.9 116.4 300`（北京中纬）。
Read `/tmp/earth_capture_0.png`。
Expected: 与改动前一致（仍是 lyrs=y 烤标注），证明加 uniform 无回归。

- [ ] **Step 1.6：提交**
```bash
git add pipeline/UtilitiesEx.cpp assets/shaders/scattering_globe.frag.glsl
git commit -m "feat(earth): add LabelOpacity uniform gating the ExtraLayer composite"
```

---

## Task 2：底图换纯卫星 + 接通 USER 标注层（lyrs=h）

**Files:**
- Modify: `applications/earth_explorer/earth_main.cpp`（`createCustomPath` ~191-201；`earthURLs` ~226-233）
- Modify: `plugins/osgdb_tms/ReaderWriterTMS.cpp`（option 解析、createTile 签名与调用、USER 预载）

- [ ] **Step 2.1：`createCustomPath` 底图换 lyrs=s、USER 返回 lyrs=h**

在 `applications/earth_explorer/earth_main.cpp` 的 `createCustomPath`：
把 ORTHOPHOTO 分支的：
```cpp
        static const std::string google = "https://mt1.google.com/vt/lyrs=y&x={x}&y={y}&z={z}";
        return osgVerse::TileCallback::createPath(google, x, yXYZ, z);
```
改为：
```cpp
        static const std::string google = "https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}";
        return osgVerse::TileCallback::createPath(google, x, yXYZ, z);
```
把 USER 分支：
```cpp
    else if (type == osgVerse::TileCallback::USER)
        return osgVerse::TileCallback::createPath(prefix, x, yXYZ, z);
```
改为：
```cpp
    else if (type == osgVerse::TileCallback::USER)
    {
        // Google 透明标注层（路网 + 地名），与底图同瓦片方案，叠在 ExtraLayer(unit2)
        static const std::string googleLabels = "https://mt1.google.com/vt/lyrs=h&x={x}&y={y}&z={z}";
        return osgVerse::TileCallback::createPath(googleLabels, x, yXYZ, z);
    }
```

- [ ] **Step 2.2：`earthURLs` 启用 USER 层占位符**

在 `applications/earth_explorer/earth_main.cpp` 的 `earthURLs` 串里，`Orthophoto=google` 之后加一行 `User=googleLabels`（占位符，真实 URL 在 createCustomPath 拼）：
```cpp
        " Orthophoto=google"
        " User=googleLabels"
        " Elevation=https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png"
```

- [ ] **Step 2.3：TMS 插件声明并解析 `User` 选项**

在 `plugins/osgdb_tms/ReaderWriterTMS.cpp` 构造函数里，`supportsOption("OceanMask", ...)` 之后加：
```cpp
        supportsOption("User", "TMS server URL with wildcards or .mbtiles, applied as extra overlay layer");
```
在 `readNode` 里，`orthoAddr` 解析（约 112-113 行）之后加：
```cpp
            std::string userAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("User"));
```
把 `createTile(...)` 调用（约 139-141 行）改为多传 `userAddr`：
```cpp
                    osg::ref_ptr<osg::Node> node = createTile(
                        elevAddr, orthoAddr, maskAddr, userAddr, x + xx, y + yy, z,
                        extentMin, extentMax, options, useWM, flatten);
```

- [ ] **Step 2.4：createTile 签名加 userPath，设层路径**

把 `createTile` 签名（约 162-165 行）改为：
```cpp
    osg::Node* createTile(const std::string& elevPath, const std::string& orthPath,
                          const std::string& maskPath, const std::string& userPath, int x, int y, int z,
                          const osg::Vec3d& extentMin, const osg::Vec3d& extentMax,
                          const Options* opt, bool useWM, bool flatten) const
```
在 `tileCB->setLayerPath(... OCEAN_MASK, maskPath);`（约 184 行）之后加：
```cpp
        tileCB->setLayerPath(osgVerse::TileCallback::USER, userPath);
```

- [ ] **Step 2.5：createTile 同步预载 USER 纹理并绑 texUnit 2 + UvOffset3**

在 maskImage 绑定块（约 247-258 行）之后、`osg::ref_ptr<osg::Geode> geode ...` 之前，加（仿 maskImage 块）：
```cpp
        bool emptyPathU = false;
        osg::ref_ptr<osg::Texture> userImage =
            tileCB->createLayerImage(osgVerse::TileCallback::USER, emptyPathU, opt);
        if (userImage.valid())
        {
            userImage->setClientStorageHint(false);
            userImage->setUnRefImageDataAfterApply(true);
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
            geom->getOrCreateStateSet()->setTextureAttribute(2, userImage.get());
#else
            geom->getOrCreateStateSet()->setTextureAttributeAndModes(2, userImage.get());
#endif
            geom->getOrCreateStateSet()->getOrCreateUniform("UvOffset3", osg::Uniform::FLOAT_VEC4)
                                       ->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        }
```

- [ ] **Step 2.6：构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
Expected: 构建成功。

- [ ] **Step 2.7：截图验证——标注此刻应显示（默认 LabelOpacity=1）且底图变纯卫星**

Run 截图模板：`--goto 39.9 116.4 300`（北京），再 `--goto 45 12 6000`（全欧洲低 zoom）。
Read 两图。
Expected: 仍能看到路网/地名（来自 lyrs=h 标注层），但底图是纯卫星（lyrs=s）。欧洲低 zoom 海岸线比例正常无回归。若标注完全没出现 → USER 层未加载，回查 Step 2.3-2.5。

- [ ] **Step 2.8：提交**
```bash
git add applications/earth_explorer/earth_main.cpp plugins/osgdb_tms/ReaderWriterTMS.cpp
git commit -m "feat(earth): split labels into USER/lyrs=h overlay over lyrs=s base imagery"
```

---

## Task 3：最小 LayerManager

**Files:**
- Create: `applications/earth_explorer/LayerManager.h`

- [ ] **Step 3.1：写 LayerManager.h**

新建 `applications/earth_explorer/LayerManager.h`：
```cpp
#ifndef EARTH_LAYER_MANAGER_H
#define EARTH_LAYER_MANAGER_H

#include <string>
#include <vector>
#include <functional>

// 统一图层注册表。P0 只管标注层；结构留好分组/类型/透明度，供后续 P1+ 扩展。
struct OverlayLayer
{
    enum Type { RasterTile, PointFeed, Grid };
    std::string id, displayName, group;
    Type type = RasterTile;
    bool enabled = false;
    bool hasOpacity = false;     // 栅格层有透明度滑块
    float opacity = 1.0f;
    bool needsKey = false;       // UI 标 🔑、缺 key 时灰显
    // 应用回调：enabled/opacity 变化时被调用（P0 里标注层把它接到 LabelOpacity uniform）
    std::function<void(const OverlayLayer&)> apply;
};

class LayerManager
{
public:
    OverlayLayer& add(const OverlayLayer& l) { _layers.push_back(l); return _layers.back(); }
    std::vector<OverlayLayer>& layers() { return _layers; }

    void setEnabled(const std::string& id, bool on)
    {
        if (OverlayLayer* l = find(id)) { l->enabled = on; if (l->apply) l->apply(*l); }
    }
    void setOpacity(const std::string& id, float v)
    {
        if (OverlayLayer* l = find(id)) { l->opacity = v; if (l->apply) l->apply(*l); }
    }
    OverlayLayer* find(const std::string& id)
    {
        for (size_t i = 0; i < _layers.size(); ++i)
            if (_layers[i].id == id) return &_layers[i];
        return nullptr;
    }
private:
    std::vector<OverlayLayer> _layers;
};

#endif
```

- [ ] **Step 3.2：构建确认头文件无语法错（被 Task 4 引入后再真正编译）**

LayerManager.h 暂未被任何 .cpp 包含，本步只做语法自检：
Run: `g++ -std=c++14 -fsyntax-only -x c++ /Users/franklee/osgverse/applications/earth_explorer/LayerManager.h`
Expected: 无输出（通过）。

- [ ] **Step 3.3：提交**
```bash
git add applications/earth_explorer/LayerManager.h
git commit -m "feat(earth): minimal LayerManager for overlay layer registry"
```

---

## Task 4：「图层」UI 折叠栏（方案 B 分类折叠）+ 标注开关

**Files:**
- Modify: `applications/earth_explorer/EarthControlUI.h`（新增成员、面板节）
- Modify: `applications/earth_explorer/earth_main.cpp`（构造 LayerManager、注册标注层、传入 UI）

- [ ] **Step 4.1：EarthControlUI 持有 LayerManager 指针**

在 `applications/earth_explorer/EarthControlUI.h` 顶部加 `#include "LayerManager.h"`。
在结构体成员区加：
```cpp
    LayerManager* _layers = nullptr;   // 由 main 注入
```

- [ ] **Step 4.2：新增「图层 Layers」折叠栏（在「渲染 Render」节之后插入）**

在 `runInternal` 里「渲染 Render」`CollapsingHeader` 块结束（`}`）之后、`// ---- 跳转 ----` 之前，插入：
```cpp
            // ---- 图层 ----
            if (_layers && ImGui::CollapsingHeader(u8"图层 Layers", ImGuiTreeNodeFlags_DefaultOpen))
            {
                std::string curGroup; bool groupOpen = false;
                std::vector<OverlayLayer>& ls = _layers->layers();
                for (size_t i = 0; i < ls.size(); ++i)
                {
                    OverlayLayer& l = ls[i];
                    if (l.group != curGroup)   // 新分类 → 开一个可折叠子树（方案 B）
                    {
                        if (groupOpen) ImGui::TreePop();
                        curGroup = l.group;
                        groupOpen = ImGui::TreeNodeEx(l.group.c_str(),
                            (l.group == u8"底图 / 标注") ? ImGuiTreeNodeFlags_DefaultOpen : 0);
                    }
                    if (!groupOpen) continue;
                    ImGui::PushID((int)i);
                    if (l.needsKey) ImGui::BeginDisabled();
                    bool en = l.enabled;
                    if (ImGui::Checkbox(l.displayName.c_str(), &en)) _layers->setEnabled(l.id, en);
                    if (l.needsKey) { ImGui::SameLine(); ImGui::TextDisabled(u8"🔑"); ImGui::EndDisabled(); }
                    if (l.hasOpacity && l.enabled)
                    {
                        float op = l.opacity;
                        if (ImGui::SliderFloat(u8"透明度", &op, 0.0f, 1.0f, "%.2f"))
                            _layers->setOpacity(l.id, op);
                    }
                    ImGui::PopID();
                }
                if (groupOpen) ImGui::TreePop();
            }
```
（依赖图层按 group 连续排列；Task 4.4 注册时保证。）

- [ ] **Step 4.3：main 构造 LayerManager 并注入 UI**

在 `applications/earth_explorer/earth_main.cpp` 创建 `EarthControlUI` 的地方（搜索 `new EarthControlUI` 或 `EarthControlUI` 实例化处），其前构造：
```cpp
    LayerManager layerMgr;
```
并在实例化 UI 后设置 `ui->_layers = &layerMgr;`（按现有 UI 创建写法适配；若是栈对象则 `ui._layers = &layerMgr;`）。
顶部加 `#include "LayerManager.h"`。

- [ ] **Step 4.4：注册「底图/标注」两层，标注层接 LabelOpacity**

紧接 Step 4.3 注册图层（`earthRenderingUtils` 即 `EarthAtmosphereOcean`，UI 里叫 `_earth`）：
```cpp
    {
        OverlayLayer base; base.id = "base"; base.displayName = u8"卫星影像";
        base.group = u8"底图 / 标注"; base.enabled = true; base.hasOpacity = false;
        layerMgr.add(base);   // 基础底图，常开、无开关动作

        OverlayLayer labels; labels.id = "labels"; labels.displayName = u8"路网·地名";
        labels.group = u8"底图 / 标注"; labels.enabled = true; labels.hasOpacity = true; labels.opacity = 1.0f;
        EarthAtmosphereOcean* eptr = &earthRenderingUtils;
        labels.apply = [eptr](const OverlayLayer& l) {
            float v = l.enabled ? l.opacity : 0.0f;
            if (eptr->commonUniforms.count("LabelOpacity"))
                eptr->commonUniforms["LabelOpacity"]->set(v);
        };
        layerMgr.add(labels);
    }
```
（`base` 无 apply，勾选无副作用；后续可禁用其 checkbox。）

- [ ] **Step 4.5：构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`
Expected: 构建成功。

- [ ] **Step 4.6：截图验证——贴地开/关标注**

先开（默认）：Run 截图模板 `--goto 39.9 116.4 0.5`（北京贴地 500m）。Read 图：应看到路网/地名压在卫星图上。
关标注需交互（无头跑默认开），所以改为**程序内默认关**临时验证：把 Step 4.4 的 `labels.enabled` 暂设 `false` 重新构建截图同视角；Read 图：应**只剩纯卫星、无标注遮挡**。确认后把 `labels.enabled` 改回 `true`、重建。
Expected: 开→有标注；关→纯净卫星图。两者底图一致（lyrs=s）。

- [ ] **Step 4.7：全球无回归截图**

Run 截图模板（无 --goto，全球）。Read 图。
Expected: 地球比例正常、大气正常、极点星状无变化（未碰极地代码）。

- [ ] **Step 4.8：提交**
```bash
git add applications/earth_explorer/EarthControlUI.h applications/earth_explorer/earth_main.cpp
git commit -m "feat(earth): Layers panel (nested-collapsible) with label toggle + opacity"
```

---

## Task 5：重打包 .app + 收尾验证

**Files:**（无源码改动）

- [ ] **Step 5.1：重打包 .app（含新 shader）**

Run: `bash /Users/franklee/osgverse/packaging/package_macos.sh`
Expected: `Built: .../dist/EarthExplorer.app`。确认 `assets/shaders/scattering_globe.frag.glsl` 的新版被复制进 `dist/EarthExplorer.app/Contents/shaders/`（diff 一下两文件）。

- [ ] **Step 5.2：跑 .app 二进制贴地截图，确认与 dev 构建一致**

用 .app 内二进制（`DYLD_LIBRARY_PATH=.../Contents/lib`）跑 `--goto 39.9 116.4 0.5`，Read 图：标注默认显示、底图纯卫星。
Expected: 与 Task 4 一致。

- [ ] **Step 5.3：更新文档**

在 `HANDOFF.md` 记一行：标注已拆为可开关 USER/lyrs=h 层（LabelOpacity uniform），底图改 lyrs=s；图层框架/UI 雏形落地（P0 完成）。

- [ ] **Step 5.4：提交**
```bash
git add HANDOFF.md
git commit -m "docs(earth): record P0 label-layer split + Layers panel"
```

---

## Self-Review 覆盖核对（写计划者自查）

- **底图换 lyrs=s**：Task 2.1 ✓
- **标注变可开关层**：USER/lyrs=h 通路 Task 2.1-2.5；开关/透明度 Task 4.2/4.4 ✓
- **LabelOpacity 门控 + 向后兼容**：Task 1（默认 1.0，core 注册）✓
- **LayerManager**：Task 3 ✓
- **「图层」UI 方案 B 分类折叠**：Task 4.2 ✓
- **每源策略字段（needsKey/opacity）**：OverlayLayer 已含，UI 已用 needsKey 灰显 ✓
- **验证含贴地开/关 + 低 zoom 中纬 + 全球无回归**：Task 2.7 / 4.6 / 4.7 ✓
- **.app 同步**：Task 5 ✓
- 类型一致性：`commonUniforms`、`OverlayLayer`、`LayerManager::setEnabled/setOpacity/add/layers`、`EarthAtmosphereOcean` 全程一致 ✓
- 待定/占位符扫描：仅 Step 0.1（运行时 shader 路径确认）和 Step 4.3（按现有 UI 创建写法适配）为「需现场对照代码」项，已显式标注、非占位敷衍。
