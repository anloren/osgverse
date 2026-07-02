# EarthExplorer AI Chat 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 EarthExplorer 中加入 AI Chat:自然语言驱动相机/图层/数据查询+右上角图表,叠加生成式媒体(视角照片、首尾帧巡航视频),Gemini API,ToolRegistry 可扩展架构。

**Architecture:** 一切能力皆 Tool(name/描述/schema/execute),AIChatCore 是通用 Gemini function-calling 代理循环,不认识任何具体工具;网络在后台线程(libhv 同步 API),工具执行经队列回主线程(FRAME handler);生成式媒体走 JobManager 异步任务。spec 见 `docs/superpowers/specs/2026-07-02-earth-ai-chat-design.md`。

**Tech Stack:** C++11/OSG/ImGui(仓库自带)、libhv(`3rdparty/libhv/all/client/requests.h`,quake_data 同款)、picojson、Gemini REST v1beta(generateContent / flash-image / Veo predictLongRunning)。

**约定(全计划通用):**
- 构建:`cmake --build /Users/franklee/osgverse/build/verse_core --target install --config Release`(产物在 `build/sdk_core/bin`)。
- headless 验证跑法与既有一致:`DYLD_LIBRARY_PATH=.../build/sdk_core/lib OSG_NOTIFY_LEVEL=NOTICE EARTH_AUTOCAP=400 ./osgVerse_EarthExplorer --no-wait --resolution 800 600`(沙箱 Bash 需 `dangerouslyDisableSandbox` 才能写 /tmp 截图,仅日志断言则不需要)。
- 单元测试:新增 `tests/ai_chat_tests.cpp`(`NEW_SIMPLE_EXECUTABLE` 宏),纯逻辑零窗口零网络,exit code 非 0 即失败。
- 提交信息用 `feat(earth-ai): ...` 前缀;每个 Task 至少一个 commit。
- **红线**:不碰 globe 着色器/太阳/海洋;无 `EARTH_AI_KEY` 且无 `EARTH_AI_FAKE` 时应用行为与现状完全一致。

## 文件结构(职责锁定)

| 文件 | 职责 |
|---|---|
| Create `applications/earth_explorer/ai_tools.h` | Tool/ToolRegistry/AIJob/JobManager 纯头文件抽象(无 OSG 依赖,可单测) |
| Create `applications/earth_explorer/ai_chat.h/.cpp` | LLMProvider 接口、GeminiProvider、FakeProvider(EARTH_AI_FAKE)、AIChatCore 代理循环 |
| Create `applications/earth_explorer/ai_media.h/.cpp` | 快照抓帧、GeminiMediaProvider(生图/Veo 视频)、generate_photo/generate_video 工具与 Job |
| Create `applications/earth_explorer/ai_ui.h/.cpp` | 底部输入条+对话历史;右上角卡片(图表 ImDrawList 手绘/照片/Job 进度) |
| Modify `applications/earth_explorer/earth_main.cpp` | 注册工具(fly_to/set_layer/get_view_state/数据查询)、实例化 Core、FRAME drain |
| Modify `applications/earth_explorer/EarthControlUI.h` | `runInternal` 末尾调用 AI UI 绘制 |
| Modify `applications/earth_explorer/quake_data.h/.cpp`、`flight_data.h/.cpp` | 增加只读 summary 查询接口 |
| Modify `applications/earth_explorer/CMakeLists.txt` | 新增源文件 |
| Modify `tests/CMakeLists.txt` + Create `tests/ai_chat_tests.cpp` | 单测可执行 |

---

### Task 1: ai_tools.h —— ToolRegistry 与 Job 抽象(纯逻辑,单测先行)

**Files:**
- Create: `applications/earth_explorer/ai_tools.h`
- Create: `tests/ai_chat_tests.cpp`
- Modify: `tests/CMakeLists.txt`(仿既有行追加 `NEW_SIMPLE_EXECUTABLE(osgVerse_Test_Ai_Chat FALSE ai_chat_tests.cpp)`;先看文件里现有调用行照抄格式)

- [ ] **Step 1: 写失败测试**

