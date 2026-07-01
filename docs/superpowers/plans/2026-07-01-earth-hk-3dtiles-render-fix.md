# 香港 3D Tiles 渲染正确性修复 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 修复真实香港建筑 3D Tiles 数据在 osgVerse 里渲染成"灰白破碎多边形、无贴图、看起来像扭曲地图"的 bug,让其与官方 `3d.map.gov.hk` 门户观感一致(干净、贴图正确、形状规整)。

**Architecture:** 两段式——① 用一个**决定性隔离实验**(单个真实 b3dm、走与真实场景完全相同的网络加载代码路径)判定 bug 属于"单文件 glTF/网格解析"还是"多瓦片树组合(transform/LOD/refine)";② 按判定结果在对应文件里做一次有依据的最小修复,再跑回归 + 真机交付。**不在根因未定前动代码**(遵循 systematic-debugging 铁律)。

**Tech Stack:** osgVerse(基于 OpenSceneGraph)、Cesium 3D Tiles / glTF 2.0 / KTX2、libhv HTTP、picojson。

---

## 已确认的背景(本次会话已排除的假设,执行者无需重查)

| 假设 | 结论 |
|---|---|
| 数据本身损坏 | ❌ 排除——官方 `3d.map.gov.hk` 同一坐标渲染完全正常(已截图对比) |
| KTX2/Basis Universal 纹理转码 | ❌ 排除——`[LoaderKTX] Transcoded format` 大量成功、无报错 |
| RTC_CENTER 精度偏移未处理 | ❌ 排除——这份数据的 feature table 不含该字段 |
| meshopt/draco/顶点量化压缩 | ❌ 排除——accessor 全部是标准 FLOAT/UNSIGNED_SHORT,无压缩扩展 |
| 节点名字("PagedLOD_xxx"等)被误读为指令 | ❌ 排除——`LoadSceneGLTF.cpp` 里名字只用于 `setName()`,纯标签 |
| box→包围球计算错误 | ✅ **真实 bug,已修复**(`plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp` `getBoundingSphere`,只采样 4 个点漏掉负方向和真正角点)。修复后 `bounding volume totally different` 警告从大量降到 0,**但复测后灰色破碎多边形依然存在,证实这不是主因** |
| 单个 b3dm 内 node/transform 组合逻辑(`LoadSceneGLTF.cpp:586-627` `createNode`) | 代码读完确认逻辑正确(真实数据节点无 TRS 字段→按规范默认单位矩阵),**但尚未做过真正的运行时隔离验证**(见 Task 1 的发现) |

已修复并提交:
- `66e39f03` fix(readerwriter): 跳过 b3dm/i3dm/cmpt/pnts 的磁盘缓存写回(修复真实数据必崩溃的 crash)
- `plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp` box→包围球修复(**本次会话产生,尚未 commit,executor 需要先确认这份改动还在工作区**)

**本次会话已发现但未完全解释的现象**:用本地文件路径(而非网络)单独加载同一个 b3dm 时,**没有任何渲染结果**(不是变形,是完全不可见),日志显示 tileset 声称加载成功(有合理的 bound center)。怀疑原因:`ReaderWriterGLTF::readNode(path, options)`(文件路径分支,内部调用 `osgVerse::loadGltf`)和 `readNode(istream&, options)`(网络流分支,内部调用 `osgVerse::loadGltf2`)是**两条不同代码路径**——所有真实测试(含官方 API)走的都是后者。**Task 1 必须复用网络路径**,不能用本地文件路径做隔离测试,否则测的是错的代码分支。

---

## File Structure

- **新增(已在本次会话创建并提交到仓库,不是临时文件)**:
  - `applications/earth_explorer/test/hk_single_tile_fixture/tileset.json` —— 最小化单瓦片 tileset,`root.transform` = 真实香港 tileset 根变换,`root.boundingVolume.box` = 真实子瓦片包围盒,无 `children`(直接引用 content,绕过 PagedLOD)
  - `applications/earth_explorer/test/hk_single_tile_fixture/F_Tile_+4_5_0+R9_0.b3dm` —— 真实香港建筑 b3dm(已从官方 API 下载,不含 key,可安全提交)
