# P1：栅格 overlay 框架 + GIBS 近实时影像层 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 P0 的图层框架上加第二个 GPU 合成栅格叠加槽，接入 NASA GIBS 近实时真彩影像（含当日云图），可在「图层」面板开关 + 调透明度。

**Architecture:** 复用 P0 的 GPU 合成方式。地面着色器 `scattering_globe.frag.glsl` 当前采样 Scene(0)/Mask(1)/ExtraLayer=标注(2)，大气采样器从 unit 3 起。新增 `Overlay2Sampler`(unit 3) + `UvOffset4` + `Overlay2Opacity`，大气起始单元从 3 挪到 4（移到 4–7）。新增 `TileCallback::OVERLAY=4` 层（texUnit 3 / UvOffset4），经 TMS 插件加载 GIBS 瓦片。`Overlay2Opacity` 默认 0 → 其它 app（earth_test/wasm）无回归。

**Tech Stack:** C++ / OpenSceneGraph (CORE/GL4.1) / osgVerse / GLSL / NASA GIBS WMTS。

**GIBS 源（已实测 200/jpeg）：**
```
https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/MODIS_Terra_CorrectedReflectance_TrueColor/default/default/GoogleMapsCompatible_Level9/{z}/{y}/{x}.jpg
```
- EPSG:3857（与现有 Google 瓦片同方案，用 yXYZ 翻转行）；`time=default` 自动取最新（无需算日期）；jpg；**最大 z9**（>z9 无数据，createCustomPath 返回 "" 截断）。免 key、可缓存。MODIS Terra 可一行换成 `VIIRS_SNPP_CorrectedReflectance_TrueColor` 等。

**验证方式：** 无单元测试，用无头截图。构建：`cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`。截图模板见下，`EARTH_AUTOCAP` ≤ ~650。

---

## Task 1：着色器 + applyToGlobe 增第二个栅格合成槽（Overlay2）

**Files:**
- Modify: `assets/shaders/scattering_globe.frag.glsl`
- Modify: `pipeline/UtilitiesEx.cpp`（`applyToGlobe` ~266-290）

- [ ] **Step 1.1：着色器声明新采样器/uniform**

`assets/shaders/scattering_globe.frag.glsl` 顶部：
- `uniform sampler2D SceneSampler, MaskSampler, ExtraLayerSampler;` 改为追加 `Overlay2Sampler`：
  `uniform sampler2D SceneSampler, MaskSampler, ExtraLayerSampler, Overlay2Sampler;`
- `uniform vec4 UvOffset1, UvOffset2, UvOffset3;` 改为：
  `uniform vec4 UvOffset1, UvOffset2, UvOffset3, UvOffset4;`
- `uniform float HdrExposure, GlobalOpaque, LabelOpacity;` 改为追加 `Overlay2Opacity`：
  `uniform float HdrExposure, GlobalOpaque, LabelOpacity, Overlay2Opacity;`

- [ ] **Step 1.2：着色器在标注合成之后叠加 Overlay2**

紧跟现有这行（标注合成）：
```glsl
    groundColor.rgb = mix(groundColor.rgb, layerColor.rgb, layerColor.a * clamp(LabelOpacity, 0.0, 1.0));
```
之后插入：
```glsl
    vec4 overlay2Color = VERSE_TEX2D(Overlay2Sampler, texCoord.st * UvOffset4.zw + UvOffset4.xy);
    groundColor.rgb = mix(groundColor.rgb, overlay2Color.rgb, overlay2Color.a * clamp(Overlay2Opacity, 0.0, 1.0));
```
（GIBS jpg 无 alpha → `overlay2Color.a==1`，故混合权重 = `Overlay2Opacity`，即 GIBS 与底图按透明度混合。）

- [ ] **Step 1.3：applyToGlobe 绑 Overlay2Sampler(unit3) + 大气挪到 startU=4**