```cpp
// tests/ai_chat_tests.cpp
#include <iostream>
#include <cassert>
#include "../applications/earth_explorer/ai_tools.h"
#include <picojson.h>

static picojson::value parse(const std::string& s)
{ picojson::value v; picojson::parse(v, s); return v; }

int main(int, char**)
{
    using namespace earthai;
    ToolRegistry reg;
    Tool t; t.name = "fly_to"; t.description = u8"飞到指定经纬度";
    t.parametersJson =
        "{\"type\":\"object\",\"properties\":{"
        "\"lat\":{\"type\":\"number\"},\"lon\":{\"type\":\"number\"},"
        "\"alt_km\":{\"type\":\"number\"}},\"required\":[\"lat\",\"lon\"]}";
    bool called = false; double gotLat = 0.0;
    t.execute = [&](const picojson::value& args) {
        called = true; gotLat = args.get("lat").get<double>();
        return parse("{\"ok\":true}");
    };
    reg.add(t);

    // 1) declarations JSON 覆盖 name/description/parameters
    std::string decls = reg.buildDeclarationsJson();
    picojson::value dv; std::string err = picojson::parse(dv, decls);
    assert(err.empty());
    picojson::array& arr = dv.get<picojson::array>();
    assert(arr.size() == 1);
    assert(arr[0].get("name").to_str() == "fly_to");
    assert(arr[0].get("parameters").get("required").get<picojson::array>().size() == 2);

    // 2) dispatch:按名执行,拿到结果
    picojson::value result;
    bool ok = reg.dispatch("fly_to", parse("{\"lat\":40.7,\"lon\":-74.0}"), result);
    assert(ok && called && gotLat > 40.0 && result.get("ok").get<bool>());

    // 3) 未知工具:dispatch 返回 false,result 带 error 字段
    ok = reg.dispatch("nope", parse("{}"), result);
    assert(!ok && result.contains("error"));

    // 4) JobManager:创建→更新→查询
    JobManager jm;
    int id = jm.create("video", u8"生成巡航视频");
    jm.update(id, AIJob::RUNNING, 0.5f, "", "");
    AIJob snap; assert(jm.get(id, snap) && snap.status == AIJob::RUNNING && snap.progress > 0.4f);
    std::cout << "ai_tools tests OK\n"; return 0;
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: 在 `tests/CMakeLists.txt` 加目标后构建 → 预期 **编译失败**(ai_tools.h 不存在)。

- [ ] **Step 3: 最小实现 ai_tools.h**

```cpp
#ifndef EARTH_AI_TOOLS_H
#define EARTH_AI_TOOLS_H
// AI Chat 核心抽象:一切能力皆 Tool。本头文件不依赖 OSG,可被 tests/ 单测。
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <picojson.h>

namespace earthai
{
    struct Tool
    {
        std::string name, description;
        std::string parametersJson;   // JSON schema(object)字符串,直接嵌入 declarations
        // 主线程执行(由 AIChatCore 的 FRAME drain 保证);返回值回传给模型
        std::function<picojson::value(const picojson::value& args)> execute;
    };

    class ToolRegistry
    {
    public:
        void add(const Tool& t) { _tools.push_back(t); }
        const std::vector<Tool>& tools() const { return _tools; }

        // Gemini functionDeclarations 数组 JSON
        std::string buildDeclarationsJson() const
        {
            std::string s = "[";
            for (size_t i = 0; i < _tools.size(); ++i)
            {
                const Tool& t = _tools[i];
                picojson::object o;
                o["name"] = picojson::value(t.name);
                o["description"] = picojson::value(t.description);
                s += picojson::value(o).serialize();
                // parameters 是现成 JSON 字符串,拼接进对象(避免二次解析)
                s.insert(s.size() - 1, ",\"parameters\":" + t.parametersJson);
                if (i + 1 < _tools.size()) s += ",";
            }
            return s + "]";
        }

        bool dispatch(const std::string& name, const picojson::value& args, picojson::value& result)
        {
            for (size_t i = 0; i < _tools.size(); ++i)
                if (_tools[i].name == name) { result = _tools[i].execute(args); return true; }
            picojson::object err; err["error"] = picojson::value("unknown tool: " + name);
            result = picojson::value(err); return false;
        }
    private:
        std::vector<Tool> _tools;
    };

    struct AIJob
    {
        enum Status { PENDING = 0, RUNNING, DONE, FAILED };
        int id = 0; std::string kind, title;
        Status status = PENDING; float progress = 0.0f;
        std::string resultPath, error;
    };

