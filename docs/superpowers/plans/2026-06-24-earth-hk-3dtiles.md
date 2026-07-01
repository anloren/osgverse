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

- [x] 构建通过;不设 `EARTH_3DTILES` 时行为与改动前一致。
- [x] 样本 tileset 渲染出来(标准 1)——合成 fixture + **真香港 building API 均验证**。
- [x] 落在正确经纬(标准 2)——合成 fixture 与**真数据**两次独立数值验证(见下)。
- [x] 大致贴地(标准 3)——数值在椭球面 h≈0(真数据 root transform 平移量与理论 ECEF 吻合)。
- [x] 缩放/平移流式 LOD 正常、不崩、不冻结(标准 4)——真数据 44 子瓦片树、3 次独立 headless 跑(含深 LOD 定点)均 exit 0、无冻结;修复一处发现的 crash 后确认。
- [x] 不破坏 globe/大气/海洋/地震/航班;关钩子零影响(标准 5)。
- [ ] 真机交付用户肉眼确认(落位/贴地/流式/共存、真实建筑网格可见)——遵循"务必肉眼验证"铁律;headless 街道级深流式受时间预算限制(与既有 z16 深瓦片同类限制),留真机。

## 真香港 building API 验证(2026-07-01,key 到位后)

用户提供地政总署免费 API key,`EARTH_3DTILES=".../3dsd/WGS84/building/tileset.json?key=<key>"` 实测:

- **key 生效**:root tileset.json 200 OK,真数据(218668 components、1218 万三角形、3651 万顶点,45 子瓦片,`gltfUpAxis:"Y"`)。
- **子资源无需带 key**:子瓦片 json、叶子 `.b3dm` 网格均 200(curl 验证),证明现有插件"相对 URI 拼接不带查询串"的机制**天然兼容**该 API,无需额外传播 key 的代码。
- **URL 含 `?key=` 未破坏解析**:`osgdb_web`(`ReaderWriterWeb.cpp`)在选具体 reader 前已把查询串从扩展名里剥离(`ext.find("?")` 处理,`:266-267`),`prefix` 提取也在查询串之前的 `/` 截断——**均已有既有机制兜住,未改代码**。
- **落位数值验证(真数据)**:root transform 平移 `(-2412000, 5378000, 2428000)` + 加载后 bound center `(-2.4124e6, 5.38653e6, 2.41204e6)`,换算 ECEF→LLA ≈ `22.36°N, 114.14°E`——**落在香港境内**,与地政总署"全港覆盖"数据集一致。

### 发现并修复一处 crash(共享代码,超出原计划"只碰新模块"范围)

首次真数据 headless 跑**进程崩溃**(exit 139)。根因排查(非我们新模块的 bug):
- `.b3dm` 内容含 Cesium 专属顶点属性 `_BATCHID`(`readerwriter/LoadSceneGLTF.cpp:858`,读取支持但历史遗留 `// TODO` 未完整处理)。
- **任何网络加载成功的资源,`plugins/osgdb_web/ReaderWriterWeb.cpp` 都会自动写回本地磁盘缓存**(`readFile` 尾部);对 `.b3dm` 走的是 `ReaderWriterGLTF::writeNode`→`saveGltf2`→tinygltf 序列化,遇到 `_BATCHID` 直接崩溃。此路径此前从未被触发(地震/航班/降水都不走"网络拉取 3D 网格并写回缓存"这条路)。
- **修复**(`plugins/osgdb_web/ReaderWriterWeb.cpp`):写缓存前跳过 4 个 Cesium 专属扩展名(`b3dm`/`i3dm`/`cmpt`/`pnts`)——这些格式当前全项目**只有** 3D Tiles 用到,不影响 GIBS/Google/地震/航班/降水现有流量。9 行改动,不碰 3dtiles 插件本身、不碰 globe 着色器/太阳/海洋。
- 修复后 3 次独立 headless 跑(含深 LOD 定点、40ms/帧×3500帧长流式)均 exit 0,tileset 稳定加载、bound center 数值一致。
- **这处改动touches 共享插件文件**(超出 spec 原定"只在 tiles3d_data.cpp 内隔离"的范围),已如实记录,push 前需用户知情确认。

## 实现结果(2026-06-24)

- **Phase 1 完成**(`049ec32a`):`tiles3d_data.{h,cpp}` + `EARTH_3DTILES` 钩子 + earth_main 挂载 + CMake。关钩子 headless exit0、零日志、地球渲染正常;bogus URL 优雅降级不崩;`git show` 确认未碰 glsl/sun/ocean → 4 类回归之外。
- **数据源调整**:计划中的 earthsdk 大雁塔、以及搜到的 ArcGIS Stuttgart 公开样本**均 404 失效**。改用**合成本地 fixture**:`applications/earth_explorer/test/gen_hk_3dtiles_fixture.py`(WGS84 ENU→ECEF root transform 烘焙在香港中环,引用自带 `girl.glb`)。更可控、可复现,直接验香港落位。
- **落位验证 = 数值铁证**(`d0360e8d` 加诊断):加载节点 ECEF bound center = `(-2.4165e6, 5.38758e6, 2.40337e6)`,与香港中环理论 ECEF `(-2416519, 5387607, 2403296)` 三轴吻合到**几十米**(偏差 = 模型局部中心 × scale)。证明 osgdb_3dtiles 正确应用 tileset root transform、与地震/航班同 ECEF 系。**矩阵约定**(Cesium 列主序数组 = 插件 `osg::Matrix(m)` 行主序 + v*M)经实测确认。
- **局限**:合成用的 `girl.glb` 是 PBR 人物、headless 俯视/无纹理下肉眼难辨(真数据带纹理建筑自然可见,数值验证更精确)。

## 交接给后续(spike 通过后另立)

key 到位 → 换香港 building URL、处理 `?key=` 查询串解析、验证多层流式 LOD + 真机视觉、做成正式可开关图层 + UI 署名。详见 spec「后续路径」。
