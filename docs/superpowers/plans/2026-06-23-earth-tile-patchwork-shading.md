# 消除补丁状瓦片——随高度退场的地表着色 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** 让地球在中低空显示清晰自然的影像(消除"补丁状瓦片"),高空保留现有太空/大气观感;不复现 4 类老回归。

**Architecture:** 改 `assets/shaders/scattering_globe.frag.glsl`:最终色 = `mix(spaceColor, clearColor, groundClarity)`——`spaceColor`=现有大气着色(高空保留)、`clearColor`=原始影像×平滑昼夜(低空清晰、无雾、无块)、`groundClarity`=由相机高度算的 smoothstep。同时用平滑球面法线 `normalize(P)`(去掉 z3 粗 mask 法线扰动)消除块状。clarity 高度 band 由 `render_effects.cpp` 加的两个 uniform(env 可调)控制——**只动应用层,不碰共享管线 UtilitiesEx.cpp**。

**Tech Stack:** GLSL(osgVerse VERSE 宏)、OSG uniform。

**铁律(4 类回归,每步必验):** 海洋不橙、晨昏线在且平滑、低空倾斜不穿模、不碰程序化海洋/OceanOpaque/太阳方向/几何。`spaceColor` 分支与现有 `mix(hdr(compositeColor), originalGroundColor, cTheta)` 等价(高空观感零变化)。

**构建/验证:**
```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```
shader 改的是 `assets/shaders/`,但运行时从 `build/sdk_core/shaders/` 加载——`--target install` 会拷贝。headless 截图模板见各步。

---

## File Structure
- **Modify** `applications/earth_explorer/render_effects.cpp` — `configureEarthRendering` 里给 globe(earthCamera)stateset 加 `ClarityAltLo`/`ClarityAltHi` uniform,从 env `EARTH_CLARITY_ALTLO`/`EARTH_CLARITY_ALTHI`(km)读、有默认。
- **Modify** `assets/shaders/scattering_globe.frag.glsl` — 声明两个 uniform;main() 末段改为统一混合 + 平滑法线。

---

## Task 1: 加 clarity band uniform(C++,无视觉变化)

**Files:** Modify `applications/earth_explorer/render_effects.cpp`

- [ ] **Step 1: 在 `configureEarthRendering` 里 `applyToGlobe(...)` 调用之后、`root->addChild(earthCamera)` 之前,加 uniform(env 可调,单位 km→m)**

定位 `earthRenderingUtils.applyToGlobe(earthCamera->getOrCreateStateSet(), ...)`(约 94-96 行),其后紧接加:
```cpp
    // 地表清晰度高度 band(消"补丁"):相机高度 < AltLo → 完全清晰影像;> AltHi → 完全太空大气观感。
    // env EARTH_CLARITY_ALTLO / EARTH_CLARITY_ALTHI(km)可调,默认 2000 / 8000 km。
    {
        const char* loEnv = getenv("EARTH_CLARITY_ALTLO");
        const char* hiEnv = getenv("EARTH_CLARITY_ALTHI");
        float altLo = (loEnv && *loEnv) ? (float)atof(loEnv) * 1000.0f : 2000000.0f;
        float altHi = (hiEnv && *hiEnv) ? (float)atof(hiEnv) * 1000.0f : 8000000.0f;
        osg::StateSet* gss = earthCamera->getOrCreateStateSet();
        gss->getOrCreateUniform("ClarityAltLo", osg::Uniform::FLOAT)->set(altLo);
        gss->getOrCreateUniform("ClarityAltHi", osg::Uniform::FLOAT)->set(altHi);
    }
```
(`<cstdlib>` for getenv/atof — render_effects.cpp 已 include `<cstdlib>` 间接或加之;若编译报未声明,在顶部加 `#include <cstdlib>`。)

- [ ] **Step 2: 构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -4`
Expected: 成功。

- [ ] **Step 3: headless 验证无视觉变化(shader 还没用这俩 uniform)**

```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=400 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 35 105 2000 > /tmp/run.log 2>&1
```
Read `/tmp/earth_capture_0.png`。Expected: 与当前一致(仍是带补丁的现状——本步只加了未使用的 uniform),无崩溃。

- [ ] **Step 4: 提交**
```bash
cd /Users/franklee/osgverse
git add applications/earth_explorer/render_effects.cpp
git commit -m "feat(earth): add ClarityAltLo/Hi uniforms (env-tunable) for altitude-faded shading

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 2: 统一混合着色(shader,视觉修复 + 重度验证)

**Files:** Modify `assets/shaders/scattering_globe.frag.glsl`

