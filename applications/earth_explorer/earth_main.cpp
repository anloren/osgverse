#include PREPENDED_HEADER
#include <osg/io_utils>
#include <osg/CullFace>
#include <osg/Texture2D>
#include <osg/MatrixTransform>
#include <osgDB/Archive>
#include <osgDB/ReadFile>
#include <osgDB/WriteFile>
#include <osgGA/StateSetManipulator>
#include <osgGA/TrackballManipulator>
#include <osgGA/EventVisitor>
#include <osgViewer/Viewer>
#include <osgViewer/ViewerEventHandlers>

#include <modeling/Math.h>
#include <readerwriter/EarthManipulator.h>
#include <readerwriter/TileCallback.h>
#include <readerwriter/FileCache.h>
#include <pipeline/Pipeline.h>
#include "EarthControlUI.h"
#include "LayerManager.h"
#include "quake_data.h"
#include "precip_data.h"
#include "flight_data.h"
#include "tiles3d_data.h"
#include "ai_tools.h"
#include "ai_chat.h"
#include <VerseCommon.h>
#include <iostream>
#include <sstream>
#include <ctime>
#include <thread>
#include <atomic>
#include <vector>

#ifdef OSG_LIBRARY_STATIC
USE_OSG_PLUGINS()
USE_VERSE_PLUGINS()
USE_SERIALIZER_WRAPPER(DracoGeometry)
#endif

#define EARTH_INTERSECTION_MASK 0xf0000000
#define SIMPLE_VERSION 1

extern std::vector<osg::Camera*> configureEarthRendering(
        osgViewer::View& viewer, osg::Group* root, osg::Node* earth, osgVerse::EarthAtmosphereOcean& eData,
        const std::string& mainFolder, unsigned int mask, int w, int h);
extern osg::Node* configureCityData(osgViewer::View& viewer, osg::Node* earthRoot,
                                    osgVerse::EarthAtmosphereOcean& earthRenderingUtils,
                                    const std::string& mainFolder, unsigned int mask, bool waitingMode);
extern osg::Camera* configureUI(osgViewer::View& viewer, osg::Group* root,
                                const std::string& mainFolder, int w, int h);

class EnvironmentHandler : public osgGA::GUIEventHandler
{
public:
    EnvironmentHandler(osgVerse::EarthAtmosphereOcean* eao, const std::string& folder)
    :   _earthData(eao), _mainFolder(folder), _pressingKey(0), _pathIndex(0), _sunAngle(0.0f)
    {
        _earthData->commonUniforms["OceanOpaque"]->set(0.0f);
        _earthData->commonUniforms["WorldSunDir"]->set(
            osg::Vec3(-1.0f, 0.0f, 0.0f) * osg::Matrix::rotate(_sunAngle, osg::Z_AXIS));
    }

    bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
    {
        osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
        if (ea.getEventType() == osgGA::GUIEventAdapter::USER)
        {
            const osgDB::Options* ev = dynamic_cast<const osgDB::Options*>(ea.getUserData());
            std::string command = ev ? ev->getOptionString() : "";

            std::vector<std::string> commmandPair; osgDB::split(command, commmandPair, '/');
            handleCommand(view, commmandPair.front(), commmandPair.back());
        }
        else if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
        {
            for (std::map<std::string, bool>::iterator itr = _toggles.begin(); itr != _toggles.end(); ++itr)
            {
                const std::string& cmd = itr->first; if (!itr->second) continue;
                if (cmd == "light")
                {
                    _sunAngle += 0.01f;
                    _earthData->commonUniforms["WorldSunDir"]->set(
                        osg::Vec3(-1.0f, 0.0f, 0.0f) * osg::Matrix::rotate(_sunAngle, osg::Z_AXIS));
                    view->getEventQueue()->userEvent(new osgDB::Options("value/" + std::to_string(_sunAngle)));
                }
                else if (cmd == "auto_rotate")
                {
                    // TODO
                }
            }
        }
        else if (ea.getEventType() == osgGA::GUIEventAdapter::KEYDOWN) _pressingKey = ea.getKey();
        else if (ea.getEventType() == osgGA::GUIEventAdapter::KEYUP)
        {
            osgVerse::EarthManipulator* earthMani =
                static_cast<osgVerse::EarthManipulator*>(view->getCameraManipulator());
            if (ea.getKey() == 'o')  // set an animation-path frame
            {
                earthMani->insertControlPointFromCurrentView((float)_pathIndex); _pathIndex += 60;
            }
            else if (ea.getKey() == 'i')  // save the animation-path
            {
                std::ofstream out("../city_path.txt", std::ios::out);
                const osgVerse::EarthManipulator::ControlPointSet& path = earthMani->getControlPoints();
                for (osgVerse::EarthManipulator::ControlPointSet::const_iterator it = path.begin();
                     it != path.end(); ++it)
                {
                    osgVerse::EarthManipulator::ControlPoint* cp = (*it).get();
                    out << cp->_time << "," << cp->_rotation << "," << cp->_tilt << "," << cp->_distance << "\n";
                }
            }
            else if (ea.getKey() == 'p')  // load and play the animation-path
            {
                std::ifstream in("../city_path.txt", std::ios::in); std::string line;
                if (in)
                {
                    earthMani->clearControlPoints();
                    while (std::getline(in, line))
                    {
                        if (line.empty()) continue; else if (line[0] == '#') continue;
                        std::vector<std::string> values; osgDB::split(line, values, ',');

                        if (values.size() < 4) continue;
                        osg::Quat q; std::stringstream ss(values[1]); ss >> q;
                        double time = atof(values[0].c_str()), tilt = atof(values[2].c_str());
                        double distance = atof(values[3].c_str());
                        earthMani->insertControlPoint(
                            new osgVerse::EarthManipulator::ControlPoint(time, q, distance, tilt));
                    }

                    if (earthMani->isAnimationRunning()) earthMani->stopAnimation();
                    earthMani->startAnimation();
                }
            }
            _pressingKey = 0;
        }

        if (_pressingKey > 0)
        {
            switch (_pressingKey)
            {
            case ',': case '<':
                _sunAngle -= 0.00005f;
                _earthData->commonUniforms["WorldSunDir"]->set(
                    osg::Vec3(-1.0f, 0.0f, 0.0f) * osg::Matrix::rotate(_sunAngle, osg::Z_AXIS));
                view->getEventQueue()->userEvent(new osgDB::Options("value/" + std::to_string(_sunAngle))); break;
            case '.': case '>':
                _sunAngle += 0.00005f;
                _earthData->commonUniforms["WorldSunDir"]->set(
                    osg::Vec3(-1.0f, 0.0f, 0.0f) * osg::Matrix::rotate(_sunAngle, osg::Z_AXIS));
                view->getEventQueue()->userEvent(new osgDB::Options("value/" + std::to_string(_sunAngle))); break;
            }
        }
        return false;
    }

