# osgVerse EarthExplorer — 工作交接（HANDOFF）

> 给**全新会话**的接手文档（你没有之前的上下文）。先读这份，再读 `tasks/lessons.md`（全部排坑细节）。
> 用户主语言中文，请用中文回复。macOS / Apple Silicon。

---
## ✅ 2026-07-02 会话(续)— AI Chat 已全量落地(最新,先读这个)

自然语言驱动 EarthExplorer 的 AI 对话功能(Gemini)已完整实现、10 个任务全部完成,详见 `docs/superpowers/plans/2026-07-02-earth-ai-chat.md`(全部勾选 + 末尾"执行记录"章节)与 `docs/superpowers/specs/2026-07-02-earth-ai-chat-design.md`。**代码已 commit(本地,未 push)**,commit 范围 `6b512db4..7f5286ea`(20 个 commit)。

### 功能清单

- **自然语言飞行**:"飞到纽约"→ `fly_to` 工具解析经纬度/高度,相机平滑飞过去。
- **图层控制**:自然语言开关地震/航班/降水/云图等既有图层(`set_layer` 工具)。
- **数据统计 + 图表**:`get_quakes_summary`/`get_flights_summary`(总数/分桶/最新)驱动对话回答,`show_chart` 工具在右上角手绘卡片(bar/donut/stat/line 四种,ImDrawList 直绘,不额外引入绘图库)。
- **📷 实景照片**:截取当前三维地球视角 → Gemini 图像模型生成同地点同视角写实照片,异步 Job,完成后右上角照片卡展示,点开大图。
- **🎬 首尾帧巡航视频(费用确认门)**:记录当前相机为起点 A → 移动相机记录终点 B → 自动算大圆方位角/距离/升降 → 生成英文运动提示词 → 弹确认框(**约 $2-6,必须用户点确认才会真正调用 Veo**,自然语言路径与 UI 按钮共用同一状态机,工具本身永不跳过确认框)→ 提交 `predictLongRunning` → 轮询 → 完成后视频卡片。

### 架构

- **ToolRegistry(一切能力皆工具)**:`ai_tools.h`,`Tool{name, description, parametersJson, execute}`,`AIChatCore` 是通用 Gemini function-calling 代理循环,**不认识任何具体工具**——加新能力只需注册一个 Tool,不改核心循环。
- **AIChatCore 通用代理循环**:`ai_chat.h/.cpp`,网络请求(libhv 同步 API)放后台线程,工具执行经队列回主线程(`AIFrameHandler` 的 FRAME handler 里 `drainMainThread()`),避免长网络调用卡住渲染帧。`LLMProvider` 接口下有 `GeminiProvider`(真实 REST v1beta)与 `FakeProvider`(`EARTH_AI_FAKE=<script.json>`,脚本化多轮 function-call,离线确定性 E2E 用)。
- **MediaManager Job 状态机**:`ai_media.h/.cpp`,照片走 `PendingState{IDLE,WAITING_SNAPSHOT,GENERATING,DONE_HANDLED}`,视频走独立的 `VideoJob::Phase{IDLE,WAIT_A,WAIT_B,CAPTURING_B,AWAIT_CONFIRM,SUBMITTING,POLLING,DOWNLOADING_FAKE}`(两套状态机字段差异大,拆两个结构体比硬凑通用类型更清楚)。`JobManager`(ai_tools.h)统一管理异步任务的 进度/状态/结果路径,右上角卡片轮询它渲染。
- **ai_cards 卡片系统**:`ai_cards.h/.cpp`,图表/照片/视频/Job 进度统一堆叠到右上角、支持溢出换列、`[x]` 各自独立关闭——从 ai_ui.cpp 抽出的独立子系统。
- **ai_motion.h**:header-only 纯函数 `buildMotionPrompt(llaA, llaB)`(大圆方位角 8 方位罗盘 + haversine 距离 + 升降描述 → 英文 Veo 提示词),不依赖 osgViewer/libhv,tests 直接 include,不必拖入整个 ai_media.cpp 编译单元。

### 模块文件表

| 文件 | 职责 |
|---|---|
| `applications/earth_explorer/ai_tools.h` | Tool/ToolRegistry/AIJob/JobManager 纯头文件抽象(无 OSG 依赖,单测覆盖) |
| `applications/earth_explorer/ai_chat.h/.cpp` | LLMProvider 接口、GeminiProvider、FakeProvider、AIChatCore 代理循环 |
| `applications/earth_explorer/ai_media.h/.cpp` | SnapshotGrabber 快照抓帧、GeminiMediaProvider(生图)、VeoVideoProvider(视频)、MediaManager 状态机 |
| `applications/earth_explorer/ai_motion.h` | buildMotionPrompt 纯函数(A/B 两点 → Veo 运动提示词) |
| `applications/earth_explorer/ai_cards.h/.cpp` | 右上角卡片系统(图表/照片/视频/Job 进度堆叠) |
| `applications/earth_explorer/ai_ui.h/.cpp` | 底部输入条 + 对话历史 + 📷/🎬 按钮 + 视频确认 Modal |
| `applications/earth_explorer/ai_setup.h/.cpp` | `configureAIChat()`:注册全部工具、接线 MediaManager/AIChatCore/AIFrameHandler |
| `applications/earth_explorer/earth_main.cpp` | 调用 `configureAIChat()`、AIChatUI 绘制挂进 EarthControlUI |
| `applications/earth_explorer/quake_data.h/.cpp`、`flight_data.h/.cpp` | 增加 `summaryJson()` 只读统计接口供 AI 工具调用 |
| `tests/ai_chat_tests.cpp` | 单测(`osgVerse_Test_Ai_Chat`):ToolRegistry/JobManager/AIChatCore 代理循环各分支/parseGeminiResponse 防御解析/buildMotionPrompt |
| `applications/earth_explorer/test/ai_fake_*.json` | 7 组 FakeProvider fixture 脚本,离线驱动 E2E |

### 环境钩子表

| 变量 | 作用 |
|---|---|
| `EARTH_AI_KEY` | Gemini API key,设置后 AI 对话真正可用(未设置且无 `EARTH_AI_FAKE` 时,AI 相关代码路径完全不激活,行为与无此功能时一致) |
| `EARTH_AI_MODEL` | 对话模型覆盖,默认 `gemini-3.5-flash` |
| `EARTH_AI_VIDEO_MODEL` | 视频模型覆盖,默认 `gemini-omni-flash-preview`(Interactions API 同步生成;**不支持首尾帧插值**,B 点只进提示词);严格首尾帧穿越设 `veo-3.1-fast-generate-preview`/`veo-3.1-generate-preview` |
| `EARTH_AI_IMAGE_MODEL` | 生图模型覆盖,默认 `nano-banana-pro-preview`(可切回 `gemini-2.5-flash-image`) |
| `EARTH_AI_FAKE=<script.json>` | 离线 E2E:脚本化多轮 function-call 对话,不联网、不需要 key(见 `test/ai_fake_*.json`) |
| `EARTH_AI_FAKE_IMG=<png路径>` | 离线 E2E:`generate_photo` 的生图步骤替换成拷贝该文件字节 |
| `EARTH_AI_FAKE_MP4=<mp4路径>` | 离线 E2E:`confirmVideo` 的提交+轮询+下载整段替换成拷贝该文件字节 |
| `EARTH_AI_OUTDIR=<dir>` | 快照/生成结果输出目录覆盖,默认 `$HOME/Pictures/EarthExplorer` |
| `EARTH_AI_AUTOSUBMIT=<text>` | 测试专用:启动后自动 `core->submit(text)`,headless E2E 用 |
| `EARTH_AI_AUTOSUBMIT_DELAY_FRAMES=<N>` | 配合上面:延迟 N 帧再提交,给地震/航班等异步数据抓取线程留出同步到主线程的时间(默认 0 立即提交) |
| `EARTH_AI_VIDEO_AUTOTEST=1` | 测试专用:headless 无法点 UI 确认 Modal,固定帧数程序化走一遍 beginVideoCapture→captureVideoEnd→confirmVideo,复用真实状态机 |

### 测试体系

