#include <osg/Referenced>
#include <osg/ref_ptr>
#include <iostream>
#include <string>
#include "precip_data.h"

namespace
{
    class PrecipControllerImpl : public PrecipController
    {
    public:
        PrecipControllerImpl() : _enabled(false) {}
        virtual void setEnabled(bool on) { _enabled = on; }
        virtual bool isEnabled() const { return _enabled; }
    protected:
        virtual ~PrecipControllerImpl() {}
        bool _enabled;
    };
}

osg::ref_ptr<PrecipController> configurePrecipLayer()
{
    return osg::ref_ptr<PrecipController>(new PrecipControllerImpl);
}
