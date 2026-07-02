#include <osg/Group>
#include <osg/Node>
#include <osg/Program>
#include <osg/io_utils>
#include <osgDB/ReadFile>
#include <osgDB/Options>
#include <osgViewer/View>
#include <pipeline/Pipeline.h>
#include <pipeline/Utilities.h>
#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include "tiles3d_data.h"

// 3D Tiles(Cesium b3dm/glTF)几何在 sceneCamera(= earthCamera)下渲染。earthCamera 的
// StateSet 绑定了 scattering_globe 着色器程序(见 render_effects.cpp applyToGlobe),子节点
// 默认继承该程序。若 Tiles3DLayer 不自带程序,b3dm 建筑会被"地球地形着色器"渲染 —— 用地表
// clarity/大气混合把真实带贴图的楼渲成惨白破碎的平面片。因此这里必须给 3D Tiles 子图挂一个
// 自带的、采样 DiffuseMap(单元 0,LoadSceneGLTF 里材质就绑在单元 0)的网格着色器,覆盖继承的
// 地球着色器。着色器自成一体,不依赖大气/globe 专用 uniform,以免与 globe 管线耦合。
static const char* tilesVertCode = {
    "uniform mat4 osg_ViewMatrixInverse; \n"
    "VERSE_VS_OUT vec3 normalInWorld; \n"
    "VERSE_VS_OUT vec4 texCoord; \n"
    "void main() {\n"
    "    normalInWorld = normalize(vec3(osg_ViewMatrixInverse * vec4(VERSE_MATRIX_N * gl_Normal, 0.0))); \n"
    "    texCoord = osg_MultiTexCoord0; \n"
    "    gl_Position = VERSE_MATRIX_MVP * osg_Vertex; \n"
    "}\n"
};

static const char* tilesFragCode = {
    "uniform sampler2D DiffuseMap; \n"
    "VERSE_FS_IN vec3 normalInWorld; \n"
    "VERSE_FS_IN vec4 texCoord; \n"
    "#ifdef VERSE_GLES3\n"
    "layout(location = 0) VERSE_FS_OUT vec4 fragColor;\n"
    "layout(location = 1) VERSE_FS_OUT vec4 fragOrigin;\n"
    "#endif\n"
    "void main() {\n"
    "    vec4 baseColor = VERSE_TEX2D(DiffuseMap, texCoord.st); \n"
    // f2 的 glb 声明了 KHR_materials_unlit:摄影测量纹理自带烘焙光照,再打光就是双重
    // 着色——曾用半兰伯特(0.55-1.0)把网格整体压暗,和明亮底图对比强烈,任何未覆盖/
    // 未细化区域都成了亮补丁(斑驳感放大)。改为按数据本意 unlit 直出,仅留极轻的顶光
    // 起伏(0.92-1.0)保住立面体积感,同时亮度与底图基本一致。
    "    vec3 N = normalize(normalInWorld); \n"
    "    float ndl = max(dot(N, vec3(0.0, 0.0, 1.0)), 0.0); \n"
    "    float diffuse = 0.92 + 0.08 * ndl; \n"
    "    vec4 finalColor = vec4(baseColor.rgb * diffuse, baseColor.a); \n"
    "#ifdef VERSE_GLES3\n"
    "    fragColor = finalColor; \n"
    "    fragOrigin = vec4(0.0, 0.0, 1.0, 1.0); \n"
    "#else\n"
    "    gl_FragData[0] = finalColor; \n"
    "    gl_FragData[1] = vec4(0.0, 0.0, 1.0, 1.0); \n"
    "#endif\n"
    "}\n"
};

static void applyTilesShader(osg::StateSet* ss)
{
    osg::Shader* vs = new osg::Shader(osg::Shader::VERTEX, tilesVertCode);
    osg::Shader* fs = new osg::Shader(osg::Shader::FRAGMENT, tilesFragCode);
    vs->setName("Tiles3D_VS"); fs->setName("Tiles3D_FS");
    osgVerse::Pipeline::createShaderDefinitions(vs, 100, 130);
    osgVerse::Pipeline::createShaderDefinitions(fs, 100, 130);

    osg::ref_ptr<osg::Program> program = new osg::Program;
    program->addShader(vs); program->addShader(fs);
    // applyToGlobe() 绑定 globe 程序时不带 OVERRIDE(UtilitiesEx.cpp apply()),普通子节点自己的
    // 程序本就能替换继承值,不需要 OVERRIDE 才能生效。这里加 OVERRIDE 是防御性的:3D Tiles 内容
    // 来自外部数据源(Cesium tileset,非本项目自制),若某天遇到内嵌自带 Program/材质覆盖的瓦片,
    // OVERRIDE 能保证仍然用这份专用着色器渲染,不被瓦片自带的状态意外顶掉。
    ss->setAttributeAndModes(program.get(),
        osg::StateAttribute::ON | osg::StateAttribute::OVERRIDE);
    ss->getOrCreateUniform("DiffuseMap", osg::Uniform::INT)->set((int)0);  // 材质贴图绑在单元 0
}

