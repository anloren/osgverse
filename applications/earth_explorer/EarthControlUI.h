#ifndef EARTH_CONTROL_UI_H
#define EARTH_CONTROL_UI_H

#include <cmath>
#include <osg/Vec3d>
#include <osgViewer/Viewer>
#include <ui/ImGui.h>
#include <ui/ImGuiComponents.h>
#include <readerwriter/EarthManipulator.h>
#include <pipeline/Utilities.h>

// EarthExplorer 的 ImGui 控制面板。直接驱动 EarthManipulator 与 EarthAtmosphereOcean，
// 不经 USER 事件中转。
struct EarthControlUI : public osgVerse::ImGuiContentHandler
{
    osgVerse::EarthManipulator* _mani;
    osgVerse::EarthAtmosphereOcean* _earth;
    osgViewer::Viewer* _viewer;                      // 用于退出程序
    float _sunAz, _sunEl;     // 太阳方位角/高度角（度）
    float _exposure;          // HDR 曝光
    bool  _ocean;             // 海洋开关
    float _gotoLat, _gotoLon, _gotoAltKm;            // 跳转目标
    int   _bookmarkTime;                             // 下一个书签的时间戳（帧）

    EarthControlUI(osgVerse::EarthManipulator* m, osgVerse::EarthAtmosphereOcean* e, osgViewer::Viewer* v)
        : _mani(m), _earth(e), _viewer(v), _sunAz(0.0f), _sunEl(0.0f), _exposure(0.25f), _ocean(true)
        , _gotoLat(35.36f), _gotoLon(138.73f), _gotoAltKm(50.0f), _bookmarkTime(0) {}

    void updateSun()
    {
        float az = osg::DegreesToRadians(_sunAz), el = osg::DegreesToRadians(_sunEl);
        osg::Vec3 dir(cosf(el) * cosf(az), cosf(el) * sinf(az), sinf(el));
        _earth->commonUniforms["WorldSunDir"]->set(dir);
    }

    virtual void runInternal(osgVerse::ImGuiManager* mgr)
    {
        ImFont* font = ImGuiFonts.count("LXGWFasmartGothic") ? ImGuiFonts["LXGWFasmartGothic"] : NULL;
        if (font) ImGui::PushFont(font);
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        // 自适应内容高度，不出现滚动条
        if (ImGui::Begin("Earth Control / 地球控制台", NULL,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoScrollbar))
        {
            // ---- 相机读数 ----
            if (ImGui::CollapsingHeader(u8"相机 Camera", ImGuiTreeNodeFlags_DefaultOpen))
            {
                osg::Vec3d lla = _mani->computeEyeLatLonHeight();  // 弧度, 弧度, 米
                ImGui::Text(u8"纬度 Lat: %.5f", osg::RadiansToDegrees(lla[0]));
                ImGui::Text(u8"经度 Lon: %.5f", osg::RadiansToDegrees(lla[1]));
                ImGui::Text(u8"高度 Alt: %.1f km", lla[2] / 1000.0);
                if (ImGui::Button(u8"回到全球视角 Home")) _mani->home(0.0);
            }

            // ---- 太阳/光照 ----
            if (ImGui::CollapsingHeader(u8"太阳 Sun", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::SliderFloat(u8"方位角 Azimuth", &_sunAz, -180.0f, 180.0f, "%.0f")) updateSun();
                if (ImGui::SliderFloat(u8"高度角 Elevation", &_sunEl, -90.0f, 90.0f, "%.0f")) updateSun();
                if (ImGui::Button(u8"太阳对准相机")) {
                    osg::Vec3d lla = _mani->computeEyeLatLonHeight();
                    _sunAz = (float)osg::RadiansToDegrees(lla[1]);
                    _sunEl = (float)osg::RadiansToDegrees(lla[0]);
                    updateSun();
                }
            }

            // ---- 渲染 ----
            if (ImGui::CollapsingHeader(u8"渲染 Render", ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::Checkbox(u8"海洋 Ocean", &_ocean))
                    _earth->commonUniforms["OceanOpaque"]->set(_ocean ? 1.0f : 0.0f);
                if (ImGui::SliderFloat(u8"曝光 Exposure", &_exposure, 0.05f, 1.0f, "%.2f"))
                    _earth->commonUniforms["HdrExposure"]->set(_exposure);
            }
            // ---- 跳转 ----
            if (ImGui::CollapsingHeader(u8"跳转 Go To", ImGuiTreeNodeFlags_DefaultOpen))
            {
                ImGui::InputFloat(u8"纬度 Lat", &_gotoLat, 0.0f, 0.0f, "%.4f");
                ImGui::InputFloat(u8"经度 Lon", &_gotoLon, 0.0f, 0.0f, "%.4f");
                ImGui::InputFloat(u8"高度 km", &_gotoAltKm, 0.0f, 0.0f, "%.1f");
                if (ImGui::Button(u8"飞过去 Go"))
                {
                    _mani->setByEye(osg::DegreesToRadians((double)_gotoLat),
                                    osg::DegreesToRadians((double)_gotoLon),
                                    (double)_gotoAltKm * 1000.0);
                }
            }

            // ---- 书签/巡游 ----
            if (ImGui::CollapsingHeader(u8"书签 Bookmarks"))
            {
                if (ImGui::Button(u8"记录当前视角 Save"))
                {
                    _mani->insertControlPointFromCurrentView((float)_bookmarkTime);
                    _bookmarkTime += 60;
                }
                ImGui::SameLine();
                ImGui::Text(u8"已存 %d", (int)_mani->getControlPoints().size());
                if (ImGui::Button(u8"播放巡游 Play"))
                {
                    if (!_mani->getControlPoints().empty()) _mani->startAnimation();
                }
                ImGui::SameLine();
                if (ImGui::Button(u8"停止 Stop")) _mani->stopAnimation(false);
                if (ImGui::Button(u8"清空 Clear")) { _mani->clearControlPoints(); _bookmarkTime = 0; }
            }

            // ---- 退出 ----
            ImGui::Separator();
            if (ImGui::Button(u8"退出程序 Quit", ImVec2(-1.0f, 0.0f)))
                if (_viewer) _viewer->setDone(true);
        }
        ImGui::End();
        if (font) ImGui::PopFont();
    }
};

#endif