- **单测** `osgVerse_Test_Ai_Chat`(`tests/ai_chat_tests.cpp`):ToolRegistry/JobManager 基础行为、AIChatCore 代理循环(基础/忙碌态拒绝提交/多轮/错误呈现/循环上限配对/工具异常隔离)、`parseGeminiResponse` 防御性解析(畸形响应不崩)、`buildMotionPrompt`(西南向下降/持平/上升/北向 wrap 跨 337.5°边界)。纯逻辑、零窗口、零网络,exit code 断言。
- **7 组 fixture E2E**(`EARTH_AI_FAKE=test/ai_fake_*.json` + `EARTH_AI_AUTOSUBMIT`):flyto / quakes-summary / chart(bar) / chart-multi / flights-summary / photo(started→done) / video(两种:工具路径 phase=A、`EARTH_AI_VIDEO_AUTOTEST` 全流程 done→tour_*.mp4)。全部断言 `[AIChat]` 日志行,离线确定性,不需要真实 key。
- **零影响不变式**:不设任何 `EARTH_AI_*` 环境变量跑 600 帧,`grep -c "\[AIChat\]"` 必须为 0、exit code 必须为 0——这是红线,任何改动都不能破坏"没配置 AI 就完全没有 AI 代码路径被激活"。

### 已知边界

- **真机 Gemini/Veo 未冒烟**:本次实现全程没有可用的真实 `EARTH_AI_KEY`,对话/生图/视频三条真网络路径只验证到"请求构造正确、响应解析防御性够强(畸形/缺字段不崩)、HTTP 错误分支不崩溃"这一层,**没有验证过真实响应内容的观感质量**。Veo 视频生成需要付费层(约 $2-6/条),额外需要用户自己的计费账号。
- **快照/生成文件不清理**:`EARTH_AI_OUTDIR` 下的截图/生图/生成视频文件会持续累积,没有做过期清理或大小上限,长期使用需要用户自己清理(默认 `$HOME/Pictures/EarthExplorer`)。
- **ai_media.cpp 接近拆分阈值**:当前约 1090 行(照片状态机 + 视频状态机 + 两个 Provider 都在一个文件里),未来如果再加生成式媒体功能(比如更多视频模型/多图生成),建议先把视频那部分拆到独立的 `ai_video.cpp`,不要继续往这个文件里堆。
- **香港 f2 3D Tiles 建筑体未渲染(与本功能无关的既有问题)**:回归验证时发现 3D Tiles 图层只加载根 tileset.json,子瓦片(建筑体 mesh)从未流式渲染出来(地面本身平整、无 DSM 鼓包,之前的回归修复仍然有效)。已确认不是本次 AI Chat 改动引入,已 flag 给用户作为独立任务跟进。终审复核补充:本次 headless 签名(0 次子瓦片请求、0 次 KTX 转码)与旧的"headless 可见性之谜"(1413 次 KTX 转码但截图无差异)不同,排查时勿混为一谈;真机交互曾亲眼确认建筑可见,按既有结论应真机判断。

### 待用户验收清单

1. `export EARTH_AI_KEY=<你的 Gemini key>` 后跑 `dist/EarthExplorer.app`(或 `build/sdk_core/bin/osgVerse_EarthExplorer`)。
2. 底部对话条输入"飞到纽约上空" → 确认相机平滑飞过去、对话历史显示问答。
3. 输入"统计一下全球地震情况" → 确认地震层自动开启(若未开)、回答里有总数/分桶信息。
4. 点「照片」按钮 → 确认右上角出现生成中 Job 卡 → 完成后可点开查看生成的写实照片,判断图质是否可接受。
5. 点「视频」记录起点 A → 移动相机 → 点「完成B点」→ 确认框里核对 A/B 坐标与运动提示词是否合理 → 点「确认生成」(**注意:这一步真会花钱,约 $2-6**)→ 等待 Job 完成 → 查看生成的 8 秒巡航视频质量。
6. 若 Veo 账号没有付费层权限,确认收到的是清晰的 403/权限错误提示(灰显 + 聊天里的错误文案),而不是卡死或崩溃。

---
## ✅ 2026-07-02 会话(续2)— f2 正式接入为图层 + EARTH_BASEMAP 底图钩子

用户拍板"都做",已完成并验证。详见 `docs/superpowers/plans/2026-07-02-earth-hk-f2-layer.md`。

- **f2 正式图层**:`tiles3d_data.{h,cpp}` 重构为 `Tiles3DLayer` 控制器(仿 QuakeLayer);默认源=f2(免 key,实测),**懒加载**(勾选才后台线程拉根 tileset → update 回调主线程挂树;NodeMask 开关,关=零流量)。UI 新组「三维城市 / 3D City」+ 署名小字(OverlayLayer 新增通用 subtitle 字段)。`EARTH_3DTILES`:`1`=默认源随启动开 / `<url>`=换源 / `0`=关 / 未设=UI 控制。
- **EARTH_BASEMAP 钩子**:`google`(默认)/`esri`/自定义模板,createCustomPath ORTHOPHOTO 分支,预热同源。
- **"地面扭曲"真根因(用户指认"扭曲层更亮≠建筑"后三度修正,最终版)**:**Terrarium 高程是含楼高的 DSM**(尖沙咀 z15 瓦片 p90≈37m/最高 78m,真实地面 3-8m)× `TileElevationScale=2.0` → 城区 80-120m 假土包,亮色底图绷上面拉扯变形,与 f2(真实高度、偏暗着色)错位。**也是更早会话"地图扭曲"悬案的真根因。**次要因素:Google/Esri 正射掺倾斜航拍(楼烘焙进地面)。
- **修复(`e66e4f96`)**:九龙+港岛北岸平原 bbox 内 z>=12 高程返回空路径(平坦几何),假土包消失,太平山/狮子山在 bbox 外保留;`EARTH_HK_FLATDEM=0` 可关。效果图=f2 建筑站平整地面、维港干净、背景群山自然。不动共享插件。
- **验证**:f2 尖沙咀街道级细化 ✓、默认零流量 ✓、全球远景+极点回归 ✓、Esri 切源 ✓、flatdem 三连跑稳定 ✓(一次 teardown SIGBUS 偶发,复现 0/3,不阻塞)。真机交互验证待用户。
- 未闭环小事项(不阻塞)见 plan 文档:粗 LOD 观感无 SSE 调节钩子、EARTH_TILT 时序、dist 未重打包。

---
## ✅ 2026-07-02 会话(续)— f2 决策已定 + 已 push + f2 实测:数据全免 key 直达,但插件缺"外链 tileset 矩阵继承"(结论后被修正,见续2)

**用户把 f2 调研与否的决定权交给本会话,已按计划执行完毕**:9 个 commit 已 push `origin/master`(HEAD=`1484f3f7`),f2 实测完成,**结论:接入 f2 不是换个 URL 就行,插件有一个明确的缺口要修**。

### f2(3D Visualisation Map API)实测结果 —— 数据侧全绿,渲染侧一个明确缺口