    void handleCommand(osgViewer::View* view, const std::string& type, const std::string& cmd)
    {
        if (type == "button")
        {
            osgVerse::EarthManipulator* earthMani =
                static_cast<osgVerse::EarthManipulator*>(view->getCameraManipulator());
            if (cmd == "light") _toggles[cmd] = !_toggles[cmd];
            else if (cmd == "auto_rotate") _toggles[cmd] = !_toggles[cmd];
            else if (cmd == "go_home") earthMani->home(0.0);
            else if (cmd == "ocean")
            {
                bool v = !_toggles[cmd]; _toggles[cmd] = v;
                _earthData->commonUniforms["OceanOpaque"]->set(v ? 1.0f : 0.0f);
            }
            /*else if (cmd == "globe")
            {
                bool v = !_toggles[cmd]; _toggles[cmd] = v;
                _earthData->commonUniforms["GlobalOpaque"]->set(v ? 0.5f : 1.0f);
            }*/
            else if (cmd == "zoom_in") earthMani->performScale(osgGA::GUIEventAdapter::SCROLL_UP);
            else if (cmd == "zoom_out") earthMani->performScale(osgGA::GUIEventAdapter::SCROLL_DOWN);
        }
        else if (type == "item")
        {
            osgVerse::TileManager* mgr = osgVerse::TileManager::instance();
            if (cmd.find("seg") != std::string::npos)
                mgr->setLayerPath(osgVerse::TileCallback::USER, _mainFolder + "/Tiles/" + cmd + "/{z}/{x}/{y}.png");
        }
    }

protected:
    std::map<std::string, bool> _toggles;
    osgVerse::EarthAtmosphereOcean* _earthData;
    std::string _mainFolder;
    int _pressingKey, _pathIndex;
    float _sunAngle;
};

// AWS Terrarium 高程瓦片 URL 模板;earthURLs 与启动预热两处共用,保证预热与渲染同源、URL 永不漂移。
static const std::string kTerrariumUrl =
    "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png";

// —— 香港高程运行时过滤(经 ElevationFilterFunction 注入 osgdb_tms)——
// 两个问题一并修:① Terrarium(SRTM 派生)是含楼高的表面模型,高楼区把楼高掺进地形
// (实测尖沙咀 p90≈37m,真实地面 3-8m);② 引擎统一 ×TileElevationScale(2.0) 夸张 →
// 香港地表处处高于实景三维网格(真实高度),底图贴图"污染"进 3D 城区(盖住网格地面)。
// 处方(全层级同一个连续函数 → 父子 LOD 与邻接由构造保证一致,不会再出"漏空壳"):
//   城区平原核心(九龙+港岛北岸):高度=0(海平面,低于网格地面 3-5m → 被网格盖住);
//   都会区(含太平山/狮子山/港岛南等):高度=原始×0.5-2m(抵消 ×2 夸张≈真实高度、略沉,
//   山形保留,网格贴着山面);区外:原始值。两条边界各带 ~1-2km smoothstep 渐变。
// f2 关闭时城区地面平整、山体回归真实高度,同样比"假土包"更对。EARTH_HK_FLATDEM=0 关闭。
static void hkElevationFilter(float* hts, int w, int h, int x, int y, int z)
{
    static const bool enabled = []() {
        const char* e = getenv("EARTH_HK_FLATDEM");
        return !(e && *e && atoi(e) == 0);
    }();
    if (!enabled || !hts || w <= 0 || h <= 0) return;

    // z>15 时图像内容是 z15 祖先瓦片(见 createCustomPath 同规则),按祖先坐标换算范围
    int tz = z, tx = x, tyTMS = y;
    if (z > 15) { int dz = z - 15; tx = x >> dz; tyTMS = y >> dz; tz = 15; }
    double n = (double)(1 << tz);
    int tyXYZ = (int)n - 1 - tyTMS;
    double lonMin = tx / n * 360.0 - 180.0, lonSpan = 360.0 / n;
    double latN = atan(sinh(osg::PI * (1.0 - 2.0 * tyXYZ / n))) * 180.0 / osg::PI;
    double latS = atan(sinh(osg::PI * (1.0 - 2.0 * (tyXYZ + 1) / n))) * 180.0 / osg::PI;
    // 快速剔除:与都会外包络(含渐变带)不相交的瓦片原样返回(全球其它地区零改动)
    if (lonMin > 114.37 || lonMin + lonSpan < 113.88 || latS > 22.44 || latN < 22.17) return;

    auto smooth01 = [](double t) {
        t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); return t * t * (3.0 - 2.0 * t);
    };
    // 到矩形 [la0,la1]x[lo0,lo1] 的外距离经 f 宽渐变:核心=0 → 带外=1
    auto outerW = [&smooth01](double lat, double lon, double la0, double la1,
                              double lo0, double lo1, double f) {
        double dla = std::max(std::max(la0 - lat, lat - la1), 0.0);
        double dlo = std::max(std::max(lo0 - lon, lon - lo1), 0.0);
        return smooth01(sqrt(dla * dla + dlo * dlo) / f);
    };

    for (int row = 0; row < h; ++row)
    {
        // 解码图自底向上(row 0=南);行中心对应的 XYZ 行分数 → 墨卡托纬度
        double yf = (double)tyXYZ + 1.0 - ((double)row + 0.5) / (double)h;
        double lat = atan(sinh(osg::PI * (1.0 - 2.0 * yf / n))) * 180.0 / osg::PI;
        for (int col = 0; col < w; ++col)
        {
            double lon = lonMin + ((double)col + 0.5) / (double)w * lonSpan;
            double wMetro = outerW(lat, lon, 22.19, 22.42, 113.90, 114.35, 0.02);
            if (wMetro >= 1.0) continue;
            double wFlat = outerW(lat, lon, 22.276, 22.335, 114.115, 114.225, 0.012);
            float& hv = hts[row * w + col];
            double hMetro = (double)hv * 0.5 - 2.0;   // 抵消 ×2 夸张,整体略沉
            double hIn = wFlat * hMetro;              // 平原核心=0,向山地平滑过渡
            hv = (float)(wMetro * (double)hv + (1.0 - wMetro) * hIn);
        }
    }
}