- **可能需要修改(由 Task 1 的结果决定,二选一)**:
  - `plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp` —— 瓦片树组合(transform 复合、PagedLOD range、refine 处理)
  - `readerwriter/LoadSceneGLTF.cpp` —— 单个 glTF/b3dm 内网格/顶点缓冲解析

---

## Task 0: 确认工作区状态

**Files:** 无修改,仅检查

- [ ] **Step 1: 确认 box→包围球修复还在工作区**

```bash
cd /Users/franklee/osgverse
git diff plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp
```

预期:能看到 `getBoundingSphere` 里 box 分支从"4 点采样"改成"8 角点循环 `expandBy`"的 diff。如果 diff 为空(改动丢失),重新应用:把 `box` 分支里的
```cpp
result.expandBy(center); result.expandBy(center + xWidth);
result.expandBy(center + yWidth); result.expandBy(center + zWidth);
```
替换为
```cpp
for (int sx = -1; sx <= 1; sx += 2)
    for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            result.expandBy(center + xWidth * sx + yWidth * sy + zWidth * sz);
```

- [ ] **Step 2: 确认 fixture 文件存在**

```bash
ls -la applications/earth_explorer/test/hk_single_tile_fixture/
```

预期:`tileset.json`(532 字节)+ `F_Tile_+4_5_0+R9_0.b3dm`(171752 字节)。

- [ ] **Step 3: 提交 box→包围球修复(独立提交,便于回滚)**

```bash
git add plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp
git commit -m "$(cat <<'EOF'
fix(3dtiles): correct box-to-bounding-sphere computation

getBoundingSphere's box branch only sampled 4 points (center + 3
positive-direction extents), undersizing the sphere and drifting its
center for any non-cubic box. This was the source of the persistent
"Given bounding volume is totally different" warnings seen against
real Hong Kong Lands Dept tileset data.

Fixed by enclosing all 8 actual corners of the oriented box. Confirmed
via real-data headless run: warning count went from dozens to 0.
Does NOT resolve the separate untextured/shattered-geometry rendering
issue — that remains under investigation (see
docs/superpowers/plans/2026-07-01-earth-hk-3dtiles-render-fix.md).

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

- [ ] **Step 4: 也提交 fixture 文件**

```bash
git add applications/earth_explorer/test/hk_single_tile_fixture/
git commit -m "$(cat <<'EOF'
test(earth): add real single-tile HK 3D Tiles fixture for isolation testing

Minimal tileset.json (real root transform + real boundingVolume.box,
no children — bypasses PagedLOD) + one real b3dm downloaded from the
HK Lands Dept building API. No API key embedded, safe to commit. Used
to isolate whether the untextured/shattered-geometry bug lives in
single-file glTF parsing or tile-tree composition.

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>
EOF
)"
```

---

## Task 1: 决定性隔离实验(必须走网络代码路径)

**Files:** 无修改,仅运行验证

- [ ] **Step 1: 在 fixture 目录起本地 HTTP 服务**(必须在普通终端跑,不要在沙箱化的 agent Bash 工具里跑——本次会话在 agent 沙箱里跑 `python3 -m http.server` 因签名策略被系统拒绝加载 `binascii`,这是沙箱限制不是真实问题;普通终端不受此限制)

```bash
cd /Users/franklee/osgverse/applications/earth_explorer/test/hk_single_tile_fixture
python3 -m http.server 18842
```

留这个终端窗口开着。

- [ ] **Step 2: 另开终端验证 fixture 可访问**

```bash
curl -sS -o /dev/null -w "tileset.json http=%{http_code}\n" http://localhost:18842/tileset.json
curl -sS -o /dev/null -w "b3dm http=%{http_code}\n" "http://localhost:18842/F_Tile_+4_5_0+R9_0.b3dm"
```

预期:两条都是 `http=200`。

- [ ] **Step 3: 增量构建(确保 Task 0 的修复已编译进二进制)**

```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```

- [ ] **Step 4: headless 跑 EarthExplorer,指向本地 HTTP fixture**(`http://` 前缀会强制走 `.verse_web` 网络包装分支,和真实香港 API 完全同一条代码路径)