- **完全不需要 key(实测)**:`https://data.map.gov.hk/api/3d-data/3dtiles/f2/tileset.json` 及其下所有子 tileset/b3dm,**不带 key 全部 HTTP 200 直接下载**。官方文档说的 key(同一邮箱 `3dmap@landsd.gov.hk` 免费申请)当前未强制。署名要求与 3dsd 相同。
- **纹理是真 photorealistic**:抽查叶子 b3dm,嵌入 **KTX2**(Basis)压缩纹理,单瓦片 74KB 真实航拍纹理。**引擎的 KTX2 转码链路实测工作正常**(`LoaderGLTF`→`loadKtx2`→`[LoaderKTX] Transcoded format ... 83f0`=DXT1,一次 headless 跑出数千次成功转码)。
- **结构(ContextCapture 风味,与 3dsd 不同)**:root tileset(asset 1.1,refine=ADD,box 为绝对 ECEF)→ 17 个外链子 tileset(如 `1/tileset.json`,**root.transform=纯 ECEF 平移**,box 为局部坐标)→ 再外链叶子 tileset(如 `1/Data/temp0/Tile_5_15_L4.json`,**自己没有 transform**,坐标沿用父级局部系)→ b3dm(gltfUpAxis=Y)。
- **f2 网格实际能渲染(用户亲眼确认)**:我一度根据"叶子 tileset 无自身 transform + headless A/B diff 0.02%"判断插件缺外链矩阵继承 → **这个结论是错的,已撤回**。真相:`ReaderWriter3dTiles.cpp::createTile` 先构建 content/children 子树、**最后**才用本文件 root 的 transform 包 `MatrixTransform`(约 L281-293),外链 tileset 作为子节点天然嵌套在引用方的矩阵之下,场景图自动完成累积——矩阵链路是通的。用户在测试窗口交互拖动时**亲眼看到 f2 带真实纹理的建筑**。
- **仍未解释的小谜(下轮如果正式接入 f2 时再查)**:headless `--goto 22.296 114.156 2.0` 俯视、开/关 f2 的截图逐像素 diff 只有 0.02%,但同次运行有 1413 次 KTX 成功转码——"瓦片已加载已挂树"与"该视角截图无差异"的矛盾没有闭环(怀疑与 LOD 挂接时机/相机视角有关,交互拖动时确认可见)。诊断时注意:headless 截图需 `dangerouslyDisableSandbox`(沙箱拦应用写 /tmp);`EARTH_TILT` 与 goto 动画时序冲突,低空斜视角构图 headless 复现不出来,别再浪费时间,直接真机交互看。
- **"地面地图扭曲"已定性 = Google 源影像自带(非 bug)**:直接 curl 原始瓦片(`mt1.google.com/vt/lyrs=s` 半山 z17)可见楼体大面积倾斜——香港高密度区 Google 正射拼接掺了倾斜航拍,楼的侧立面被烘焙进地面影像。与昆明瓦片错位是两回事,改渲染代码修不了。开 3D 建筑层后视觉冲突更明显(立体楼直立 vs 地面"画着"倒伏楼)。缓解方向:换更接近真正射的底图(如 Esri World Imagery,可做 EARTH_BASEMAP 钩子对比)或接受现状(f2 网格自带真实地面纹理,近距离会盖住底图)。
- **下一步(待用户点头)**:正式接入 f2 为可开关图层(URL 默认值/UI 开关/数据来源署名),顺带查上面那个 headless 可见性小谜;3dsd 路线继续可用,不冲突。

---
## ✅ 2026-07-02 会话 — 香港真实 3D Tiles：破碎 bug 已修复 + 纹理"问题"查明是官方预期行为

**本会话代码工作已全部完成、已提交(8 个 commit,`ed6257c3`..`ecb6e3b2`)、未 push**。真机已用真实 key 验证过。剩一个**用户尚未回答的决策**,新会话开场就该先问。

### 破碎 bug —— 已修复,已被用户真机确认

- **决定性隔离实验**:单独一个真实 b3dm 瓦片(脱离瓦片树、走与真实场景相同网络路径)依然破碎 → bug 在单文件渲染层,不在瓦片树组合。
- **真根因(出乎意料,不是数据解析问题)**:3D Tiles 图层挂在 `sceneCamera` 下,默认**继承**了地球的 `scattering_globe` 大气散射着色器(自己没挂程序)→ 被地表 clarity/大气混合逻辑渲染成惨白破碎面片。`LoadSceneGLTF.cpp` 的顶点/索引/纹理解码经运行时插桩验证**完全正确**,不是它的锅。
- **修复**(`c0dd09ff`+`dcc6ad28`,仅 `applications/earth_explorer/tiles3d_data.cpp` +62行):给这层挂一个自成一体的最小网格着色器(采样 DiffuseMap + 简单半兰伯特光照),`OVERRIDE` 覆盖继承的 globe 程序。仿照 `city_data.cpp` 建筑图层的既有模式。**`LoadSceneGLTF.cpp`/globe 着色器零改动**。
- 顺带修复 `ReaderWriter3dTiles.cpp` 的 box→包围球计算 bug(`ed6257c3`,独立成立,不是破碎问题主因)。
- **验证链条**:本地 fixture 隔离测试肉眼前后对比 → 独立子代理二审(合规性+代码质量,均过,读了 OSG 源码真核实,不是走过场)→ 4 类历史回归 headless 复测 + 关钩子零影响 + 与地震/航班共存 → **用户用真实 key 在真机跑了交互版**,九龙密集区截图确认**建筑形状已规整**(不再是破碎多边形),shape 层面的 bug 确认修复生效。

### 纹理"没贴图/发灰" —— 查明是官方数据集设计如此,不是 bug

用户真机截图显示建筑规整但**大片是纯灰色无贴图**,一度怀疑修复不彻底。深挖后:

- **数值铁证**:同一片密集区抽查两栋楼,一栋纹理 512×512(真实照片细节),另一栋只有 **8×8 像素**(纯色占位)——同一份数据里精度天差地别。
- **官方文档逐字确认(非推测)**:地政总署 `hkmapmeta.gov.hk` 原文——建筑分 **Level 1/2/3**,Level 1(footprint>4㎡ 的绝大多数普通建筑)"**do not apply photorealistic texture**",只有 Level 2/3(**"prominent buildings with landmark importance"**)才 "photorealistic texture applied"。8×8 占位 = Level 1 普通楼,512×512 真贴图 = Level 2/3 地标楼,**完全吻合官方设计**,不是我们代码的锅。
- 官方还有一个**独立产品"3D Visualisation Map"**(不同端点 `3dtiles/f2`,标榜"highly photorealistic"),`3d.map.gov.hk` 门户默认展示的很可能是这个,不是我们申请到的 `3dsd`(3D Spatial Data,偏 GIS 分析用途)——之前拿门户效果对比,是在比两个不同档次的数据产品。
- 顺带排查过"地图扭曲"的其他可能原因,均排除:`city_data.cpp` 城市列表不含香港(无图层重叠)、GIBS 云图 404 确认无关且早于本次工作存在、Google 卫星底图深层缩放 404 确实存在但集中在市区以南海域(不在建筑聚集区)。
- 已核实"API 迁移截止 2026-05-09"的说法是过期缓存信息,当前 API 文档页无此通知,我们用的端点结构和文档一致,无需改动。

### ✅ 悬而未决 → 已解决(2026-07-02 续会话)

用户把决定权交给了续会话,执行结果:**已 push**(9 个 commit 到 `origin/master`)+ **f2 已实测**(见最顶部章节:数据免 key 直达、KTX2 转码正常,但插件缺外链 tileset 的 transform 继承,待用户决定是否修)。

详见 `docs/superpowers/plans/2026-07-01-earth-hk-3dtiles-render-fix.md`(完整排查过程+判定分支)、`docs/superpowers/plans/2026-06-24-earth-hk-3dtiles.md`(累积实现结果)。

---
## ✅ 2026-06-24 会话 — 香港官方 3D Tiles 接入 spike（最新,先读这个）

香港地政总署官方 3D 城市模型接入的**最小验证 spike**,已合并 master(`a43e984f`,**未 push,待用户决定 push 时机/真机看一眼**)。核心落位链路**数值验证通过**。详见记忆 [[earth-hk-3dtiles-spike-verified]]、`docs/superpowers/specs|plans/2026-06-24-earth-hk-3dtiles*`。