static std::string createCustomPath(int type, const std::string& prefix, int x, int y, int z)
{
    // 内部瓦片是 TMS（OriginBottomLeft=1，原点左下）。在线服务（ArcGIS / Terrarium）
    // 都是 XYZ（原点左上），所以把 Y 翻转成 XYZ 行号。
    int yXYZ = (int)pow(2.0, (double)z) - 1 - y;
    if (type == osgVerse::TileCallback::ORTHOPHOTO)
    {
        // 卫星底图模板。URL 含 '='/'&',osgDB::Options 解析会把多 '=' 的值截断,
        // 所以在这里硬编码而非放进 earthURLs。EARTH_BASEMAP 可换源(启动时读一次):
        //   未设/google → Google lyrs=s(香港等高密度区掺倾斜航拍,楼体在地面影像里是斜的);
        //   esri        → Esri World Imagery(多数城市更接近真正射,可对比"地面扭曲"观感);
        //   其它        → 当作自定义模板(需含 {x}/{y}/{z} 占位符)。
        static const std::string basemap = []() {
            const char* e = getenv("EARTH_BASEMAP");
            std::string v = e ? e : "";
            if (v.empty() || v == "google")
                return std::string("https://mt1.google.com/vt/lyrs=s&x={x}&y={y}&z={z}");
            if (v == "esri")
                return std::string("https://server.arcgisonline.com/ArcGIS/rest/services/"
                                   "World_Imagery/MapServer/tile/{z}/{y}/{x}");
            return v;
        }();
        return osgVerse::TileCallback::createPath(basemap, x, yXYZ, z);
    }
    else if (type == osgVerse::TileCallback::ELEVATION)
    {
        // (香港城区 DSM 假土包/×2 夸张的修正不在这里做路径分流,而是经 ElevationFilterFunction
        //  钩子在解码后逐像素过滤——见上方 hkElevationFilter 注释。)
        // AWS Terrarium 高程最高约 z15。深瓦片(z>15)取 z15 **祖先瓦片**,createTile 用 elevScaleBias
        // 采子区一步烘焙正确高度 → 与 z15 父级连续、消除 LOD 边界落差/看穿孔洞;配套 EarthManipulator
        // 的相机地形地板(Step A)防穿模。一步烘焙按瓦片坐标确定性算出,无运行时继承的时序竞态。
        if (z > 15)
        {
            int dz = z - 15, ax = x >> dz, ay = y >> dz;
            int ayXYZ = (1 << 15) - 1 - ay;
            return osgVerse::TileCallback::createPath(prefix, ax, ayXYZ, 15);
        }
        return osgVerse::TileCallback::createPath(prefix, x, yXYZ, z);
    }
    else if (type == osgVerse::TileCallback::USER)
    {
        // Google 透明标注层（路网 + 地名），与底图同瓦片方案，叠在 ExtraLayer(unit2)
        static const std::string googleLabels = "https://mt1.google.com/vt/lyrs=h&x={x}&y={y}&z={z}";
        return osgVerse::TileCallback::createPath(googleLabels, x, yXYZ, z);
    }
    else if (type == osgVerse::TileCallback::OVERLAY)
    {
        // OVERLAY 槽按 prefix(= 该层路径,由 TileManager::setLayerPath 设)分流:
        //   "gibs"      → GIBS VIIRS 云图(默认);
        //   以 http 开头 → 当作完整瓦片模板(RainViewer 降水雷达);
        //   其它(空)    → 不加载(返回空)。
        if (prefix.rfind("http", 0) == 0)   // RainViewer 模板(已含 {z}/{x}/{y})
        {
            if (z > 10) return "";          // 雷达分辨率粗,z>10 不取
            return osgVerse::TileCallback::createPath(prefix, x, yXYZ, z);
        }
        if (prefix != "gibs") return "";    // 空/未知 → 不加载叠加瓦片

        if (z > 9) return "";  // GIBS GoogleMapsCompatible_Level9 max zoom is 9; no data above
        // 用 VIIRS_SNPP 真彩而非 MODIS_Terra:MODIS 单星刈幅赤道留黑色刈幅空隙;VIIRS 无缝。
        // 取「今天-2 天」(UTC) 完整日期,只算一次(长跑不刷新;跨 UTC 午夜需重启)。
        // 南极极夜瓦片 VIIRS 返回 404 → 加载失败回退默认透明贴图(露底图)而非黑斑,无需特殊处理。
        static const std::string gibsDate = []() {
            time_t t = time(NULL) - 2 * 86400; struct tm g; gmtime_r(&t, &g);
            char buf[16]; strftime(buf, sizeof(buf), "%Y-%m-%d", &g); return std::string(buf);
        }();
        std::string gibs =
            "https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/"
            "VIIRS_SNPP_CorrectedReflectance_TrueColor/default/" + gibsDate +
            "/GoogleMapsCompatible_Level9/{z}/{y}/{x}.jpg";
        return osgVerse::TileCallback::createPath(gibs, x, yXYZ, z);
    }
    // OCEAN_MASK：仍用本地 mbtiles（TMS，不翻转），深层级丢弃
    if (z > 3) return "";
    return osgVerse::TileCallback::createPath(prefix, x, y, z);
}

