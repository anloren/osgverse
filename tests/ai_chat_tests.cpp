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