- **数据**:香港 **3D Spatial Data API = Cesium 3D Tiles + WGS84**(`https://data.map.gov.hk/api/3d-data/3dsd/WGS84/{building,infrastructure}/tileset.json?key=KEY`)。**免费 key 已发邮件 `3dmap@landsd.gov.hk` 申请、待回**;授权可商用但**需署名 attribution**。下载版另有 OBJ/OSGB/Cesium3DTiles(3d.map.gov.hk 交互选区,非批量;**不下载全港**——流式才对)。
- **引擎已自带**:`plugins/osgdb_3dtiles`(tileset.json 递归 + root transform + PagedLOD + HTTP)+ `osgdb_gltf`(b3dm/glb),直吃,无需造轮子。
- **实现**:新模块 `applications/earth_explorer/tiles3d_data.{h,cpp}` + `EARTH_3DTILES=<url>` 钩子,挂 `sceneCamera`(同地震/航班 ECEF 系)。http→`readNodeFile(url+".verse_web", Options("Extension=verse_tiles"))`/local→`url+".verse_tiles"`。**不碰 globe 着色器/太阳/海洋 → 4 类回归之外**;默认不设钩子零影响。
- **验证 = 数值铁证**:公开样本(earthsdk 大雁塔、ArcGIS Stuttgart)**都 404** → 合成本地 fixture `applications/earth_explorer/test/gen_hk_3dtiles_fixture.py`(WGS84 ENU→ECEF 烘焙在香港坐标,引用自带 `girl.glb`)。加载节点 ECEF bound center 与香港中环理论值 `(-2416519,5387607,2403296)` **三轴吻合几十米** = 落位正确。矩阵约定:Cesium 列主序数组 = 插件 `osg::Matrix(m)` 行主序 + v*M,**直接填不转置**。
- **待真数据(key 到了)**:换香港 building URL → 处理 `?key=` 查询串解析、验**多层流式 LOD** + 真机视觉、做正式可开关图层 + UI 数据来源署名。spike 局限:单 tile fixture 验不了多层流式;合成 PBR 人物 headless 难肉眼辨(真数据带纹理建筑自然可见)。

---
## ✅ 2026-06-23 会话(续2)— 实时航班层 OpenSky

第三个实时数据流:**全球航班**。v0.10 已打标(地震+降水里程碑)。航班层 6 个功能任务子代理驱动 + 每任务审查,**已全部 push `origin/master`(HEAD=`93894f60`)**。`dist/EarthExplorer.app` 已重打包。用户已接受现状(含下述覆盖限制)。

- 新模块 `applications/earth_explorer/flight_data.{h,cpp}`(仿地震点要素模板)。**独立点图层 + 独立着色器,不碰 globe 着色器/太阳/海洋 → 4 类回归之外。**
- 数据:**OpenSky `states/all` 视口包围盒查询**(无 key;主线程每帧由相机算可见区 bbox→mutex→worker 抓;实测欧洲 900km 视口 3648 架)。libhv 后台 GET + picojson。
- 渲染(震撼+快):单 `GL_POINTS` + **按航向旋转的发光箭头**(片元旋转 gl_PointCoord 画三角)+ **高度配色**(低暖橙/中黄白/高冷青蓝);**每帧 CPU 外推位置→平滑滑行**(velMS×heading×elapsed,数千架仍廉价)。
- 交互:点击航班→**右上角「航班详情」面板**(呼号/国家/高度/速度/航向,叠在地震卡下方,遵循信息UI右上角偏好)。
- UI:「实时数据 / Live」组加「航班 (OpenSky)」开关(默认关)。钩子 `EARTH_FLIGHTS=1`、`EARTH_FLIGHTS_FILE=<states.json>`。
- 设计/计划:`docs/superpowers/specs/2026-06-23-earth-flight-layer-design.md`、`docs/superpowers/plans/2026-06-23-earth-flight-layer.md`。详见 [[earth-flight-layer-done]]。
- 注:首次抓取偶尔 0 架(启动 bbox 未稳),~12s 后正常;3648 架在 900km 视口偏密(低空更稀),如嫌密可调 bbox/尺寸。
- **数据覆盖(用户已问"中国为何少"、已确认非 bug)**:OpenSky 众包 ADS-B,覆盖随接收站。实测美国 7195/欧洲 3517/中国 338/印度 258/日韩 42——东亚稀疏=接收站少+中国管制,**改代码补不全**(注册账号只提频率不增覆盖;真·全球需 FR24/Aireon 商业付费源)。用户接受现状。详见 [[earth-flight-layer-done]]。
- **下一步候选(未定,均结合"无 key 优先")**:火灾 FIRMS(需免费 key、视觉震撼、套点模板)、空气质量 OpenAQ(无 key、最快)、极光 SWPC(无 key、需新极区渲染);或打磨现有(航班拖尾/密度/UI)。

---
## ✅ 2026-06-23 会话(续)— 消除"补丁状瓦片" + 降水两处修复

**已全部 push `origin/master`(HEAD=`f48e61af`,含降水层+性能/切换/补丁修复共 14 提交)。** 用户已确认观感 OK(瞬态加载补丁=在线流式固有,选择维持现状)。`dist/EarthExplorer.app` 已重打包。

**① 补丁状瓦片修复(`7495299e`)** —— 用户报"全球操作时地球花得像补丁、各瓦片阴影程度不同"。
- **根因(决定性诊断,非源影像)**:把 globe frag 临时改成纯 `originalGroundColor` 对比 → 补丁消失 → 证明是**渲染着色**:大气散射+HDR 把影像洗灰发雾,且 z3 粗 OCEAN_MASK 派生的太阳着色法线逐瓦片块状跳变。
- **修法**(`scattering_globe.frag.glsl` + `render_effects.cpp`):`finalColor = mix(spaceColor, clearColor, groundClarity)` —— `spaceColor`=现有大气观感(高空保留)、`clearColor`=原始影像×平滑昼夜(低空清晰、无雾)、`groundClarity`=随相机高度 smoothstep。法线改平滑 `normalize(P)`(删粗 mask 法线扰动=块状根源;真实起伏仍由高程几何体现)。band env 可调 `EARTH_CLARITY_ALTLO/ALTHI`(km,默认 2000/8000)。uniform 加在应用层 globe stateset,**不碰共享管线**。
- **验证**:低空清晰无补丁、中空平滑过渡、高空太空观感保留、晨昏线平滑、海洋不橙、低空倾斜不穿模、地形起伏在。详见 [[earth-tile-patchwork-fix]]。

**② 降水性能修复(`fd198070`)** —— 开降水后任何操作卡顿。根因:OVERLAY 路径切换触发**主线程同步逐瓦片拉网络**(最多1500个)。修法:`updateLayerData` 的 OVERLAY 改用 `nv->getImageRequestHandler()`(场景自带 ImagePager)异步加载。详见 [[earth-precip-layer-done]]。

**③ 降水切换 bug 修复(`c78b1f59`)** —— 云图↔降水来回切后降水不再出现。根因:`_lastTemplate` 去重跨开关失效。修法:再开启时 `force` 绕过去重。

---
## ✅ 2026-06-23 会话 — 降水雷达图层 RainViewer

接入**第二个实时数据流**:RainViewer 降水雷达(原计划"地震+降水"的 Step 2,补齐)。**复用 GIBS 云图的 OVERLAY 槽、与云图二选一、零着色器改动**。4 任务提交(子代理驱动 + 每任务双审,含一处线程安全修复),**待用户真机交互确认后 push**。`dist/EarthExplorer.app` 已重打包(打包二进制验证通过)。

- **OVERLAY 槽按 prefix 分流**(`earth_main.cpp` createCustomPath):`"gibs"`→GIBS / 以 `http` 开头→RainViewer 模板(z>10 不取)/ 空→不加载。默认仍 `"gibs"`,行为不变。
- 数据:`weather-maps.json`(无 key)→ `host`+`radar.past` 末项 `path` → 模板 `{host}{path}/256/{z}/{x}/{y}/4/1_1.png`(256px **RGBA**,无降水处透明)。已 curl 验证。
- 新模块 `precip_data.{h,cpp}`:`PrecipController`(osg::Referenced)+ 后台 `FetchThread`(~8min,仅开启时联网,15s 超时)。**worker 不直接动 TileManager**(`_layerPaths` 无锁、主/cull 线程每帧 `check()` 读)——worker 经 mutex 交模板,**主线程 FRAME handler `applyPending` 调 `TileManager::setLayerPath(OVERLAY, 模板)`**;per-tile `check()` 自动重载(city_data 切 USER 层同款)。`_enabled`/`_refreshNow` 用 `std::atomic<bool>`。
- 互斥 + UI:「影像 / 天气」组两个互斥复选框「GIBS 影像/云图」+「降水雷达 RainViewer」。apply 用**直接写对方 `enabled` 字段**(不调 `setEnabled`)避免递归;`Overlay2Opacity` 由 `applyOverlayOpacity` 统一算(降水>云图>0)。
- 钩子:`EARTH_PRECIP=1`(强制开)、`EARTH_PRECIP_FILE=<weather-maps.json>`(离线样本)。
- 关键坑(已记):①`<readerwriter/TileCallback.h>` 独立编译需先 `#include <osgDB/Options>`(有 `ref_ptr<osgDB::Options>` 成员);② worker→TileManager 数据竞争(已用主线程 handler 修)。详见 [[earth-precip-layer-done]]。
- headless 验证:欧洲实时降水雷达可见、晨昏线/海洋无橙、默认关零抓取。**真机待确认:开关互斥、~8min 刷新**。
- 计划/设计:`docs/superpowers/plans/2026-06-23-earth-precip-layer.md`、spec 的 Step 2 段。

