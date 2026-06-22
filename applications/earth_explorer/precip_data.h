#ifndef EARTH_PRECIP_DATA_H
#define EARTH_PRECIP_DATA_H

#include <osg/Referenced>
#include <osg/ref_ptr>
#include <osgViewer/View>

// 降水(RainViewer)层控制器。后台抓帧并通过 TileManager 设置 OVERLAY 槽的瓦片模板。
// 不渲染节点、不碰着色器——只切瓦片层路径(复用现有 OVERLAY 管线 + check() 重载)。
class PrecipController : public osg::Referenced
{
public:
    virtual void setEnabled(bool on) = 0;   // 开:后台抓帧→设 OVERLAY=RV 模板;关:线程空转
    virtual bool isEnabled() const = 0;
protected:
    virtual ~PrecipController() {}
};

// 创建控制器并启动后台线程(常驻,仅 isEnabled() 时联网)。调用方(main)用 ref_ptr 持有。
osg::ref_ptr<PrecipController> configurePrecipLayer(osgViewer::View& viewer);

#endif
