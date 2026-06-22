#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Point>
#include <osgDB/FileUtils>
#include <osgGA/GUIEventHandler>
#include <OpenThreads/Thread>
#include <OpenThreads/Mutex>
#include <modeling/Math.h>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <readerwriter/EarthManipulator.h>
#include <VerseCommon.h>
#include <iostream>
#include <vector>
#include <picojson.h>
#include "3rdparty/libhv/all/client/requests.h"
#include <fstream>
#include <sstream>
#include "quake_data.h"

namespace
{
    struct Quake
    {
        double lon, lat, depthKm, mag;
        long long timeMs;
        std::string place, url;
        osg::Vec3d ecef;   // 预算好的世界坐标(抬升后)
    };

    // USGS feed:M2.5+、过去24h、无 key、HTTPS、约每分钟更新。
    static const char* kQuakeFeedUrl =
        "https://earthquake.usgs.gov/earthquakes/feed/v1.0/summary/2.5_day.geojson";

    static const double kQuakeLiftMeters = 3000.0;  // 标记抬离地表,防 z-fight、保可见

    // 解析 GeoJSON FeatureCollection → vector<Quake>。容错:坏 feature 跳过,不抛。
    std::vector<Quake> parseQuakeGeoJson(const std::string& text)
    {
        std::vector<Quake> out;
        picojson::value root;
        std::string err = picojson::parse(root, text);
        if (!err.empty() || !root.is<picojson::object>()) {
            std::cout << "[Quake] JSON parse error: " << err << "\n"; return out;
        }
        const picojson::value& feats = root.get("features");
        if (!feats.is<picojson::array>()) return out;
        const picojson::array& arr = feats.get<picojson::array>();
        for (size_t i = 0; i < arr.size(); ++i)
        {
            const picojson::value& f = arr[i];
            if (!f.is<picojson::object>()) continue;
            const picojson::value& geom = f.get("geometry");
            const picojson::value& props = f.get("properties");
            if (!geom.is<picojson::object>() || !props.is<picojson::object>()) continue;
            const picojson::value& coords = geom.get("coordinates");
            if (!coords.is<picojson::array>()) continue;
            const picojson::array& c = coords.get<picojson::array>();
            if (c.size() < 3 || !c[0].is<double>() || !c[1].is<double>() || !c[2].is<double>()) continue;

            Quake q;
            q.lon = c[0].get<double>(); q.lat = c[1].get<double>(); q.depthKm = c[2].get<double>();
            q.mag = props.get("mag").is<double>() ? props.get("mag").get<double>() : 0.0;
            q.timeMs = props.get("time").is<double>() ? (long long)props.get("time").get<double>() : 0;
            q.place = props.get("place").is<std::string>() ? props.get("place").get<std::string>() : "";
            q.url = props.get("url").is<std::string>() ? props.get("url").get<std::string>() : "";
            q.ecef = osgVerse::Coordinate::convertLLAtoECEF(osg::Vec3d(
                osg::DegreesToRadians(q.lat), osg::DegreesToRadians(q.lon), kQuakeLiftMeters));
            out.push_back(q);
        }
        return out;
    }

    // 取数据:优先 EARTH_QUAKES_FILE 本地样本(离线确定性验证),否则联网 USGS。
    std::vector<Quake> fetchQuakes()
    {
        const char* fixtureFile = getenv("EARTH_QUAKES_FILE");
        if (fixtureFile && *fixtureFile)
        {
            std::ifstream in(fixtureFile);
            if (!in) { std::cout << "[Quake] fixture open failed: " << fixtureFile << "\n"; return std::vector<Quake>(); }
            std::stringstream ss; ss << in.rdbuf();
            std::vector<Quake> qs = parseQuakeGeoJson(ss.str());
            std::cout << "[Quake] Parsed " << qs.size() << " quakes (fixture)\n";
            return qs;
        }
        auto resp = requests::get(kQuakeFeedUrl);
        if (!resp || resp->status_code != 200)
        {
            std::cout << "[Quake] fetch failed, status="
                      << (resp ? (int)resp->status_code : -1) << "\n";
            return std::vector<Quake>();
        }
        std::vector<Quake> qs = parseQuakeGeoJson(resp->body);
        std::cout << "[Quake] Parsed " << qs.size() << " quakes (network)\n";
        return qs;
    }

    // 内部具体类(后续任务逐步填充)。返回的 Group 经 setUserData 持有本对象;
    // 本对象用裸指针引用 Group(不拥有它),避免引用环。
    class QuakeLayerImpl : public osg::Referenced, public QuakeLayer
    {
    public:
        QuakeLayerImpl() : _root(nullptr), _enabled(false) {}
        virtual void setEnabled(bool on) { _enabled = on; if (_root) _root->setNodeMask(on ? ~0u : 0u); }
        virtual bool isEnabled() const { return _enabled; }
        virtual QuakeInfo getSelected() const { return QuakeInfo(); }
        virtual void clearSelected() {}

        osg::Group* root() { return _root; }

        // 创建并返回场景根(调用方负责持有/拥有它)。
        osg::Group* buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);
            std::vector<Quake> qs = fetchQuakes();   // 临时:验证解析(Task 3 移到线程/回调)
            for (size_t i = 0; i < qs.size(); ++i)
                std::cout << "[Quake]   M" << qs[i].mag << " depth=" << qs[i].depthKm
                          << "km " << qs[i].place << "\n";
            return _root;
        }
    protected:
        osg::Group* _root;   // 裸指针:由返回节点经 UserData 拥有,本对象不拥有
        bool _enabled;
    };
}

osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                              const std::string& mainFolder, QuakeLayer** outLayer)
{
    osg::ref_ptr<QuakeLayerImpl> impl = new QuakeLayerImpl;
    osg::Group* root = impl->buildScene();
    root->setUserData(impl.get());   // root 拥有 impl(无环:impl 用裸指针引用 root)
    if (outLayer) *outLayer = impl.get();
    return root;
}