`pipeline/UtilitiesEx.cpp` 的 `applyToGlobe`（~278-289）。当前：
```cpp
    ss->setTextureAttributeAndModes(0, baseTex);
    ss->setTextureAttributeAndModes(1, maskTex);
    ss->setTextureAttributeAndModes(2, extraTex);
    ss->getOrCreateUniform("SceneSampler", osg::Uniform::INT)->set((int)0);
    ss->getOrCreateUniform("MaskSampler", osg::Uniform::INT)->set((int)1);
    ss->getOrCreateUniform("ExtraLayerSampler", osg::Uniform::INT)->set((int)2);
    ss->getOrCreateUniform("UvOffset1", osg::Uniform::FLOAT_VEC4)->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
    ss->getOrCreateUniform("UvOffset2", osg::Uniform::FLOAT_VEC4)->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
    ss->getOrCreateUniform("UvOffset3", osg::Uniform::FLOAT_VEC4)->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));

    osg::Program* program = apply(ss, vs, fs, 3, ref);
```
改为（绑 unit3 透明默认纹理 + UvOffset4，大气起始单元改 4）：
```cpp
    ss->setTextureAttributeAndModes(0, baseTex);
    ss->setTextureAttributeAndModes(1, maskTex);
    ss->setTextureAttributeAndModes(2, extraTex);
    osg::Texture* overlay2Def = createDefaultTexture(osg::Vec4(0.0f, 0.0f, 0.0f, 0.0f));  // 透明默认
    ss->setTextureAttributeAndModes(3, overlay2Def);
    ss->getOrCreateUniform("SceneSampler", osg::Uniform::INT)->set((int)0);
    ss->getOrCreateUniform("MaskSampler", osg::Uniform::INT)->set((int)1);
    ss->getOrCreateUniform("ExtraLayerSampler", osg::Uniform::INT)->set((int)2);
    ss->getOrCreateUniform("Overlay2Sampler", osg::Uniform::INT)->set((int)3);
    ss->getOrCreateUniform("UvOffset1", osg::Uniform::FLOAT_VEC4)->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
    ss->getOrCreateUniform("UvOffset2", osg::Uniform::FLOAT_VEC4)->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
    ss->getOrCreateUniform("UvOffset3", osg::Uniform::FLOAT_VEC4)->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
    ss->getOrCreateUniform("UvOffset4", osg::Uniform::FLOAT_VEC4)->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));

    osg::Program* program = apply(ss, vs, fs, 4, ref);  // 大气采样器移到 unit 4-7，让出 unit3 给 Overlay2
```
确认 `createDefaultTexture` 在本文件可见（render_effects.cpp 用的是 `osgVerse::createDefaultTexture`；本文件是 osgVerse 命名空间内，直接调 `createDefaultTexture` 即可；若编译报未声明，加 `osgVerse::` 或包含其声明头）。

- [ ] **Step 1.4：commonUniforms 注册 Overlay2Opacity（默认 0.0 = 关）**

`pipeline/UtilitiesEx.cpp` 的 commonUniforms 初始化块（`commonUniforms["LabelOpacity"] = ...` 那行附近）追加：
```cpp
    commonUniforms["Overlay2Opacity"] = new osg::Uniform("Overlay2Opacity", 0.0f);
```
默认 0 → 未接 overlay 的 app 无视觉变化（earth_test/wasm 在 unit3 是透明默认 + opacity 0）。

- [ ] **Step 1.5：构建 + 全球无回归截图**

Run: 构建命令。再截图（全球，无 --goto）：
```
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=550 EARTH_SUN_TO_CAMERA=1 EARTH_FRAME_SLEEP_MS=30 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
```
Read `/tmp/earth_capture_0.png`。Expected: 地球 + 大气 + 标注完全正常（大气挪了纹理单元但靠 uniform 绑定、外观应一致）。**若大气消失/发黑 → 大气采样器单元没对上，回查 Step 1.3。** 这是本任务关键回归点。

- [ ] **Step 1.6：提交**
```bash
git add assets/shaders/scattering_globe.frag.glsl pipeline/UtilitiesEx.cpp
git commit -m "feat(earth): add second GPU overlay slot (Overlay2, unit3) for raster overlays"
```

