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
        requests::Request req(new HttpRequest);
        req->method = HTTP_GET;
        req->url = kQuakeFeedUrl;
        req->timeout = 15;   // 秒:界定退出时后台线程 join() 的最坏等待
        requests::Response resp = requests::request(req);
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

    // 点精灵:VS 写 gl_PointSize(取自 texcoord0.x),FS 画圆盘、圆外 discard。
    // 独立 shader(非 globe 着色器),不接大气散射 → 标记昼夜两侧都清晰可读。
    // MRT 双输出(COLOR_BUFFER0 颜色 + COLOR_BUFFER1 掩码),同 city_data。
    const char* quakeVertCode = {
        "VERSE_VS_OUT vec4 pointColor;\n"
        "void main() {\n"
        "    pointColor = osg_Color;\n"
        "    gl_PointSize = osg_MultiTexCoord0.x;\n"
        "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex;\n"
        "}\n"
    };
    const char* quakeFragCode = {
        "VERSE_FS_IN vec4 pointColor;\n"
        "#ifdef VERSE_GLES3\n"
        "layout(location = 0) VERSE_FS_OUT vec4 fragColor;\n"
        "layout(location = 1) VERSE_FS_OUT vec4 fragOrigin;\n"
        "#endif\n"
        "void main() {\n"
        "    vec2 d = gl_PointCoord - vec2(0.5);\n"
        "    float r2 = dot(d, d);\n"
        "    if (r2 > 0.25) discard;\n"
        "    float edge = 1.0 - smoothstep(0.16, 0.25, r2);\n"
        "    vec3 rgb = mix(pointColor.rgb * 0.7, pointColor.rgb, edge);\n"
        "#ifdef VERSE_GLES3\n"
        "    fragColor = vec4(rgb, 1.0); fragOrigin = vec4(1.0);\n"
        "#else\n"
        "    gl_FragData[0] = vec4(rgb, 1.0); gl_FragData[1] = vec4(1.0);\n"
        "#endif\n"
        "}\n"
    };

    // 深度→颜色(USGS 习惯:浅红 → 中橙黄 → 深蓝)。
    osg::Vec4 depthColor(double depthKm)
    {
        if (depthKm < 70.0)  return osg::Vec4(0.90f, 0.18f, 0.15f, 1.0f);   // 浅:红
        if (depthKm < 300.0) return osg::Vec4(0.98f, 0.66f, 0.12f, 1.0f);   // 中:橙黄
        return osg::Vec4(0.20f, 0.45f, 0.95f, 1.0f);                        // 深:蓝
    }

    // 震级→屏幕点像素大小。
    float magSizePx(double mag)
    {
        float s = 4.0f + (float)(mag - 2.5) * 3.0f;
        return s < 4.0f ? 4.0f : (s > 28.0f ? 28.0f : s);
    }

