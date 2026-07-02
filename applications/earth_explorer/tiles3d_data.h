#ifndef EARTH_TILES3D_DATA_H
#define EARTH_TILES3D_DATA_H

#include <string>
#include <osg/Node>
#include <osgViewer/View>

// 3D Tiles 城市图层对外接口。EarthControlUI / earth_main 只依赖这个抽象。
// 数据默认为香港地政总署「可视化三维地图」(3D Visualisation Map, f2 端点,
// Cesium 3D Tiles + WGS84);EARTH_3DTILES=<url> 可换源,=1 用默认源。
class Tiles3DLayer
{
public:
    virtual ~Tiles3DLayer() {}
    // 图层开关。首次开启时才在后台线程拉取根 tileset(懒加载,不阻塞主线程),
    // 就绪后由 update 回调在主线程挂树;之后的开关只切 NodeMask。
    virtual void setEnabled(bool on) = 0;
    virtual bool isEnabled() const = 0;
};

// 构建 3D Tiles 图层节点(返回挂到 sceneCamera 的 Group,构造时不联网)。
// 通过 outLayer 输出控制接口(生命周期随返回节点)。
osg::Node* configure3DTilesLayer(osgViewer::View& viewer, osg::Node* earthRoot,
                                 const std::string& mainFolder, Tiles3DLayer** outLayer);

#endif
