# 2026-07-02 — 香港 f2 实景三维正式图层 + EARTH_BASEMAP 底图钩子

用户决策:"都做"(① f2 正式接入为可开关图层;② 底图可换源缓解"地面扭曲"观感)。

## 实现

### ① f2 正式图层(懒加载 + UI 开关 + 署名)

- `tiles3d_data.{h,cpp}`:新增 `Tiles3DLayer` 抽象(仿 `QuakeLayer` 模式)。
  - 默认数据源 = 地政总署 3D Visualisation Map `https://data.map.gov.hk/api/3d-data/3dtiles/f2/tileset.json`(实测免 key;官方要求署名)。
  - **懒加载**:构造时不联网(NodeMask=0);首次开启才在**后台 std::thread** 拉根 tileset(同步网络秒级,不能放主线程),经 `_pending` 槽由 update 回调(主线程)挂树;之后开关只切 NodeMask(0 = update/cull 均不遍历 → 关闭即停,不发瓦片请求)。
  - 线程保活:lambda 捕获 `osg::ref_ptr<Tiles3DLayerImpl>`;impl 即 group 的 UpdateCallback,防环引用用 `observer_ptr<Group>`。
- `earth_main.cpp`:注册 OverlayLayer `hk3d`,新 UI 组「三维城市 / 3D City」,默认关;
  subtitle = `© 香港特区政府地政总署 data.map.gov.hk`(满足署名条款,常显在开关下方)。
  - `EARTH_3DTILES` 语义:`=1` 默认源随启动开启;`=<url>` 换源+开启;`=0` 显式关;未设 → UI 勾选触发。
- `LayerManager.h` + `EarthControlUI.h`:OverlayLayer 加通用 `subtitle` 字段,UI 以 `TextDisabled` 渲染。

### ② EARTH_BASEMAP 底图钩子

`earth_main.cpp::createCustomPath` ORTHOPHOTO 分支:`EARTH_BASEMAP=google(默认)/esri/自定义模板`。
Esri World Imagery = `https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}`。
启动读一次(static lambda);预热与渲染同走此函数,自然同源。

## 验证(headless,均 dangerouslyDisableSandbox 绕沙箱写 /tmp)

- **f2 渲染确认**:`EARTH_3DTILES=1 --goto 22.298 114.1722 0.6`,截图可见尖沙咀成片带真实纹理的规整建筑 + UI 出现「三维城市 / 3D City」组。7000 帧长跑后海滨细化到街道级清晰。
- **"地面扭曲"最终定性(两层原因)**:
  1. 主因 = **f2 自身粗 LOD 摄影测量网格**(ContextCapture 粗层 = 融化蜡像观感,贴图拉成白色条纹),流式细化跟上后变清晰 → 渐进细化中间态,Google Earth 同款行为,非 bug;
  2. 次因 = Google/Esri 正射影像在香港高密度区掺倾斜航拍,楼体烘焙进地面(curl 原始瓦片验证,两家都有,Esri 略轻)。
- **零影响**:默认(无 env)运行 0 次 data.map.gov.hk 请求、无图层日志(INFO 级以下)。
- **回归**:全球远景(晨昏线/星空/海色正常)+ 北极 89° 闭合星形极点(用户偏好)均通过。
- **Esri 钩子**:同视角出图,影像源确实切换(色调/拍摄期不同)。

## 已知事项 / 未闭环

- 粗 LOD"融化"观感:插件 `_maxScreenSpaceError` 写死 16,无调节钩子;如需更激进细化需给插件加 Options 支持(未做,涉共享插件)。
- 早前"headless 同视角开关 f2 diff 仅 0.02% 但有 1413 次 KTX 转码"的矛盾未完全解释(疑与当时二进制未换干净的构建 gremlin 有关——后期同路径测试均正常可见);不阻塞。
- `EARTH_TILT` 与 goto 动画时序冲突,低空斜视角 headless 摆不出来(相机会飘到海上/看天),验证斜视角请真机拖动。
- dist/EarthExplorer.app 未重打包(dist 目录当前为空)。
