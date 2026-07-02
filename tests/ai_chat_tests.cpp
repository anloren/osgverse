// tests/ai_chat_tests.cpp
#include <iostream>
#include <cstdlib>
#include "../applications/earth_explorer/ai_tools.h"
#include <picojson.h>

// Release 构建带 -DNDEBUG 会吞掉 assert —— 用自定义 CHECK 保证断言永远生效
#define CHECK(x) do { if (!(x)) { \
    std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " #x << std::endl; \
    std::abort(); } } while (0)

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
    return 0;
}