---
## ✅ 2026-06-22 会话 — 实时地震图层接入

接入**第一个真·实时全球数据流**：USGS 地震叠加层。**5 个任务提交**(子代理驱动 + 每任务双重审查 + 整体终审,均 headless 验证),**待用户真机交互确认后 push**。`dist/EarthExplorer.app` 已用最新二进制重打包(打包二进制 headless 跑通)。

- 新模块 `applications/earth_explorer/quake_data.{h,cpp}`(仿 `city_data.cpp`),对外只暴露 `QuakeLayer` 抽象 + `configureQuakeData`。**完全不碰 globe 着色器/太阳/海洋 → 4 类回归零风险**(终审确认 diff 只动 6 个文件,无 `scattering_globe.frag.glsl`/sun/OceanOpaque)。
- 数据:**USGS `2.5_day.geojson`**(M2.5+、过去24h、无 key、HTTPS),`libhv`(已编进 osgVerseDependency,CMake 加 `-DHV_STATICLIB`+include)后台线程 GET + `picojson` 解析。headless 实测联网拉到 **49 条**真实地震。
- 渲染:`GL_POINTS` 圆点精灵(独立 shader,MRT 双输出 gl_FragData[0/1],非 globe 着色器),**大小∝震级、颜色∝深度**(浅红/中橙/深蓝),ECEF 抬升 3km、深度测试遮挡背面。
- 实时:后台 `FetchThread` **每 ~60s** 刷新,**仅图层开启时联网**(关闭空转);`requests` 设 15s 超时界定退出 join。主线程 update 回调重建几何(worker 不碰 GL)。
- 交互:**点击地震点**(屏幕拾取 + 前半球过滤)→ ImGui「地震详情」卡片(震级/深度/位置/经纬/相对时间/USGS 链接)。
- **拾取 bug 已修(用户报告点击无卡片,系统化调试找到根因)**:`pickAt` 原有 `win.z∈[0,1]` 深度闸门把所有点都剔除了——事件阶段读到的相机投影近/远≠渲染时那套(osgViewer 在 cull 才按场景包围盒重算近/远,地球在 RTT 子相机渲染),地表点 z 饱和到 ≈1.0001。前半球测试已负责剔背面,故删掉脆弱的 z 闸门(只用 win.x/y),拾取容差 6→10px。新增 `EARTH_QUAKE_PICKDBG` 自测钩子(投影全表+自拾取)。提交 `24222220`。
- UI:面板新增「实时数据 / Live」分组 +「地震 (USGS)」开关(默认关)。测试钩子 `EARTH_QUAKES=1`(强制开)、`EARTH_QUAKES_FILE=<path>`(本地样本离线验证)、`EARTH_QUAKE_PICKDBG=1`(拾取自测)。
- 设计/计划文档:`docs/superpowers/specs/2026-06-22-earth-quake-precip-overlays-design.md`、`docs/superpowers/plans/2026-06-22-earth-quake-layer.md`。
- **下一步 = Step 2 降水**(RainViewer 雷达,复用云图 OVERLAY 槽、二选一、零着色器改动),设计已记入上面 spec。
- 终审 1 条 minor 备注(非阻塞):`QuakePickHandler`/`EarthControlUI` 持 `QuakeLayerImpl` 裸指针,当前"全随进程退出销毁"生命周期下无害(city_data 同款风格);若日后模块复用需改 observer_ptr。

---
## ✅ 2026-06-21 会话 — 已完成与现状

本会话在干净 master 上做完一批，**全部已 push `origin/master`（HEAD=`b2377bda`）**。**`dist/EarthExplorer.app` 已重打包为最新**（双击即最新）。

**已完成（均 headless 验证 + push）：**
- `56a8729b` fix：**GIBS 影像换 `VIIRS_SNPP`**。原 `MODIS_Terra` 单星日产真彩在赤道留黑色刈幅空隙（梳齿黑楔）；VIIRS 无缝。新增 `EARTH_CLOUDS` 测试钩子。详见记忆 [[earth-gibs-swath-gaps-use-viirs]]。
- `74706d8a`+`db694f27` feat：**启动磁盘预热**（回退功能重做）`prefetchLowLODGlobe`+`EARTH_PREFETCH`（默认 4，0 关）+ `loadFileData` 原子 `tmp+rename`。
- `c9d15a94` perf：**地形地板每帧全场景求交节流**。`updateTerrainFloor` 从每帧 → 移动>330m 或每 15 帧才求交（静止低空降 95%），消低空卡顿，**可能也缓解 #8**。
- `873e2761`+`6c041827` perf：**瓦片 5 层并行加载**（回退功能重做）`LayerLoadPool`+`EARTH_TILE_POOL`（默认 8，0 关）+ reader-writer 缓存加锁。冷缓存吞吐 **~4×**（92→364）。**回归守卫过**（昆明深瓦片+tilt，pool=8≈pool=0，无孔洞/穿模/橙）——但 headless 够不到 z16，真机街道级深下钻+高 tilt 仍建议留意那 3 类。
- `b2377bda` feat：**自适应曝光**（修低空发暗"像黑夜"）。随高度设 `HdrExposure`（低空 1.0、高空 0.25，80–4000km smoothstep 过渡），**纯 C++、着色器不动 → 4 类回归零风险**。`自适应曝光 Auto` 复选框（默认开）+ env `EARTH_EXP_LO/HI/ALTLO/ALTHI` 可调。详见 [[earth-low-altitude-dark-atmosphere]]。

**近平面 Step1 = 查清"不必做"**：`_minDistance` 是死代码（钳眼到地心、永不触发）；防穿靠地形地板 `_terrainLift`；高峰轻微穿模用户接受不修。详见 [[earth-near-plane-mindistance-dead-and-input-freeze]]。

**测试钩子（均 env，默认无影响）**：`EARTH_CLOUDS`、`EARTH_SUN_AZEL=az,el`、`EARTH_TILT`、`EARTH_INPUT_DEBUG`（写 `/tmp/earth_input.log`）、`EARTH_TERRAIN_DEBUG`、`EARTH_TILE_POOL`、`EARTH_PREFETCH`、`EARTH_EXP_*` + 既有 `EARTH_AUTOCAP`/`EARTH_FRAME_SLEEP_MS`/`EARTH_SUN_TO_CAMERA`/`EARTH_TILE_CACHE`。

**仍开放（详见下方 backlog #7/#8/#9 + 记忆）**：
- **#8 偶发输入失灵死锁**：低空探一阵后鼠标对地球失灵（Go To/ImGui 仍可用）、macOS 彩球、Home 救不回、**重启恢复**。未解；疑 ImGui 粘住捕获鼠标；`EARTH_INPUT_DEBUG` 待抓；地形节流或已缓解。
- **#9 掠射太阳橙块**：疑程序化海洋（`OceanOpaque`），**未证实**。决定性测试 = 在出橙时取消「海洋 Ocean」看橙是否消失（本会话用户只对**发暗**做过取消海洋测试=无变化；**橙的取消海洋测试还没做**）。
- **剩余回退功能**：高程感知裙边（只能交互验，易触发 #8）、海洋遮罩深瓦片继承（可选/低优先，建议用祖先烘焙）、完整异步层加载（高风险）。

---
## ⚠️ 2026-06-20 会话已整体回退 —— 必读，别再踩