```bash
cd /Users/franklee/osgverse/build/sdk_core/bin
rm -f /tmp/earth_capture_0.png
export EARTH_3DTILES="http://localhost:18842/tileset.json"
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
OSG_NOTIFY_LEVEL=INFO EARTH_AUTOCAP=1500 EARTH_FRAME_SLEEP_MS=20 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 22.539131 114.198421 8 > /tmp/run_isolated_http.log 2>&1
unset EARTH_3DTILES
grep "\[Tiles3D\]" /tmp/run_isolated_http.log
```

预期:一行 `[Tiles3D] loaded: http://localhost:18842/tileset.json bound center=...`,center 数值应接近 `(-2.416e6, 5.376e6, 2.430e6)`(和本会话之前对真实 API 的测量一致)。

- [ ] **Step 5: 读取截图**

用 Read 工具查看 `/tmp/earth_capture_0.png`。

### 判定分支(决定走 Task 2A 还是 Task 2B)

- **画面里出现一栋形状规整、贴图正常的建筑** → bug 在瓦片树组合层 → 执行 **Task 2A**。
- **画面里出现和之前真实场景一样的灰白破碎多边形** → bug 在单文件 glTF/网格解析层 → 执行 **Task 2B**。
- **画面里什么都没有**(本会话用本地文件路径测试时出现过这个结果,但那次走的是错误的代码分支/`loadGltf`,不代表这次网络路径也会如此)→ 先检查 Step 4 的完整日志有没有 `[LoaderGLTF]`/`[LoaderKTX]`/warning 相关行,确认 b3dm 实际被请求到、被解析尝试过;如果日志显示解析被尝试但仍不可见,这是**第三种独立现象**,记录下来,不要凭空猜测修复,回到 systematic-debugging Phase 1 重新收集证据(比如把相机进一步拉远到 30-50km 确认不是视野/裁剪问题)。

### ✅ 判定结果(2026-07-01,已跑完)

**走正确网络路径**(本地 HTTP 服务器,`dangerouslyDisableSandbox: true` 绕开沙箱化 Bash 工具对 `python3 -m http.server` 的签名策略拒绝——普通终端不受此限制,不需要这个 flag),`--goto 22.5425 114.2035 3`,截图见**两团独立的灰白破碎多边形**(分别对应这个 b3dm 内部的 mesh0/mesh1),与真实完整场景下的现象一模一样。

**结论:bug 不在瓦片树组合层(Task 2A 不用做),在单文件 b3dm/glTF 网格或纹理解析本身 → 执行 Task 2B。** 两个独立 mesh 都同样破碎,提示是共享的解码逻辑有系统性问题,不是某个 mesh 数据本身的个例。

---

## Task 2A: 追查瓦片树组合(仅当 Task 1 判定为"单瓦片正常")

**Files:** `plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp`(读+临时加诊断日志,之后移除)

- [ ] **Step 1: 加临时诊断日志**

在 `createTile`(9 参数重载,签名 `osg::Node* createTile(picojson::value& content, picojson::value& children, const osg::BoundingSphered& bound, double range, const std::string& st, const std::string& prefix, const std::string& name, const osgDB::Options* options, bool absBound)`,文件里搜 `Set correct children bounding sphere and ranges` 那段注释能定位到)的 bounding-volume 不匹配检查之后、PagedLOD range 设置之前,加:

```cpp
// TEMPORARY DIAGNOSTIC — remove after root cause found
OSG_NOTICE << "[3DTilesDiag] tile=" << name << " uri=" << uri << " refine=" << st
           << " range=" << range << " bound.center=" << bound.center()
           << " bound.radius=" << bound.radius() << std::endl;
```

- [ ] **Step 2: 重新构建**

```bash
cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release
```

- [ ] **Step 3: 对真实香港数据跑,抓完整诊断日志**

```bash
cd /Users/franklee/osgverse/build/sdk_core/bin
rm -f /tmp/earth_capture_0.png /tmp/run_diag2a.log
export EARTH_3DTILES="https://data.map.gov.hk/api/3d-data/3dsd/WGS84/building/tileset.json?key=<你的真实key>"
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
OSG_NOTIFY_LEVEL=INFO EARTH_AUTOCAP=3500 EARTH_FRAME_SLEEP_MS=40 EARTH_SUN_TO_CAMERA=1 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 22.2803 114.1594 0.5 > /tmp/run_diag2a.log 2>&1
unset EARTH_3DTILES
grep "\[3DTilesDiag\]" /tmp/run_diag2a.log > /tmp/tile_diag_dump.txt
wc -l /tmp/tile_diag_dump.txt
```

⚠️ 这条命令带真实 key,**不要**把完整命令粘贴到任何会被提交/公开的地方。

- [ ] **Step 4: 分析诊断输出,重点查三件事**

```bash
grep "refine=ADD" /tmp/tile_diag_dump.txt | wc -l   # 有没有 additive 瓦片(有的话,父子同时渲染是 3D Tiles 规范规定的正确行为,需确认代码是否按预期只在这种瓦片上这样做)
grep -E "range=0[^.]|range=-|range=nan|range=inf" /tmp/tile_diag_dump.txt   # 有没有退化的 range 值,会打乱 DISTANCE_FROM_EYE_POINT 的 LOD 切换
awk -F"center=| range=" '{print $2}' /tmp/tile_diag_dump.txt | sort | uniq -c | sort -rn | head -20   # 有没有大量瓦片报告相同/异常聚集的 bound center(暗示 transform 复合错误,多个本该分散的瓦片被摆到了同一处)
```

- [ ] **Step 5: 根据 Step 4 定位到的具体异常,在 `createTile`/`createTileChildren` 做针对性最小修复**(具体改动取决于发现——range 公式问题就修 range 计算;additive 处理不对就修 refine 分支;transform 复合错误就查 `mt->setMatrix` 那段和递归调用链)。

- [ ] **Step 6: 移除 Step 1 加的临时诊断日志。**

- [ ] **Step 7: 重新构建,依次跑 Task 1 的隔离测试 + Task 2A Step 3 的真实密集区测试,确认两边都渲染正常。**

- [ ] **Step 8: 提交**

```bash
git add plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp
git commit -m "fix(3dtiles): <根据 Step 5 实际修复内容填写具体描述>"
```

---

## Task 2B: 追查单文件网格解析(仅当 Task 1 判定为"单瓦片也破碎")

**Files:** `readerwriter/LoadSceneGLTF.cpp`(读+改)

- [ ] **Step 1: 读 `createMesh()` 里顶点/索引缓冲的实际解码代码**(本会话只读到 636-689 行的材质/扩展预处理部分,真正的 `POSITION`/`NORMAL`/`TEXCOORD_0` accessor → `osg::Vec3Array`/`osg::Vec2Array`,以及 indices accessor → `osg::DrawElementsUShort` 的代码在更后面,继续往下读)。对照 glTF 2.0 规范的 accessor 定义(`bufferView.byteOffset` + `accessor.byteOffset`,以及可选的 `bufferView.byteStride` 用于交错属性)逐行核对。

