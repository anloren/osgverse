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

    // 点精灵:VS 写 gl_PointSize(取自 texcoord0.x)+ headingRad(texcoord0.y)。
    // FS 在点精灵坐标系内旋转后画箭头三角形,外面 discard。
    // MRT 双输出(COLOR_BUFFER0 颜色 + COLOR_BUFFER1 掩码),同 quake_data。
    const char* flightVertCode = {
        "VERSE_VS_OUT vec4 pointColor;\n"
        "VERSE_VS_OUT float headingRad;\n"
        "void main() {\n"
        "    pointColor = osg_Color;\n"
        "    gl_PointSize = osg_MultiTexCoord0.x;\n"
        "    headingRad = osg_MultiTexCoord0.y;\n"
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };
    const char* flightFragCode = {
        "VERSE_FS_IN vec4 pointColor;\n"
        "VERSE_FS_IN float headingRad;\n"
        "#ifdef VERSE_GLES3\n"
        "layout(location=0) VERSE_FS_OUT vec4 fragColor;\n"
        "layout(location=1) VERSE_FS_OUT vec4 fragOrigin;\n"
        "#endif\n"
        "void main() {\n"
        "    vec2 d = gl_PointCoord - vec2(0.5);\n"
        "    float s = sin(headingRad), c = cos(headingRad);\n"
        "    vec2 r = vec2(c*d.x - s*d.y, s*d.x + c*d.y);\n"
        "    r.y = -r.y;\n"
        "    // arrowhead triangle: apex(0,0.42), base(+-0.30,-0.32); inside => draw\n"
        "    float inTri = step(-0.32, r.y) * step(abs(r.x), 0.30 * (0.42 - r.y) / 0.74);\n"
        "    if (inTri < 0.5) discard;\n"
        "    vec3 rgb = pointColor.rgb;\n"
        "#ifdef VERSE_GLES3\n"
        "    fragColor = vec4(rgb, 1.0); fragOrigin = vec4(1.0);\n"
        "#else\n"
        "    gl_FragData[0] = vec4(rgb, 1.0); gl_FragData[1] = vec4(1.0);\n"
        "#endif\n"
        "}\n"
    };

    // 高度→颜色:低暖橙 / 中黄白 / 高冷青蓝。
    osg::Vec4 altColor(double altM)
    {
        if (altM < 3000.0)  return osg::Vec4(1.00f, 0.55f, 0.20f, 1.0f);   // 低:暖橙
        if (altM < 9000.0)  return osg::Vec4(1.00f, 0.95f, 0.70f, 1.0f);   // 中:黄白
        return osg::Vec4(0.45f, 0.85f, 1.00f, 1.0f);                       // 高:冷青蓝
    }
    static const float kFlightSizePx = 22.0f;

#ifndef GL_PROGRAM_POINT_SIZE
#define GL_PROGRAM_POINT_SIZE 0x8642
#endif

    // 由一批 Flight 构建一个 Geode(GL_POINTS)。
    osg::Geode* buildFlightGeode(const std::vector<Flight>& fs, osg::StateSet* sharedSS)
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        osg::ref_ptr<osg::Vec2Array> attrs = new osg::Vec2Array;  // (sizePx, headingRad)
        for (size_t i = 0; i < fs.size(); ++i)
        {
            verts->push_back(fs[i].ecef);
            colors->push_back(altColor(fs[i].altM));
            attrs->push_back(osg::Vec2(kFlightSizePx, (float)fs[i].headingRad));
        }
        geom->setVertexArray(verts.get());
        geom->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
        geom->setTexCoordArray(0, attrs.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_POINTS, 0, (GLsizei)verts->size()));
        geom->setUseDisplayList(false); geom->setUseVertexBufferObjects(true);
        geom->setCullingActive(false);
        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geom.get()); geode->setStateSet(sharedSS);
        return geode.release();
    }

    // 前向声明
    class FlightLayerImpl;

    class SyncCallback : public osg::NodeCallback
    {
    public:
        SyncCallback(FlightLayerImpl* o) : _owner(o) {}
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv);  // 仅声明,定义在 FlightLayerImpl 之后
    protected:
        FlightLayerImpl* _owner;
    };

    class FlightLayerImpl : public osg::Referenced, public FlightLayer
    {
    public:
        FlightLayerImpl() : _root(0), _enabled(false), _dirty(false),
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

        // 后台线程(或临时直调)交付新快照(加锁)。
        void postSnapshot(const std::vector<Flight>& fs)
        { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex); _pending = fs; _dirty = true; }

        // 主线程(update 遍历)调用:若有新数据则重建 geode。
        void syncIfDirty()
        {
            std::vector<Flight> fs;
            { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex);
              if (!_dirty) return; fs = _pending; _dirty = false; }
            _flights = fs;
            if (_geode.valid()) _root->removeChild(_geode.get());
            _geode = buildFlightGeode(fs, _ss.get());
            _root->addChild(_geode.get());
        }

        osg::Group* buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);   // 默认关

            osg::Shader* vs = new osg::Shader(osg::Shader::VERTEX, flightVertCode);
            osg::Shader* fs = new osg::Shader(osg::Shader::FRAGMENT, flightFragCode);
            vs->setName("Flight_VS"); fs->setName("Flight_FS");
            osgVerse::Pipeline::createShaderDefinitions(vs, 100, 130);
            osgVerse::Pipeline::createShaderDefinitions(fs, 100, 130);
            osg::ref_ptr<osg::Program> prog = new osg::Program;
            prog->addShader(vs); prog->addShader(fs);
            _ss = new osg::StateSet;
            _ss->setAttributeAndModes(prog.get(), osg::StateAttribute::ON);
            _ss->setMode(GL_PROGRAM_POINT_SIZE, osg::StateAttribute::ON);
            _ss->setRenderBinDetails(11, "RenderBin");  // 在地表/海洋 pass 之后画

            _root->addUpdateCallback(new SyncCallback(this));
            return _root;
        }
    protected:
        virtual ~FlightLayerImpl() {}
        osg::Group* _root;   // 裸指针:由返回节点经 UserData 拥有,本对象不拥有(无引用环)
        osg::ref_ptr<osg::Geode> _geode;
        osg::ref_ptr<osg::StateSet> _ss;
        std::vector<Flight> _flights, _pending;
        OpenThreads::Mutex _mutex;
        bool _enabled, _dirty;
        double _bbLatMin, _bbLonMin, _bbLatMax, _bbLonMax;
        OpenThreads::Mutex _bbMutex;
    };

    void SyncCallback::operator()(osg::Node* node, osg::NodeVisitor* nv)
    { _owner->syncIfDirty(); traverse(node, nv); }
}

osg::Node* configureFlightLayer(osgViewer::View& viewer, osg::Node* earthRoot,
                                const std::string& mainFolder, FlightLayer** outLayer)
{
    osg::ref_ptr<FlightLayerImpl> impl = new FlightLayerImpl;
    osg::Group* root = impl->buildScene();
    root->setUserData(impl.get());
    if (outLayer) *outLayer = impl.get();
    impl->postSnapshot(fetchFlights(-85, -180, 85, 180));  // 临时:Task 4 改后台线程
    impl->setEnabled(true);                                 // 临时:Task 4 改回默认关
    return root;
}
