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
    // 简单半兰伯特光照(固定顶光),让立面有体积感;不接大气,自成一体。
    "    vec3 N = normalize(normalInWorld); \n"
    "    float ndl = max(dot(N, vec3(0.0, 0.0, 1.0)), 0.0); \n"
    "    float diffuse = 0.55 + 0.45 * ndl; \n"
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

osg::Node* configure3DTilesLayer(osgViewer::View& /*viewer*/,
                                 osg::Node* /*earthRoot*/,
                                 const std::string& /*mainFolder*/)
{
    osg::ref_ptr<osg::Group> group = new osg::Group;
    group->setName("Tiles3DLayer");

    const char* env = getenv("EARTH_3DTILES");
    if (!env || !env[0]) return group.release();   // 未设置 → 零影响

    std::string url(env);
    osg::ref_ptr<osg::Node> tiles;

    bool isHttp = (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0);
    if (isHttp)
    {
        // 网络 URL:通过 verse_web 读取器获取内容,并以 verse_tiles 解析
        // (参见 plugins/osgdb_3dtiles/ReaderWriter3dTiles.cpp 约第 49 行注释)
        osg::ref_ptr<osgDB::Options> opt = new osgDB::Options("Extension=verse_tiles");
        tiles = osgDB::readNodeFile(url + ".verse_web", opt.get());
    }
    else
    {
        tiles = osgDB::readNodeFile(url + ".verse_tiles");
    }

    if (tiles.valid())
    {
        group->addChild(tiles.get());
        // 给整层挂自带的网格着色器,覆盖从 earthCamera 继承来的 globe 程序。
        applyTilesShader(group->getOrCreateStateSet());
        const osg::BoundingSphere& bs = tiles->getBound();
        OSG_NOTICE << "[Tiles3D] loaded: " << url << "  bound center=" << bs.center()
                   << " radius=" << bs.radius() << std::endl;
    }
    else
    {
        OSG_WARN << "[Tiles3D] FAILED to load: " << url << std::endl;
    }

    return group.release();
}
