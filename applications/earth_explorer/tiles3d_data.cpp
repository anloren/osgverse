#include <osg/Group>
#include <osg/Node>
#include <osgDB/ReadFile>
#include <osgDB/Options>
#include <osgViewer/View>
#include <iostream>
#include "tiles3d_data.h"

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
        OSG_NOTICE << "[Tiles3D] loaded: " << url << std::endl;
    }
    else
    {
        OSG_WARN << "[Tiles3D] FAILED to load: " << url << std::endl;
    }

    return group.release();
}
