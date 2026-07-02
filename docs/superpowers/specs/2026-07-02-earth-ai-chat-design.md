# EarthExplorer AI Chat 设计(spec)

日期:2026-07-02 · 状态:用户已确认方向 · 下一步:writing-plans 实现计划

## 目标

让整个地球可以被 AI 交互:自然语言驱动相机/图层/数据查询,叠加生成式媒体交互
(视角实拍照片、首尾帧巡航视频)。接入 Gemini API(用户提供 key)。
架构必须为"未来更多数据接口接入"留好插槽——新数据源接入成本 ≈ 注册一个工具。

## 核心架构:ToolRegistry,一切能力皆工具

```
聊天/按钮 UI ──→ AIChatCore(Gemini 代理循环,通用) ──→ ToolRegistry
                                                          ├ fly_to / set_layer / get_view_state
                                                          ├ get_quakes_summary / get_flights_summary
                                                          ├ show_chart
                                                          ├ generate_photo / generate_video(异步 Job)
                                                          └ (未来:FIRMS 火灾、AIS 船舶、天气……)
```

- **Tool** = `{ name, description, JSON 参数 schema, execute(args)→JSON }`,execute 一律主线程
  (FRAME handler 队列派发,与降水层同模式);长任务用 `executeAsync → Job`。
- **AIChatCore 通用**:把注册表翻译成 Gemini function declarations;代理循环:
  用户输入 → 模型 →(若 functionCall → 主线程执行 → functionResponse 回传,循环)→ 最终中文回答。
  核心不认识任何具体工具。**新数据源 = 新增一个 Tool 注册(约 30 行),核心/UI/提示词零改动。**
- **Provider 薄抽象**:`LLMProvider`(对话+function calling)、`MediaProvider`(生图/生视频),
  现仅 GeminiProvider 实现两者;未来换模型不动上层。
- **JobManager**:异步任务统一为 Job{id, kind, status, progress, resultPath},右上角进度卡;
  生视频等分钟级任务复用。

## v1 工具集(全部映射既有能力)

| 工具 | 映射 |
|---|---|
| `fly_to(lat,lon,alt_km,label)` | `EarthManipulator::setByEye`(模型自带地理知识,无需地理编码服务) |
| `set_layer(id,enabled,opacity)` | `LayerManager`(quakes/flights/clouds/precip/hk3d/labels) |
| `get_view_state()` | 相机经纬高朝向 + 各层开关状态 |
| `get_quakes_summary()` | 复用 QuakeLayer(层未开则后台拉一次 USGS feed);返回震级分布/最大震/区域计数 |
| `get_flights_summary()` | 复用 OpenSky 数据 |
| `show_chart(spec)` | 右上角图表卡;spec=`{type: bar/donut/line/stat, title, labels[], values[]}` |
| `generate_photo(prompt?)` | 生成式管线(下节) |
| `generate_video(...)` | 首尾帧巡航视频(下节) |

## 生成式媒体交互(v1 三个,同一条管线)

**管线基础——视角快照**:post-draw 回调抓当前 3D 渲染帧(EARTH_AUTOCAP 已有同类代码)
→ PNG(内存)+ 视角元数据(经纬高/朝向;地名不做反查,由模型按经纬度自行推断)
→ 多模态输入喂 `gemini-2.5-flash-image`:
"以此视角此地生成实拍照片"。渲染图供构图/地形轮廓,模型补真实感。

1. **📷 实景快照**:输入条旁按钮或自然语言触发 → 右上角出图 → 可保存
   (默认 `~/Pictures/EarthExplorer/`)。
2. **🎬 首尾帧巡航视频**:点 🎬 记录 A 点快照 → 用户移动相机 → 再点完成 B 点快照
   → 应用按 A→B 轨迹自动生成运动提示词("镜头自 500m 向东北掠过维港降至 200m…")
   → Veo 首尾帧生视频(异步 1-5 分钟,Job 进度卡,完成出缩略图,点击系统播放器打开 mp4)。
   **提交前必须用户确认(费用防误触)**。
3. **🕰 时光机**:同一快照管线的提示词变体("这里 1900 年/雪天/黄昏的样子"),
   自然语言触发,无独立按钮。

v2 候选(不做,列备忘):环绕短片(单图+运镜)、多书签行程预告片、风格化明信片。

## UI

- **底部居中悬浮输入条**(用户选定):半透明圆角,回车发送;对话历史向上展开、可折叠、
  限高;思考中 spinner;📷/🎬 快捷按钮挂在输入条右侧。
- **右上角卡片区**(用户既定偏好):图表卡/照片卡/视频进度卡,独立 `Begin`、右上锚定、
  [x] 可关,与地震/航班详情卡同风格。图表用 ImDrawList 手绘(深色圆角柱状/环形/折线/
  大数字统计卡),零新依赖。

## 配置与安全

- `EARTH_AI_KEY`(必须;只从环境变量读,不入库、不落日志)。
- `EARTH_AI_MODEL`(默认 `gemini-2.5-flash`);图像模型 `gemini-2.5-flash-image`、
  视频模型 Veo(key 无 Veo 权限 → 🎬 灰显,其余不受影响)。
- 无 key → 输入条灰显"未配置 AI Key",应用其余部分零影响。
- 费用知情:生图约 $0.04/张;Veo 约 $0.2-0.75/秒(8s 一条 $2-6),视频提交前确认。

## 线程与错误

- 网络(libhv POST + picojson,非流式 v1)全部后台线程;工具执行/场景改动全部主线程
  (FRAME 队列)。对话历史仅保留最近 ~10 轮。
- 网络/配额错误显示在对话里;工具执行失败回传模型自行解释;30s 超时(生视频除外,走 Job)。
- 不做(v1):流式逐字输出、语音、对话持久化。

## 测试

- `EARTH_AI_FAKE=<json文件>`:伪造模型响应,headless 验证代理循环+工具执行链,零配额消耗。
- 快照管线可 headless 验证(抓帧→文件);生成调用真机验收。
- 验收场景:①"飞到纽约"→ 相机到纽约;②"统计一下目前全球的地震信息"→ 右上角图表;
  ③ 📷 在维港出照片;④ 🎬 A→B 出视频。
- 回归红线:无 key/关闭时零影响;不碰 globe 着色器/太阳/海洋(4 类回归之外)。

## 模块清单

- `applications/earth_explorer/ai_tools.h` —— Tool/ToolRegistry/Job 接口(纯头文件抽象)
- `applications/earth_explorer/ai_chat.{h,cpp}` —— AIChatCore + GeminiProvider(LLM+媒体)
- `applications/earth_explorer/ai_media.{h,cpp}` —— 快照管线 + 生图/生视频 Job
- `applications/earth_explorer/ai_ui.{h,cpp}` —— 底部输入条 + 右上卡片(图表手绘)
- 各既有模块(quake/flight/LayerManager)提供只读查询接口,注册为 Tool
