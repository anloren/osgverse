# 香港官方 3D Tiles 最小验证 spike — 实现计划

一行:用 env 钩子把一个公开 WGS84 Cesium 3D Tiles(大雁塔)流式加载、挂 `sceneCamera`,headless 实测落位/贴地/流式,扫清接香港 building API 前的坐标对齐风险。

## Context

- Spec:`docs/superpowers/specs/2026-06-24-earth-hk-3dtiles-design.md`
- 关键约束:
  - 不碰 `scattering_globe.frag.glsl` / 太阳 / `OceanOpaque` / 几何 / 相机地形地板 → **4 类历史回归之外**。
  - 集成只走 env 钩子 `EARTH_3DTILES=<url>`,**不设钩子时零影响**。
  - 数据流式(HTTP),不本地全存。
- 现有可复用:
  - 加载:`plugins/osgdb_3dtiles`(tileset.json + root `transform` + bounding + HTTP + PagedLOD)、`plugins/osgdb_gltf`(b3dm/glb)。
  - 坐标:`modeling/Math.h` `Coordinate::convertLLAtoECEF`(Z-up ECEF)。
  - 挂载点:`applications/earth_explorer/earth_main.cpp:367`(地震 `sceneCamera->addChild(...)`)。
  - 模块模板:`applications/earth_explorer/quake_data.{h,cpp}`、接口风格见 `quake_data.h`。
  - 样本:`https://earthsdk.com/v/last/Apps/assets/dayanta/tileset.json`(插件注释 `ReaderWriter3dTiles.cpp:49`)。
- 构建/运行:沿用项目既有 CMake 构建 + headless 截图验证流程(同前三个图层会话)。

## Phase 1：模块骨架 + 钩子接线(不加载任何数据)

### Task 1.1：新建 `tiles3d_data.{h,cpp}`
**Files:** `applications/earth_explorer/tiles3d_data.h`、`applications/earth_explorer/tiles3d_data.cpp`
- 声明 `osg::Node* configure3DTilesLayer(osgViewer::View& viewer, osg::Node* earthRoot, const std::string& mainFolder);`(签名对齐 `configureQuakeData` 风格)。
- 实现:读 `getenv("EARTH_3DTILES")`;**为空 → 返回空 `osg::Group`(名 `"Tiles3DLayer(off)"`)**,不做任何加载。
- 暂不加载数据(Phase 2 再加),先确保"挂上去什么都不发生"。

**Verification:** 编译通过;代码审查确认未 include / 未触碰 globe 着色器、太阳、海洋相关文件。

### Task 1.2：在 earth_main.cpp 接线
**Files:** `applications/earth_explorer/earth_main.cpp`(挂载段 ~363–372)、构建脚本(`CMakeLists.txt` 加新源文件)
- 仿 `:367` 加一行:`sceneCamera->addChild(configure3DTilesLayer(viewer, earthRoot.get(), mainFolder));`
- `#include "tiles3d_data.h"`。
- CMake 把 `tiles3d_data.cpp` 加入 earth_explorer target。

**Verification:** 不设 `EARTH_3DTILES`,headless 启动跑足帧 → 与改动前行为一致、无新报错、地球/地震/航班正常。

## Phase 2：加载样本 tileset(落位先不矫正)

### Task 2.1：configure3DTilesLayer 加载 tileset
**Files:** `applications/earth_explorer/tiles3d_data.cpp`
- env 非空 → `osg::ref_ptr<osg::Node> tiles = osgDB::readNodeFile(url + ".verse_tiles");`
- 加到返回 Group;加载失败打 `OSG_WARN` 并返回空 Group(不崩)。
- Group 命名含 url 末段,便于场景图调试。

**Verification:** `EARTH_3DTILES=<大雁塔 url>` headless 启动 → 日志见 `[ReaderWriter3dtiles]` 加载 tileset / 子瓦片、无 `ERROR_IN_READING_FILE`;场景图 dump 见 `TileLod:` / `TileTransform` 节点。

### Task 2.2：headless 相机定位到样本
**Files:** `applications/earth_explorer/earth_main.cpp`(若无现成手段)
- 确认 headless 下如何把相机对准任意经纬:优先复用现有 Go To / `earthManipulator->moveTo`(参考 `:386-388` 注释);若 headless 无触发途径,加临时钩子 `EARTH_GOTO=lat,lon,altM` 在启动后调 `moveTo(convertLLAtoECEF(...))`。
- 目标:大雁塔 ≈ `34.218, 108.964`,低空(如 2–5 km)。

**Verification:** 设 `EARTH_GOTO` 后截图,视野对准西安大雁塔区域(地球影像可辨认)。

## Phase 3：坐标对齐实测 + 按需矫正

### Task 3.1：落位/贴地实测
**Files:**（仅观测,先不改码）
- `EARTH_3DTILES=<大雁塔> EARTH_GOTO=34.218,108.964,3000` headless 截图。
- 判定:模型是否出现在视野中央(大雁塔位置)、基座是否大致贴地(不悬空/不深陷)、有无明显平移或旋转偏差。

**Verification:** 截图 + 文字记录实际现象(落对/偏移量级/贴地情况)。

### Task 3.2：如有偏差,包矫正变换
**Files:** `applications/earth_explorer/tiles3d_data.cpp`
- 仅当 3.1 见明显几何偏移或 LOD/裁剪异常时:在 Group 与 tiles 间插一层 `osg::MatrixTransform` 做矫正(平移/轴向),**不改插件**。
- 记录矫正量与推断原因(如 Y-up/Z-up、高度基准),写入实现笔记供香港数据复用。

**Verification:** 矫正后截图落位/贴地正确;缩放/平移见 PagedLOD 流式细化、不崩、不冻结地球。

## Phase 4：回归 + 共存

### Task 4.1：4 类回归 + 叠加共存
**Files:**（仅运行验证)
- headless 跑两种态:不设钩子 / 设钩子;各做 4 类回归批次(海洋不橙、晨昏线在、倾斜不穿模、无程序化海洋)。
- 同时开地震/航班钩子,确认共存、无相互干扰。

**Verification:** 截图比对,4 类回归全过;关钩子零残留。

## Final Verification(对齐 spec 成功标准)

- [ ] 构建通过;不设 `EARTH_3DTILES` 时行为与改动前一致。
- [ ] 样本 tileset 渲染出来(标准 1)。
- [ ] 落在大雁塔正确经纬(标准 2)。
- [ ] 大致贴地(标准 3)。
- [ ] 缩放/平移流式 LOD 正常、不崩、不冻结(标准 4)。
- [ ] 不破坏 globe/大气/海洋/地震/航班;关钩子零影响(标准 5)。
- [ ] 真机交付用户肉眼确认(落位/贴地/流式/共存)——遵循"务必肉眼验证"铁律。

## 交接给后续(spike 通过后另立)

key 到位 → 换香港 building URL、处理 `?key=` 查询串解析、做成正式可开关图层 + UI 署名。详见 spec「后续路径」。