// 默认数据源:香港地政总署「可视化三维地图」(3D Visualisation Map,f2 端点)。
// Cesium 3D Tiles + WGS84,实景摄影测量网格(KTX2 纹理);实测无 key 亦可访问,
// 官方要求署名(UI 图层开关下方有署名小字,见 earth_main.cpp 注册处)。
static const char* kDefaultTilesUrl =
    "https://data.map.gov.hk/api/3d-data/3dtiles/f2/tileset.json";

// 根 tileset 的同步网络读取(秒级)不能放主线程 → 首次开启时后台线程拉取,
// 结果经 _pending 槽由本回调(update 阶段,主线程)挂树。挂树后开关只切 NodeMask。
class Tiles3DLayerImpl : public Tiles3DLayer, public osg::NodeCallback
{
public:
    Tiles3DLayerImpl(osg::Group* group, const std::string& url)
        : _group(group), _url(url), _enabled(false), _loadStarted(false), _loadDone(false) {}

    virtual void setEnabled(bool on)
    {
        _enabled = on;
        OSG_INFO << "[Tiles3D] setEnabled(" << (on ? 1 : 0) << "), loadStarted=" << _loadStarted << std::endl;
        // NodeMask=0 时 update/cull 都不遍历 → 关闭即停:不发瓦片请求、不渲染。
        _group->setNodeMask(on ? ~0u : 0u);
        if (on && !_loadStarted)
        {
            _loadStarted = true;
            std::string url = _url;
            osg::ref_ptr<Tiles3DLayerImpl> self(this);   // 保活到线程结束
            std::thread([self, url]() {
                OSG_INFO << "[Tiles3D] background load begins: " << url << std::endl;
                osg::ref_ptr<osg::Node> tiles;
                bool isHttp = (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0);
                // EARTH_3DTILES_SSE=<阈值> 覆盖 LOD 细化激进度(默认 16;越小越早细化、
                // 越清晰,减轻"相邻瓦片细化深度不一"的拼布/斑驳感,代价是流量与内存)
                const char* sseEnv = getenv("EARTH_3DTILES_SSE");
                if (isHttp)
                {
                    // 网络 URL:通过 verse_web 读取器获取内容,并以 verse_tiles 解析
                    // (参见 plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp 约第 49 行注释)
                    osg::ref_ptr<osgDB::Options> opt = new osgDB::Options("Extension=verse_tiles");
                    if (sseEnv && *sseEnv) opt->setPluginStringData("MaxScreenSpaceError", sseEnv);
                    tiles = osgDB::readNodeFile(url + ".verse_web", opt.get());
                }
                else
                {
                    osg::ref_ptr<osgDB::Options> opt = new osgDB::Options;
                    if (sseEnv && *sseEnv) opt->setPluginStringData("MaxScreenSpaceError", sseEnv);
                    tiles = osgDB::readNodeFile(url + ".verse_tiles", opt.get());
                }

                std::lock_guard<std::mutex> guard(self->_mutex);
                self->_pending = tiles; self->_loadDone = true;
            }).detach();
        }
    }
    virtual bool isEnabled() const { return _enabled; }

    virtual void operator()(osg::Node* node, osg::NodeVisitor* nv)
    {
        if (_loadDone.exchange(false))
        {
            osg::ref_ptr<osg::Node> tiles;
            { std::lock_guard<std::mutex> guard(_mutex); tiles = _pending; _pending = NULL; }
            if (tiles.valid())
            {
                _group->addChild(tiles.get());
                const osg::BoundingSphere& bs = tiles->getBound();
                OSG_NOTICE << "[Tiles3D] loaded: " << _url << "  bound center=" << bs.center()
                           << " radius=" << bs.radius() << std::endl;
            }
            else
                OSG_WARN << "[Tiles3D] FAILED to load: " << _url << std::endl;
        }
        traverse(node, nv);
    }

protected:
    virtual ~Tiles3DLayerImpl() {}
    osg::observer_ptr<osg::Group> _group;   // group 持有本回调,防环引用
    std::string _url;
    std::mutex _mutex;
    osg::ref_ptr<osg::Node> _pending;
    bool _enabled, _loadStarted;
    std::atomic<bool> _loadDone;
};

osg::Node* configure3DTilesLayer(osgViewer::View& /*viewer*/,
                                 osg::Node* /*earthRoot*/,
                                 const std::string& /*mainFolder*/,
                                 Tiles3DLayer** outLayer)
{
    osg::ref_ptr<osg::Group> group = new osg::Group;
    group->setName("Tiles3DLayer");
    group->setNodeMask(0);   // 默认关(懒加载,不联网)

    // EARTH_3DTILES=<url> 换数据源;=1 用默认源(香港 f2)。是否随启动开启由
    // earth_main 的图层注册处统一决定(与 EARTH_QUAKES/EARTH_FLIGHTS 同模式)。
    std::string url = kDefaultTilesUrl;
    const char* env = getenv("EARTH_3DTILES");
    if (env && env[0] && std::string(env) != "1") url = env;

    // 给整层挂自带的网格着色器,覆盖从 earthCamera 继承来的 globe 程序。
    applyTilesShader(group->getOrCreateStateSet());

    Tiles3DLayerImpl* impl = new Tiles3DLayerImpl(group.get(), url);
    group->setUpdateCallback(impl);
    if (outLayer) *outLayer = impl;
    return group.release();
}