上一会话试图修"昆明深瓦片错位"，结果**越改越多**，被用户要求**整体回退到会话前版本 `0f902837`**。
当时引入、现已删除的一系列回归 bug —— **下一步迭代严禁再次引入**：

1. **整片海洋变橙红**（全球俯视 33000–43000km，连夜侧也橙）。根因：把默认太阳方向翻成照亮正面后，
   `scattering_globe.frag.glsl` 的**太阳直射辐射 sunL**（低太阳角=落日橙）叠在近黑的海面上 → 全橙；亮地面(撒哈拉)过曝发白。
   **不是瓦片、不是缓存**（Google 海洋瓦片本来就是蓝的，已 curl 验证）。
2. **晨昏线被删**：曾用"直接显示影像(fullbright)"去压橙色，结果把昼夜分界整个干掉了。**用户要保留晨昏线。**
3. **倾斜穿模 / "高耸面片"扇形**：低空(如昆明 2.5km)倾斜时相机钻到地下、看穿地球看见另一侧。
   根因：相机地形地板只**正下方**射线，漏掉**旁边更高的地形**(西山 2.5–2.8km)，倾斜看向它就穿。
4. **桔红程序化水面 / OceanOpaque 默认乱动**：用户明确"程序化光滑水面原来从不显示"，别默认开它。

**下一步迭代铁律**：① 一次只改一个、当场用真实复现条件验证、确认不引入上面 4 类再继续；
② 在**用户报告的确切条件**下复现(高度段/开关/太阳)，别乱缩放截图；③ 改渲染前先 curl 真实瓦片确认不是数据问题；
④ 不要碰默认太阳方向/OceanOpaque 去"修"颜色。详见记忆 [[earth-orange-ocean-rootcause]]、[[earth-debug-verify-at-exact-condition]]。

**✅ 原始 bug 已修复并推送(2026-06-20 后续会话)**：昆明/滇池岸"**看穿孔洞**"(z>15 平瓦片 0m vs z15 高程 ~1890m 的 LOD 边界看穿到地球背面)**已修，用户交互确认大孔洞消失**。详见记忆 [[earth-seethrough-hole-fix]]（含根因、诊断法、窗口真相）。本会话提交(均已 push `origin/master`)：
- `b34ce064` test：`EARTH_TILT` 测试钩子(headless 斜视复现用)。
- `b5976ac7` fix：**相机地形地板**(防穿模，用户确认"始终为正")—— `EarthManipulator` 每帧按**眼睛自身经纬度**竖直射线测地形、把眼睛抬到地形+150m；倾斜可用、far/全球视图跳过。
- `48e38efd` fix：**深瓦片高程一步烘焙**(消看穿孔洞)—— `createCustomPath` 对 z>15 返回 **z15 祖先瓦片** URL；`createTile` 算 `elevScaleBias=((x&(subN-1))/subN,(y&(subN-1))/subN,1/subN,1/subN)`；`createTileGeometry` 用 `euv=uv*scale+bias` 采祖先子区一步烘焙真实高度(影像仍各自全分辨率)。**确定性、无运行时继承竞态**(重做被回退的 `fabf2747`/`e4c177e2`，这次单独做+验证)。配相机地板防穿模；不碰太阳/海洋/着色器→无 4 类回归。
- **全球性已验**(纯瓦片坐标、无地点假设)：珠峰深瓦片 ~8662m、昆明 ~1900-4000m、上海读到东海海床 bathymetry 负值(Terrarium 数据本身、非回归)。
- `dist/EarthExplorer.app` 已用最新二进制**重打包**(全屏，`NSHighResolutionCapable=false`)。

**残留(用户接受、待办、卫星图可接受)**：陡坡瓦片高程范围大→包围球被撑大→**过度细分到 z>15**→偶有极小缝/小范围 LOD 跳变。根治是独立 perf 项(修包围球/`PIXEL_SIZE_ON_SCREEN` 估算)。沿海城市真实街道级观感待人工扫一眼(headless 够不到 z16)。

**窗口"左下 1/4"别再乱修**：是 `.app` 的 Info.plist `NSHighResolutionCapable=false` 修的、**裸二进制 1/4 是正常的**、`getScreenResolution` 返回点数。详见 [[earth-seethrough-hole-fix]] + `tasks/lessons.md:126`。

**更早的校正会话**(同日早些)：补齐漏回退的 `ReaderWriterWeb.cpp`(代码树曾 = `0f902837`)+ 校正本 HANDOFF 陈旧描述。
---

## 这是什么
osgVerse（基于 OpenSceneGraph 的 3D 引擎，`github.com/anloren/osgverse`）的 **EarthExplorer**：一个在线流式高清地球应用，已移植到 macOS（OpenGL 4.1 Core），有 ImGui 控制面板、可双击 `.app`。

## 关键路径
- 仓库：`/Users/franklee/osgverse`（远端 `origin` = github.com/anloren/osgverse，已用账号 `anloren` 登录 gh）
- OSG 源码（预克隆，含本地补丁）：`/Users/franklee/OpenSceneGraph`
- CORE 构建产物：`/Users/franklee/osgverse/build/sdk_core/`；增量构建目录：`/Users/franklee/osgverse/build/verse_core/`
- 可执行：`build/sdk_core/bin/osgVerse_EarthExplorer`；桌面应用：`dist/EarthExplorer.app`
- 实现计划：`docs/superpowers/plans/`；排坑记录：`tasks/lessons.md`；用户文档：`docs/EarthExplorer.md`

## 必须知道的硬约束（否则白干）
1. **macOS 必须用 CORE 模式**（`Setup.sh CORE` → sdk_core/verse_core）。兼容档只有 GL2.1/GLSL120，osgVerse 着色器要 ≥130；CORE 才有 GL4.1/GLSL410。
2. 直接跑二进制要设 `DYLD_LIBRARY_PATH=build/sdk_core/lib`（rpath 用了 Linux 的 `$ORIGIN`，macOS 无效）。
3. **增量构建**：`cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`（秒级到分钟级，别重跑 Setup.sh）。
4. **无头截图验证**（OS `screencapture` 缺权限，用应用内 `ScreenCaptureHandler`）：
   ```bash
   cd /Users/franklee/osgverse/build/sdk_core/bin && rm -f /tmp/earth_capture_0.png && \
   DYLD_LIBRARY_PATH=/Users/franklee/osgverse/build/sdk_core/lib \
   EARTH_AUTOCAP=1500 EARTH_SUN_TO_CAMERA=1 \
   ./osgVerse_EarthExplorer --no-wait --resolution 1280 800 > /tmp/run.log 2>&1
   # 然后用 Read 工具看 /tmp/earth_capture_0.png
   ```
   - `EARTH_AUTOCAP=N`：渲染 N 帧后截图到 `/tmp/earth_capture_0.png` 退出。**注意**：本机约 ~700 帧后 ImGui 后端会 shutdown，N 设太大反而抓不到；定点深瓦片用 `EARTH_FRAME_SLEEP_MS=30` 给网络时间，`EARTH_AUTOCAP=2500` 左右。
   - `EARTH_SUN_TO_CAMERA=1`：太阳对准相机，便于看清（会每帧覆盖 WorldSunDir）。
   - `--goto <纬> <经> <高度km>`：启动直飞某地（俯视）。如 `--goto 37.77 -122.42 6` 看旧金山街道级。
   - `EARTH_FRAME_SLEEP_MS=30`：每帧延时，给在线瓦片流式加载真实时间。
5. **深层瓦片是延迟受限**：拉近后要等几秒瓦片流式加载（z 逐级 3→6→14…到街道级），不是瞬时。已加 TCP/TLS 长连接复用 + 20 HTTP 线程加速。

