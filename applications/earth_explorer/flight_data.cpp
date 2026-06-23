#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Point>
#include <osg/NodeCallback>
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
#include <cstdio>
#include "flight_data.h"

namespace
{
    struct Flight { double lon, lat, altM, velMS, headingRad; std::string callsign, country; osg::Vec3d ecef; };
    static const double kFlightLiftMeters = 0.0;

    std::vector<Flight> parseOpenSky(const std::string& text)
    {
        std::vector<Flight> out;
        picojson::value root; std::string err = picojson::parse(root, text);
        if (!err.empty() || !root.is<picojson::object>()) { std::cout << "[Flight] JSON err: " << err << "\n"; return out; }
        const picojson::value& states = root.get("states");
        if (!states.is<picojson::array>()) return out;
        const picojson::array& arr = states.get<picojson::array>();
        for (size_t i = 0; i < arr.size(); ++i)
        {
            if (!arr[i].is<picojson::array>()) continue;
            const picojson::array& s = arr[i].get<picojson::array>();
            if (s.size() < 11) continue;
            if (s[8].is<bool>() && s[8].get<bool>()) continue;          // on_ground
            if (!s[5].is<double>() || !s[6].is<double>()) continue;     // 缺经纬度
            Flight f;
            f.lon = s[5].get<double>(); f.lat = s[6].get<double>();
            f.altM = s[7].is<double>() ? s[7].get<double>() : 0.0;
            f.velMS = s[9].is<double>() ? s[9].get<double>() : 0.0;
            f.headingRad = osg::DegreesToRadians(s[10].is<double>() ? s[10].get<double>() : 0.0);
            f.callsign = s[1].is<std::string>() ? s[1].get<std::string>() : "";
            f.country = s[2].is<std::string>() ? s[2].get<std::string>() : "";
            f.ecef = osgVerse::Coordinate::convertLLAtoECEF(osg::Vec3d(
                osg::DegreesToRadians(f.lat), osg::DegreesToRadians(f.lon), f.altM + kFlightLiftMeters));
            out.push_back(f);
        }
        return out;
    }

    std::vector<Flight> fetchFlights(double latMin, double lonMin, double latMax, double lonMax)
    {
        const char* fixtureFile = getenv("EARTH_FLIGHTS_FILE");
        if (fixtureFile && *fixtureFile)
        {
            std::ifstream in(fixtureFile);
            if (!in) { std::cout << "[Flight] fixture open failed\n"; return std::vector<Flight>(); }
            std::stringstream ss; ss << in.rdbuf();
            std::vector<Flight> fs = parseOpenSky(ss.str());
            std::cout << "[Flight] Parsed " << fs.size() << " flights (fixture)\n";
            return fs;
        }
        char url[256];
        snprintf(url, sizeof(url),
            "https://opensky-network.org/api/states/all?lamin=%.4f&lomin=%.4f&lamax=%.4f&lomax=%.4f",
            latMin, lonMin, latMax, lonMax);
        requests::Request req(new HttpRequest);
        req->method = HTTP_GET; req->url = url; req->timeout = 20;
        requests::Response resp = requests::request(req);
        if (!resp || resp->status_code != 200)
        { std::cout << "[Flight] fetch failed status=" << (resp ? (int)resp->status_code : -1) << "\n"; return std::vector<Flight>(); }
        std::vector<Flight> fs = parseOpenSky(resp->body);
        std::cout << "[Flight] Parsed " << fs.size() << " flights (network)\n";
        return fs;
    }

    class FlightLayerImpl : public osg::Referenced, public FlightLayer
    {
    public:
        FlightLayerImpl() : _root(0), _enabled(false),
                            _bbLatMin(-85.0), _bbLonMin(-180.0), _bbLatMax(85.0), _bbLonMax(180.0) {}
        virtual void setEnabled(bool on) { _enabled = on; if (_root) _root->setNodeMask(on ? ~0u : 0u); }
        virtual bool isEnabled() const { return _enabled; }
        virtual void setViewBBox(double latMin, double lonMin, double latMax, double lonMax)
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_bbMutex);
            _bbLatMin = latMin; _bbLonMin = lonMin; _bbLatMax = latMax; _bbLonMax = lonMax;
        }
        virtual FlightInfo getSelected() const { return FlightInfo(); }
        virtual void clearSelected() {}

        osg::Group* root() { return _root; }
        osg::Group* buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);   // 默认关
            return _root;
        }
    protected:
        virtual ~FlightLayerImpl() {}
        osg::Group* _root;   // 裸指针:由返回节点经 UserData 拥有,本对象不拥有(无引用环)
        bool _enabled;
        double _bbLatMin, _bbLonMin, _bbLatMax, _bbLonMax;
        OpenThreads::Mutex _bbMutex;
    };
}

osg::Node* configureFlightLayer(osgViewer::View& viewer, osg::Node* earthRoot,
                                const std::string& mainFolder, FlightLayer** outLayer)
{
    osg::ref_ptr<FlightLayerImpl> impl = new FlightLayerImpl;
    osg::Group* root = impl->buildScene();
    root->setUserData(impl.get());
    if (outLayer) *outLayer = impl.get();
    if (getenv("EARTH_FLIGHTS_FILE")) { std::vector<Flight> fs = fetchFlights(-85,-180,85,180);
        std::cout << "[Flight] (temp) got " << fs.size() << "\n"; }
    return root;
}