    class JobManager   // 全部方法线程安全(生成任务在后台线程更新,UI 主线程读)
    {
    public:
        int create(const std::string& kind, const std::string& title)
        {
            std::lock_guard<std::mutex> g(_mutex);
            AIJob j; j.id = ++_nextId; j.kind = kind; j.title = title;
            _jobs[j.id] = j; return j.id;
        }
        void update(int id, AIJob::Status st, float progress,
                    const std::string& resultPath, const std::string& error)
        {
            std::lock_guard<std::mutex> g(_mutex);
            std::map<int, AIJob>::iterator it = _jobs.find(id); if (it == _jobs.end()) return;
            it->second.status = st; it->second.progress = progress;
            if (!resultPath.empty()) it->second.resultPath = resultPath;
            if (!error.empty()) it->second.error = error;
        }
        bool get(int id, AIJob& out) const
        {
            std::lock_guard<std::mutex> g(_mutex);
            std::map<int, AIJob>::const_iterator it = _jobs.find(id);
            if (it == _jobs.end()) return false; out = it->second; return true;
        }
        std::vector<AIJob> list() const
        {
            std::lock_guard<std::mutex> g(_mutex);
            std::vector<AIJob> v; for (std::map<int, AIJob>::const_iterator it = _jobs.begin();
                                        it != _jobs.end(); ++it) v.push_back(it->second);
            return v;
        }
    private:
        mutable std::mutex _mutex; std::map<int, AIJob> _jobs; int _nextId = 0;
    };
}
#endif
```

- [ ] **Step 4: 构建并跑测试,确认通过**

Run: 构建后 `DYLD_LIBRARY_PATH=.../lib ./build/sdk_core/bin/osgVerse_Test_Ai_Chat`
Expected: 输出 `ai_tools tests OK`,exit 0。

- [ ] **Step 5: Commit** `feat(earth-ai): tool registry and job manager abstractions with tests`

---

### Task 2: AIChatCore 代理循环 + FakeProvider(可完全离线单测)

**Files:**
- Create: `applications/earth_explorer/ai_chat.h`、`ai_chat.cpp`
- Modify: `applications/earth_explorer/CMakeLists.txt`(把 ai_chat.cpp 加入源列表;照抄 quake_data.cpp 所在行格式)
- Modify: `tests/ai_chat_tests.cpp`(追加用例)、`tests/CMakeLists.txt` 若需链接 ai_chat.cpp(单测直接 `#include "../applications/earth_explorer/ai_chat.cpp"` 最简,零改链接)

**接口(ai_chat.h):**

```cpp
#ifndef EARTH_AI_CHAT_H
#define EARTH_AI_CHAT_H
#include "ai_tools.h"

namespace earthai
{
    // 一轮模型输出:要么纯文本,要么一批函数调用
    struct FunctionCall { std::string name; picojson::value args; };
    struct LLMTurn { std::string text; std::vector<FunctionCall> calls; std::string error; };

    class LLMProvider
    {
    public:
        virtual ~LLMProvider() {}
        // contentsJson: Gemini contents 数组(序列化字符串);declsJson: functionDeclarations 数组。
        // 同步调用,由 AIChatCore 的工作线程执行,不得在主线程调。
        virtual LLMTurn chat(const std::string& contentsJson, const std::string& declsJson) = 0;
    };

    // EARTH_AI_FAKE=<json文件>:数组,按序出队,每项 {"text":...} 或 {"calls":[{name,args}]}
    class FakeProvider : public LLMProvider { /* 读文件,逐轮弹出 */ };

    struct ChatEntry { enum Kind { USER, ASSISTANT, TOOL_NOTE, ERR } kind; std::string text; };

    class AIChatCore
    {
    public:
        AIChatCore(LLMProvider* p, ToolRegistry* reg);   // 不持有 provider/registry 所有权
        void submit(const std::string& userText);        // 主线程调;若 busy 则忽略并提示
        bool busy() const;
        // 主线程每帧调:执行排队的工具调用(工具必须主线程),推进代理循环
        void drainMainThread();
        std::vector<ChatEntry> transcript() const;       // UI 读(加锁快照)
        std::string systemPrompt;                        // 中文系统提示词,earth_main 里设置
    };
}
#endif
```

**代理循环状态机(ai_chat.cpp 核心,必须按此实现):**
`submit` → 启动一次性 std::thread:构建 contents(system+历史+新输入)→ `provider->chat` →
- 返回 text → 追加 ASSISTANT entry,线程结束;
- 返回 calls → 存入 `_pendingCalls`(mutex),线程结束;
`drainMainThread`(FRAME):若有 `_pendingCalls` → 逐个 `registry->dispatch`(主线程!)→ 把 functionResponse 追加进对话 → 再启动线程做下一轮 `chat`;循环上限 6 轮防失控(超限报 ERR)。历史保留最近 10 轮(user+assistant 计数,functionCall/Response 成对保留)。

- [ ] **Step 1: 失败测试**(追加到 tests/ai_chat_tests.cppmain 中)

