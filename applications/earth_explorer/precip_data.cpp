#include <osg/Referenced>
#include <osg/ref_ptr>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <picojson.h>
#include "3rdparty/libhv/all/client/requests.h"
#include "precip_data.h"

namespace
{
    static const char* kWeatherMapsUrl = "https://api.rainviewer.com/public/weather-maps.json";

    // 解析 weather-maps.json → 最新观测帧的瓦片模板(空串=失败)。
    // 模板形如 https://tilecache.rainviewer.com/v2/radar/<hash>/256/{z}/{x}/{y}/4/1_1.png
    std::string parseRainViewerTemplate(const std::string& text)
    {
        picojson::value root;
        std::string err = picojson::parse(root, text);
        if (!err.empty() || !root.is<picojson::object>()) {
            std::cout << "[Precip] JSON parse error: " << err << "\n"; return "";
        }
        const picojson::value& host = root.get("host");
        const picojson::value& radar = root.get("radar");
        if (!host.is<std::string>() || !radar.is<picojson::object>()) return "";
        const picojson::value& past = radar.get("past");
        if (!past.is<picojson::array>()) return "";
        const picojson::array& arr = past.get<picojson::array>();
        if (arr.empty()) return "";
        const picojson::value& last = arr.back();   // 最新观测帧
        if (!last.is<picojson::object>()) return "";
        const picojson::value& path = last.get("path");
        if (!path.is<std::string>()) return "";
        // 后缀:/256=瓦片像素;/4=RainViewer 色板(Original);/1_1=选项(平滑+含雪)。
        return host.get<std::string>() + path.get<std::string>() + "/256/{z}/{x}/{y}/4/1_1.png";
    }

    // 取最新 RV 模板:优先 EARTH_PRECIP_FILE 本地样本,否则联网。空串=失败。
    std::string fetchRainViewerTemplate()
    {
        const char* fixtureFile = getenv("EARTH_PRECIP_FILE");
        if (fixtureFile && *fixtureFile)
        {
            std::ifstream in(fixtureFile);
            if (!in) { std::cout << "[Precip] fixture open failed: " << fixtureFile << "\n"; return ""; }
            std::stringstream ss; ss << in.rdbuf();
            std::string tmpl = parseRainViewerTemplate(ss.str());
            std::cout << "[Precip] template (fixture): " << tmpl << "\n";
            return tmpl;
        }
        requests::Request req(new HttpRequest);
        req->method = HTTP_GET; req->url = kWeatherMapsUrl; req->timeout = 15;
        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        {
            std::cout << "[Precip] fetch failed, status=" << (resp ? (int)resp->status_code : -1) << "\n";
            return "";
        }
        std::string tmpl = parseRainViewerTemplate(resp->body);
        std::cout << "[Precip] template (network): " << tmpl << "\n";
        return tmpl;
    }

    class PrecipControllerImpl : public PrecipController
    {
    public:
        PrecipControllerImpl() : _enabled(false) {}
        virtual void setEnabled(bool on)
        {
            _enabled = on;
            if (on) { std::string t = fetchRainViewerTemplate(); std::cout << "[Precip] got len=" << t.size() << "\n"; }
        }
        virtual bool isEnabled() const { return _enabled; }
    protected:
        virtual ~PrecipControllerImpl() {}
        bool _enabled;
    };
}

osg::ref_ptr<PrecipController> configurePrecipLayer()
{
    osg::ref_ptr<PrecipController> c(new PrecipControllerImpl);
    if (getenv("EARTH_PRECIP_FILE") || getenv("EARTH_PRECIP")) c->setEnabled(true);  // 临时:验抓取(Task 3 删)
    return c;
}