#ifndef GL_PROGRAM_POINT_SIZE
#define GL_PROGRAM_POINT_SIZE 0x8642
#endif

    static const float kDragThreshPx2    = 25.0f;   // 5px 拖动阈值的平方
    static const float kPickExtraRadiusPx = 10.0f;  // 拾取容差:点半径外再加的像素裕量(小点也好点)

    // 由一批 Quake 构建一个 Geode(GL_POINTS)。
    osg::Geode* buildQuakeGeode(const std::vector<Quake>& qs, osg::StateSet* sharedSS)
    {
        osg::ref_ptr<osg::Geometry> geom = new osg::Geometry;
        osg::ref_ptr<osg::Vec3Array> verts = new osg::Vec3Array;
        osg::ref_ptr<osg::Vec4Array> colors = new osg::Vec4Array;
        osg::ref_ptr<osg::Vec2Array> sizes = new osg::Vec2Array;  // .x = 点像素大小
        for (size_t i = 0; i < qs.size(); ++i)
        {
            verts->push_back(qs[i].ecef);
            colors->push_back(depthColor(qs[i].depthKm));
            sizes->push_back(osg::Vec2(magSizePx(qs[i].mag), 0.0f));
        }
        geom->setVertexArray(verts.get());
        geom->setColorArray(colors.get(), osg::Array::BIND_PER_VERTEX);
        geom->setTexCoordArray(0, sizes.get());
        geom->addPrimitiveSet(new osg::DrawArrays(GL_POINTS, 0, (GLsizei)verts->size()));
        geom->setUseDisplayList(false); geom->setUseVertexBufferObjects(true);
        geom->setCullingActive(false);

        osg::ref_ptr<osg::Geode> geode = new osg::Geode;
        geode->addDrawable(geom.get());
        geode->setStateSet(sharedSS);
        return geode.release();
    }

    // 前向声明
    class QuakeLayerImpl;

    class SyncCallback : public osg::NodeCallback
    {
    public:
        SyncCallback(QuakeLayerImpl* o) : _owner(o) {}
        virtual void operator()(osg::Node* node, osg::NodeVisitor* nv);  // 仅声明,定义在 QuakeLayerImpl 之后
    protected:
        QuakeLayerImpl* _owner;
    };

    // 抓取线程:仅图层开启时每 ~60s 拉一次(分片 sleep 便于秒退)。不碰 GL/场景图。
    class FetchThread : public OpenThreads::Thread
    {
    public:
        FetchThread(QuakeLayerImpl* o) : _owner(o), _done(false) {}
        virtual int cancel() { _done = true; return OpenThreads::Thread::cancel(); }
        virtual void run();   // 定义在 QuakeLayerImpl 之后(需其完整定义)
    protected:
        QuakeLayerImpl* _owner; bool _done;
    };

    // 内部具体类。返回的 Group 经 setUserData 持有本对象;
    // 本对象用裸指针引用 Group(不拥有它),避免引用环。
    class QuakeLayerImpl : public osg::Referenced, public QuakeLayer
    {
    public:
        QuakeLayerImpl() : _root(nullptr), _enabled(false), _dirty(false), _thread(nullptr) {}
        virtual void setEnabled(bool on) { _enabled = on; if (_root) _root->setNodeMask(on ? ~0u : 0u); if (!on) clearSelected(); }

        void startFetch() { if (!_thread) { _thread = new FetchThread(this); _thread->startThread(); } }
        virtual bool isEnabled() const { return _enabled; }
        virtual QuakeInfo getSelected() const
        {
            OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_selMutex);
            return _selected;
        }
        virtual void clearSelected()
        { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_selMutex); _selected = QuakeInfo(); }

        // 屏幕拾取:相机 + 窗口鼠标坐标(y 向上)。选最近的前半球地震。仅主线程调用。
        void pickAt(osg::Camera* cam, float mx, float my)
        {
            if (!_enabled || _quakes.empty() || !cam->getViewport()) return;
            osg::Vec3d eye, center, up; cam->getViewMatrixAsLookAt(eye, center, up);
            osg::Matrixd VPW = cam->getViewMatrix() * cam->getProjectionMatrix()
                             * cam->getViewport()->computeWindowMatrix();
            double bestD2 = 1e18; int best = -1;
            for (size_t i = 0; i < _quakes.size(); ++i)
            {
                const osg::Vec3d& P = _quakes[i].ecef;
                if ((eye * P) <= (P * P)) continue;            // 前半球:dot(eye,P) > |P|^2(已能剔背面)
                osg::Vec3d win = P * VPW;
                // 只用 win.x/y 像素坐标。不查 win.z:事件阶段读到的相机投影近/远非渲染时
                // 那套(osgViewer 在 cull 才按场景包围盒重算近/远),地表点 z 会饱和到 ≈1.0001,
                // 用 [0,1] 闸门会误剔全部点(本 bug 根因)。前半球测试已负责可见性剔除。
                double d2 = (win.x() - mx) * (win.x() - mx) + (win.y() - my) * (win.y() - my);
                float tol = magSizePx(_quakes[i].mag) * 0.5f + kPickExtraRadiusPx;
                if (d2 < (double)(tol * tol) && d2 < bestD2) { bestD2 = d2; best = (int)i; }
            }
            if (best >= 0)
            {
                const Quake& q = _quakes[best];
                QuakeInfo info; info.valid = true;
                info.lon = q.lon; info.lat = q.lat; info.depthKm = q.depthKm; info.mag = q.mag;
                info.timeMs = q.timeMs; info.place = q.place; info.url = q.url;
                OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_selMutex); _selected = info;
            }
        }

        // 测试钩子 EARTH_QUAKE_PICKDBG:headless 投影全部地震点 + 自拾取自测(无界面验证拾取链路)。
        void debugPick(osg::Camera* cam)
        {
            std::cout << "[PickDBG] enabled=" << _enabled << " nQuakes=" << _quakes.size();
            if (!cam || !cam->getViewport()) { std::cout << " NO_VIEWPORT\n"; return; }
            osg::Vec3d eye, center, up; cam->getViewMatrixAsLookAt(eye, center, up);
            osg::Matrixd VPW = cam->getViewMatrix() * cam->getProjectionMatrix()
                             * cam->getViewport()->computeWindowMatrix();
            std::cout << " vp=" << (int)cam->getViewport()->width() << "x" << (int)cam->getViewport()->height()
                      << " eyeLen=" << eye.length() << "\n";
            int firstFront = -1;
            for (size_t i = 0; i < _quakes.size(); ++i)
            {
                const osg::Vec3d& P = _quakes[i].ecef;
                bool front = (eye * P) > (P * P);
                osg::Vec3d win = P * VPW;
                std::cout << "[PickDBG] q" << i << " front=" << front
                          << " win=(" << win.x() << "," << win.y() << "," << win.z() << ")"
                          << " mag=" << _quakes[i].mag << " " << _quakes[i].place << "\n";
                if (front && firstFront < 0) firstFront = (int)i;
            }
            if (firstFront >= 0)
            {
                osg::Vec3d w = _quakes[firstFront].ecef * VPW;
                std::cout << "[PickDBG] self-pick q" << firstFront << " at (" << w.x() << "," << w.y() << ")\n";
                clearSelected();
                pickAt(cam, (float)w.x(), (float)w.y());
                QuakeInfo s = getSelected();
                std::cout << "[PickDBG] after self-pick: valid=" << s.valid << " place=" << s.place << "\n";
            }
            else std::cout << "[PickDBG] no front-facing on-screen quake\n";
        }

        osg::Group* root() { return _root; }

        // 后台线程交付新快照(加锁)。
        void postSnapshot(const std::vector<Quake>& qs)
        { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex); _pending = qs; _dirty = true; }

        // 主线程(update 遍历)调用:若有新数据则重建 geode。
        void syncIfDirty()
        {
            std::vector<Quake> qs;
            { OpenThreads::ScopedLock<OpenThreads::Mutex> lk(_mutex);
              if (!_dirty) return; qs = _pending; _dirty = false; }
            _quakes = qs;
            if (_geode.valid()) _root->removeChild(_geode.get());
            _geode = buildQuakeGeode(qs, _ss.get());
            _root->addChild(_geode.get());
        }

        // 仅主线程读(syncIfDirty 在 update 遍历写;Task 5 拾取在事件处理读,同主线程)。
        const std::vector<Quake>& quakes() const { return _quakes; }

        osg::Group* buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);   // 默认关

            osg::Shader* vs = new osg::Shader(osg::Shader::VERTEX, quakeVertCode);
            osg::Shader* fs = new osg::Shader(osg::Shader::FRAGMENT, quakeFragCode);
            vs->setName("Quake_VS"); fs->setName("Quake_FS");
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
        virtual ~QuakeLayerImpl()
        { if (_thread) { _thread->cancel(); _thread->join(); delete _thread; _thread = nullptr; } }

        osg::Group* _root;   // 裸指针:由返回节点经 UserData 拥有
        osg::ref_ptr<osg::Geode> _geode;
        osg::ref_ptr<osg::StateSet> _ss;
        std::vector<Quake> _quakes, _pending;
        OpenThreads::Mutex _mutex;
        bool _enabled, _dirty;
        FetchThread* _thread;
        QuakeInfo _selected;
        mutable OpenThreads::Mutex _selMutex;
    };

    class QuakePickHandler : public osgGA::GUIEventHandler
    {
    public:
        QuakePickHandler(QuakeLayerImpl* o) : _owner(o), _downX(0.0f), _downY(0.0f), _pushed(false) {}
        virtual bool handle(const osgGA::GUIEventAdapter& ea, osgGA::GUIActionAdapter& aa)
        {
            if (ea.getEventType() == osgGA::GUIEventAdapter::PUSH
                && ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
            { _downX = ea.getX(); _downY = ea.getY(); _pushed = true; }
            else if (ea.getEventType() == osgGA::GUIEventAdapter::RELEASE
                     && ea.getButton() == osgGA::GUIEventAdapter::LEFT_MOUSE_BUTTON)
            {
                float dx = ea.getX() - _downX, dy = ea.getY() - _downY;
                if (_pushed && dx * dx + dy * dy < kDragThreshPx2)   // 点击(非拖动)
                {
                    osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
                    osg::Camera* cam = view->getCamera();
                    const osg::Viewport* vp = cam->getViewport();
                    if (vp)
                    {
                        float my = ea.getY();
                        if (ea.getMouseYOrientation() == osgGA::GUIEventAdapter::Y_INCREASING_DOWNWARDS)
                            my = vp->height() - my;
                        _owner->pickAt(cam, ea.getX(), my);
                    }
                }
                _pushed = false;
            }
            else if (ea.getEventType() == osgGA::GUIEventAdapter::FRAME)
            {
                // EARTH_QUAKE_PICKDBG=1:场景稳定后跑一次投影/自拾取自测(headless 验证用)
                static const char* dbg = getenv("EARTH_QUAKE_PICKDBG");
                static bool dbgDone = false;
                if (dbg && *dbg && !dbgDone)
                {
                    osgViewer::View* view = static_cast<osgViewer::View*>(&aa);
                    if (view->getFrameStamp()->getFrameNumber() > 250)
                    { _owner->debugPick(view->getCamera()); dbgDone = true; }
                }
            }
            return false;   // 不拦截,放行给 manipulator
        }
    protected:
        QuakeLayerImpl* _owner; float _downX, _downY; bool _pushed;
    };

    void SyncCallback::operator()(osg::Node* node, osg::NodeVisitor* nv)
    { _owner->syncIfDirty(); traverse(node, nv); }

    void FetchThread::run()
    {
        const int kIntervalTicks = 600;   // 60s ÷ 100ms/tick
        int tick = 0;
        while (!_done)
        {
            if (_owner->isEnabled())
            {
                if (tick <= 0) { _owner->postSnapshot(fetchQuakes()); tick = kIntervalTicks; }
                else tick--;
            }
            else tick = 0;   // 关闭时,下次开启立即抓
            OpenThreads::Thread::microSleep(100000);  // 100ms
        }
        _done = true;
    }
}

osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                              const std::string& mainFolder, QuakeLayer** outLayer)
{
    osg::ref_ptr<QuakeLayerImpl> impl = new QuakeLayerImpl;
    osg::Group* root = impl->buildScene();
    root->setUserData(impl.get());
    if (outLayer) *outLayer = impl.get();
    impl->startFetch();   // 线程常驻;仅 isEnabled() 时才真正联网
    viewer.addEventHandler(new QuakePickHandler(impl.get()));
    return root;
}
