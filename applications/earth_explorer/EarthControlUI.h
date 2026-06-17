#ifndef EARTH_CONTROL_UI_H
#define EARTH_CONTROL_UI_H

#include <osg/Vec3d>
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

    EarthControlUI(osgVerse::EarthManipulator* m, osgVerse::EarthAtmosphereOcean* e)
        : _mani(m), _earth(e) {}

    virtual void runInternal(osgVerse::ImGuiManager* mgr)
    {
        ImFont* font = ImGuiFonts.count("LXGWFasmartGothic") ? ImGuiFonts["LXGWFasmartGothic"] : NULL;
        if (font) ImGui::PushFont(font);
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(340, 480), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Earth Control / 地球控制台"))
        {
            ImGui::Text("osgVerse EarthExplorer");
        }
        ImGui::End();
        if (font) ImGui::PopFont();
    }
};

#endif
