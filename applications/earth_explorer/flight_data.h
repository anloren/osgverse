#ifndef EARTH_FLIGHT_DATA_H
#define EARTH_FLIGHT_DATA_H
#include <string>
#include <osg/Node>
#include <osgViewer/View>

struct FlightInfo
{
    std::string callsign, country;
    double lon = 0.0, lat = 0.0, altM = 0.0, velMS = 0.0, headingDeg = 0.0;
    bool valid = false;
};

class FlightLayer
{
public:
    virtual ~FlightLayer() {}
    virtual void setEnabled(bool on) = 0;
    virtual bool isEnabled() const = 0;
    virtual void setViewBBox(double latMin, double lonMin, double latMax, double lonMax) = 0; // 主线程每帧设
    virtual FlightInfo getSelected() const = 0;
    virtual void clearSelected() = 0;
};

extern osg::Node* configureFlightLayer(osgViewer::View& viewer, osg::Node* earthRoot,
                                       const std::string& mainFolder, FlightLayer** outLayer);
#endif
