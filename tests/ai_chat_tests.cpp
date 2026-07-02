// tests/ai_chat_tests.cpp
#include <iostream>
#include <cstdlib>
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
        // 脚本只有一项,且刻意让 chat() 在被调用前有机会观察到 busy=true:
        // 用一个会阻塞的 provider 包装,确保第一轮 worker 还没跑完时发起第二次 submit。
        struct SlowProvider : public LLMProvider
        {
            virtual LLMTurn chat(const std::string&, const std::string&)
            {
                usleep(200000); // 200ms,足够主线程在此期间调用第二次 submit
                LLMTurn t; t.text = u8"完成"; return t;
            }
        } slow;

        ToolRegistry reg3;
        AIChatCore core(&slow, &reg3);
        core.submit(u8"第一条");
        CHECK(core.busy()); // 刚提交,worker 还在跑(至少 200ms)
        core.submit(u8"第二条(应被忽略)");

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

    std::cout << "ai_chat tests OK\n";
    return 0;
}
