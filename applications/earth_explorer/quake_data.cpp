#include <osg/Geometry>
#include <osg/Geode>
#include <osg/Point>
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
#include "quake_data.h"

namespace
{
    // 内部具体类(后续任务逐步填充)。返回的 Group 经 setUserData 持有本对象;
    // 本对象用裸指针引用 Group(不拥有它),避免引用环。
    class QuakeLayerImpl : public osg::Referenced, public QuakeLayer
    {
    public:
        QuakeLayerImpl() : _root(nullptr), _enabled(false) {}
        virtual void setEnabled(bool on) { _enabled = on; if (_root) _root->setNodeMask(on ? ~0u : 0u); }
        virtual bool isEnabled() const { return _enabled; }
        virtual QuakeInfo getSelected() const { return QuakeInfo(); }
        virtual void clearSelected() {}

        osg::Group* root() { return _root; }

        // 创建并返回场景根(调用方负责持有/拥有它)。
        osg::Group* buildScene()
        {
            _root = new osg::Group;
            _root->setNodeMask(0);   // 默认关
            return _root;
        }
    protected:
        osg::Group* _root;   // 裸指针:由返回节点经 UserData 拥有,本对象不拥有
        bool _enabled;
    };
}

osg::Node* configureQuakeData(osgViewer::View& viewer, osg::Node* earthRoot,
                              const std::string& mainFolder, QuakeLayer** outLayer)
{
    osg::ref_ptr<QuakeLayerImpl> impl = new QuakeLayerImpl;
    osg::Group* root = impl->buildScene();
    root->setUserData(impl.get());   // root 拥有 impl(无环:impl 用裸指针引用 root)
    if (outLayer) *outLayer = impl.get();
    return root;
}