```cpp
    // ---- AIChatCore 代理循环(FakeProvider 脚本驱动)----
    {
        // 脚本:第1轮请求 fly_to,第2轮给最终文本
        const char* script =
            "[{\"calls\":[{\"name\":\"fly_to\",\"args\":{\"lat\":40.71,\"lon\":-74.0,\"alt_km\":50}}]},"
            " {\"text\":\"已带你飞到纽约\"}]";
        FakeProvider fp; fp.loadFromString(script);
        ToolRegistry reg2; double flownLat = 0.0;
        Tool fly; fly.name = "fly_to"; fly.description = "";
        fly.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        fly.execute = [&](const picojson::value& a)
        { flownLat = a.get("lat").get<double>(); return parse("{\"ok\":true}"); };
        reg2.add(fly);

        AIChatCore core(&fp, &reg2);
        core.submit(u8"飞到纽约");
        // 模拟主循环:轮询 drain 直到空闲(最多 5 秒)
        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();
        assert(flownLat > 40.0);                       // 工具在 drain(主线程)被执行
        std::vector<ChatEntry> ts = core.transcript();
        assert(!ts.empty() && ts.back().text == u8"已带你飞到纽约");
    }
```

- [ ] **Step 2: 跑测试确认编译失败**(FakeProvider/AIChatCore 未实现)
- [ ] **Step 3: 实现 ai_chat.cpp**(FakeProvider::loadFromString/loadFromFile + 上述状态机;Gemini contents 构造函数 `buildContentsJson(history)` 独立成自由函数,Task 3 复用)
- [ ] **Step 4: 跑测试确认通过**(`ai_tools tests OK` + 新断言全过)
- [ ] **Step 5: Commit** `feat(earth-ai): generic agent loop with scripted fake provider`

---

### Task 3: GeminiProvider(真网络,libhv)

**Files:**
- Modify: `applications/earth_explorer/ai_chat.h/.cpp`

- [ ] **Step 1: 实现 GeminiProvider**(接口已定,无法离线断言真响应;测试=Task 4 的 E2E + 手工冒烟)

```cpp
// ai_chat.cpp(关键片段;头文件声明 class GeminiProvider : public LLMProvider)
#include "3rdparty/libhv/all/client/requests.h"

LLMTurn GeminiProvider::chat(const std::string& contentsJson, const std::string& declsJson)
{
    LLMTurn turn;
    std::string body = "{\"system_instruction\":{\"parts\":[{\"text\":" +
        picojson::value(_systemPrompt).serialize() + "}]},"
        "\"contents\":" + contentsJson + ","
        "\"tools\":[{\"functionDeclarations\":" + declsJson + "}]}";
    requests::Request req(new HttpRequest);
    req->method = HTTP_POST; req->timeout = 30;
    req->url = "https://generativelanguage.googleapis.com/v1beta/models/" + _model +
               ":generateContent?key=" + _apiKey;      // key 不落日志!
    req->headers["Content-Type"] = "application/json";
    req->body = body;
    requests::Response resp = requests::request(req);
    if (!resp || resp->status_code != 200)
    { turn.error = "HTTP " + std::to_string(resp ? resp->status_code : -1) +
                   (resp ? (": " + resp->body.substr(0, 200)) : ""); return turn; }

    picojson::value v; std::string err = picojson::parse(v, resp->body);
    if (!err.empty()) { turn.error = "bad json"; return turn; }
    picojson::value& parts = v.get("candidates").get(0).get("content").get("parts");
    picojson::array& pa = parts.get<picojson::array>();
    for (size_t i = 0; i < pa.size(); ++i)
    {
        if (pa[i].contains("functionCall"))
        {
            FunctionCall fc; picojson::value& f = pa[i].get("functionCall");
            fc.name = f.get("name").to_str(); fc.args = f.get("args");
            turn.calls.push_back(fc);
        }
        else if (pa[i].contains("text")) turn.text += pa[i].get("text").to_str();
    }
    return turn;
}
```

模型回参构造(在 AIChatCore 里,两个 provider 共用):functionResponse 以
`{"role":"user","parts":[{"functionResponse":{"name":N,"response":R}}]}` 追加(v1beta 约定)。

- [ ] **Step 2: 手工冒烟(可选,需 key)**

```bash
EARTH_AI_KEY=<key> curl -s "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=$EARTH_AI_KEY" \
  -H 'Content-Type: application/json' -d '{"contents":[{"role":"user","parts":[{"text":"hi"}]}]}' | head -c 300
```
Expected: JSON 含 candidates。

- [ ] **Step 3: 构建全绿 + Commit** `feat(earth-ai): gemini rest provider`

---

### Task 4: earth_main 接线 —— fly_to/set_layer/get_view_state + FRAME drain + E2E

