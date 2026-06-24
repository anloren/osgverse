#ifndef EARTH_TILES3D_DATA_H
#define EARTH_TILES3D_DATA_H

#include <string>
#include <osg/Node>
#include <osgViewer/View>

// 构建 3D Tiles 叠加层。当环境变量 EARTH_3DTILES=<url> 已设置时,通过
// osgdb_3dtiles 插件流式加载 Cesium 3D Tiles 集合并返回挂到 sceneCamera
// 的节点;未设置时返回空 Group(零影响)。
osg::Node* configure3DTilesLayer(osgViewer::View& viewer, osg::Node* earthRoot,
                                 const std::string& mainFolder);

#endif