## Git 现状（已校正 2026-06-20 —— 回退后真实状态，旧正文作废）
- **HEAD = `878eb044`，代码等价于 `0f902837`**。本会话补齐了当初漏回退的半残留：`git checkout 0f902837 -- plugins/osgdb_web/ReaderWriterWeb.cpp`（那个文件的 `_cachedRWMutex` 锁是 #3 并行池的遗留，#3 已回退故一并清掉）。现**整棵代码树与 `0f902837` 逐字节一致**（`git diff 0f902837 -- '*.cpp' '*.h' '*.glsl'` 为空）。
- **真正在 master（活着的）**：P0 标注层、P1 GIBS 影像层、解析 ray-椭球求交(99d3f223)、**elevation z>15 截断(dfd8fd09 —— 正是下面开放 bug 的成因)**、深海蓝未加载默认+常驻 LOD(7a6d1635)、晨昏线调淡(0f902837)。详见下方 P0/P1/性能小节（那几节描述的代码确实在 master）。
- **⚠️ 已被回退、master 里没有（下个 session 别假设存在！）**：启动预热 #1(ba191b19)、5 层并行池 #3(da251b96)、深瓦片高程继承(f99b591e/e4c177e2)、海洋遮罩继承(87b9d155)、最小距离 150m(747b67a5)、相机地形地板/地形感知钳制(e4c177e2/dbeaafa4)、高程感知裙边(837ff999)、太阳翻面+OceanOpaque+fullbright 着色器(4b6f5333/34041206/21dc5777)。**回退一直退到 0f902837，把整条 6-19/6-20 深瓦片线全带走了。** 本文件**旧正文**曾把这些写成"已推送 master / ✅ 已做"——那是回退前描述，**全部作废**（下方 backlog #1/#3 同此）。
- **开放 bug —— 本会话已在现行代码核实根因（昆明/滇池岸深瓦片错位）**：
  - `applications/earth_explorer/earth_main.cpp:204` `if (z > 15) return ""`：z>15 高程路径为空（terrarium 上限，避免 404 主线程阻塞——这是有意保留的 perf 修复）。
  - `readerwriter/TileCallback.cpp:570` `if (!tex && !emptyPath && ...)`：父级高程继承被 `!emptyPath` 门挡住，而截断使 `emptyPath=true`，故继承**永不触发** → 深瓦片停在 0m 海平面、比 z15(~1890m)邻居低 ~1890m → LOD 边界裂缝/看穿/视差错位深块。
  - **为何难修（耦合，上次栽点）**：修法 = 让 z>15 继承 z15 高程（把地形抬到 ~1890m）；但 `readerwriter/EarthManipulator.h:76` `clampDistanceValue=max(dist,minDist)` 是按**椭球基准(0m)**钳制相机、不知地形（解析求交也忽略地形）→ 抬高地形后任何近地缩放都穿模。所以**高程继承与相机地形地板必须配套**，而地形地板（只向正下方射线、漏掉旁边更高地形）正是上次倾斜穿模回归的根。**且 headless 复现不出那个斜视角（本会话已实测），只能交互验证。** 留待用户有空时，按"一次一步、每步交互验证"推进。
- **.app 状态**：`dist/EarthExplorer.app` 很可能是回退前的旧打包（含已回退代码）；要用双击版须从**当前 master 重新** `packaging/package_macos.sh` 打包。Tag v0.8/v0.9/v0.9.1 都在，但**只有 v0.8 是 GitHub Release**——release 须用户确认，否则只同步仓库（见记忆 `release-requires-confirmation`）。
- 设计 spec：`docs/superpowers/specs/2026-06-18-earth-overlay-layers-design.md`（整体路线）。计划：`plans/2026-06-18-...p0-labels.md`、`...p1-gibs-clouds.md`、`2026-06-19-earth-perf.md`。
- **P0 标注层**：底图 `lyrs=s`；标注 `lyrs=h`(USER,texUnit2/UvOffset3)；`LabelOpacity` 门控；`LayerManager`(`applications/earth_explorer/LayerManager.h`)+「图层」UI(方案B 分类折叠)。
- **P1 GIBS 影像层**：GIBS MODIS 真彩(OVERLAY,texUnit3/UvOffset4,`GoogleMapsCompatible_Level9`,**z>9截断**,日期=`UTC今天-2天`)；`Overlay2Opacity` 门控(**大气采样器起始单元 3→4**,applyToGlobe)；面板「影像/天气」组、默认关、透明度0.7。**取舍**：earthURLs 常驻加载→默认关时仍下载(P1.1 改按需)。
- **性能/视觉优化**(plan `2026-06-19-earth-perf.md`)：
  - 求交(99d3f223)：`EarthManipulator::calcIntersectPoint` 改解析 ray-椭球，去掉每次鼠标的全场景 `IntersectionVisitor` 遍历。
  - elevation z>15 截断(dfd8fd09)：AWS terrarium 最高 ~z15，深 zoom 的 z16-19 必 404→主线程阻塞(近地卡死主因)+刷屏，已消除；深瓦片复用父级高程。
  - 橙块(7a6d1635)：未加载底图默认 白→深海蓝`Vec4(0.03,0.06,0.10)`(Scene 单独，Mask 仍白=陆地，见 `render_effects.cpp:89`)；`pager->setTargetMaximumNumberOfPageLOD(1500)`(默认300太小→低/中LOD过期重载露白底)。**用户实测：半个地球橙块已消失** ✅。
  - 晨昏线(0f902837)：橙带=昼夜分界大气 inscatter+饱和过强。`scattering_globe.frag.glsl` inscatter `*0.5`、饱和 `1.18→1.08`(仅夜/晨昏侧 cTheta→0 用到，不动白天影像)。**调淡/调浓就改这两个数。**
  - **缓存查明本就好**：真缓存在 `loadFileData`(`UtilitiesEx.cpp:496`)，per-tile `.tile`、`$HOME/.osgverse_earth_cache`(绝对、env `EARTH_TILE_CACHE` 可覆盖)、无限大小；实测 8537 块/重访 0 新增。registry 的 simdb `FileCache` 不用于 HTTP 瓦片（别再去改它，曾误改致崩溃）。

## 待用户交互验证（headless 无法验）
鼠标旋转/缩放手感、近地流畅度、晨昏线观感（太重→inscatter 再降到 0.3；太淡→回 0.7）。