**Files:**
- Modify: `applications/earth_explorer/earth_main.cpp`(main 中、imgui 初始化之前)
- Create: `applications/earth_explorer/test/ai_fake_flyto.json`(E2E 脚本 fixture)

- [ ] **Step 1: 写 E2E fixture(即失败测试)**

```json
[{"calls":[{"name":"fly_to","args":{"lat":40.7128,"lon":-74.006,"alt_km":80}}]},
 {"text":"已飞抵纽约上空"}]
```

- [ ] **Step 2: 跑 E2E 确认失败**

```bash
EARTH_AI_FAKE=applications/earth_explorer/test/ai_fake_flyto.json EARTH_AI_AUTOSUBMIT="飞到纽约" \
  DYLD_LIBRARY_PATH=... EARTH_AUTOCAP=300 ./osgVerse_EarthExplorer --no-wait --resolution 640 480 2>&1 | grep "\[AIChat\]"
```
Expected(现在):无输出(未接线)。

- [ ] **Step 3: 接线实现**(要点,完整写入 main)

```cpp
    // ---- AI Chat(spec: docs/superpowers/specs/2026-07-02-earth-ai-chat-design.md)----
    // 无 EARTH_AI_KEY 且无 EARTH_AI_FAKE → aiCore 为空,后续全部跳过,零影响。
    earthai::ToolRegistry* aiRegistry = new earthai::ToolRegistry;
    earthai::AIChatCore* aiCore = nullptr;
    {
        earthai::Tool fly; fly.name = "fly_to";
        fly.description = u8"把相机飞到指定经纬度与高度。用户说地名时你自己换算经纬度。";
        fly.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"lat\":{\"type\":\"number\"},\"lon\":{\"type\":\"number\"},"
            "\"alt_km\":{\"type\":\"number\",\"description\":\"default 50\"}},"
            "\"required\":[\"lat\",\"lon\"]}";
        osgVerse::EarthManipulator* mani = earthManipulator.get();
        fly.execute = [mani](const picojson::value& a) {
            double lat = a.get("lat").get<double>(), lon = a.get("lon").get<double>();
            double alt = a.contains("alt_km") ? a.get("alt_km").get<double>() : 50.0;
            mani->setByEye(osg::inDegrees(lat), osg::inDegrees(lon), alt * 1000.0);
            OSG_NOTICE << "[AIChat] fly_to " << lat << "," << lon << "," << alt << "km" << std::endl;
            picojson::object r; r["ok"] = picojson::value(true); return picojson::value(r);
        };
        aiRegistry->add(fly);
        // set_layer / get_view_state 同模式:set_layer 用 lmptr->setEnabled(id,on)+opacity;
        // get_view_state 返回相机 LLA(EarthManipulator 或 UI 已有换算)+ 各层 enabled 表。
        //(两个工具的 execute 均 ~10 行,声明照 fly_to 格式写全)
    }
    const char* aiKey = getenv("EARTH_AI_KEY");
    const char* aiFake = getenv("EARTH_AI_FAKE");
    if (aiFake && *aiFake)
    { earthai::FakeProvider* fp = new earthai::FakeProvider; fp->loadFromFile(aiFake);
      aiCore = new earthai::AIChatCore(fp, aiRegistry); }
    else if (aiKey && *aiKey)
    { const char* m = getenv("EARTH_AI_MODEL");
      aiCore = new earthai::AIChatCore(
          new earthai::GeminiProvider(aiKey, m && *m ? m : "gemini-2.5-flash"), aiRegistry); }
    if (aiCore)
    {
        aiCore->systemPrompt = u8"你是 EarthExplorer 三维地球的中文助手…(工具优先,回答简洁)";
        // FRAME drain(与降水层同模式的事件处理器)
        class AIFrameHandler : public osgGA::GUIEventHandler {
            earthai::AIChatCore* _core;
        public: AIFrameHandler(earthai::AIChatCore* c) : _core(c) {}
            virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
            { if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME) _core->drainMainThread();
              return false; }
        };
        viewer.addEventHandler(new AIFrameHandler(aiCore));
        const char* autoSubmit = getenv("EARTH_AI_AUTOSUBMIT");   // headless E2E 用
        if (autoSubmit && *autoSubmit) aiCore->submit(autoSubmit);
    }
```

- [ ] **Step 4: 跑 Step 2 命令确认通过**
Expected: 日志有 `[AIChat] fly_to 40.7128,-74.006,80km`;截图相机纬度≈40.7(UI Camera 行)。
- [ ] **Step 5: 无钩子回归**:不设 EARTH_AI_*,跑 300 帧,日志无 `[AIChat]`,退出 0。
- [ ] **Step 6: Commit** `feat(earth-ai): wire registry+core into app, fly_to/set_layer/view_state`

