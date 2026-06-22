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
#include <atomic>
#include <osgViewer/View>
#include <osgGA/GUIEventHandler>
#include <osgDB/Options>            // 必需:TileCallback.h 有 ref_ptr<osgDB::Options> 成员,需完整类型,勿删
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
        PrecipControllerImpl() : _enabled(false), _refreshNow(false), _pendingDirty(false), _thread(nullptr) {}
        virtual void setEnabled(bool on) { _enabled = on; if (on) _refreshNow = true; }
        virtual bool isEnabled() const { return _enabled; }
        bool takeRefreshNow() { return _refreshNow.exchange(false); }
        void startFetch() { if (!_thread) { _thread = new FetchThread(this); _thread->startThread(); } }

        // 后台线程:抓帧;变化则把模板交给主线程。worker 不直接动 TileManager
        // ——TileManager::_layerPaths 无锁,主/cull 线程每帧 check() 读它,worker 写会数据竞争。
        void refreshIfEnabled()
        {
            if (!_enabled) return;
            std::string tmpl = fetchRainViewerTemplate();
            if (tmpl.empty() || !_enabled) return;   // 抓取期间可能被关
            if (tmpl != _lastTemplate)
            {
                _lastTemplate = tmpl;
                OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex);
                _pendingTemplate = tmpl; _pendingDirty = true;
            }
        }

        // 主线程(FRAME 事件)调用:把待定模板设进 OVERLAY 槽(TileManager 仅主线程访问,安全)。
        void applyPending()
        {
            std::string tmpl;
            { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex);
              if (!_pendingDirty) return; tmpl = _pendingTemplate; _pendingDirty = false; }
            osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, tmpl);
            std::cout << "[Precip] OVERLAY set to RainViewer frame\n";
        }
    protected:
        virtual ~PrecipControllerImpl()
        { if (_thread) { _thread->cancel(); _thread->join(); delete _thread; _thread = nullptr; } }
        std::atomic<bool> _enabled, _refreshNow;   // 跨线程(主写、worker 读)→ atomic
        bool _pendingDirty;                         // 仅 _mutex 内访问
        std::string _lastTemplate, _pendingTemplate;
        OpenThreads::Mutex _mutex;
        FetchThread* _thread;
    };

    // 主线程 FRAME:把后台抓到的待定模板应用到 OVERLAY 槽(确保 setLayerPath 在主线程)。
    class PrecipApplyHandler : public osgGA::GUIEventHandler
    {
    public:
        PrecipApplyHandler(PrecipControllerImpl* c) : _ctrl(c) {}
        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
        { if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME) _ctrl->applyPending(); return false; }
    protected:
        osg::ref_ptr<PrecipControllerImpl> _ctrl;
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

osg::ref_ptr<PrecipController> configurePrecipLayer(osgViewer::View& viewer)
{
    osg::ref_ptr<PrecipControllerImpl> c(new PrecipControllerImpl);
    c->startFetch();   // 线程常驻;仅 isEnabled() 时联网
    viewer.addEventHandler(new PrecipApplyHandler(c.get()));   // 主线程应用待定模板
    return c;   // ref_ptr<PrecipControllerImpl> → ref_ptr<PrecipController> 隐式上转
}