// 启动磁盘预热:把 z0..maxZ 全球低 LOD 瓦片的 底图+标注+高程 三层预拉进磁盘缓存,用户首次
// 平移/缩放到新区时粗瓦片立即出图。仅磁盘预热;4 worker 共享原子游标分摊;loadFileData 命中
// 缓存即秒回 → 天然去重、重跑零浪费。瓦片数 = Σ 4^z = (4^(maxZ+1)-1)/3。
static void prefetchLowLODGlobe(int maxZ)
{
    struct PT { int z, x, y; };
    std::vector<PT> tiles;
    for (int z = 0; z <= maxZ; ++z)
    {
        int n = 1 << z;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            { PT t; t.z = z; t.x = x; t.y = y; tiles.push_back(t); }
    }

    std::atomic<size_t> cursor(0);
    std::atomic<int> warmed(0);
    const int kWorkers = 4;
    std::vector<std::thread> pool;
    for (int w = 0; w < kWorkers; ++w)
    {
        pool.push_back(std::thread([&]() {
            for (;;)
            {
                size_t i = cursor.fetch_add(1);
                if (i >= tiles.size()) break;
                const PT& t = tiles[i];
                const int types[3] = { osgVerse::TileCallback::ORTHOPHOTO,
                                       osgVerse::TileCallback::USER,
                                       osgVerse::TileCallback::ELEVATION };
                for (int k = 0; k < 3; ++k)
                {
                    std::string prefix =
                        (types[k] == osgVerse::TileCallback::ELEVATION) ? kTerrariumUrl : std::string();
                    std::string path = createCustomPath(types[k], prefix, t.x, t.y, t.z);
                    if (!path.empty())
                    { std::string mime, enc; osgVerse::loadFileData(path, mime, enc); }
                }
                warmed.fetch_add(1);
            }
        }));
    }
    for (size_t i = 0; i < pool.size(); ++i) pool[i].join();
    OSG_NOTICE << "[prefetch] Warmed " << warmed.load() << " low-LOD globe tiles (z0-"
               << maxZ << ", base+labels+elevation)\n";
}