---

### Task 5: 数据查询工具(quakes/flights summary)

**Files:**
- Modify: `quake_data.h/.cpp`(QuakeLayer 增 `virtual std::string summaryJson() const = 0;` 实现:总数/最大震级/按震级分桶/最近 24h;**若层未开启**,execute 里先 `setEnabled(true)` 拉数据并在结果注明"已自动开启地震层")
- Modify: `flight_data.h/.cpp`(同理:当前视口架次/高度分桶/最快)
- Modify: `earth_main.cpp`(注册 `get_quakes_summary`/`get_flights_summary` 两个 Tool,execute 直接返回对应 summaryJson 解析后的 picojson)

- [ ] **Step 1: fixture E2E 失败测试**:`test/ai_fake_quakes.json` 脚本第 1 轮调 `get_quakes_summary`,第 2 轮 text;配合既有 `EARTH_QUAKES_FILE` 地震 fixture(仓库已有)断言日志 `[AIChat] get_quakes_summary count=N (N>0)`。
- [ ] **Step 2: 实现两个 summary + 注册**(summaryJson 在主线程 drain 中调用——QuakeLayerImpl::quakes() 本就主线程只读,安全)
- [ ] **Step 3: E2E 通过 + 无钩子回归**
- [ ] **Step 4: Commit** `feat(earth-ai): quake/flight data summary tools`

---

### Task 6: ai_ui —— 底部输入条 + 对话历史

**Files:**
- Create: `applications/earth_explorer/ai_ui.h/.cpp`(`class AIChatUI { void draw(AIChatCore*, JobManager*, int winW, int winH); }`)
- Modify: `EarthControlUI.h`(成员 `AIChatUI* _aiUI = nullptr; AIChatCore* _aiCore = nullptr;`,`runInternal` 末尾 `if (_aiUI && _aiCore) _aiUI->draw(...)`)
- Modify: `earth_main.cpp`(`ctrlUI->_aiCore = aiCore; ctrlUI->_aiUI = new AIChatUI;`)
- Modify: `CMakeLists.txt`