- [ ] **Step 1: 声明两个 uniform**

第 8 行 `uniform float HdrExposure, GlobalOpaque, LabelOpacity, Overlay2Opacity;` 之后加:
```glsl
uniform float ClarityAltLo, ClarityAltHi;
```

- [ ] **Step 2: 重写 main() 的着色末段(从 `vec3 WSD = ...` 到 `finalColor` 定义)**

把现有这段(约 75-99 行,从 `vec3 WSD = WorldSunDir` 到 `vec4 finalColor = vec4(mix(...), groundColor.a);`)整体替换为:
```glsl
    vec3 WSD = WorldSunDir, WCP = WorldCameraPos;
    vec3 P = vertexInWorld, N = normalize(P);
    P = N * (length(P) * 0.99);  // FIXME

    // 平滑球面法线昼夜(不再用 z3 粗 mask 法线扰动 → 无逐瓦片块状跳变;mask 仅用于上面的 maskValue 输出)。
    vec3 originalGroundColor = groundColor.rgb;
    float cTheta = max(dot(N, WSD), 0.0); vec3 sunL, skyE;
    sunRadianceAndSkyIrradiance(P, N, WSD, sunL, skyE);
    groundColor.rgb *= max((sunL * cTheta + skyE) / 3.14159265, vec3(0.1));
    groundColor.a *= clamp(GlobalOpaque, 0.0, 1.0);

    vec3 extinction = vec3(1.0);
    // 晨昏线掠射太阳 inscatter 是落日橙;调淡只柔化那条带、不影响白天侧。
    vec3 inscatter = inScattering(WCP, P, WSD, extinction, 0.0) * 0.5;
    vec3 compositeColor = groundColor.rgb * extinction + inscatter;

    // 高空/太空:现有观感(与原 finalColor 等价,仅法线改平滑 → 高空逐瓦片差异本就极小)。
    vec3 spaceColor = mix(hdr(compositeColor), originalGroundColor, cTheta);

    // 低空:清晰原始影像 × 平滑昼夜(白天≈1、夜≈0.06、晨昏平滑),无雾、无 HDR 重调色 → 消补丁。
    float dayNight = mix(0.06, 1.0, smoothstep(-0.05, 0.25, dot(N, WSD)));
    vec3 clearColor = originalGroundColor * dayNight;

    // 按相机高度混合:alt<AltLo→clearColor(清晰),alt>AltHi→spaceColor(太空)。
    float camAlt = length(WCP) - PLANET_RADIUS;
    float groundClarity = smoothstep(ClarityAltHi, ClarityAltLo, camAlt);
    vec4 finalColor = vec4(mix(spaceColor, clearColor, groundClarity), groundColor.a);
```
**保持不变**:上方 groundColor/layer/overlay 合成(54-58)、skirt discard(59)、maskColor 采样与 9-tap(61-73)、底部 `fragColor/fragOrigin` 输出(101-107)。**删除**原 79-85 的 `aspect/slope/localN/east/north/terrainDetails` 块(粗法线扰动,块状根源)。

- [ ] **Step 3: 构建**

Run: `cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release 2>&1 | tail -4`
Expected: 成功(确认 shader 拷到 `build/sdk_core/shaders/`)。

- [ ] **Step 4: 验证补丁消失(原补丁处,低空)**

```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=700 EARTH_FRAME_SLEEP_MS=45 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 35 105 2000 > /tmp/run.log 2>&1
```
Read PNG。Expected: 影像清晰自然、**无块状亮暗补丁**(接近诊断时的 rawonly 干净效果),仍有合理昼夜。

- [ ] **Step 5: 验证高空太空观感不变 + 中空过渡自然**

```bash
# 高空全球(默认 home ~38000km)
cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_AUTOCAP=400 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
# 看 PNG:与改动前高空一致(大气/太空观感保留)
```
Read PNG。Expected: 高空与现状一致(spaceColor 主导)。若高空被 clearColor 影响(太"平"无大气),调高 `ClarityAltHi`(env)复验。

- [ ] **Step 6: 验证 4 类回归不复现**

