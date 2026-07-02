#ifndef EARTH_QUAKE_DATA_H
#define EARTH_QUAKE_DATA_H

#include <string>
#include <osg/Node>
#include <osgViewer/View>

// 单条地震记录(也是 UI 详情卡片的数据载体)。
struct QuakeInfo
{
    double lon = 0.0, lat = 0.0;   // 度
    double depthKm = 0.0, mag = 0.0;
    long long timeMs = 0;          // 毫秒 epoch (UTC)
    std::string place, url;
    bool valid = false;            // false = 当前无选中
};

// 地震层对外接口。EarthControlUI / earth_main 只依赖这个抽象,不碰内部实现。
class QuakeLayer
{
public:
    virtual ~QuakeLayer() {}
    virtual void setEnabled(bool on) = 0;       // 图层开关:开→联网抓取;关→线程空转+隐藏
    virtual bool isEnabled() const = 0;
    virtual QuakeInfo getSelected() const = 0;  // 点击选中的地震(valid=false 表示无)
    virtual void clearSelected() = 0;
    // 汇总统计(供 AI 工具 get_quakes_summary 用):总数、最大震级(+地点/时间)、
    // 震级直方图、近 24h 数量。JSON 字符串,字段见 quake_data.cpp 实现注释。
    virtual std::string summaryJson() const = 0;
};

// 构建地震层场景节点。返回挂到 sceneCamera 的 osg::Group;
// 通过 outLayer 输出 QuakeLayer* 供 UI/main 控制(生命周期随返回节点)。
osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                             const std::string& mainFolder, QuakeLayer** outLayer);

#endif