**绘制规格**(遵守既定 UI 分区:不占左上/右上):
- 底部居中 `ImGui::SetNextWindowPos({winW*0.5f, winH - 16.0f}, ImGuiCond_Always, {0.5f, 1.0f})`,无标题栏、半透明圆角;宽 `min(720, winW*0.6)`。
- 一行:`InputText`(回车发送,busy 时禁用)+ 📷 + 🎬 按钮(本 Task 先画灰占位,Task 8/9 接线)。
- 历史:输入条上方子窗口,高 ≤ winH*0.3,可折叠(默认展开最近 6 条);USER 右对齐蓝、ASSISTANT 左对齐白、ERR 红;busy 时最后一行画 spinner(`|/-\` 字符轮转即可)。
- 无 aiCore(未配 key):画禁用输入框,占位文本"设置 EARTH_AI_KEY 启用 AI 对话"。

- [ ] **Step 1: 实现绘制**(纯 UI,无逻辑测试;编译过 + 视觉验收)
- [ ] **Step 2: 视觉验证**:`EARTH_AI_FAKE=...flyto.json` 交互跑真机(.app),输入"飞到纽约"回车 → 相机飞走、历史区出现问答;无 key 跑一次看灰显提示。截图留档 /tmp/ai_ui_smoke.png。
- [ ] **Step 3: Commit** `feat(earth-ai): bottom chat bar UI`

---

### Task 7: show_chart 工具 + 右上角图表卡

**Files:**
- Modify: `ai_ui.h/.cpp`(卡片系统:`struct AICard { enum {CHART, PHOTO, JOB} type; picojson::value spec; std::string path; int jobId; bool open; }`,`std::vector<AICard>` 由 AIChatUI 持有;右上角锚定 `SetNextWindowPos({winW-16, 16}, Always, {1,0})` 向下堆叠,每张卡独立 Begin + [x] 关闭——与地震详情卡同风格)
- Modify: `earth_main.cpp`(注册 `show_chart` Tool:execute 把 spec 塞给 AIChatUI 的卡片队列——通过 `AIChatUI::pushChart(spec)`;主线程安全因 execute 本就在 drain)

**图表手绘规格(ImDrawList,全部深色圆角风格):**
- `bar`:横向圆角条 + 数值右标,accent 色 `IM_COL32(90,170,255,255)`;
- `donut`:`PathArcTo` 分段环 + 中心大数字;
- `line`:`AddPolyline` + 渐变填充(`AddConvexPolyFilled` 近似);
- `stat`:大数字 + 小标题卡。
- spec 字段:`{type,title,labels[],values[]}`,labels/values 长度不符时取 min,空数据画"无数据"。

- [ ] **Step 1: E2E fixture 失败测试**:`test/ai_fake_chart.json`(第 1 轮 `get_quakes_summary`,第 2 轮 `show_chart` bar spec,第 3 轮 text);断言日志 `[AIChat] show_chart type=bar items=5`。
- [ ] **Step 2: 实现卡片与四种图 + 注册工具**
- [ ] **Step 3: E2E 日志断言过 + 真机视觉验收**(fixture 驱动,右上角出柱状图,[x] 可关,不遮地震详情卡——两者同列向下堆)
- [ ] **Step 4: Commit** `feat(earth-ai): show_chart tool with hand-drawn top-right chart cards`

---

### Task 8: 快照管线 + generate_photo(生图)

**Files:**
- Create: `applications/earth_explorer/ai_media.h/.cpp`
- Modify: `earth_main.cpp`(注册 generate_photo;把 viewer/相机指针交给 ai_media)
- Modify: `ai_ui.cpp`(📷 按钮接线 = `aiCore->submit(u8"生成一张当前视角的实拍照片")`;PHOTO 卡:显示保存路径 + 「打开」按钮 `system("open '<path>'")`)

**快照实现(复用 EARTH_AUTOCAP 同款 ScreenCaptureHandler):**

```cpp
// ai_media.h 关键接口
namespace earthai
{
    // 抓下一帧到 PNG 文件(异步:handler 在渲染线程写盘,done 回调在之后的 FRAME drain 里查询)
    class SnapshotGrabber
    {
    public:
        SnapshotGrabber(osgViewer::Viewer* v);
        void grab(const std::string& pngPath);              // 触发一次抓帧
        bool ready(const std::string& pngPath) const;       // 文件已写完(存在且 size 稳定)
    };
    class GeminiMediaProvider
    {
    public:
        GeminiMediaProvider(const std::string& key);
        // 同步,工作线程调用。imagePng 为文件内容;返回生成 PNG 字节,失败返回空并填 err
        std::string generateImage(const std::string& imagePng, const std::string& prompt,
                                  std::string& err);        // gemini-2.5-flash-image
    };
}
```

generateImage 请求体:`contents.parts = [{text: prompt}, {inline_data:{mime_type:"image/png", data:<base64>}}]`,`generationConfig:{responseModalities:["TEXT","IMAGE"]}`;响应取 `candidates[0].content.parts[].inlineData.data` base64 解码。base64 编解码用 libhv 自带 `hv/base64.h`。

**generate_photo 工具执行流**(串成 Job,kind="photo"):
1. execute(主线程):建 Job(RUNNING 0.1)→ SnapshotGrabber::grab(`~/Pictures/EarthExplorer/snap_<time>.png`)→ 返回 `{status:"started", job_id}` 给模型(模型据此回复"正在生成…")。
2. drain 每帧查 ready → 就绪后起工作线程:读 PNG → 组 prompt(视角元数据 lat/lon/alt/heading + 用户风格词)→ generateImage → 存 `gen_<time>.png` → Job DONE + AIChatUI push PHOTO 卡。

- [ ] **Step 1: 快照单独失败测试**:临时钩子 `EARTH_AI_SNAPTEST=1` → 启动后第 60 帧 grab 到 /tmp/ai_snap_test.png,跑 headless 断言文件存在且 >10KB(现在:无此钩子,失败)。
- [ ] **Step 2: 实现 SnapshotGrabber + 钩子,测试过**(注意:headless 跑需 dangerouslyDisableSandbox 写 /tmp)
- [ ] **Step 3: 实现 GeminiMediaProvider + generate_photo Job 流 + 📷/卡片接线**(编译过;E2E fixture `ai_fake_photo.json` 走 FakeProvider 触发 generate_photo,但真生图需 key——fixture 模式下 MediaProvider 用 `EARTH_AI_FAKE_IMG=<png>` 直接拷贝该文件当"生成结果",全链路离线可测:断言日志 `[AIChat] photo job done -> <path>` 且文件存在)
- [ ] **Step 4: 真机 key 冒烟**(用户 key,一张维港照片,验收图质)
- [ ] **Step 5: Commit** `feat(earth-ai): view snapshot pipeline + generate_photo with job cards`
- ai_fake_photo2.json:手动二次提交验证 already-running 拒绝路径(自动化单次 AUTOSUBMIT 消费不到)

---

### Task 9: 🎬 首尾帧巡航视频(Veo)

**Files:**
- Modify: `ai_media.h/.cpp`(`generateVideoFirstLast(firstPng, lastPng, motionPrompt) → operationName` + `pollVideoOperation(name, outMp4Path, err)`;轨迹提示词构造独立纯函数 `buildMotionPrompt(A_lla, B_lla, heading)` 放 ai_media.h 顶部,**tests/ai_chat_tests.cpp 加单测**:A=尖沙咀 500m、B=中环 200m → 文本含 "southwest"/"descend")
- Modify: `ai_ui.cpp`(🎬 三态按钮:空闲「🎬」→ 已录 A「完成 B 点 ✓ / 取消 ✕」;完成 B 后弹确认 Modal(标注约 $2-6 费用)→ 提交 Job)
- Modify: `earth_main.cpp`(注册 generate_video 工具,自然语言路径同样走确认 Modal)

**Veo 调用**(模型名经 `EARTH_AI_VIDEO_MODEL` 配置,默认 `veo-3.1-generate-001`;实现首日先跑 Step 1 的 curl 探测,若字段与文档漂移按响应修正——探测命令与预期写死如下):

```bash
curl -s "https://generativelanguage.googleapis.com/v1beta/models/$MODEL:predictLongRunning?key=$EARTH_AI_KEY" \
 -H 'Content-Type: application/json' -d '{"instances":[{"prompt":"test","image":{"bytesBase64Encoded":"...","mimeType":"image/png"},"lastFrame":{"bytesBase64Encoded":"...","mimeType":"image/png"}}]}'
```
Expected: `{"name":"models/.../operations/..."}`;无权限则 403 → 🎬 灰显路径。
轮询:`GET v1beta/{operationName}?key=` 每 10s(Job progress 按 30s/60s/… 阶梯涨),`done:true` 后取 `response.generateVideoResponse.generatedSamples[0].video`(uri 或内联)下载存 `~/Pictures/EarthExplorer/tour_<time>.mp4`。

- [ ] **Step 1: buildMotionPrompt 单测(失败→实现→通过)**
- [ ] **Step 2: UI 三态 + 确认 Modal + Job 接线**(FakeProvider + `EARTH_AI_FAKE_MP4=<file>` 离线全链路:断言日志 `[AIChat] video job done -> <path>`)
- [ ] **Step 3: 真机 key 冒烟**(维港 A→B 一条 8s;无 Veo 权限则验证 403→灰显+聊天报错文案)
- [ ] **Step 4: Commit** `feat(earth-ai): first/last-frame tour video via Veo with confirm gate`

---

### Task 10: 收尾 —— 文档/回归/打包

- [ ] **Step 1: 全量回归**:①无任何 EARTH_AI_* 钩子跑 4 类历史回归(全球远景/极点/昆明倾斜/香港 f2)——AI 代码路径零激活;②`osgVerse_Test_Ai_Chat` 全绿;③三个 E2E fixture 日志断言全过。
- [ ] **Step 2: HANDOFF.md 顶部新章节**(架构图、钩子清单:EARTH_AI_KEY/MODEL/VIDEO_MODEL/FAKE/FAKE_IMG/FAKE_MP4/AUTOSUBMIT、费用提示、验收场景)+ 更新记忆。
- [ ] **Step 3: 重打包** `bash packaging/package_macos.sh`,真机双击验收两个 spec 场景(纽约/地震图表)。
- [ ] **Step 4: Commit + push**;里程碑 tag 由用户决定(默认不打)。

## Self-Review 结果

- **Spec 覆盖**:ToolRegistry(T1)/代理循环+Fake(T2)/Gemini(T3)/fly_to+set_layer+view_state(T4)/数据查询(T5)/底部输入条(T6)/图表卡(T7)/快照+生图+📷(T8)/首尾帧视频+🎬+费用确认(T9)/无 key 零影响+回归+文档(T4 Step5、T10)——spec 各节均有对应任务。时光机=快照管线+提示词,T8 自然覆盖,无需独立任务。
- **占位符扫描**:set_layer/get_view_state 执行体在 T4 以"同模式 ~10 行"带出——已给出 fly_to 完整范本与数据来源(lmptr/相机),属可执行指引而非 TBD;Veo 字段漂移风险以"探测 curl+预期输出"锁定为可执行步骤。
- **类型一致性**:Tool.parametersJson/execute、AIJob::Status、LLMTurn/FunctionCall、pushChart/AICard 各任务间名称已核对一致。