---

## Task 2：TileCallback 增 OVERLAY 层（texUnit 3 / UvOffset4）

**Files:**
- Modify: `readerwriter/TileCallback.h`（LayerType 枚举）
- Modify: `readerwriter/TileCallback.cpp`（`findAndUseParentData` switch ~528-532；`updateLayerData` switch ~549-560）

- [ ] **Step 2.1：枚举加 OVERLAY=4**

`readerwriter/TileCallback.h` 的 `enum LayerType { ELEVATION = 0, ORTHOPHOTO, OCEAN_MASK, USER };` 改为：
`enum LayerType { ELEVATION = 0, ORTHOPHOTO, OCEAN_MASK, USER, OVERLAY };`
（OVERLAY=4 → UvOffset 名自动为 "UvOffset4"，与着色器一致。）

- [ ] **Step 2.2：findAndUseParentData 的 texUnit switch 加 OVERLAY→3**

`readerwriter/TileCallback.cpp` ~528：
```cpp
            switch (id)
            {
            case ORTHOPHOTO: texUnit = 0; break;
            case OCEAN_MASK: texUnit = 1; break;
            default: texUnit = 2; break;
            }
```
改为：
```cpp
            switch (id)
            {
            case ORTHOPHOTO: texUnit = 0; break;
            case OCEAN_MASK: texUnit = 1; break;
            case OVERLAY: texUnit = 3; break;
            default: texUnit = 2; break;  // USER
            }
```

- [ ] **Step 2.3：updateLayerData 的 switch 加 OVERLAY→texUnit 3**

`readerwriter/TileCallback.cpp` ~549：
```cpp
    case OCEAN_MASK:
        tex = createLayerImage(id, emptyPath, opt); texUnit = 1; break;
    default:  // USER
```
在 `case OCEAN_MASK:` 块之后、`default:` 之前插入：
```cpp
    case OVERLAY:
        tex = createLayerImage(id, emptyPath, opt); texUnit = 3; break;
```

- [ ] **Step 2.4：构建**

Run 构建命令。Expected: 成功（纯增量逻辑，OVERLAY 暂未被任何路径设置，行为不变）。

- [ ] **Step 2.5：提交**
```bash
git add readerwriter/TileCallback.h readerwriter/TileCallback.cpp
git commit -m "feat(earth): add OVERLAY tile layer (texUnit 3 / UvOffset4)"
```

---

## Task 3：TMS 插件接通 OVERLAY 层

**Files:**
- Modify: `plugins/osgdb_tms/ReaderWriterTMS.cpp`

仿 P0 的 USER 层接法（spec/历史可参考），加 `Overlay` 选项：

- [ ] **Step 3.1：声明 + 解析 Overlay 选项**

构造函数 `supportsOption("User", ...);` 之后加：
```cpp
        supportsOption("Overlay", "TMS/WMTS server URL with wildcards, applied as second overlay layer (e.g. GIBS)");
```
`readNode` 里 `userAddr` 解析之后加：
```cpp
            std::string overlayAddr = osgVerse::WebAuxiliary::urlDecode(options->getPluginStringData("Overlay"));
```
`createTile(...)` 调用加 `overlayAddr`（放 `userAddr` 之后）：
```cpp
                    osg::ref_ptr<osg::Node> node = createTile(
                        elevAddr, orthoAddr, maskAddr, userAddr, overlayAddr, x + xx, y + yy, z,
                        extentMin, extentMax, options, useWM, flatten);
```

- [ ] **Step 3.2：createTile 签名加 overlayPath + 设层路径**

签名加 `const std::string& overlayPath`（放 `userPath` 之后）：
```cpp
    osg::Node* createTile(const std::string& elevPath, const std::string& orthPath,
                          const std::string& maskPath, const std::string& userPath,
                          const std::string& overlayPath, int x, int y, int z,
                          const osg::Vec3d& extentMin, const osg::Vec3d& extentMax,
                          const Options* opt, bool useWM, bool flatten) const
```
`tileCB->setLayerPath(... USER, userPath);` 之后加：
```cpp
        tileCB->setLayerPath(osgVerse::TileCallback::OVERLAY, overlayPath);
```