int main(int argc, char** argv)
{
    osgViewer::Viewer viewer;
    osg::ArgumentParser arguments = osgVerse::globalInitialize(argc, argv, osgVerse::defaultInitParameters());
    osg::setNotifyHandler(new osgVerse::ConsoleHandler(false));
    osgVerse::updateOsgBinaryWrappers();
    osgDB::Registry::instance()->addFileExtensionAlias("tif", "verse_tiff");

    std::string mainFolder = BASE_DIR + "/models"; arguments.read("--folder", mainFolder);
    std::string skirtRatio = "0.05"; arguments.read("--skirt", skirtRatio);
    int w = 1920, h = 1080; arguments.read("--resolution", w, h);
    bool cityWaitingTiles = true, manipulatorCanThrow = false;
    if (arguments.read("--no-wait")) cityWaitingTiles = false;
    if (arguments.read("--thrown")) manipulatorCanThrow = true;
    // 启动即跳转到指定经纬度+高度（度, 度, 千米），便于直接查看街道级瓦片
    double gotoLat = 1.0e9, gotoLon = 0.0, gotoAltKm = 0.0;
    arguments.read("--goto", gotoLat, gotoLon, gotoAltKm);

    // Create earth
    std::string earthURLs =
        // Orthophoto 值仅为占位符（非空即启用影像层）；真实 Google 混合瓦片 URL 在
        // createCustomPath 里拼装——Google URL 含多个 '='，放这里会被 Options 解析截断。
        " Orthophoto=google"          // non-empty placeholder; real lyrs=s URL built in createCustomPath
        " User=googleLabels"          // non-empty placeholder; real lyrs=h URL built in createCustomPath
        " Overlay=gibs"               // non-empty placeholder; real GIBS URL built in createCustomPath
        " Elevation=" + kTerrariumUrl +
        " OceanMask=mbtiles://" + mainFolder + "/Earth/Mask_lv3.mbtiles/{z}-{x}-{y}.tif"
        " ElevationEncoding=terrarium MaximumLevel=19 UseWebMercator=1 UseEarth3D=1 OriginBottomLeft=1"
        " TileElevationScale=2.0 TileSkirtRatio=" + skirtRatio;
    osg::ref_ptr<osgDB::Options> earthOptions = new osgDB::Options(earthURLs);
    earthOptions->setPluginData("UrlPathFunction", (void*)createCustomPath);
    earthOptions->setPluginData("ElevationFilterFunction", (void*)hkElevationFilter);

    osg::ref_ptr<osg::Node> earth = osgDB::readNodeFile("0-0-0.verse_tms", earthOptions.get());
    if (!earth) { OSG_FATAL << "Main earth scene is missing!\n"; return 1; }

    // 启动磁盘预热(后台 detach 线程,进程退出即回收)。EARTH_PREFETCH=最大 zoom(默认 4,0=关)。
    {
        const char* pfEnv = getenv("EARTH_PREFETCH");
        int prefetchZ = pfEnv ? atoi(pfEnv) : 4;
        if (prefetchZ > 0) std::thread(prefetchLowLODGlobe, prefetchZ).detach();
    }

    // Configure scene components
    osgVerse::EarthAtmosphereOcean earthRenderingUtils;
    osg::ref_ptr<osg::MatrixTransform> earthRoot = new osg::MatrixTransform;
    std::vector<osg::Camera*> cameras = configureEarthRendering(
        viewer, earthRoot.get(), earth.get(), earthRenderingUtils, mainFolder, EARTH_INTERSECTION_MASK, w, h);
    earthRoot->getOrCreateStateSet()->setMode(GL_LIGHTING, osg::StateAttribute::OFF);

    osg::Camera* sceneCamera = cameras[0];
    //osg::Camera* atmosphereCamera = cameras[1];
    //osg::Camera* oceanCamera = cameras[2];
    sceneCamera->setLODScale(0.8f);
    sceneCamera->setNearFarRatio(0.00001);
    sceneCamera->setSmallFeatureCullingPixelSize(10.0f);
    sceneCamera->addChild(configureCityData(
        viewer, earthRoot.get(), earthRenderingUtils, mainFolder, EARTH_INTERSECTION_MASK, cityWaitingTiles));

    QuakeLayer* quakeLayer = nullptr;
    sceneCamera->addChild(configureQuakeData(viewer, earthRoot.get(), mainFolder, &quakeLayer));

    FlightLayer* flightLayer = nullptr;
    sceneCamera->addChild(configureFlightLayer(viewer, earthRoot.get(), mainFolder, &flightLayer));

    Tiles3DLayer* tiles3dLayer = nullptr;
    sceneCamera->addChild(configure3DTilesLayer(viewer, earthRoot.get(), mainFolder, &tiles3dLayer));

    osg::ref_ptr<PrecipController> precip = configurePrecipLayer(viewer);

    osg::ref_ptr<osg::Group> root = new osg::Group;
    root->addChild(earthRoot.get());
#if !SIMPLE_VERSION
    root->addChild(configureUI(viewer, earthRoot.get(), mainFolder, w, h));
#endif

    // Configure the manipulator
    osg::ref_ptr<osgVerse::EarthManipulator> earthManipulator = new osgVerse::EarthManipulator;
    earthManipulator->setIntersectionMask(EARTH_INTERSECTION_MASK);
    earthManipulator->setWorldNode(earth.get());
    earthManipulator->setThrowAllowed(manipulatorCanThrow);

    //osg::Vec3d pos = osgVerse::Coordinate::convertLLAtoECEF(
    //    osg::Vec3d(osg::inDegrees(0.0), osg::inDegrees(120.0), 10000.0));
    //earthManipulator->moveTo(pos, 0.0, 120.0);

    // Start the viewer
    osg::ref_ptr<osgVerse::FileCache> fileCache = new osgVerse::FileCache("earth_cache");
    osgDB::Registry::instance()->setFileCache(fileCache.get());

    //osg::ref_ptr<osgVerse::EarthProjectionMatrixCallback> epmcb =
    //    new osgVerse::EarthProjectionMatrixCallback(sceneCamera, earth->getBound().center());
    //epmcb->setNearFirstModeThreshold(2000.0);
    //sceneCamera->setClampProjectionMatrixCallback(epmcb.get());

    osgDB::DatabasePager* pager = new osgDB::DatabasePager;
    pager->setDrawablePolicy(osgDB::DatabasePager::USE_VERTEX_BUFFER_OBJECTS);
    // Online satellite/elevation tiles are fetched synchronously over HTTPS on the
    // pager's HTTP threads. Each deeper LOD level needs ~4x as many tiles, so tile
    // streaming throughput is the bottleneck for drilling down to street level.
    // Give the pager many more HTTP threads so tiles load in parallel (combined with
    // keep-alive connection reuse in loadFileData) instead of trickling in one host
    // round-trip at a time.
    pager->setDoPreCompile(true); pager->setUpThreads(20, 16);
    // 默认 targetMaximumNumberOfPageLOD=300，低于整个地球 z1-4(=340 块)就开始 LRU 过期卸载：
    // 深 zoom 进某地后，其它区域的中低 LOD 瓦片被卸载，缩放外推回全球时它们正在重载 → 露出默认
    // 底图("半个地球橙色")。提高上限让覆盖全球的低/中 LOD 瓦片常驻，外推秒回、不再重载露馅。
    pager->setTargetMaximumNumberOfPageLOD(1500);

    viewer.addEventHandler(new EnvironmentHandler(&earthRenderingUtils, mainFolder));
    viewer.addEventHandler(new osgViewer::StatsHandler);
    viewer.addEventHandler(new osgViewer::WindowSizeHandler);
    viewer.addEventHandler(new osgGA::StateSetManipulator(viewer.getCamera()->getOrCreateStateSet()));
    viewer.setRealizeOperation(new osgVerse::RealizeOperation);
    viewer.setCameraManipulator(earthManipulator.get());
    viewer.setDatabasePager(pager);
    viewer.setSceneData(root.get());
    //viewer.setThreadingModel(osgViewer::Viewer::SingleThreaded);

    // 图层注册（P0：底图 + 标注）
    LayerManager layerMgr;
    {
        OverlayLayer base; base.id = "base"; base.displayName = u8"卫星影像";
        base.group = u8"底图 / 标注"; base.enabled = true; base.hasOpacity = false;
        layerMgr.add(base);   // 基础底图，常开、无开关动作

        OverlayLayer labels; labels.id = "labels"; labels.displayName = u8"路网·地名";
        labels.group = u8"底图 / 标注"; labels.enabled = true; labels.hasOpacity = true; labels.opacity = 1.0f;
        osgVerse::EarthAtmosphereOcean* eptr = &earthRenderingUtils;
        labels.apply = [eptr](const OverlayLayer& l) {
            float v = l.enabled ? l.opacity : 0.0f;
            if (eptr->commonUniforms.count("LabelOpacity"))
                eptr->commonUniforms["LabelOpacity"]->set(v);
        };
        layerMgr.add(labels);

        LayerManager* lmptr = &layerMgr;
        PrecipController* pcptr = precip.get();
        // OVERLAY 槽可见度:降水开→降水透明度;否则云图开→云图透明度;都关→0。
        auto applyOverlayOpacity = [eptr, lmptr]() {   // eptr/lmptr 为裸指针,浅复制进 lambda(对象会话期存活)
            float op = 0.0f;
            OverlayLayer* cl = lmptr->find("clouds");
            OverlayLayer* pp = lmptr->find("precip");
            if (pp && pp->enabled) op = pp->opacity;
            else if (cl && cl->enabled) op = cl->opacity;
            if (eptr->commonUniforms.count("Overlay2Opacity"))
                eptr->commonUniforms["Overlay2Opacity"]->set(op);
        };

        OverlayLayer clouds; clouds.id = "clouds"; clouds.displayName = u8"GIBS 影像/云图";
        clouds.group = u8"影像 / 天气"; clouds.enabled = false; clouds.hasOpacity = true; clouds.opacity = 0.7f;
        clouds.apply = [lmptr, pcptr, applyOverlayOpacity](const OverlayLayer& l) {
            if (l.enabled)   // 互斥:开云图 → 关降水 + OVERLAY 切回 gibs
            {
                if (OverlayLayer* pp = lmptr->find("precip")) pp->enabled = false;
                if (pcptr) pcptr->setEnabled(false);
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, "gibs");
            }
            applyOverlayOpacity();
        };
        layerMgr.add(clouds);

        OverlayLayer precipL; precipL.id = "precip"; precipL.displayName = u8"降水雷达 RainViewer";
        precipL.group = u8"影像 / 天气"; precipL.enabled = false; precipL.hasOpacity = true; precipL.opacity = 0.85f;
        precipL.apply = [lmptr, pcptr, applyOverlayOpacity](const OverlayLayer& l) {
            if (l.enabled)   // 互斥:开降水 → 关云图;先清 OVERLAY,待控制器拼好 RV 模板再上
            {
                if (OverlayLayer* cl = lmptr->find("clouds")) cl->enabled = false;
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, "");
                if (pcptr) pcptr->setEnabled(true);   // 控制器抓帧后由主线程 FRAME handler setLayerPath(OVERLAY, RV模板)
            }
            else   // 关降水 → OVERLAY 回 gibs(云图开则可见,否则 opacity 0)
            {
                if (pcptr) pcptr->setEnabled(false);
                osgVerse::TileManager::instance()->setLayerPath(osgVerse::TileCallback::OVERLAY, "gibs");
            }
            applyOverlayOpacity();
        };
        layerMgr.add(precipL);

        OverlayLayer quakes; quakes.id = "quakes"; quakes.displayName = u8"地震 (USGS)";
        quakes.group = u8"实时数据 / Live"; quakes.enabled = false; quakes.hasOpacity = false;
        QuakeLayer* qptr = quakeLayer;
        quakes.apply = [qptr](const OverlayLayer& l) { if (qptr) qptr->setEnabled(l.enabled); };
        layerMgr.add(quakes);

        OverlayLayer flights; flights.id = "flights"; flights.displayName = u8"航班 (OpenSky)";
        flights.group = u8"实时数据 / Live"; flights.enabled = false; flights.hasOpacity = false;
        FlightLayer* fptr = flightLayer;
        flights.apply = [fptr](const OverlayLayer& l) { if (fptr) fptr->setEnabled(l.enabled); };
        layerMgr.add(flights);

        // 香港实景三维(地政总署 3D Visualisation Map,Cesium 3D Tiles 流式)。
        // 懒加载:首次勾选才联网;官方使用条款要求署名 → subtitle 常显数据来源。
        OverlayLayer hk3d; hk3d.id = "hk3d"; hk3d.displayName = u8"香港实景三维 (LandsD)";
        hk3d.group = u8"三维城市 / 3D City"; hk3d.enabled = false; hk3d.hasOpacity = false;
        hk3d.subtitle = u8"© 香港特区政府地政总署 data.map.gov.hk";
        Tiles3DLayer* tptr = tiles3dLayer;
        hk3d.apply = [tptr](const OverlayLayer& l) { if (tptr) tptr->setEnabled(l.enabled); };
        layerMgr.add(hk3d);
    }
    // 同步初始状态到 uniform（apply 只在交互时触发，这里推一次初值）
    if (OverlayLayer* lbl = layerMgr.find("labels"))
        layerMgr.setEnabled("labels", lbl->enabled);
    if (OverlayLayer* cl = layerMgr.find("clouds"))
    {
        // 默认关 → Overlay2Opacity 保持 0。EARTH_CLOUDS=<不透明度> 可在 headless/脚本里强制
        // 开启 GIBS 影像/云图层(取值即不透明度，0=关)，供无界面验证用（同 EARTH_TILT 测试钩子）。
        const char* cloudsEnv = getenv("EARTH_CLOUDS");
        if (cloudsEnv && *cloudsEnv)
        {
            float op = (float)atof(cloudsEnv);
            cl->enabled = (op > 0.0f);
            cl->opacity = (op < 0.0f) ? 0.0f : (op > 1.0f ? 1.0f : op);
        }
        layerMgr.setEnabled("clouds", cl->enabled);  // default off → Overlay2Opacity stays 0
    }
    if (OverlayLayer* qk = layerMgr.find("quakes"))
    {
        // EARTH_QUAKES=<非0> 强制开启地震层(headless 验证用)。类似 EARTH_CLOUDS,
        // 但本层无不透明度,取值仅作 0/1 开关。
        const char* qkEnv = getenv("EARTH_QUAKES");
        if (qkEnv && *qkEnv) qk->enabled = (atoi(qkEnv) != 0);
        layerMgr.setEnabled("quakes", qk->enabled);
    }
    if (OverlayLayer* pp = layerMgr.find("precip"))
    {
        // EARTH_PRECIP=1 强制开启降水层(headless 验证用;与 EARTH_CLOUDS 互斥,后开者生效)。
        const char* pEnv = getenv("EARTH_PRECIP");
        if (pEnv && *pEnv) pp->enabled = (atoi(pEnv) != 0);
        layerMgr.setEnabled("precip", pp->enabled);
    }
    if (OverlayLayer* fl = layerMgr.find("flights"))
    {
        // EARTH_FLIGHTS=<非0> 强制开启航班层(headless 验证用)。
        const char* flEnv = getenv("EARTH_FLIGHTS");
        if (flEnv && *flEnv) fl->enabled = (atoi(flEnv) != 0);
        layerMgr.setEnabled("flights", fl->enabled);
    }
    if (OverlayLayer* t3 = layerMgr.find("hk3d"))
    {
        // EARTH_3DTILES 已设置(=1 默认源 / =<url> 换源,见 tiles3d_data.cpp)→ 随启动开启;
        // 值为 0 视作显式关闭。未设置 → 默认关,由 UI 勾选触发懒加载。
        const char* tEnv = getenv("EARTH_3DTILES");
        if (tEnv && *tEnv) t3->enabled = (std::string(tEnv) != "0");
        layerMgr.setEnabled("hk3d", t3->enabled);
    }

    // ---- AI Chat（spec: docs/superpowers/specs/2026-07-02-earth-ai-chat-design.md）----
    // 无 EARTH_AI_KEY 且无 EARTH_AI_FAKE → aiCore 为空，后续全部跳过，零影响。
    // registry/aiCore 用裸指针、进程生命周期存活，与本文件其余单例/裸 new 风格一致。
    earthai::ToolRegistry* aiRegistry = new earthai::ToolRegistry;
    earthai::AIChatCore* aiCore = nullptr;
    {
        earthai::Tool fly; fly.name = "fly_to";
        fly.description = u8"把相机飞到指定经纬度与高度。用户说地名时你自己换算经纬度。";
        fly.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"lat\":{\"type\":\"number\"},\"lon\":{\"type\":\"number\"},"
            "\"alt_km\":{\"type\":\"number\",\"description\":\"default 50\"}},"
            "\"required\":[\"lat\",\"lon\"]}";
        osgVerse::EarthManipulator* mani = earthManipulator.get();
        fly.execute = [mani](const picojson::value& a) {
            double lat = a.get("lat").get<double>(), lon = a.get("lon").get<double>();
            double alt = a.contains("alt_km") ? a.get("alt_km").get<double>() : 50.0;
            mani->setByEye(osg::inDegrees(lat), osg::inDegrees(lon), alt * 1000.0);
            OSG_NOTICE << "[AIChat] fly_to " << lat << "," << lon << "," << alt << "km" << std::endl;
            picojson::object r; r["ok"] = picojson::value(true); return picojson::value(r);
        };
        aiRegistry->add(fly);

        earthai::Tool setLayer; setLayer.name = "set_layer";
        setLayer.description = u8"开关或调整一个数据图层。可用 id："
            u8"labels(路网·地名)、clouds(GIBS 影像/云图)、precip(降水雷达)、"
            u8"quakes(实时地震)、flights(实时航班)、hk3d(香港实景三维)。"
            u8"opacity 仅对支持透明度的图层有效（可选，0-1）。";
        setLayer.parametersJson = "{\"type\":\"object\",\"properties\":{"
            "\"id\":{\"type\":\"string\"},\"enabled\":{\"type\":\"boolean\"},"
            "\"opacity\":{\"type\":\"number\"}},\"required\":[\"id\",\"enabled\"]}";
        LayerManager* lmptr = &layerMgr;
        setLayer.execute = [lmptr](const picojson::value& a) {
            std::string id = a.get("id").get<std::string>();
            bool enabled = a.get("enabled").get<bool>();
            OverlayLayer* l = lmptr->find(id);
            if (!l)
            {
                std::string avail;
                std::vector<OverlayLayer>& all = lmptr->layers();
                for (size_t i = 0; i < all.size(); ++i) avail += (i ? "," : "") + all[i].id;
                picojson::object err;
                err["error"] = picojson::value("unknown layer id " + id + ", available: " + avail);
                return picojson::value(err);
            }
            if (!l->apply)   // "base" 底图常开、无开关动作
            {
                picojson::object err; err["error"] = picojson::value("layer not togglable");
                return picojson::value(err);
            }
            lmptr->setEnabled(id, enabled);
            if (a.contains("opacity") && l->hasOpacity)
                lmptr->setOpacity(id, (float)a.get("opacity").get<double>());
            OSG_NOTICE << "[AIChat] set_layer " << id << " " << (enabled ? "on" : "off") << std::endl;
            picojson::object r;
            r["ok"] = picojson::value(true); r["layer"] = picojson::value(id);
            r["enabled"] = picojson::value(enabled);
            return picojson::value(r);
        };
        aiRegistry->add(setLayer);

        earthai::Tool viewState; viewState.name = "get_view_state";
        viewState.description = u8"读取当前相机的经纬度/高度，以及各图层开关状态。";
        viewState.parametersJson = "{\"type\":\"object\",\"properties\":{}}";
        osgVerse::EarthManipulator* maniV = earthManipulator.get();
        viewState.execute = [maniV, lmptr](const picojson::value&) {
            // 与 EarthControlUI「相机 Camera」小节同一取值方式：computeEyeLatLonHeight()
            // 返回 (纬度弧度, 经度弧度, 高度米)。
            osg::Vec3d lla = maniV->computeEyeLatLonHeight();
            picojson::object r;
            r["lat"] = picojson::value(osg::RadiansToDegrees(lla[0]));
            r["lon"] = picojson::value(osg::RadiansToDegrees(lla[1]));
            r["alt_km"] = picojson::value(lla[2] / 1000.0);
            picojson::object layersObj;
            std::vector<OverlayLayer>& all = lmptr->layers();
            for (size_t i = 0; i < all.size(); ++i)
                layersObj[all[i].id] = picojson::value(all[i].enabled);
            r["layers"] = picojson::value(layersObj);
            OSG_NOTICE << "[AIChat] get_view_state" << std::endl;
            return picojson::value(r);
        };
        aiRegistry->add(viewState);
    }
    const char* aiKey = getenv("EARTH_AI_KEY");
    const char* aiFake = getenv("EARTH_AI_FAKE");
    if (aiFake && *aiFake)
    {
        earthai::FakeProvider* fp = new earthai::FakeProvider;
        if (!fp->loadFromFile(aiFake)) OSG_WARN << "[AIChat] bad fake script: " << aiFake << std::endl;
        aiCore = new earthai::AIChatCore(fp, aiRegistry);
    }
    else if (aiKey && *aiKey)
    {
        const char* m = getenv("EARTH_AI_MODEL");
        earthai::GeminiProvider* gp = new earthai::GeminiProvider(
            aiKey, (m && *m) ? m : "gemini-2.5-flash");
        gp->setSystemPrompt(u8"你是 EarthExplorer 三维地球应用的中文助手。优先使用提供的工具完成用户请求；"
                            u8"用户提到地名时自行换算经纬度；回答保持简洁；不要编造工具没有返回的数据。");
        aiCore = new earthai::AIChatCore(gp, aiRegistry);
    }
    if (aiCore)
    {
        // FRAME drain（工具必须主线程执行；与降水层 FRAME handler 同模式）
        class AIFrameHandler : public osgGA::GUIEventHandler
        {
            earthai::AIChatCore* _core;
        public:
            AIFrameHandler(earthai::AIChatCore* c) : _core(c) {}
            virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter&)
            {
                if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME) _core->drainMainThread();
                return false;
            }
        };
        viewer.addEventHandler(new AIFrameHandler(aiCore));
        const char* autoSubmit = getenv("EARTH_AI_AUTOSUBMIT");   // headless E2E 用
        if (autoSubmit && *autoSubmit) aiCore->submit(autoSubmit);
    }

    // ImGui 控制面板 — 挂到最终 HUD 相机（cameras[3]），确保在地球图像之上绘制
    osg::ref_ptr<osgVerse::ImGuiManager> imgui = new osgVerse::ImGuiManager;
    imgui->setChineseSimplifiedFont(MISC_DIR + std::string("LXGWFasmartGothic.otf"));
    EarthControlUI* ctrlUI = new EarthControlUI(earthManipulator.get(), &earthRenderingUtils, &viewer);
    ctrlUI->_layers = &layerMgr;
    ctrlUI->_quake = quakeLayer;
    ctrlUI->_flight = flightLayer;
    imgui->initialize(ctrlUI, false);
    imgui->addToView(&viewer, cameras[3]);  // cameras[3] = finalCamera (HUD, renders to screen)

    int screenNo = 0; arguments.read("--screen", screenNo);
    viewer.setUpViewOnSingleScreen(screenNo);

    if (gotoLat < 1.0e8)  // --goto 指定了起始视点
        earthManipulator->setByEye(osg::inDegrees(gotoLat), osg::inDegrees(gotoLon), gotoAltKm * 1000.0);

    // Headless verification aid only (no effect on the actual fix/rendering):
    // EARTH_TILT=<radians> applies a startup camera pitch after --goto, so oblique views
    // — where LOD-boundary cracks and terrain penetration appear — can be reproduced and
    // captured without a live mouse drag. makeDeltaTilt subtracts its arg from _tilt.
    const char* tiltEnv = getenv("EARTH_TILT");
    if (tiltEnv && tiltEnv[0] && gotoLat < 1.0e8)
        earthManipulator->makeDeltaTilt(-(float)atof(tiltEnv));

    // Headless auto-capture: render a fixed number of frames (letting the database
    // pager stream tiles) then grab the GL framebuffer to a PNG. Used to verify the
    // earth renders without relying on OS screen-capture permissions.
    const char* autoCap = getenv("EARTH_AUTOCAP");
    if (autoCap && autoCap[0])
    {
        osg::ref_ptr<osgViewer::ScreenCaptureHandler::WriteToFile> writer =
            new osgViewer::ScreenCaptureHandler::WriteToFile(
                "/tmp/earth_capture", "png",
                osgViewer::ScreenCaptureHandler::WriteToFile::OVERWRITE);
        osg::ref_ptr<osgViewer::ScreenCaptureHandler> capturer =
            new osgViewer::ScreenCaptureHandler(writer.get(), 1);
        viewer.addEventHandler(capturer.get());

        int total = atoi(autoCap); if (total < 100) total = 600;
        // Optional: aim the sun at the camera-facing hemisphere so the day side
        // is visible in the capture. EARTH_SUN_TO_CAMERA=1 (or -1 to flip sign).
        const char* sunCam = getenv("EARTH_SUN_TO_CAMERA");
        float sunSign = (sunCam && atof(sunCam) < 0.0) ? -1.0f : 1.0f;
        // Optional per-frame delay (ms) so the database pager has real wall-clock
        // time to stream deep online tiles before the capture frame.
        const char* fsleep = getenv("EARTH_FRAME_SLEEP_MS");
        int frameSleepMs = fsleep ? atoi(fsleep) : 0;
        // Optional: pin the sun to an exact azimuth/elevation (UI 滑块 convention) to
        // reproduce a reported lighting condition headless. EARTH_SUN_AZEL=az,el (度)。
        const char* sunAzEl = getenv("EARTH_SUN_AZEL");
        bool useSunAzEl = false; osg::Vec3 sunAzElDir(-1.0f, 0.0f, 0.0f);
        if (sunAzEl && *sunAzEl)
        {
            float az = 0.0f, el = 0.0f;
            if (sscanf(sunAzEl, "%f,%f", &az, &el) >= 1)
            {
                float ar = osg::DegreesToRadians(az), er = osg::DegreesToRadians(el);
                sunAzElDir.set(cosf(er) * cosf(ar), cosf(er) * sinf(ar), sinf(er));
                useSunAzEl = true;
            }
        }
        if (!viewer.isRealized()) viewer.realize();
        for (int i = 0; i < total && !viewer.done(); ++i)
        {
            if (sunCam)
            {
                osg::Vec3d eye, center, up;
                viewer.getCamera()->getViewMatrixAsLookAt(eye, center, up);
                osg::Vec3 dir(eye); dir.normalize(); dir *= sunSign;
                earthRenderingUtils.commonUniforms["WorldSunDir"]->set(dir);
            }
            if (useSunAzEl)
                earthRenderingUtils.commonUniforms["WorldSunDir"]->set(sunAzElDir);
            viewer.frame();
            if (frameSleepMs > 0) OpenThreads::Thread::microSleep((unsigned int)frameSleepMs * 1000);
            if (i == total - 5) capturer->captureNextFrame(viewer);
        }
        viewer.frame();  // flush the pending capture
        return 0;
    }
    return viewer.run();
}
