// tests/ai_chat_tests.cpp
#include <iostream>
#include <cstdlib>
#include <condition_variable>
#include "../applications/earth_explorer/ai_tools.h"
#include <picojson.h>

// Release 构建带 -DNDEBUG 会吞掉 assert —— 用自定义 CHECK 保证断言永远生效
#define CHECK(x) do { if (!(x)) { \
    std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " #x << std::endl; \
    std::abort(); } } while (0)

// ai_chat.cpp 目前没有独立的 CMake 目标可链接进测试可执行文件(NEW_TEST 宏只接受单个源文件),
// 为了不改动测试的链接规则,这里直接 #include 实现文件把它编译进本测试的翻译单元。
#include "../applications/earth_explorer/ai_chat.cpp"

#ifdef _WIN32
#include <windows.h>
static void usleep(unsigned int usec) { Sleep(usec / 1000 == 0 ? 1 : usec / 1000); }
#else
#include <unistd.h>
#endif

static picojson::value parse(const std::string& s)
{ picojson::value v; picojson::parse(v, s); return v; }

int main(int, char**)
{
    using namespace earthai;

    // ---- ToolRegistry / JobManager(Task 1)----
    {
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

        // 第二个工具:覆盖 buildDeclarationsJson 的多元素逗号拼接路径
        Tool t2; t2.name = "set_layer"; t2.description = u8"切换图层";
        t2.parametersJson =
            "{\"type\":\"object\",\"properties\":{"
            "\"name\":{\"type\":\"string\"}},\"required\":[\"name\"]}";
        t2.execute = [&](const picojson::value&) { return parse("{\"ok\":true}"); };
        reg.add(t2);

        // 1) declarations JSON 覆盖 name/description/parameters,以及多工具的逗号拼接
        std::string decls = reg.buildDeclarationsJson();
        picojson::value dv; std::string err = picojson::parse(dv, decls);
        CHECK(err.empty());
        picojson::array& arr = dv.get<picojson::array>();
        CHECK(arr.size() == 2);
        CHECK(arr[0].get("name").to_str() == "fly_to");
        CHECK(arr[0].get("parameters").get("required").get<picojson::array>().size() == 2);
        CHECK(arr[1].get("name").to_str() == "set_layer");

        // 2) dispatch:按名执行,拿到结果
        picojson::value result;
        bool ok = reg.dispatch("fly_to", parse("{\"lat\":40.7,\"lon\":-74.0}"), result);
        CHECK(ok);
        CHECK(called);
        CHECK(gotLat > 40.0);
        CHECK(result.get("ok").get<bool>());

        // 3) 未知工具:dispatch 返回 false,result 带 error 字段
        ok = reg.dispatch("nope", parse("{}"), result);
        CHECK(!ok);
        CHECK(result.contains("error"));

        // 4) JobManager:创建→更新→查询
        JobManager jm;
        int id = jm.create("video", u8"生成巡航视频");
        jm.update(id, AIJob::RUNNING, 0.5f, "", "");
        AIJob snap;
        CHECK(jm.get(id, snap));
        CHECK(snap.status == AIJob::RUNNING);
        CHECK(snap.progress > 0.4f);
        std::cout << "ai_tools tests OK\n";
    }

    // ---- AIChatCore 代理循环(FakeProvider 脚本驱动)----
    {
        // 脚本:第1轮请求 fly_to,第2轮给最终文本
        const char* script =
            "[{\"calls\":[{\"name\":\"fly_to\",\"args\":{\"lat\":40.71,\"lon\":-74.0,\"alt_km\":50}}]},"
            " {\"text\":\"已带你飞到纽约\"}]";
        FakeProvider fp; CHECK(fp.loadFromString(script));
        ToolRegistry reg2; double flownLat = 0.0;
        Tool fly; fly.name = "fly_to"; fly.description = "";
        fly.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        fly.execute = [&](const picojson::value& a)
        { flownLat = a.get("lat").get<double>(); return parse("{\"ok\":true}"); };
        reg2.add(fly);

        AIChatCore core(&fp, &reg2);
        core.submit(u8"飞到纽约");
        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();
        CHECK(flownLat > 40.0);
        std::vector<ChatEntry> ts = core.transcript();
        CHECK(!ts.empty());
        CHECK(ts.back().text == u8"已带你飞到纽约");
        std::cout << "AIChatCore basic agent loop OK\n";
    }

    // ---- submit while busy 被礼貌忽略 ----
    {
        // 用条件变量门控的 provider:chat() 阻塞到测试显式放行,
        // 保证第二次 submit 一定发生在第一轮 worker 完成之前(无任何时间假设)。
        struct GatedProvider : public LLMProvider
        {
            std::mutex m; std::condition_variable cv; bool released = false;
            virtual LLMTurn chat(const std::string&, const std::string&)
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [this]() { return released; });
                LLMTurn t; t.text = u8"完成"; return t;
            }
            void release()
            {
                { std::lock_guard<std::mutex> g(m); released = true; }
                cv.notify_all();
            }
        } gated;

        ToolRegistry reg3;
        AIChatCore core(&gated, &reg3);
        core.submit(u8"第一条");
        CHECK(core.busy()); // worker 被门控阻塞,busy 必然还在
        core.submit(u8"第二条(应被忽略)");
        gated.release(); // 放行第一轮 worker

        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();

        std::vector<ChatEntry> ts = core.transcript();
        bool sawBusyNote = false;
        for (size_t i = 0; i < ts.size(); ++i)
            if (ts[i].kind == ChatEntry::TOOL_NOTE && ts[i].text == u8"上一条还在处理中") sawBusyNote = true;
        CHECK(sawBusyNote);
        // "第二条"文本不应作为独立 USER 条目出现在 transcript 中
        bool sawSecondUserEntry = false;
        for (size_t i = 0; i < ts.size(); ++i)
            if (ts[i].kind == ChatEntry::USER && ts[i].text == u8"第二条(应被忽略)") sawSecondUserEntry = true;
        CHECK(!sawSecondUserEntry);
        std::cout << "AIChatCore busy-submit ignored OK\n";
    }

    // ---- 三轮脚本(calls -> calls -> text)证明多轮循环能跑通 ----
    {
        const char* script =
            "[{\"calls\":[{\"name\":\"step_a\",\"args\":{\"n\":1}}]},"
            " {\"calls\":[{\"name\":\"step_b\",\"args\":{\"n\":2}}]},"
            " {\"text\":\"两步都做完了\"}]";
        FakeProvider fp; CHECK(fp.loadFromString(script));

        ToolRegistry reg4;
        int callOrder[2] = { 0, 0 }; int callCount = 0;
        Tool a; a.name = "step_a"; a.description = "";
        a.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        a.execute = [&](const picojson::value&) { callOrder[callCount++] = 1; return parse("{\"ok\":true}"); };
        Tool b; b.name = "step_b"; b.description = "";
        b.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        b.execute = [&](const picojson::value&) { callOrder[callCount++] = 2; return parse("{\"ok\":true}"); };
        reg4.add(a); reg4.add(b);

        AIChatCore core(&fp, &reg4);
        core.submit(u8"跑两步");
        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();

        CHECK(callCount == 2);
        CHECK(callOrder[0] == 1);
        CHECK(callOrder[1] == 2);
        std::vector<ChatEntry> ts = core.transcript();
        CHECK(ts.back().text == u8"两步都做完了");
        std::cout << "AIChatCore multi-round agent loop OK\n";
    }

    // ---- FakeProvider 返回 {"error":"boom"} -> ERR 条目 + busy 清除 ----
    {
        const char* script = "[{\"error\":\"boom\"}]";
        FakeProvider fp; CHECK(fp.loadFromString(script));
        ToolRegistry reg5;

        AIChatCore core(&fp, &reg5);
        core.submit(u8"触发错误");
        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();

        CHECK(!core.busy());
        std::vector<ChatEntry> ts = core.transcript();
        CHECK(ts.back().kind == ChatEntry::ERR);
        CHECK(ts.back().text == "boom");
        std::cout << "AIChatCore error surfaces as ERR entry OK\n";
    }

    // ---- 6 轮上限:超限后历史仍保持 functionCall/functionResponse 严格配对 ----
    {
        // 7 个 calls 条目:第 1~6 轮正常执行工具,第 7 轮触发"工具循环超限";
        // 最后再补一个 text 条目,供超限后的下一次 submit 正常收尾。
        std::string script = "[";
        for (int i = 0; i < 7; ++i)
            script += "{\"calls\":[{\"name\":\"spin\",\"args\":{\"i\":" + std::to_string(i) + "}}]},";
        script += "{\"text\":\"好的\"}]";
        FakeProvider fp; CHECK(fp.loadFromString(script));

        ToolRegistry reg6; int spinCount = 0;
        Tool spin; spin.name = "spin"; spin.description = "";
        spin.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        spin.execute = [&](const picojson::value&) { ++spinCount; return parse("{\"ok\":true}"); };
        reg6.add(spin);

        AIChatCore core(&fp, &reg6);
        core.submit(u8"死循环");
        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();

        CHECK(!core.busy());
        CHECK(spinCount == 6); // 只执行了 6 轮,第 7 轮被上限拦下
        std::vector<ChatEntry> ts = core.transcript();
        bool sawLimitErr = false;
        for (size_t i = 0; i < ts.size(); ++i)
            if (ts[i].kind == ChatEntry::ERR && ts[i].text == u8"工具循环超限") sawLimitErr = true;
        CHECK(sawLimitErr);

        // 超限后再 submit 一次正常脚本,确认会话可继续
        core.submit(u8"继续");
        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();
        CHECK(core.transcript().back().text == u8"好的");

        // 校验历史:functionCall 与 functionResponse 数量必须相等(不配对会让真实 Gemini API 400)
        picojson::value hv; std::string herr = picojson::parse(hv, core.historyContentsForTest());
        CHECK(herr.empty());
        const picojson::array& contents = hv.get<picojson::array>();
        size_t nCalls = 0, nResponses = 0;
        for (size_t i = 0; i < contents.size(); ++i)
        {
            const picojson::array& parts = contents[i].get("parts").get<picojson::array>();
            for (size_t j = 0; j < parts.size(); ++j)
            {
                if (parts[j].contains("functionCall")) ++nCalls;
                if (parts[j].contains("functionResponse")) ++nResponses;
            }
        }
        CHECK(nCalls == 7); // 6 轮执行 + 1 轮被拦(仍记入历史)
        CHECK(nCalls == nResponses); // 被拦的那轮由合成 functionResponse 补齐配对
        std::cout << "AIChatCore loop-limit keeps call/response paired OK\n";
    }

    // ---- 工具执行抛异常:drainMainThread 兜底为 error functionResponse,不崩、busy 清零 ----
    {
        // 第 1 轮调会抛异常的工具(模拟真 Gemini 发错参数类型/漏必填 → picojson get() 抛
        // std::runtime_error),第 2 轮模型收到 error 响应后给最终文本正常收尾。
        const char* script =
            "[{\"calls\":[{\"name\":\"boomer\",\"args\":{\"x\":1}}]},"
            " {\"text\":\"工具出错但我还活着\"}]";
        FakeProvider fp; CHECK(fp.loadFromString(script));

        ToolRegistry reg7;
        Tool boomer; boomer.name = "boomer"; boomer.description = "";
        boomer.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        boomer.execute = [](const picojson::value&) -> picojson::value
        { throw std::runtime_error("boom"); };
        reg7.add(boomer);

        AIChatCore core(&fp, &reg7);
        core.submit(u8"炸一下");
        for (int i = 0; i < 500 && core.busy(); ++i)
        { core.drainMainThread(); usleep(10000); }
        core.drainMainThread();

        // 1) 不崩 + busy 清零(异常若穿出 drainMainThread,进程早已 terminate 到不了这里)
        CHECK(!core.busy());
        // 2) 后续轮次正常收尾:模型收到 error 响应后给出的文本上屏
        std::vector<ChatEntry> ts = core.transcript();
        CHECK(ts.back().text == u8"工具出错但我还活着");
        // 3) 历史 functionCall/functionResponse 严格配对,且 response 含 error("tool threw: boom")
        picojson::value hv; std::string herr = picojson::parse(hv, core.historyContentsForTest());
        CHECK(herr.empty());
        const picojson::array& contents = hv.get<picojson::array>();
        size_t nCalls = 0, nResponses = 0; bool sawToolThrew = false;
        for (size_t i = 0; i < contents.size(); ++i)
        {
            const picojson::array& parts = contents[i].get("parts").get<picojson::array>();
            for (size_t j = 0; j < parts.size(); ++j)
            {
                if (parts[j].contains("functionCall")) ++nCalls;
                if (parts[j].contains("functionResponse"))
                {
                    ++nResponses;
                    const picojson::value& resp = parts[j].get("functionResponse").get("response");
                    if (resp.contains("error") &&
                        resp.get("error").to_str().find("tool threw: boom") != std::string::npos)
                        sawToolThrew = true;
                }
            }
        }
        CHECK(nCalls == 1);
        CHECK(nCalls == nResponses);
        CHECK(sawToolThrew);
        std::cout << "AIChatCore tool exception contained OK\n";
    }

    // ---- parseGeminiResponse:纯解析,不碰网络(Task 3)----
    {
        // a) 纯文本响应
        {
            std::string body =
                "{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"你好\"}],\"role\":\"model\"},"
                "\"finishReason\":\"STOP\"}]}";
            LLMTurn t = parseGeminiResponse(body);
            CHECK(t.error.empty());
            CHECK(t.text == u8"你好");
            CHECK(t.calls.empty());
        }
        // b) functionCall 响应
        {
            std::string body =
                "{\"candidates\":[{\"content\":{\"parts\":[{\"functionCall\":{\"name\":\"fly_to\","
                "\"args\":{\"lat\":40.7,\"lon\":-74.0}}}],\"role\":\"model\"}}]}";
            LLMTurn t = parseGeminiResponse(body);
            CHECK(t.error.empty());
            CHECK(t.text.empty());
            CHECK(t.calls.size() == 1);
            CHECK(t.calls[0].name == "fly_to");
            CHECK(t.calls[0].args.get("lat").get<double>() > 40.0);
        }
        // c) 混合 parts:文本 + functionCall 都要收集到
        {
            std::string body =
                "{\"candidates\":[{\"content\":{\"parts\":["
                "{\"text\":\"帮你查一下\"},"
                "{\"functionCall\":{\"name\":\"set_layer\",\"args\":{\"name\":\"basemap\"}}}"
                "],\"role\":\"model\"}}]}";
            LLMTurn t = parseGeminiResponse(body);
            CHECK(t.error.empty());
            CHECK(t.text == u8"帮你查一下");
            CHECK(t.calls.size() == 1);
            CHECK(t.calls[0].name == "set_layer");
        }
        // d) 空 candidates + promptFeedback.blockReason -> error
        {
            std::string body =
                "{\"candidates\":[],\"promptFeedback\":{\"blockReason\":\"SAFETY\"}}";
            LLMTurn t = parseGeminiResponse(body);
            CHECK(!t.error.empty());
            CHECK(t.error.find("SAFETY") != std::string::npos);
            CHECK(t.text.empty());
            CHECK(t.calls.empty());
        }
        std::cout << "parseGeminiResponse canned-body tests OK\n";
    }

    // ---- GeminiProvider 可构造、不联网也不崩 ----
    {
        GeminiProvider gp("dummy-key-not-real", "gemini-2.5-flash");
        gp.setSystemPrompt(u8"你是地球助手");
        (void)gp; // 仅验证可构造/可设置系统提示;真实网络请求见 smoke test(见 REPORT)
        std::cout << "GeminiProvider construct OK\n";
    }

    std::cout << "ai_chat tests OK\n";
    return 0;
}