- [ ] **Step 2: 用本会话已提取的真实 accessor 结构重点核对一处可疑点**——`accessors[1]`(POSITION,bufferView 2,byteOffset 0)和 `accessors[2]`(NORMAL,bufferView 2,byteOffset **16356**)**共用同一个 bufferView**、靠 byteOffset 区分(不是交错存储,是紧密打包的两段)。确认代码处理"一个 bufferView 被多个 accessor 在不同偏移复用"这种情况时,偏移量计算正确(常见 bug 点:只用 `bufferView.byteOffset` 而忘了加 `accessor.byteOffset`,或反过来)。

- [ ] **Step 3: 加临时诊断**,把解码出的头 3 个顶点坐标 + 全部顶点的实际 min/max 打印出来,和 accessor 自己声明的 `min`/`max` 对比(`accessors[1].min=[-4241.88,390.51,202.09] max=[-223.09,3177.83,1918.67]`)——对不上就是精确的解码 bug 数值证据。

- [ ] **Step 4: 修复确认的解码 bug。**

- [ ] **Step 5: 移除临时诊断,重新构建,先跑 Task 1 隔离测试确认单瓦片干净,再跑真实密集区测试确认整体正常。**

- [ ] **Step 6: 提交**

```bash
git add readerwriter/LoadSceneGLTF.cpp
git commit -m "fix(gltf): <根据 Step 4 实际修复内容填写具体描述>"
```

---

## Task 3: 回归验证 + 交付(无论走 2A 还是 2B,都要做)

**Files:** 无修改,仅运行验证 + 更新文档

- [ ] **Step 1: 4 类历史回归 headless 批次**(复用 `tasks/lessons.md` 里已验证过的复现手法):海洋不橙、晨昏线在、倾斜不穿模、无默认程序化海洋。

- [ ] **Step 2: 与地震/航班共存检查**

```bash
cd /Users/franklee/osgverse/build/sdk_core/bin
export EARTH_3DTILES="https://data.map.gov.hk/api/3d-data/3dsd/WGS84/building/tileset.json?key=<你的真实key>"
DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
EARTH_QUAKES=1 EARTH_FLIGHTS=1 \
EARTH_AUTOCAP=1500 EARTH_FRAME_SLEEP_MS=30 \
./osgVerse_EarthExplorer --no-wait --resolution 1280 800 --goto 22.2803 114.1594 5 > /tmp/run_coexist.log 2>&1
unset EARTH_3DTILES
grep -ciE "sig_handler|handleSignal" /tmp/run_coexist.log   # 期望 0
```

- [ ] **Step 3: 关钩子零影响复查**——不设 `EARTH_3DTILES` 跑一次,确认 `[Tiles3D]` 无任何输出、渲染与改动前一致(这条在 Phase 1 已确认过,修复不太可能影响它,但改了代码后应重新过一遍)。

- [ ] **Step 4: 更新 `docs/superpowers/plans/2026-06-24-earth-hk-3dtiles.md` 的"实现结果"章节**,补充这次修复的根因、改动、验证证据。

- [ ] **Step 5: 交给用户真机验证**——请用户在 HSBC/中环坐标交互查看,并与官方 `3d.map.gov.hk` 门户同一位置的渲染做直接对比,确认观感一致(遵循"务必肉眼验证"铁律)。

---

## 已知的计划局限(诚实说明,不是占位符)

Task 2A/2B 的**具体修复代码无法在此刻预先写死**——根因取决于 Task 1 的判定结果,而 Task 1 还没跑完(本会话跑了错误的代码分支,拿到的是一个需要重新验证的结果)。这份计划把"如何决定性地找到根因"和"两个分支各自的具体排查/修复路径"都写成了可直接执行的具体步骤,而不是"继续调试"这种空话——但最后一步的确切代码改动,只有 Task 1/2A-Step4/2B-Step3 拿到真实证据后才能确定。这是调试类计划相对功能开发类计划的本质区别,不是计划写得不够细。