- [ ] **Step 3.3：预载 OVERLAY（仿 USER 块：load 在 setLayersDone 前 + 失败跟踪；bind 在下）**

在 USER 的 load+failtrack 处（`setLayersDone` 之前）旁加：
```cpp
        bool emptyPathO = false;
        osg::ref_ptr<osg::Texture> overlayImage =
            tileCB->createLayerImage(osgVerse::TileCallback::OVERLAY, emptyPathO, opt);
        if (!overlayImage && !emptyPathO)
            { tileCB->setLayerPathState(osgVerse::TileCallback::OVERLAY, failState); allLayersDone = false; }
```
在 USER 的纹理绑定块之后加（绑 texUnit 3 + UvOffset4）：
```cpp
        if (overlayImage.valid())
        {
            overlayImage->setClientStorageHint(false);
            overlayImage->setUnRefImageDataAfterApply(true);
#if defined(OSG_GLES2_AVAILABLE) || defined(OSG_GLES3_AVAILABLE) || defined(OSG_GL3_AVAILABLE)
            geom->getOrCreateStateSet()->setTextureAttribute(3, overlayImage.get());
#else
            geom->getOrCreateStateSet()->setTextureAttributeAndModes(3, overlayImage.get());
#endif
            geom->getOrCreateStateSet()->getOrCreateUniform("UvOffset4", osg::Uniform::FLOAT_VEC4)
                                       ->set(osg::Vec4(0.0f, 0.0f, 1.0f, 1.0f));
        }
```

- [ ] **Step 3.4：构建**

Run 构建命令。Expected: 成功（OVERLAY 暂无路径设置 → 不加载，行为不变）。

- [ ] **Step 3.5：提交**
```bash
git add plugins/osgdb_tms/ReaderWriterTMS.cpp
git commit -m "feat(earth): plumb OVERLAY layer through TMS plugin (texUnit 3)"
```

---

## Task 4：接 GIBS + 「图层」面板加云图层（端到端，关键视觉验证）

**Files:**
- Modify: `applications/earth_explorer/earth_main.cpp`（`createCustomPath`；`earthURLs`；LayerManager 注册）

- [ ] **Step 4.1：createCustomPath 处理 OVERLAY → GIBS（z>9 截断）**

`createCustomPath` 里 USER 分支之后加：
```cpp
    else if (type == osgVerse::TileCallback::OVERLAY)
    {
        if (z > 9) return "";  // GIBS GoogleMapsCompatible_Level9 最大 z9，超出无数据
        static const std::string gibs =
            "https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/"
            "MODIS_Terra_CorrectedReflectance_TrueColor/default/default/"
            "GoogleMapsCompatible_Level9/{z}/{y}/{x}.jpg";
        return osgVerse::TileCallback::createPath(gibs, x, yXYZ, z);
    }
```
（用 `yXYZ` 翻转行，与 ORTHOPHOTO/USER 一致。）

- [ ] **Step 4.2：earthURLs 启用 OVERLAY 层占位符**

`earthURLs` 里 `" User=googleLabels"` 之后加：
```cpp
        " Overlay=gibs"
```

- [ ] **Step 4.3：LayerManager 注册「影像 / 天气」组 + 云图层（默认关，透明度→Overlay2Opacity）**

`earth_main.cpp` 注册 base/labels 的块里，labels 之后加：
```cpp
        OverlayLayer clouds; clouds.id = "clouds"; clouds.displayName = u8"GIBS 影像/云图";
        clouds.group = u8"影像 / 天气"; clouds.enabled = false; clouds.hasOpacity = true; clouds.opacity = 0.7f;
        clouds.apply = [eptr](const OverlayLayer& l) {
            float v = l.enabled ? l.opacity : 0.0f;
            if (eptr->commonUniforms.count("Overlay2Opacity"))
                eptr->commonUniforms["Overlay2Opacity"]->set(v);
        };
        layerMgr.add(clouds);
```
并在初始 sync 处（`setEnabled("labels", ...)` 旁）加：
```cpp
    if (OverlayLayer* cl = layerMgr.find("clouds"))
        layerMgr.setEnabled("clouds", cl->enabled);  // 推初值（默认关 → Overlay2Opacity=0）
```
（`eptr` 即已在该块定义的 `&earthRenderingUtils`。UI 面板已按 group 自动分组，会自动多出「影像 / 天气」可折叠子树，无需改 UI 代码。）

