#include <osg/Referenced>
#include <osg/ref_ptr>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <picojson.h>
#include "3rdparty/libhv/all/client/requests.h"
#include <OpenThreads/Thread>
#include <OpenThreads/Mutex>
#include <osgDB/Options>
#include <readerwriter/TileCallback.h>
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

    class PrecipControllerImpl;

    class FetchThread : public OpenThreads::Thread
    {
    public:
        FetchThread(PrecipControllerImpl* o) : _owner(o), _done(false) {}
        virtual int cancel() { _done = true; return OpenThreads::Thread::cancel(); }
        virtual void run();   // 定义在 PrecipControllerImpl 之后
    protected:
        PrecipControllerImpl* _owner; bool _done;
    };

    class PrecipControllerImpl : public PrecipController
    {
    public:
        PrecipControllerImpl() : _enabled(false), _refreshNow(false), _thread(nullptr) {}
        virtual void setEnabled(bool on) { _enabled = on; if (on) _refreshNow = true; }
        virtual bool isEnabled() const { return _enabled; }

        // 后台线程取用:开启瞬间需立即抓一次。
        bool takeRefreshNow() { bool r = _refreshNow; _refreshNow = false; return r; }

        void startFetch() { if (!_thread) { _thread = new FetchThread(this); _thread->startThread(); } }

        // 后台线程调用:抓最新帧,若变化且仍开启则设 OVERLAY 路径(check() 会重载瓦片)。
        void refreshIfEnabled()
        {
            if (!_enabled) return;
            std::string tmpl = fetchRainViewerTemplate();
            if (tmpl.empty() || !_enabled) return;   // 抓取期间可能被关
            if (tmpl != _lastTemplate)
            {
                _lastTemplate = tmpl;
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, tmpl);
                std::cout << "[Precip] OVERLAY set to RainViewer frame\n";
            }
        }
    protected:
        virtual ~PrecipControllerImpl()
        { if (_thread) { _thread->cancel(); _thread->join(); delete _thread; _thread = nullptr; } }
        bool _enabled, _refreshNow;
        std::string _lastTemplate;
        FetchThread* _thread;
    };

    void FetchThread::run()
    {
        const int kIntervalTicks = 4800;   // ~8min ÷ 100ms/tick
        int tick = 0;
        while (!_done)
        {
            if (_owner->isEnabled())
            {
                if (_owner->takeRefreshNow() || tick <= 0) { _owner->refreshIfEnabled(); tick = kIntervalTicks; }
                else tick--;
            }
            else tick = 0;
            OpenThreads::Thread::microSleep(100000);  // 100ms
        }
        _done = true;
    }
}

osg::ref_ptr<PrecipController> configurePrecipLayer()
{
    osg::ref_ptr<PrecipControllerImpl> c(new PrecipControllerImpl);
    c->startFetch();   // 线程常驻;仅 isEnabled() 时联网
    // 临时(Task 4 删,改由图层 apply 驱动):env 强制开,供本任务 headless 验证。
    if (getenv("EARTH_PRECIP_FILE") || getenv("EARTH_PRECIP")) c->setEnabled(true);
    return osg::ref_ptr<PrecipController>(c.get());
}