```bash
# 晨昏线 + 海洋不橙:默认太阳,全球 + 低空晨昏侧
cd /Users/franklee/osgverse/build/sdk_core/bin
for args in "" "--goto 20 20 1500"; do
  rm -f /tmp/earth_capture_0.png
  DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
  EARTH_AUTOCAP=600 EARTH_FRAME_SLEEP_MS=40 \
  ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 $args > /tmp/run.log 2>&1
  cp /tmp/earth_capture_0.png /tmp/regr_$(echo $args | tr ' /' '__').png
done
# 倾斜不穿模:低空 + EARTH_TILT
rm -f /tmp/earth_capture_0.png
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_TILT=1 EARTH_AUTOCAP=600 EARTH_FRAME_SLEEP_MS=40 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 25 102 3 > /tmp/run.log 2>&1
cp /tmp/earth_capture_0.png /tmp/regr_tilt.png
```
Read 三张 PNG。Expected:① 晨昏线存在且**平滑**(不再块状)② 海洋蓝、**无橙红**、无刺眼橙带 ③ 倾斜低空不穿模看穿地球。任一不满足 → 不提交,回 Phase 1 查。

- [ ] **Step 7: 微调 band(若需要)**

若 Step 4 低空仍偏雾/不够清晰,或 Step 5 高空被洗平:用 env 调 `EARTH_CLARITY_ALTLO`/`EARTH_CLARITY_ALTHI`(km)复跑 Step 4/5 找到好值,再把**默认值**(render_effects.cpp Task1 的 2000/8000)改成该值并重建复验。记录最终值。

- [ ] **Step 8: 提交**
```bash
cd /Users/franklee/osgverse
git add assets/shaders/scattering_globe.frag.glsl applications/earth_explorer/render_effects.cpp
git commit -m "fix(earth): altitude-faded surface shading kills tile patchwork (smooth normal + clear low-alt imagery)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## Task 3: 全量回归批次 + .app 重打包 + 文档/记忆

**Files:** Modify `HANDOFF.md`, `docs/EarthExplorer.md`

- [ ] **Step 1: 真机交互验证(用户)**

裸二进制带界面,从全球缩放到街道级、各高度拖动,确认:全程无补丁、过渡自然、晨昏线平滑、海洋不橙、倾斜不穿模、降水/云图叠加层仍正常。
```bash
cd /Users/franklee/osgverse/build/sdk_core/bin && \
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib ./osgVerse_EarthExplorer --resolution 1280 800
```

- [ ] **Step 2: headless 多高度批次**(低/中/高 + 晨昏)复跑 Task2 Step4/5/6,贴图存档比对。

- [ ] **Step 3: 重打包 `.app`**
```bash
cd /Users/franklee/osgverse && bash packaging/package_macos.sh 2>&1 | tail -1
```
打包二进制 headless 跑一次确认不崩、补丁消失。

- [ ] **Step 4: 更新文档**
- `HANDOFF.md` 顶部加 2026-06-23 条目:补丁修复(随高度退场着色,平滑法线,`EARTH_CLARITY_ALTLO/HI` 钩子,根因=渲染非源影像)。
- `docs/EarthExplorer.md`:加 `EARTH_CLARITY_ALTLO/ALTHI` env 说明(若文档有 env 表)。

- [ ] **Step 5: 提交 + push**
```bash
cd /Users/franklee/osgverse
git add HANDOFF.md docs/EarthExplorer.md
git commit -m "docs(earth): HANDOFF + doc — tile patchwork fix (altitude-faded shading)

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
git push origin master
```
(push 前需用户真机确认观感 OK,同既有惯例。本次连同之前未推的降水+性能+toggle 修复一起 push。)

- [ ] **Step 6: 写记忆**
新建 `earth-tile-patchwork-fix.md`(type: project):根因=渲染着色(非源影像,诊断法:把 globe frag 改纯 originalGroundColor 对比)、修法=随高度 mix(spaceColor, clearColor=raw×dayNight, groundClarity)+ 平滑法线、`EARTH_CLARITY_*` 钩子、4 类回归守住。更新 MEMORY.md 索引;关联 [[earth-orange-ocean-rootcause]]、[[earth-low-altitude-dark-atmosphere]]。

---

## Self-Review

**Spec 覆盖**:方向①(随高度变清晰=groundClarity)、②(消块状=平滑法线+删粗法线块)、③(调淡雾/HDR=clearColor 无雾)全覆盖(Task 2 Step 2)。验证含 4 类回归(Task 2 Step 6)。env 可调(Task 1)。
**类型一致**:`ClarityAltLo/Hi`(float, 米)在 render_effects.cpp 设、shader 声明用,名字一致;`WorldCameraPos`/`PLANET_RADIUS` 沿用现有。
**占位符**:band 默认 2000/8000km 是起点,Task2 Step7 明确微调并回填默认值——非占位,是有意的截图调参步骤。
**回归边界**:spaceColor 与原 finalColor 等价(高空零变化);只在低空叠加 clearColor;不碰 OceanOpaque/太阳/几何/海洋相机。