## 下一步 backlog（下个 session 逐步做，按优先）
1. ⏪ **启动预载整个地球低 LOD 瓦片**（**已被回退、不在 master**；以下为当初实现细节，作重做参考）：`earth_main.cpp` 新增 `prefetchLowLODGlobe(maxZ)` + main 里 detach 后台线程；4 worker 拉 z0..maxZ 全球 **底图+标注+高程** 三层进磁盘缓存。`EARTH_PREFETCH` 控制最大 zoom（默认 4，0 关闭）。配套把 `loadFileData` 缓存写改成 **temp+rename 原子替换**（`UtilitiesEx.cpp`，并发安全）。细节见 `tasks/lessons.md`「启动预热」节。
2. **完整异步层加载**（消除首访顿挫）：`TileCallback::operator()`(update=主线程) 在 `_layersDone=false`/`TileManager::check()` 时同步 `readImage` 阻塞(`TileCallback.cpp` updateLayerData，异步 `requestImageFile` 被注释)。改后台 thread-pool 加载、就绪帧应用。**高风险(并发)、headless 无法验 UX**——留交互会话谨慎做。（注：#3 的 `LayerLoadPool` 已回退，不存在；#2 若做需自带线程池。）
3. ⏪ **瓦片吞吐并行**（**已被回退、不在 master**；以下为重做参考）：`ReaderWriterTMS::createTile` 的 5 层从串行改成**常驻 keep-alive 线程池** `LayerLoadPool` 并行加载（`EARTH_TILE_POOL`，默认 8，0=关闭=原串行）。**不能用临时线程**（会丢 thread_local TLS keep-alive→更慢）。配套加锁两处既存无锁 reader-writer 缓存 race（`TileManager::getReaderWriter` + `ReaderWriterWeb::getReaderWriter`）、`_layerPaths` operator[]→find()。细节见 `tasks/lessons.md`「瓦片 5 层并行加载」节。
4. **中国 GCJ-02 偏移**(已知限制)：中国区 `lyrs=h` 标注是 GCJ-02、卫星 `lyrs=s` 是 WGS-84，偏 ~50-700m。可在标注层 UV 做顶点级 GCJ-02→WGS-84 校正(仅中国、近似)。用户说"实在不行就算了"。
5. 之后按 spec：P2 RainViewer 雷达 → USGS 地震 / OpenSky 航班(点要素=独立 billboard 子图+后台拉取线程)。
6. **✅ 低空画面发暗/发蓝(2026-06-21,已修)**：高度下降时低空发暗"像黑夜"。**真因=曝光**(用户隔离测试:取消海洋无变化、拉曝光会变亮、real-time 太阳关着)——高空靠 inscatter 撑亮、低空 inscatter 消失只剩偏低 `HdrExposure=0.25`。单一曝光顾不了两头(低空要≥1、高空 1 就过爆)→ **自适应曝光(`b2377bda`)**:随相机高度设 `HdrExposure`(低空 1.0/高空 0.25/80-4000km 过渡),纯 C++、着色器不动。env `EARTH_EXP_LO/HI/ALTLO/ALTHI` 可调、`自适应曝光 Auto` 复选框。**残留**:掠射(El=0)低空仍"灰灰的"=暮色本质,曝光只提亮压不掉,用户暂可接受(想更鲜需调高 `EARTH_EXP_LO` 或太阳抬高)。**早先把它归成"散射管线雷区/sun-overhead 仍暗"是误判**(sun-overhead 其实不暗,是斜射+低曝光)。详见 [[earth-low-altitude-dark-atmosphere]]。
7. **~~近平面最小距离~~(已查清=不必做)**：实测 `_minDistance`/`clampDistance` 是**死代码**(钳的是眼到地心、永不触发),`--goto`/缩放都不受它限制;spec/lessons 说"50→150 修近平面"**是错的**,那个无效 commit 已 `git reset` 回退。真正防穿靠**相机地形地板** `_terrainLift`(b5976ac7),已生效。**残留=昆明/旧金山最高峰贴最近时"很轻微"穿模,用户决定先不修。** 详见记忆 [[earth-near-plane-mindistance-dead-and-input-freeze]]。
8. **偶发输入失灵死锁(未解)**：低空(尤其飞到冷僻新区如太平洋)+ 拖拽/倾斜探一阵后,**鼠标对地球操作全失灵**(Go To/ImGui 仍可用、能 home 到几万公里),macOS 彩球但菜单可动,**Home 救不回、重启可恢复**(偶发)。已证伪:非 `_minDistance`/非 NaN 顶点(无尖刺)/非地形地板每帧求交性能(高空仍失灵)/非纯同步加载。**最大嫌疑=ImGui 粘住捕获鼠标(WantCaptureMouse),未证实。** 已加诊断钩子 `EARTH_INPUT_DEBUG=1`(写 `/tmp/earth_input.log`)待下次复现定性。附:`updateTerrainFloor` 在 <30km **每帧全场景求交**是真实性能隐患(日后优化)。详见记忆 [[earth-near-plane-mindistance-dead-and-input-freeze]]。
9. **掠射太阳橙块(2026-06-21,用户暂搁置)**：太阳掠射(El≈0)时**海洋上出现大片橙红、边缘呈直线(瓦片形状)、随视角转动出现/消失**。决定性排除:`EARTH_SUN_TO_CAMERA=1` 全亮下橙**消失** → 是**掠射着色**,非纹理/影像 bug(那个红棕≈Terrarium 高程色是巧合,不是它)。**headless 海洋默认关(`OceanOpaque=0`)永远复现不出** → **强烈怀疑=程序化海洋(`OceanOpaque`,用户「海洋」勾着)掠射反射橙天空,直边=海洋按瓦片渲染**;未最终证实。**决定性测试(下次)**:取消「海洋 Ocean」看橙是否消失;若是→修海洋掠射反射或让它默认关。**教训**:本次错往散射/纹理找、反复 headless 复现失败,根因是漏了"用户海洋开、headless 海洋关"这个差异;我那些散射去饱和尝试**全已撤回原版**(`.app`/build/源 三处 = git HEAD)。见记忆 [[earth-orange-ocean-rootcause]]。
- **其它已知限制**：headless 截图 `EARTH_AUTOCAP`≤~280（交互不受影响）；`gmtime_r` 仅 POSIX（Windows 需适配，既有模式）。

## 极地处理：v0.9 去截断造成空洞 → v0.9.1 撤回，保留星状（2026-06-18）
**最终结论**：全球/中纬贴图本来就正确（早先以为不正确是误诊）。极点 v0.9 试过"去截断"反而留出敞开极冠盘/空洞、像球被整体变形，用户否决；**v0.9.1 已恢复截断**，回到星状极点（更顺眼、轮廓正常），星芒作为已知限制保留。

**早先误诊（别再犯）**：commit `50f73d4c` 以为"瓦片内部顶点按纬度线性插值、要在 `computeTileExtent` 做墨卡托反投影"，照此改反而把整张贴图搞扭，已回退（`47e0072e`）。

**重新定位的真相（证据驱动）**：
1. **顶点级墨卡托反投影早就正确**，藏在 `TileCallback::adjustLatitudeLongitudeAltitude`（TileCallback.cpp ~160）：extent‑Y 本身线性于墨卡托 Y（`inDegrees(extentY)=mercY/2`），adjust 里 `atan(sinh(mercY)) ≡ 2·atan(eᵐ)−π/2`（Gudermannian），所以每顶点纬度已是精确反投影。45°N 低 zoom（全欧洲）海岸线比例正确即证。**不需要改 computeTileExtent，重做是空操作。**
2. **极点星芒真因 = `convertLLAtoECEF`（Math.cpp:430）的 85.05° 硬截断**：最顶瓦片顶边 `atan(sinh(π))=85.0511°` 刚好略超阈值 → 整顶行 16 顶点 snap 到同一极点 → 扇形坍缩成星。
   - **v0.9 试过删截断**：星芒消失但极冠敞开成盘/空洞（裙边/海洋面填充、轮廓鼓出），用户认为比星状更差。
   - **v0.9.1 撤回，恢复截断**：保留星状极点（收敛点，轮廓正常）。<85° 区域三种状态都无回归（截断只在 >85.05° 触发）。
3. **遗留（已知限制）**：Web Mercator 无 >85.05° 瓦片数据，极冠无法贴图。当前取"星状收敛点"。真正干净做法 = 主动在 >85° 填平极冠（冰白 disc），属新增几何、风险高，留作后续单独立项。

**验证铁律**：极地/投影类改动必须看**低 zoom 中纬度陆地**（`--goto 45 12 6000` 全欧洲，海岸线比例）+ 极点正上方（`--goto 89.5 0 5000`）；别只看高 zoom（单瓦片跨度极小，gd 非线性可忽略，看不出问题）。

## 下一步建议（给新会话）
1. ✅ 回退已确认正常（全球/北京 40°/欧洲 45° 低 zoom/斯瓦尔巴 78° 全部无扭曲）。
2. ✅ 极点：v0.9 去截断的空洞已撤回，v0.9.1 恢复星状（用户偏好）。极冠 >85° 作为已知限制。
3. ✅ 已合并 master、打了 v0.8/v0.9 并推送。v0.9.1 = 撤回极地去截断的小修版（需重打包 .app + 推送 tag）。
4. 用户**单独立项、本轮未做**的需求：**真实感 3D 瓦片**（Google Photorealistic 3D Tiles / OGC 3D Tiles 网格，需 Google Cloud API key，另一条管线）——单独 brainstorm + 计划。

## 已完成的全部能力（截至 v0.8 + P0/P1/perf，均在 master；6-19/6-20 深瓦片线已回退）
- 在线 Google 混合影像(lyrs=y) + AWS Terrarium 高程，多级流式到街道级(z19)
- ImGui 面板：经纬度/海拔读数、太阳方位/高度、**真实时间太阳**、海洋、曝光、**大气强度**、跳转、书签、退出
- 瓦片磁盘缓存(`~/.osgverse_earth_cache`，env `EARTH_TILE_CACHE`)
- 相机最底点 50m（不穿地表）
- 滚轮缩放(macOS SCROLL_2D)、Go To 俯视、`--goto` 命令行
- 可双击 `dist/EarthExplorer.app`（`packaging/package_macos.sh` 打包，`packaging/run.sh` 命令行）

全部移植/实现细节见 `tasks/lessons.md`。