- [ ] **Step 4.4：构建**

Run 构建命令。

- [ ] **Step 4.5：端到端验证——云图 关（默认）vs 开**

1. 默认（关）：全球截图（无 --goto，AUTOCAP=550 SLEEP=30）。Read。Expected: 与 P0 一致（GIBS 不显示，因为 Overlay2Opacity=0），「图层」面板多出「影像 / 天气」组、「GIBS 影像/云图」未勾选。
2. 开（临时验证）：把 Step 4.3 的 `clouds.enabled = false` 暂改 `true`，重建，全球 + `--goto 30 100 4000`（亚洲大区低 zoom）截图。Read。Expected: 看到 GIBS 当日真彩/云图按 0.7 透明度叠在底图上（云带可见）。确认后改回 `false` 重建。
3. 注意 GIBS 仅 z≤9：贴地（如 `--goto 39.9 116.4 2`）时云图层应消失（z>9 截断）、只剩底图 + 标注——属预期。

- [ ] **Step 4.6：提交**（改回 enabled=false 后）
```bash
git add applications/earth_explorer/earth_main.cpp
git commit -m "feat(earth): GIBS near-real-time imagery overlay layer with toggle + opacity"
```

---

## Task 5：重打包 .app + 收尾

- [ ] **Step 5.1：重打包并确认新 shader 进 bundle**

Run: `bash /Users/franklee/osgverse/packaging/package_macos.sh`
确认 `grep -c Overlay2 dist/EarthExplorer.app/Contents/shaders/scattering_globe.frag.glsl` ≥ 1。

- [ ] **Step 5.2：.app 端到端抽查**

跑 .app 二进制（`DYLD_LIBRARY_PATH=.../Contents/lib`）全球截图，Read 确认面板有「影像/天气」组、默认无云图、无回归。

- [ ] **Step 5.3：更新 HANDOFF.md**：记 P1 完成（GIBS 影像层、第二 overlay 槽 unit3/UvOffset4/Overlay2Opacity、大气挪到 startU=4）。

- [ ] **Step 5.4：提交**
```bash
git add HANDOFF.md
git commit -m "docs(earth): record P1 GIBS imagery overlay layer"
```

---

## Self-Review 覆盖核对

- **第二 GPU 合成栅格槽**：Task 1（shader Overlay2 + applyToGlobe unit3 + 大气 startU=4 + Overlay2Opacity 默认0）✓
- **新瓦片层 OVERLAY**：Task 2（枚举 + 两个 switch，texUnit3/UvOffset4）✓
- **TMS 接通**：Task 3（Overlay 选项、load+failtrack、bind unit3/UvOffset4，仿 USER）✓
- **GIBS 接入 + 开关/透明度**：Task 4（createCustomPath z>9 截断、earthURLs、LayerManager 影像/天气组）✓
- **向后兼容**：Overlay2Opacity 默认 0 + unit3 透明默认纹理 + 大气靠 uniform 绑定 → earth_test/wasm 无回归（Task1.5 验证 EarthExplorer 大气无回归）✓
- **验证含 云图开/关 + 低 zoom + 贴地 z>9 截断 + 全球无回归 + .app**：Task 4.5 / 5 ✓
- 类型一致：`OVERLAY`、`Overlay2Sampler`/`UvOffset4`/`Overlay2Opacity`、`createTile` 新签名、`LayerManager` 全程一致 ✓
- 占位符扫描：仅 Step 1.3 `createDefaultTexture` 命名空间「需现场确认」一处，已显式标注。
```
